# 2D Acceleration Plan — Intel Ironlake Gen5

| | |
|---|---|
| **Status** | 📋 Plan |
| **Category** | Plan |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

# Sony Vaio VPCEB3K1E (device 0x0046)

**Date:** March 21, 2026
**Baseline benchmark (system driver):** FillRect ~66K/s, CopyBits ~9.2K/s, FullClear ~65K/s
**Baseline benchmark (patched driver):** FillRect ~60K/s, CopyBits ~9.5K/s, FullClear ~52K/s
**Goal:** Outperform the system driver on all tests, with absolute stability

---

## Current Architecture

```
CPU (app_server) --> Lock ring --> Scrivi comando MMIO --> Aggiorna TAIL --> Unlock
                                       |
                                       v
                              Ring Buffer (64KB)
                                       |
                                       v
                              GPU BLT Engine --> Framebuffer Lineare
```

**Problems:**
1. Every command = lock + MakeSpace polling + DWORD-by-DWORD write + unlock
2. Linear framebuffer = inefficient cache access for rectangles
3. Sync via HEAD register polling = busy-wait on every wait_engine_idle

---

## Target Architecture

```
CPU (app_server) --> Scrivi in Batch Buffer (WC) --> MI_BATCH_BUFFER_START nel ring
                           |                                    |
                           v                                    v
                    Batch Buffer (GTT, 64KB)            Ring Buffer (2MB)
                           |                                    |
                           v                                    v
                    GPU BLT Engine --> Framebuffer X-Tiled + Fence Register
                           |
                           v
                    MI_STORE_DWORD_INDEX --> HWS Page (sequence number)
                           |
                           v
                    CPU legge seq# dalla HWS (no polling HEAD)
```

---

## PHASE 1: Batch Buffer System

### Goal
Eliminate the per-command overhead of the ring buffer. Write BLT commands
into a GTT buffer (fast, write-combining) and submit them with a single
`MI_BATCH_BUFFER_START` in the ring.

### Prerequisites
- Allocate a GTT buffer for the batch (64KB)
- Define MI_BATCH_BUFFER_START (0x31 << 23 = 0x62000000) and MI_BATCH_BUFFER_END (0x0A << 23 = 0x14000000)

### Files to Modify

**intel_extreme.h** - Add defines:
```cpp
#define MI_BATCH_BUFFER_START    (0x31 << 23)
#define MI_BATCH_BUFFER_END      (0x0A << 23)
#define MI_STORE_DWORD_INDEX     (0x21 << 23)
#define MI_NOOP                  0x00000000
```

**accelerant.h** - Add the batch_buffer structure:
```cpp
struct batch_buffer {
    addr_t      base;           // virtual address (WC mapped)
    uint32      offset;         // GTT offset for the GPU
    uint32      size;           // dimensione totale (64KB)
    uint32      position;       // current write position
};
```
Add `batch_buffer primary_batch;` to `intel_shared_info`.

**engine.cpp** - New BatchCommands class:
```cpp
class BatchCommands {
public:
    BatchCommands(batch_buffer &batch, ring_buffer &ring);
    ~BatchCommands();  // Scrive MI_BATCH_BUFFER_END, poi MI_BATCH_BUFFER_START nel ring

    void Put(struct command &command, size_t size);
    void Write(uint32 data);

private:
    batch_buffer &fBatch;
    ring_buffer  &fRing;
    uint32       fStartPosition;
};
```

The destructor:
1. Writes `MI_BATCH_BUFFER_END` to the batch
2. Aligns to 8 bytes with MI_NOOP
3. Acquires the ring lock
4. Writes `MI_BATCH_BUFFER_START` + batch address into the ring
5. Updates the ring TAIL
6. Releases the lock
7. Resets the batch position

### Updated BLT Functions
The functions `intel_fill_rectangle`, `intel_screen_to_screen_blit`, etc.
will use `BatchCommands` instead of `QueueCommands`:
```cpp
void intel_fill_rectangle(engine_token* token, uint32 color,
    fill_rect_params* params, uint32 count)
{
    BatchCommands batch(gInfo->shared_info->primary_batch,
                        gInfo->shared_info->primary_ring_buffer);

    xy_color_blit_command blit(false);
    blit.color = color;

    for (uint32 i = 0; i < count; i++) {
        blit.dest_left = params[i].left;
        blit.dest_top = params[i].top;
        blit.dest_right = params[i].right + 1;
        blit.dest_bottom = params[i].bottom + 1;
        batch.Put(blit, sizeof(blit));
    }
}
// ~BatchCommands() auto-submits via MI_BATCH_BUFFER_START
```

