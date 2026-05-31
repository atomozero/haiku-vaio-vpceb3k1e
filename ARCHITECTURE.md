# Intel Gen5-Gen8 GPU Driver Stack for Haiku OS — Architecture

**Target hardware:** Intel HD Graphics Gen5 (Ironlake) through Gen8 (Broadwell)
**OS:** Haiku R1~beta5 (hrev59506+)
**Status:** Phase 0 in progress (ring wrap done, media pipeline debug)

**Test machines:**
- **Primary (Gen5):** Sony Vaio VPCEB3K1E — HD Graphics 0x0046 (Ironlake), LVDS 1366x768, ALC269
- **Secondary (Gen8):** Broadwell laptop — HD Graphics 5500 0x1616, eDP 1920x1080, ALC269
  Known issue: PLL registers read 0xFFFFFFFF (FORCEWAKE not asserted in stock driver)

---

## Overview

Complete GPU driver stack: kernel driver + accelerant + DRM library + media plugins + Mesa integration. Covers the last 4 generations of Intel GPUs with classic ring buffer submission (Gen9+ uses GuC, out of scope).

## Components

```
┌─────────────────────────────────────────────────────┐
│                   Applications                       │
│  (MediaPlayer, GLApps, Haiku apps)                  │
├──────────┬──────────┬───────────────────────────────┤
│ media_kit│  Mesa    │     app_server                │
│ plugin   │  crocus  │                               │
├──────────┴────┬─────┴───────────────────────────────┤
│               │  libdrm_haiku (.so)                 │
│               │  C API for GEM/EXECBUF/context      │
├───────────────┴─────────────────────────────────────┤
│              intel_gen_accelerant                    │
│  Display | BLT | 3D Render | Media Pipeline         │
├─────────────────────────────────────────────────────┤
│              intel_gen_gpu (kernel driver)           │
│  GTT | GEM | EXECBUF2 | Ring | Reset | Power | IRQ  │
├─────────────────────────────────────────────────────┤
│                    Hardware                          │
│  Gen5 (ILK) | Gen6 (SNB) | Gen7 (IVB/HSW) | Gen8   │
└─────────────────────────────────────────────────────┘
```

## Directory Structure

```
intel_gen_gpu/                      # Kernel driver (NEW)
    intel_gen_gpu.cpp               # Module init, PCI probe
    device.cpp                      # open/close/ioctl dispatch
    gpu_device.h                    # Device state structs
    gart_gtt.cpp/.h                 # GTT management (Gen5-8)
    gem.cpp/.h                      # GEM buffer objects
    gem_execbuffer.cpp              # EXECBUFFER2
    ring.cpp/.h                     # Ring init, TAIL, health
    reset.cpp/.h                    # Hang detect + recovery
    power.cpp/.h                    # RC6, FORCEWAKE, clocks
    irq.cpp/.h                      # Interrupt handling
    gen5_workarounds.cpp            # ILK register setup
    gen6_workarounds.cpp            # SNB register setup
    gen7_workarounds.cpp            # IVB/HSW register setup
    gen8_workarounds.cpp            # BDW register setup
    ioctl_interface.h               # Versioned ioctl definitions

intel_gen_accelerant/               # Accelerant (NEW, replaces intel_extreme/accelerant)
    accelerant.cpp                  # Entry points
    gen_ops.h                       # Command encoding vtable
    gen5_ops.cpp                    # Gen5 Ironlake (TESTED)
    gen6_ops.cpp                    # Gen6 Sandy Bridge
    gen7_ops.cpp                    # Gen7 Ivy Bridge / Haswell
    gen8_ops.cpp                    # Gen8 Broadwell
    gpu_ring.cpp/.h                 # Ring submission via ioctl
    gpu_bo.cpp/.h                   # Buffer object allocator
    gpu_debug.cpp/.h                # Diagnostics
    display/
        crtc.cpp/.h                 # CRTC abstraction
        encoder.cpp/.h              # LVDS, HDMI, DP, eDP
        connector.cpp/.h            # Hotplug, EDID
        plane.cpp/.h                # Primary, cursor, overlay
        watermarks.cpp              # Per-gen watermarks
    engine.cpp                      # 2D BLT
    render.cpp/.h                   # 3D pipeline
    media_pipeline.cpp/.h           # EU kernel dispatch
    mpeg2_parser.cpp/.h             # MPEG-2 bitstream parser
    commands.h                      # HW command definitions
    kernels/                        # EU kernel sources (.g4a)

libdrm_haiku/                       # DRM compatibility library (NEW)
    libdrm_haiku.cpp/.h             # Core API
    intel_bufmgr_haiku.cpp/.h       # Buffer manager with caching
    gem.cpp                         # GEM operations
    execbuffer.cpp                  # EXECBUFFER2 + relocations
    context.cpp                     # HW context management
    syncobj.cpp                     # Sync objects
    include/
        drm.h                       # Linux-compatible headers
        drm_i915_drm.h
        xf86drm.h

media_plugins/                      # media_kit plugins (NEW)
    mpeg2/
        mpeg2_decoder_plugin.cpp    # MPEG-2 I/P/B decode
        gpu_idct.cpp/.h             # GPU IDCT via libdrm_haiku
    h264/                           # Future
        h264_decoder_plugin.cpp
        gpu_mc.cpp/.h

mesa_addon/                         # Mesa GL renderer addon (NEW)
    CrocusRenderer.cpp
    haiku_gl_bridge.cpp

tests/                              # Unified test suite
    test_ring.cpp                   # Ring submission stress
    test_gem.cpp                    # GEM alloc/free/mmap
    test_execbuffer.cpp             # Batch submission
    test_media.cpp                  # Media pipeline
    test_blt.cpp                    # BLT engine
    test_display.cpp                # Mode setting
    test_reset.cpp                  # Hang recovery
    test_gen_detect.cpp             # Generation detection
    run_all.sh                      # Test runner
```

