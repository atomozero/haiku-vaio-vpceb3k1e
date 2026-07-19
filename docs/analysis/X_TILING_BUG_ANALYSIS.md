# X-Tiling Bug Analysis — Root Cause

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Analysis |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

**Date:** March 22, 2026

---

## The Bug

Display corruption with a horizontal-stripe pattern (typical tiling
artifact) after enabling X-Tiling in the accelerant. The display was
unusable, requiring a VESA boot to recover.

## Root Cause: ABI Break in `intel_shared_info`

The `intel_shared_info` struct (defined in `intel_extreme.h`) is
**shared between the kernel driver and the accelerant** via shared
memory. The kernel driver and the accelerant are compiled separately.

The X-Tiling code inserted two fields **IN THE MIDDLE** of the struct:

```cpp
// BEFORE (kernel driver sees this layout):
addr_t    frame_buffer;           // offset 0xNN
uint32    frame_buffer_offset;    // offset 0xNN+8
uint32    fdi_link_frequency;     // offset 0xNN+12  <-- kernel writes here
uint32    hraw_clock;             // offset 0xNN+16
bool      got_vbt;                // offset 0xNN+20
// ... 30+ more fields ...

// AFTER (accelerant sees THIS layout):
addr_t    frame_buffer;           // offset 0xNN
uint32    frame_buffer_offset;    // offset 0xNN+8
bool      frame_buffer_tiled;    // offset 0xNN+12  <-- NEW, 1 byte + 3 padding
uint32    fence_register_index;  // offset 0xNN+16  <-- NEW
uint32    fdi_link_frequency;     // offset 0xNN+20  <-- SHIFTED! Kernel writes at +12
uint32    hraw_clock;             // offset 0xNN+24  <-- SHIFTED
bool      got_vbt;                // offset 0xNN+28  <-- SHIFTED
// ... every other field SHIFTED by 8 bytes ...
```

### Consequences:
- The kernel writes `fdi_link_frequency` at offset +12, but the
  accelerant reads that as `frame_buffer_tiled` → **tiling gets enabled
  at random**
- The kernel writes `got_vbt = true` at offset +20, but the accelerant
  reads `fdi_link_frequency` → **FDI frequency corrupted**
- `device_type`, `pch_info`, `pll_info` all end up at the wrong offsets →
  **unpredictable driver behavior**
- The DSPCNTR tiled bit was getting set because `frame_buffer_tiled`
  was reading a non-zero value from a kernel field → **display
  corruption**

## Why Fixing the Fence Base (0x03000) Alone Didn't Solve It

The corrected fence base (0x03000 instead of 0x100000) was correct, but
the real problem was upstream: the ABI break caused
`frame_buffer_tiled = true` regardless of the tiling code. DSPCNTR was
getting the tiled bit set even when the framebuffer was linear and no
fence was programmed.

## Correct Solution

**Do NOT modify `intel_shared_info`**. Use one of the following instead:

### Option A: Fields in `accelerant_info` (accelerant only)
```cpp
struct accelerant_info {
    // ... existing fields ...
    bool    frame_buffer_tiled;
    uint32  fence_register_index;
};
```
`accelerant_info` is defined only in the accelerant and is not shared
with the kernel.

### Option B: Static globals in engine.cpp
```cpp
static bool sFrameBufferTiled = false;
static uint32 sFenceRegisterIndex = 0;
```

### Option C: Append fields to the END of `intel_shared_info`
```cpp
struct intel_shared_info {
    // ... all existing fields UNCHANGED ...
    child_device_config device_configs[10];
    // New fields at the end — the kernel never accesses them,
    // the accelerant sees them correctly
    bool    frame_buffer_tiled;
    uint32  fence_register_index;
};
```
This option is safe because the kernel allocates the struct with a
fixed size (via `sizeof`) and the new fields will be zero (from the
initial `memset`). However, it requires the kernel to allocate enough
memory for the extended struct — it might work if there's padding, or
it might crash if there isn't.

**Option A is the safest** because it doesn't touch any shared struct.

## Checklist for the Next Attempt

- [ ] Do NOT modify `intel_shared_info` — use `accelerant_info`
- [ ] Verify that `write32(0x3000 + n*8, val)` is accessible from the
      accelerant (test with a read before the write)
- [ ] Program the fence BEFORE setting DSPCNTR tiled
- [ ] Add TRACE with ERROR() (not TRACE()) for visible debugging
- [ ] Test the fence alone FIRST (without DSPCNTR tiled) to verify
      that the fence write doesn't cause a crash
- [ ] Test the sequence incrementally:
      1. Allocation only with power-of-2 stride (no fence, no DSPCNTR)
      2. Add the fence register (without DSPCNTR tiled)
      3. Add DSPCNTR tiled
      4. Add BLT tiling bits