### Advantages
- Batch writes to WC memory = ~10x faster than per-DWORD MMIO
- A single ring lock/unlock for N commands (instead of one per batch)
- No MakeSpace polling for every single command
- The ring buffer is used only for MI_BATCH_BUFFER_START (2 DWORDs)

### Test
```
Benchmark PRIMA (QueueCommands):     FillRect ~60K/s
Benchmark DOPO  (BatchCommands):     FillRect target >100K/s
```

### Rollback
Keep `QueueCommands` as a fallback. A `use_batch_buffer` flag in shared_info
allows switching at runtime.

---

## PHASE 2: X-Tiled Framebuffer

### Goal
Convert the framebuffer from linear to X-Tiled to improve BLT engine
cache locality and reduce display engine memory bandwidth.

### What X-Tiling Is
```
Lineare:                         X-Tiled:
Riga 0: pixel 0,1,2,3,...       Tile 0 (4KB):
Riga 1: pixel 0,1,2,3,...         Riga 0: 128 pixel (512B)
Riga 2: pixel 0,1,2,3,...         Riga 1: 128 pixel (512B)
...                                ...
                                   Riga 7: 128 pixel (512B)
                                 Tile 1 (4KB):
                                   Riga 0: pixel 128-255
                                   ...
```
Vertically adjacent pixels are in the same tile (4KB) = the same
GPU cache line. For rectangular operations this is much better.

### Prerequisites
- A free fence register (Gen5 has 16)
- Stride must be a power of 2 for tiling
- Alignment: framebuffer base aligned to the size of the tiled region

### The Stride Problem
1366 pixels * 4 bytes = 5464, aligned to 64 = 5504 bytes.
For X-tiling the stride must be a power of 2: **8192 bytes** (2048 pixels).
This means allocating more memory for the framebuffer (8192 * 768 = 6.3MB
instead of 5504 * 768 = 4.2MB), but the GPU has a 256MB+ aperture.

### Files to Modify

**Kernel driver (intel_extreme.cpp)** - Allocate the tiled framebuffer:
- Compute stride as a power of 2
- Allocate the framebuffer with the required alignment
- Program a fence register:
```cpp
// FENCE register format per Gen5 (I915_FENCE):
// Bit 0: Valid
// Bit 1: X-Tile (0) / Y-Tile (1) -- 0 for us
// Bit 11:4: Pitch (in tile widths, 128B increments per X-tile)
// Bit 31:12: Start address (4KB aligned)
// Bit 43:32: End address (4KB aligned) -- in the second DWORD for 64-bit

uint32 fence_pitch_val = (stride / 128) - 1; // stride in 128B units
uint64 fence_val = start_addr | (fence_pitch_val << 2) | FENCE_VALID;
// Scrivere in FENCE_REG_SANDYBRIDGE_0 + n*8 per Gen5
```

**Accelerant (mode.cpp)** - Update set_display_mode:
- Set `DISPLAY_CONTROL_TILED` (bit 10) in DSPCNTR
- Use the tiled stride in the DSPSTRIDE register

**Accelerant (commands.h)** - Add a tiling flag to BLT commands:
```cpp
// Nel costruttore xy_command, se il framebuffer e tiled:
if (gInfo->shared_info->frame_buffer_tiled) {
    // XY_COLOR_BLT: bit 11 = dst tiling
    opcode |= (1 << 11);
    // The stride in BLT commands for tiled surfaces is in DWORDs, not bytes
    dest_bytes_per_row = gInfo->shared_info->bytes_per_row >> 2;
}
```

**Accelerant (commands.h)** - For XY_SRC_COPY_BLT with tiling:
```cpp
// bit 15 del DWORD 0 = src tiling
if (gInfo->shared_info->frame_buffer_tiled) {
    opcode |= (1 << 15);  // source tiled
    source_bytes_per_row = dest_bytes_per_row;  // already in DWORDs
}
```

### Risks
- **HIGH**: Power-of-2 stride requires modifying the kernel driver
  (different framebuffer allocation)