## Per-Generation Abstraction

Three vtable layers, each in a different address space:

### gen_ops (userspace — command encoding)
```c
struct gen_ops {
    uint32 generation;
    uint32 max_eu_threads;
    void (*emit_mi_flush)(batch_writer*);
    void (*emit_pipeline_select_media)(batch_writer*);
    void (*emit_pipeline_select_3d)(batch_writer*);
    void (*emit_state_base_address)(batch_writer*);
    void (*emit_urb_fence)(batch_writer*, uint32 count);
    void (*emit_media_state_pointers)(batch_writer*, uint32 vfe, uint32 idrt);
    void (*emit_cs_urb_state)(batch_writer*);
    void (*emit_constant_buffer)(batch_writer*, uint32 gtt, uint32 len);
    void (*emit_pipe_control)(batch_writer*, uint32 flags);
    void (*emit_3d_preamble)(batch_writer*);
    void (*emit_blt_copy)(batch_writer*, blt_params*);
    void (*emit_marker)(batch_writer*, uint32 gtt, uint32 tag);
};
```

### gen_display_ops (accelerant — display engine)
```c
struct gen_display_ops {
    void (*compute_dpll)(display_mode*, dpll_params*);
    void (*program_watermarks)(pipe_id, wm_params*);
    void (*enable_pipe)(pipe_id, display_mode*);
    void (*disable_pipe)(pipe_id);
    uint32 (*get_cdclk)(void);
};
```

### gen_kernel_ops (kernel — hardware management)
```c
struct gen_kernel_ops {
    status_t (*init_gtt)(device_info*);
    status_t (*init_ring)(ring_info*, ring_type);
    status_t (*gpu_reset)(device_info*, uint32 engine_mask);
    status_t (*forcewake_get)(device_info*);
    void (*forcewake_put)(device_info*);
    void (*apply_workarounds)(device_info*);
};
```

## Generation Differences

