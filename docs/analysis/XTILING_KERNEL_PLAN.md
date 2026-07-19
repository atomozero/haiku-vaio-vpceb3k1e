# X-Tiling via Kernel Driver — Plan

| | |
|---|---|
| **Status** | 📋 Plan |
| **Category** | Plan |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

**Date:** March 23, 2026

## Files to modify

### 1. Shared header (kernel + accelerant)
**File:** `headers/private/graphics/intel_extreme/intel_extreme.h`
**Local copy:** `intel_extreme/accelerant/intel_extreme.h`

**Change:** Add fields at the END of `intel_shared_info`:
```cpp
    child_device_config device_configs[10];

    // X-Tiling support (added at END to preserve ABI)
    bool            frame_buffer_tiled;
    uint32          fence_register_index;
    uint32          tiled_bytes_per_row;  // power-of-2 stride
};
```

**ABI safety:** The fields are at the END of the struct. The kernel allocates
`ROUND_TO_PAGE_SIZE(sizeof(intel_shared_info)) + 3 * B_PAGE_SIZE` (line 614),
so there is plenty of headroom. The fields will be zero by default (the area is zeroed).

### 2. Kernel driver
**File:** `src/add-ons/kernel/drivers/graphics/intel_extreme/intel_extreme.cpp`

**Change 1 (line ~731, after ring buffer allocation):**
Add tiled framebuffer allocation and fence programming.

```cpp
// After the ring buffer and before init_overlay_registers:

// X-Tiling: allocate tiled framebuffer for Ironlake
if (info.device_type.InGroup(INTEL_GROUP_ILK)) {
    // Compute a power-of-2 stride for 1366x768x32
    uint32 linearStride = 1366 * 4;  // will be recomputed by the accelerant
    uint32 tiledStride = 512;
    while (tiledStride < linearStride)
        tiledStride <<= 1;

    uint32 fbSize = tiledStride * 768;  // will be overwritten by the accelerant
    uint32 fenceSize = 1;
    while (fenceSize < fbSize)
        fenceSize <<= 1;

    // Do not allocate the FB here — the accelerant does it in intel_set_display_mode.
    // Only program the fence WHEN the accelerant sets frame_buffer_tiled.

    // We could pre-program the fence register here for testing purposes.
    // The fence will be updated by the accelerant when it allocates the FB.

    info.shared_info->frame_buffer_tiled = false;
    info.shared_info->fence_register_index = 0;
    info.shared_info->tiled_bytes_per_row = 0;
}
```

**Change 2 (better option):**
Do not program the fence in the kernel — let the accelerant do it as before,
but use `shared_info->frame_buffer_tiled` (now at the END of the struct,
with no ABI break) to communicate state between kernel and accelerant.

### 3. Accelerant — mode.cpp
**Change:** Use `sharedInfo.frame_buffer_tiled` instead of `gInfo->frame_buffer_tiled`.

Now that the field is at the end of shared_info (no ABI break), we can:
1. Set `sharedInfo.frame_buffer_tiled = true` after the fence
2. Have all paths (program_pipe_color_modes, BLT, etc.) read it from shared_info
3. Let the kernel read it too if needed

### 4. Accelerant — Pipes.cpp
**Change:** `program_pipe_color_modes` reads `sharedInfo.frame_buffer_tiled`
and sets/clears DSPCNTR bit 10 atomically with the color mode.

```cpp
} else {
    uint32 tiledBit = gInfo->shared_info->frame_buffer_tiled
        ? DISPLAY_CONTROL_TILED : 0;
    write32(INTEL_DISPLAY_A_CONTROL, (read32(INTEL_DISPLAY_A_CONTROL)
        & ~(DISPLAY_CONTROL_COLOR_MASK | DISPLAY_CONTROL_GAMMA
            | DISPLAY_CONTROL_TILED))
        | colorMode | tiledBit);
    // ...
}
```

This RESOLVES the race condition: the tiled bit is never cleared without
being re-set, because the decision is made in the SAME write32.

### 5. Accelerant — commands.h
**Change:** BLT tiling bits based on `gInfo->shared_info->frame_buffer_tiled`.

---

## Sequence of operations at mode set

```
intel_set_display_mode():
  1. Invalidate the fence if the previous FB was tiled
  2. Free the old framebuffer
  3. Compute power-of-2 stride (8192)
  4. Allocate FB with power-of-2 alignment
  5. Program fence at 0x03000
  6. sharedInfo.frame_buffer_tiled = true
  7. memset(FB, 0) ← through the fence, tiled data
  8. Port configuration (LVDS, FDI, etc.)
  9. program_pipe_color_modes() ← reads frame_buffer_tiled, sets DSPCNTR tiled
  10. DSPSTRIDE = 8192
  11. set_frame_buffer_base() ← arms the surface with DSPCNTR already tiled
```

The key point: at step 9, `program_pipe_color_modes` sets the tiled bit
TOGETHER with the color mode in the same write32. There is never a moment
where DSPCNTR has tiled=0 while the fence is active.

---

## Incremental tests

### Test 1: Only add fields to shared_info (no behavior change)
- Add fields at the END of shared_info
- Rebuild kernel with jam + accelerant with make
- Install both
- Verify: display works normally, fields are 0/false

### Test 2: Power-of-2 stride + aligned allocation (without fence)
- Modify mode.cpp for an 8192 stride and aligned allocation
- sharedInfo.frame_buffer_tiled = false
- Verify: linear display with a wider stride

### Test 3: Fence + DSPCNTR (the critical test)
- Program the fence in mode.cpp
- sharedInfo.frame_buffer_tiled = true
- program_pipe_color_modes reads the flag and sets DSPCNTR
- Verify: tiled display working

### Test 4: BLT tiling
- commands.h reads sharedInfo.frame_buffer_tiled
- Stride divided by 4, bits 11/15 in the opcode
- Benchmark to measure the improvement

---

## Build

### Kernel driver
```bash
cd /boot/home/Desktop/haiku-build/generated
# Copy the modified intel_extreme.h into the source tree
cp /boot/home/Desktop/Sony\ Vaio\ VPCEB3K1E/intel_extreme/accelerant/intel_extreme.h \
   ../headers/private/graphics/intel_extreme/intel_extreme.h
jam -q hda  # or jam -q intel_extreme if that target exists
```

### Accelerant
```bash
cd /boot/home/Desktop/Sony\ Vaio\ VPCEB3K1E/intel_extreme/accelerant
make clean && make && make install
```

### Kernel driver installation
Via HPKG with BlockedEntries (same as for HDA).

---

## Rollback
```bash
# GPU accelerant:
cp intel_extreme.accelerant.pre_tiling \
   /boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant

# Kernel driver (if modified):
# Remove the HPKG package and the BlockedEntries
```

---

## Risks

| Risk | Probability | Mitigation |
|---------|------------|-------------|
| shared_info ABI break | Very low | Fields at the END, oversized area |
| Fence timing | Low | DSPCNTR tiled set atomically in program_pipe_color_modes |
| DPMS resets DSPCNTR | Medium | Verify the set_display_power_mode path |
| Wrong BLT stride | Low | Flag controls the behavior |