- **MEDIUM**: The fence register must be configured correctly or
  you get total screen corruption
- **LOW**: Applications accessing the framebuffer via CPU will see
  a tiled pattern without a fence -- fences are needed for linear CPU access

### Test
```
Benchmark PRIMA (lineare):     FullClear ~65K/s, ~164 MB/s
Benchmark DOPO  (X-Tiled):    FullClear target >100K/s, >250 MB/s
```

### Rollback
A `frame_buffer_tiled` flag in shared_info. If false, everything works
as before (linear).

---

## PHASE 3: Hardware Status Page Sync

### Goal
Eliminate busy-wait polling of the ring HEAD register in
`intel_wait_engine_idle()`. Use a sequence number in the HWS page.

### Mechanism
1. After each batch of commands, insert `MI_STORE_DWORD_INDEX`:
   - Writes an incrementing sequence number to a known location in the HWS page
2. In `intel_wait_engine_idle()`:
   - Read the sequence number from the HWS page (cached, very fast)
   - If sequence_number >= last_submitted, the GPU is done
   - No MMIO polling of the HEAD register

### Files to Modify

**accelerant.h** - Add a counter:
```cpp
// In intel_shared_info:
vint32      last_submitted_seq;    // ultimo seq# inviato alla GPU
// The HWS page is already allocated: status_page / physical_status_page
// We use status_page->store[0] for our sequence number
```

**engine.cpp** - Modify BatchCommands::~BatchCommands():
```cpp
// Before MI_BATCH_BUFFER_END, add:
uint32 seq = atomic_add(&gInfo->shared_info->last_submitted_seq, 1) + 1;
Write(MI_STORE_DWORD_INDEX);
Write(0);  // offset 0 nella HWS page (store[0])
Write(seq);
Write(MI_BATCH_BUFFER_END);
```

**engine.cpp** - Modify intel_wait_engine_idle():
```cpp
void intel_wait_engine_idle()
{
    hardware_status* hws = (hardware_status*)gInfo->shared_info->status_page;
    uint32 target = gInfo->shared_info->last_submitted_seq;
    bigtime_t start = system_time();

    while (hws->store[0] < target) {
        if (system_time() > start + 1000000LL) {
            ERROR("GPU timeout waiting for seq %d (current %d)\n",
                target, hws->store[0]);
            break;
        }
        spin(1);  // micro-wait, molto piu leggero del polling MMIO
    }
}
```

### Advantages
- Reading the HWS page = cached memory read (1-2 ns)
- Reading the HEAD register = uncached MMIO (~100-500 ns)
- Sync latency reduced by 50-100x

### Test
```
Benchmark sync-heavy (molti Sync() calls):
PRIMA (polling HEAD):    latenza ~500us per sync
DOPO  (HWS seq#):       latenza target <50us per sync
```

### Rollback
If hws->store[0] is not updated by the GPU, fall back to traditional
HEAD polling.

---

## BONUS PHASE: 2MB Ring Buffer

### Goal
Increase the ring buffer from 64KB (16 pages) to 2MB (512 pages) to
reduce the frequency of ring-full stalls.

### Change
In the kernel driver, change the allocation:
```cpp
// DA: 16 pagine (64KB)
// A:  512 pagine (2MB) -- massimo supportato da Gen5
```

The RING_BUFFER_CONTROL register bits 20:12 support up to 0x1FF = 511
pages = ~2MB.

### Risk: LOW
The larger ring buffer only reduces the probability of a stall.
It does not change the operating logic.

---

## Implementation Order

| Phase | Description | Risk | Impact | Dependencies |
|------|-------------|---------|---------|------------|
| **BONUS** | 2MB Ring Buffer | Low | Low | None |
| **3** | HWS Sync | Low | Medium | None |
| **1** | Batch Buffer | Medium | **High** | Phase 3 (optional) |
| **2** | X-Tiling | High | **High** | Kernel modification |

Recommended order: BONUS → 3 → 1 → 2

The BONUS and 3 phases are independent, low-risk, and improve the
infrastructure. Phase 1 is the change with the biggest performance
impact. Phase 2 requires kernel driver modifications and should be
tackled last.

---

## Test Protocol

### Benchmark Suite
Use `bench/bench_2d` with 3 runs per configuration.
Add specific tests for sync latency.