| Feature | Gen5 (ILK) | Gen6 (SNB) | Gen7 (IVB/HSW) | Gen8 (BDW) |
|---------|------------|------------|----------------|------------|
| STATE_BASE_ADDRESS | 8 DW | 10 DW | 10 DW | 16 DW |
| Flush command | MI_FLUSH | PIPE_CONTROL | PIPE_CONTROL | PIPE_CONTROL |
| EU threads | 48 | 60 | 64/112 | 168 |
| FORCEWAKE | No | Yes | Yes | Yes |
| PPGTT | No | Aliasing | Full | Full 48-bit |
| GPU reset | ILK_GDSR | Per-engine | Per-engine | Per-engine |
| BLT ring | Shared | Separate (BCS) | Separate | Separate |
| Power mgmt | Basic | RC6 | RC6 | RC6p |
| GTT entry size | 4 bytes | 4 bytes | 4 bytes | 8 bytes |

## Kernel Ioctl Interface v2

```c
// Preserved from v1
INTEL_GET_PRIVATE_DATA
INTEL_ALLOCATE_GRAPHICS_MEMORY
INTEL_FREE_GRAPHICS_MEMORY
INTEL_RING_WRITE_TAIL

// New in v2
INTEL_GEM_CREATE          // BO alloc with handle tracking
INTEL_GEM_CLOSE           // Free BO by handle
INTEL_GEM_MMAP            // Map BO to userspace
INTEL_GEM_SET_TILING      // Program HW fence registers
INTEL_GEM_GET_TILING
INTEL_GEM_EXECBUFFER2     // Batch submit with relocations
INTEL_GEM_WAIT            // Wait for BO idle
INTEL_GEM_BUSY            // Check BO busy state
INTEL_GET_PARAM           // Chipset info queries
INTEL_GET_APERTURE        // GTT size
INTEL_GPU_RESET           // Controlled GPU reset
INTEL_RING_INIT           // Per-ring init with workarounds
INTEL_CONTEXT_CREATE
INTEL_CONTEXT_DESTROY
INTEL_IOCTL_VERSION       // Interface version query
```

## Known Hardware Bugs (24 documented)

See TODO_INTEL_GPU_HAIKU.md section "Bug Gen5 scoperti e documentati" for the
complete table. Critical rules:

1. **NEVER reset ring** after boot — kills CS permanently
2. **MMIO writes ignored** from userspace — must use kernel ioctl
3. **LRI hangs CS** on Gen5 — use kernel MMIO instead
4. **Max 48 EU threads** per media submission
5. **Single submission** for media pipeline (no MI_FLUSH between batches)
6. **SURFTYPE_BUFFER** required for OWord Block R/W
7. **gen4asm .N is in BYTES** (not element units)
8. **{compr} UB->UW widening** writes only half GRF

## Implementation Phases

| Phase | Goal | Prereq | Weeks |
|-------|------|--------|-------|
| 0 | Fix batch #3 hang, ring wrap | — | 2-3 |
| 1 | Kernel v2 (GEM, EXECBUF2, reset) | P0 | 4-6 |
| 2 | libdrm_haiku proper library | P1 | 2-3 |
| 3 | Display CRTC/encoder/connector | P1 | 3-4 |
| 4 | Gen6 Sandy Bridge bring-up | P1+P2 | 3-4 |
| 5 | Gen7 Ivy Bridge / Haswell | P4 | 2-3 |
| 6 | Gen8 Broadwell | P5 | 3-4 |
| 7 | Mesa OpenGL complete | P0+P1+P2 | 4-6 |
| 8 | H.264 decoder | P4+ | 4-6 |
| 9 | Power management | P4+ | 2-3 |

## Design Decisions

1. **Kernel EXECBUF2** vs userspace inline → Kernel. Eliminates ring overflow,
   enables GPU reset, enables fence registers for tiling.

2. **Separate kernel driver** vs patching intel_extreme → Separate.
   Clean separation of display vs GPU compute. No upstream fork conflict.

3. **C vtables** vs C++ inheritance → C vtables. Works in kernel,
   accelerant, and userspace. Trivially auditable per-gen files.

4. **Classic ring** for Gen5-8, no GuC → Correct scope. Gen9+ is a
   different architecture entirely.

5. **GGTT first**, add PPGTT later → Pragmatic. GGTT works for single
   context. PPGTT adds process isolation when needed (Gen7+).
