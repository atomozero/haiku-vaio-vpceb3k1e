# X-Tiling: Notes on the Failed Implementation

| | |
|---|---|
| **Status** | ❌ Abandoned |
| **Category** | Note |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

**Date:** 22 March 2026
**Status:** DISABLED — requires kernel driver support

## What was attempted

### Accelerant changes (commit 47d0317, later reverted):
1. **intel_extreme.h**: Added `frame_buffer_tiled`, `fence_register_index` to shared_info
2. **intel_extreme.h**: Fence register definitions (Gen5: 0x03000, Gen6+: 0x100000)
3. **intel_extreme.h**: `DISPLAY_CONTROL_TILED` (bit 10 of DSPCNTR)
4. **mode.cpp**: FB allocation with power-of-2 stride (8192 for 1366x32bpp)
5. **mode.cpp**: Fence register programming with the Linux i965 sequence
6. **Pipes.cpp**: DSPCNTR tiled bit in `program_pipe_color_modes()`
7. **commands.h**: BLT tiling bits (bit 11 dst, bit 15 src) and stride divided by 4
8. **memory.cpp**: `intel_allocate_memory()` overload with alignment
9. **accelerant.cpp**: Fence cleanup on shutdown

## Bug found and fixed

The fence register base was **WRONG**:
- We were using: `0x100000` (Gen6+ Sandy Bridge)
- Correct for Gen5: `0x03000` (i965 format)

This was fixed, but the display remained corrupted.

## Why it doesn't work

The corruption pattern (horizontal stripes) indicates that:
1. DSPCNTR says "tiled" → the display engine reads in tiled mode
2. But the framebuffer in memory is linear, OR the fence doesn't cover the correct area

### Probable causes:
- **ABI mismatch**: Adding fields to `intel_shared_info` could shift
  subsequent fields, causing corruption in structures shared
  between kernel and accelerant
- **GART alignment**: Allocation with 8MB alignment could fail
  silently in the GART, producing an unaligned buffer and a fence
  that doesn't cover the correct area
- **Fence not accessible**: Even though register 0x03000 is mapped
  in the accelerant, the fence might require kernel-side configuration

### Correct solution:
The fence register should be programmed by the **kernel driver**
(`intel_extreme.cpp`), where:
1. The physical address of the framebuffer is known
2. GART allocation is controlled with the correct alignment
3. There is no risk of ABI mismatch

## Gen5 fence register format (Linux reference)

```
Offset: FENCE_REG_965_LO(i) = 0x03000 + i*8

Low DWORD [31:0]:
  [31:12] = Start offset (GTT, 4K aligned)
  [11:2]  = Pitch: (stride/128 - 1) << 2
  [1]     = Tile walk: 0=X, 1=Y
  [0]     = Valid

High DWORD [63:32]:
  [31:12] = End offset (start + size - 4096, 4K aligned)

Write sequence (Linux i965_write_fence_reg):
  1. write32(lo, 0)     // invalidate
  2. read32(lo)          // posting read
  3. write32(hi, end)    // end address
  4. write32(lo, val)    // start+pitch+valid
  5. read32(lo)          // posting read
```

## Values for Sony Vaio 1366x768 32bpp

```
Stride tiled:     8192 bytes (power of 2)
FB size:          8192 * 768 = 6,291,456 bytes
Fence size (po2): 8,388,608 bytes (8MB)
Pitch value:      (8192/128) - 1 = 63 (0x3F)
```

## Linux reference files

- `drivers/gpu/drm/i915/gt/intel_ggtt_fencing.c`: i965_write_fence_reg()
- `drivers/gpu/drm/i915/i915_reg.h`: FENCE_REG_965_LO, I965_FENCE_PITCH_SHIFT