### Test Matrix

| Test | What It Measures | Affected By |
|------|-------------|-------------|
| FillRect (5000 random) | Fill throughput + command overhead | Phase 1 |
| CopyBits (2000 random) | Blit throughput + command overhead | Phase 1 |
| InvertRect (5000) | Invert throughput + overhead | Phase 1 |
| FullClear (500 fullscreen) | Pure GPU memory bandwidth | Phase 2 |
| SyncLatency (1000 sync) | Synchronization overhead | Phase 3 |
| SmallRects (10000 16x16) | Per-command overhead (dominant) | Phase 1 |
| LargeRects (100 800x600) | Pure bandwidth (dominant) | Phase 2 |

### New Tests to Add to bench_2d.cpp

```cpp
// Test 5: Sync latency
start = system_time();
for (int i = 0; i < 1000; i++) {
    SetHighColor(i % 256, 0, 0);
    FillRect(BRect(0, 0, 10, 10));
    Sync();
}
elapsed = system_time() - start;
printf("SyncLatency: 1000 syncs in %lld us (%.1f us/sync)\n",
    elapsed, (double)elapsed / 1000.0);

// Test 6: Small rects (overhead dominated)
start = system_time();
for (int i = 0; i < 10000; i++) {
    FillRect(BRect(rand()%780, rand()%580, rand()%780+16, rand()%580+16));
}
Sync();
elapsed = system_time() - start;
printf("SmallRects: 10000 rects in %lld us (%.1f rects/sec)\n",
    elapsed, 10000.0 * 1000000.0 / elapsed);

// Test 7: Large rects (bandwidth dominated)
start = system_time();
for (int i = 0; i < 100; i++) {
    SetHighColor(rand()%256, rand()%256, rand()%256);
    FillRect(bounds);
}
Sync();
elapsed = system_time() - start;
printf("LargeRects: 100 fills in %lld us (%.1f MB/s)\n",
    elapsed, 100.0 * WINDOW_W * WINDOW_H * 4.0 / elapsed);
```

### Success Criteria

| Metric | Baseline (system) | Target |
|---------|-------------------|--------|
| FillRect | ~66K/s | >100K/s (+50%) |
| CopyBits | ~9.2K/s | >15K/s (+63%) |
| FullClear | ~65K/s (164 MB/s) | >100K/s (250+ MB/s) |
| SyncLatency | TBD | <50 us/sync |
| SmallRects | TBD | >150K/s |
| Stability | No crashes | Zero crashes in 24h |

---

## Risks and Mitigations

### GPU Hang
**Cause:** Malformed commands, corrupted batch buffer, incorrect tiling
**Mitigation:** Always keep a backup of the working accelerant.
Add a timeout and ring reset in case of a stall.
```bash
# Rollback emergenza:
cp intel_extreme.accelerant.backup \
   /boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant
shutdown -r
```

### Display Corruption
**Cause:** Incorrect fence register, wrong tiled stride, DSPCNTR not updated
**Mitigation:** Implement X-Tiling as the last phase. Test first
with a tiled offscreen surface before converting the framebuffer.

### Performance Regression
**Cause:** Batch buffer overhead for small single operations
**Mitigation:** Minimum threshold: use batching only when count > 4.
For single operations, keep the direct ring buffer path.

---

## References

### Intel Documentation
- Intel Open Source HD Graphics PRM Volume 1 (Gen5/Ironlake)
- BSpec: BLT Engine Command Streamer
- Intel GPU Commands: MI_BATCH_BUFFER_START, MI_STORE_DWORD_INDEX

### Linux Code
- `drivers/gpu/drm/i915/gt/intel_gpu_commands.h` -- opcode commands
- `drivers/gpu/drm/i915/gt/intel_ggtt_fencing.c` -- fence registers
- `xf86-video-intel/src/sna/sna_blt.c` -- BLT 2D acceleration
- `xf86-video-intel/src/sna/kgem.c` -- batch buffer management

### Haiku Code
- `intel_extreme/accelerant/engine.cpp` -- current implementation
- `intel_extreme/accelerant/commands.h` -- BLT command structures
- `intel_extreme/accelerant/accelerant.h` -- shared structures
- `headers/private/graphics/intel_extreme/intel_extreme.h` -- HW registers
