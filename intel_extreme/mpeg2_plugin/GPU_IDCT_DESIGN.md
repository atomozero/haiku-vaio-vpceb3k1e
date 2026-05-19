# GPU IDCT Plugin — Design Document

## Problem Statement

The MPEG-2 decoder plugin needs GPU-accelerated IDCT for real-time decode.
The GPU IDCT batch is 4x faster than CPU (100-155us vs 351-905us for 400 blocks).

The plugin runs as a **standalone .so** loaded by media_server — it is NOT
the accelerant. It shares the GPU ring buffer with app_server's accelerant.

## Root Cause of Previous Failures

The original `gpu_idct.cpp` reimplemented ring submission from scratch,
introducing three fatal bugs:

1. **Shared position corruption**: Wrote directly to
   `shared_info->primary_ring_buffer.position`, corrupting app_server's
   ring tracking. App_server would then write commands at wrong offsets.

2. **HEAD polling for completion**: Waited for `HEAD == TAIL` which breaks
   when app_server adds commands between our submission and our wait.
   The working code uses MI_STORE_DATA_IMM markers in GTT memory.

3. **Ring lock misuse**: Acquired the shared ring lock, but the lock
   doesn't help because the fundamental position tracking is wrong.
   The working `gpu_ring` layer is stateless — no lock needed.

## Architecture: What Works

The accelerant's `gpu_ring` layer (`gpu_ring.cpp/h`) is proven working:
- `gpu_idct_bench`: 400 blocks, GPU 4x faster, no reboot
- `gpu_plasma_demo`: 177 FPS continuous GPU dispatch
- `media_pipeline.cpp`: all boot-time tests pass

### gpu_ring Design
- **Local position**: `ring->pos` tracks write position independently
- **Stateless**: Multiple users can coexist without locks
- **Init**: Reads hardware TAIL once, syncs `ring->pos`, never resets
- **Submit**: Writes commands to ring memory, kicks TAIL via ioctl
- **Wait**: `gpu_ring_wait_idle()` polls HEAD == TAIL (from hardware)

### Completion Tracking
Production code uses MI_STORE_DATA_IMM markers:
- GPU writes a tag (0xBEEF0xxx) to a GTT marker buffer
- CPU polls the marker address until tag appears
- Independent of ring HEAD — works even with concurrent ring users

## New Design

### Phase 1: Ring Test (MI_NOOP)
- Open device, clone shared_info and registers
- Init `gpu_ring` (sync with HW TAIL)
- Write 2x MI_NOOP, advance (TAIL kick via ioctl)
- Wait idle, verify HEAD advanced
- **Pass criteria**: HEAD == TAIL after MI_NOOP

### Phase 2: Marker Test
- Allocate marker_bo (GTT buffer, 64 bytes)
- Write MI_STORE_DATA_IMM to marker_bo via ring
- Wait for marker value in CPU-visible GTT memory
- **Pass criteria**: Marker reads 0xBEEF0001

### Phase 3: Media Preamble
- Allocate all GPU BOs (kernel, CURBE, surfaces, etc.)
- Upload IDCT kernel (`idct_single.g4b.gen5`)
- Build 10-command media preamble in batch_bo
- Submit via MI_BATCH_BUFFER_START in ring
- Wait for marker after MI_FLUSH
- **Pass criteria**: Marker written by GPU

### Phase 4: Single Block IDCT
- Upload 1 block of known coefficients to input_bo
- Add 1x MEDIA_OBJECT to batch after preamble
- Submit, wait for marker
- Read output_bo, compare with CPU reference
- **Pass criteria**: GPU output matches CPU IDCT

### Phase 5: Batch IDCT (400 blocks)
- Upload 400 blocks, dispatch 400 MEDIA_OBJECT
- Single submission (preamble + 400 dispatch + flush)
- Verify all 400 outputs match CPU reference
- **Pass criteria**: 400/400 blocks match

### Phase 6: Integration with Plugin
- Wire `gpu_idct_process()` into `_FlushBatch()`
- CPU fallback if GPU fails
- Benchmark: GPU vs CPU decode time per frame

## Key Constraints (from 24 documented Gen5 bugs)

1. **NEVER reset ring** — kills CS permanently (use sync only)
2. **Max 48 EU threads** per submission (URB recycling limit)
3. **Single submission** for media pipeline (no MI_FLUSH between batches)
4. **MMIO writes ignored** from userspace — must use kernel ioctl
5. **LRI hangs CS** on Gen5 — use INTEL_RING_INIT_3D ioctl
6. **SURFTYPE_BUFFER** for OWord Block R/W (not SURFTYPE_2D)
7. **CURBE**: 20 GRFs = 10 x 64-byte units in CONSTANT_BUFFER

## File Dependencies

```
gpu_idct.cpp (new)
  ├── gpu_ring.h/cpp     — ring submission (compile directly)
  ├── idct_ref.h         — kIdctTableGpu cosine table, CPU reference
  ├── intel_extreme.h    — ioctl definitions, ring_buffer struct
  └── kernels/idct_single.g4b.gen5  — EU kernel binary
```

## Build

```makefile
PLUGIN_SOURCES = mpeg2_decoder_plugin.cpp gpu_idct.cpp \
    ../accelerant/gpu_ring.cpp ../accelerant/mpeg2_parser.cpp
```
