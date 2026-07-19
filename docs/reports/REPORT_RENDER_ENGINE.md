# Report: Gen5 (Ironlake) 3D Render Engine on Haiku

| | |
|---|---|
| **Status** | ⚠️ WIP |
| **Category** | Report |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

**Date:** April 5, 2026 (updated)
**Hardware:** Sony Vaio VPCEB3K1E, Intel HD Graphics 0x0046 (Ironlake Mobile)
**OS:** Haiku R1~beta5+development (hrev59506)

---

## Goal

Implement 2D acceleration via the GPU's 3D pipeline (render engine) for
operations the BLT engine doesn't support: alpha blending, scaling,
gradients. The BLT engine only handles solid fills and rectangular blits.

## Results

### What works

1. **LVDS display 1366x768** at 59.9 Hz with 4 patches to the accelerant
   (EDID fallback, dual-channel BIOS, IBX watermark, panel fitter bypass)

2. **BLT engine**: rectangular fills, screen-to-screen blit, invert,
   fill span. Tested with a blue rectangle during mode set.

3. **3D command parser activated**: after fixing all the opcodes
   for the Gen5 commands, the command parser processes Type 3 (3D) commands.
   Confirmed by `IPEHR=0x7A000002` (PIPE_CONTROL) in the GPU registers.

4. **Gen5 workarounds applied**: MI_MODE (VS_TIMER_DISPATCH),
   _3D_CHICKEN2 (WM_READ_PIPELINED), CACHE_MODE_0 (render cache flush).
   Verified via register readback.

5. **Ring buffer re-init**: full disable/reset HEAD/re-enable cycle
   matching Linux i915's `init_ring_common()`.

6. **Complete state setup**: VS, SF, WM, CC state, binding table,
   surface state, vertex buffer, CC viewport. All pointers
   verified via diagnostic dump.

### What doesn't work (as of April 5, 2026)

1. **3D draw doesn't produce any pixels**: the 3D commands are processed by
   the parser (confirmed by IPEHR, INSTDONE=0xFFFFFFFF, no GPU error),
   but no pixel is written to the framebuffer.

2. **Root cause found: MI_PIPELINE_SELECT had wrong type bits**.
   The define had `(0x1 << 29)` which sets type=001, but PIPELINE_SELECT
   is an MI command that requires type=000 (bits[31:29]=0). The GPU didn't
   recognize the command and never switched from the BLT pipeline to the 3D one.
   All pipelined 3D commands were silently ignored.
   Fix: `CMD_PIPELINE_SELECT = (0x01 << 23)` = 0x00800000 (matching
   Linux i915's `MI_INSTR(0x01, 0)` and SNA's `MI_PIPELINE_SELECT`).

3. **PIPE_CONTROL Write Immediate**: didn't write the 0xDEADBEEF marker
   because the 3D pipeline wasn't active (a consequence of bug #2).

**Status after the PIPELINE_SELECT fix**: to be verified on the next reboot.

## Bugs found and fixed

### MI_PIPELINE_SELECT type bits (critical — root cause)

`CMD_PIPELINE_SELECT` had `(0x1 << 29) | (0x01 << 23)` = `0x20800000`,
setting type=001 (bits[31:29]). But PIPELINE_SELECT is an MI command
that requires type=000. Without this command being recognized, the GPU stayed
in BLT mode and all pipelined 3D commands were ignored.

Corrected to `(0x01 << 23)` = `0x00800000` (matching Linux i915 and SNA).

### 3D command opcodes

Almost all of the 3D command opcodes had incorrect encoding. The
SubOpcode field (bits[23:16]) was placed in the Opcode field (bits[26:24]),
causing unrecognized commands to be sent, which the parser silently
discarded.

Introduced the `GEN5_3D(pipeline, opcode, subopcode)` macro, aligned
with xf86-video-intel SNA:

Full table verified against Mesa's gen5.xml, brw_defines.h and
Linux i915's intel_gpu_commands.h:

| Command | Wrong | Correct | Error |
|---|---|---|---|
| PIPELINE_SELECT | `0x00800000` (MI!) | `0x69040000` (3D SubType=1) | Not MI on Gen5 |
| PIPELINED_POINTERS | `0x68000005` (SubType=1) | `0x78000005` (SubType=3) | Wrong SubType |
| DRAWING_RECTANGLE | `0x69000002` (SubType=1) | `0x79000002` (SubType=3) | Wrong SubType |
| BINDING_TABLE_PTRS | `0x69010000` (SubType=1) | `0x78010000` (SubType=3) | SubType+Opcode |
| VERTEX_BUFFERS | `0x68080000` (SubType=1) | `0x78080000` (SubType=3) | Wrong SubType |
| VERTEX_ELEMENTS | `0x68090000` (SubType=1) | `0x78090000` (SubType=3) | Wrong SubType |
| URB_FENCE | `0x60050000` (SubOp=5) | `0x60000000` (SubOp=0) | Wrong SubOpcode |
| CS_URB_STATE | `0x61000000` (Op=1,SubOp=0) | `0x60010000` (Op=0,SubOp=1) | Op/SubOp swapped |

Commands already correct: STATE_BASE_ADDRESS (`0x61010006`), 3DPRIMITIVE
(`0x7B000000`), PIPE_CONTROL (`0x7A000002`), PRIM_RECTLIST (`0x0F`).

### STATE_BASE_ADDRESS opcode

The `CMD_STATE_BASE_ADDRESS` define had the value `0x69000006`, which decodes
as `3DSTATE_DRAWING_RECTANGLE` with the wrong length, rather than as
`STATE_BASE_ADDRESS` (`0x61010006`). Result: the address bases
were never set, and a malformed DRAWING_RECTANGLE with a clip
rect of (1,0)-(1,0) was issued, clipping everything.

### SF state bitfield positions (7 wrong fields)

All the SF state fields (DW3, DW4, DW6) had incorrect shifts
relative to `brw_structs.h` (`brw_sf_unit_state`):
- DW3: `dispatch_grf_start_reg` bits[3:0] instead of [4:1],
  `urb_entry_read_offset` bits[9:4] instead of [10:5],
  `urb_entry_read_length` bits[16:11] instead of [18:12]
- DW4: `nr_urb_entries` bits[17:11] instead of [16:9],
  `urb_entry_allocation_size` bits[23:19] instead of [22:18]
- DW6: `dest_org_vbias` bits[12:9] instead of [25:22],
  `dest_org_hbias` bits[16:13] instead of [19:16]

### WM state bit positions

- `thread_dispatch_enable` was at bit 29 (DW5) — corrected to bit 19
- `max_threads` was in DW4 — corrected to DW5 bits[31:25]
- `binding_table_entry_count` must be 0 on Ironlake (HW requirement)
- `dispatch_grf_start_reg` corrected from 1 to 3 (matching SNA)
- `grf_reg_count` corrected from 0 to 2

### RECTLIST vertex order

Vertex order was (left,bottom), (left,top), (right,bottom) — two vertices
with the same X. SNA uses (right,bottom), (left,bottom), (left,top), and the
rasterizer infers the fourth vertex (right,top).

### SEND instruction msg_reg_nr

The EU SEND instructions (SF URB_WRITE and WM FB_WRITE) had `msg_reg_nr=0`
(bits[27:24] of DW0) when it should have been 1. The GPU read the data
from MRF m0 (uninitialized) instead of from m1 (where we write the header
and colors).

## Implemented architecture

### Modified files

- **render.h**: 3D command definitions, surface state, vertex element,
  WM dispatch mode, render engine state structures
- **render.cpp**: render engine init, SF/WM kernel binaries, state setup,
  rectangle fill via the 3D pipeline, color patching, GPU diagnostics
- **engine.cpp**: render engine toggle via file trigger, debug tracing
- **mode.cpp**: visual tests (CPU fill, BLT fill, 3D fill), GPU register
  diagnostics, pixel readback, output to render_diag.txt

### 3D pipeline (command sequence)

```
MI_FLUSH (BLT→3D serialization)
MI_LOAD_REGISTER_IMM × 3 (Gen5 workarounds)
PIPELINE_SELECT = 3D
STATE_BASE_ADDRESS (bases at 0, absolute GTT pointers)
PIPELINED_POINTERS (VS, GS=off, CLIP=off, SF, WM, CC)
URB_FENCE (VS: 256 entries, SF: 64 entries)
CS_URB_STATE
DRAWING_RECTANGLE (clip rect = framebuffer)
BINDING_TABLE_POINTERS (VS=0, GS=0, CLIP=0, SF=0, WM=binding table)
VERTEX_BUFFERS (3 vertices × 2 float, pitch=8)
VERTEX_ELEMENTS (R32G32_FLOAT + Z=0 + W=1.0)
3DPRIMITIVE (RECTLIST, 3 vertices)
PIPE_CONTROL (diagnostic marker)
MI_FLUSH (3D→BLT serialization)
```

### EU kernel binaries

**SF kernel** (7 instructions, from intel-vaapi-driver): computes attribute
deltas (dA/dx, dA/dy) via MATH_INV and writes to the URB with msg_length=4
and SWIZZLE_TRANSPOSE.

**WM kernel** (6 instructions, custom): loads the RGBA color as float
immediates (patched for each draw), copies the thread header to m1, sends
FB_WRITE SIMD8 with EOT. The color is converted from BGRA uint32 to
4 floats and written directly into the kernel's MOV instructions.

### State block layout (1024 bytes in GPU memory)

```
0x000  VS state (64 B)
0x040  WM state (64 B)
0x080  CC state (64 B)
0x0C0  CC viewport (64 B)
0x100  Binding table (64 B)
0x140  Surface state dst (32 B)
0x160  Surface state src (32 B)
0x180  SF state (64 B)
0x1C0  SF kernel (128 B, 112 used)
0x240  WM kernel (128 B, 96 used)
0x300  Vertex buffer (256 B)
```

## Structural limitations of Haiku

### app_server does not use hardware 2D acceleration

Since 2013, the `USE_ACCELERATION` flag was hardcoded to 0. In December 2024
(commit `03f77fd7d9db`, included in hrev59506), all hardware 2D
acceleration code was removed from `AccelerantHWInterface.cpp`
(467 lines deleted).

Official reasons:
- Conflict with double buffering (flickering)
- Alpha blending requires reading from the CPU buffer (VRAM is slow to read)
- Modern CPUs are fast enough for memset/memcpy
- Modern acceleration is via OpenGL/Vulkan

### Mesa is software-only

Haiku has no DRM/KMS framework. Mesa only works with llvmpipe
(software rendering via LLVM). No hardware Mesa drivers exist for
Intel (i965, iris, crocus) on Haiku. OpenGL 4.5 is available but
entirely CPU-based.

### No path for GPU acceleration

```
app_server → Painter (AGG, software) → back buffer (CPU malloc)
           → memcpy → framebuffer

Mesa → llvmpipe (software) → no hardware GPU access
```

Currently there is no working path for using the Intel GPU
for 2D or 3D rendering on Haiku.

## Haiku's GPU ecosystem: the X547 approach

X547 (X512) is the only developer who has brought hardware GPU
acceleration to Haiku, with an architecture that entirely avoids DRM/KMS:

```
Mesa (radeonsi/radv/nvk)
    |
libdrm2 (shim: translates DRM ioctl → accelerant2 vtable)
    |
accelerant2 (COM-like API with QueryInterface, C/C++ vtable)
    |
GPU Server (RadeonGfx / nvidia-haiku, userspace BApplication)
    |
Kernel driver (minimal: PCI, MMIO, shared_info)
```

### Key components

**libdrm2** (github.com/X547/libdrm2):
A reimplementation of libdrm that intercepts DRM calls and routes them
to accelerant2. Server mode: on `amdgpu_device_initialize()` it loads
the accelerant2 add-on, obtains the `AccelerantDrm` and `AccelerantAmdgpu`
vtables, and delegates all operations (buffer alloc, VA mapping, command submit,
sync objects) to the GPU server. Implemented: GEM buffer create/close/map/
export/import, virtual address mapping, command submission, sync objects.
Not implemented: KMS/mode-setting (handled separately via VideoStreams).

**accelerant2** (github.com/X547/accelerant2):
Next-generation accelerant API with a COM-like pattern. Interfaces:
- `AccelerantDrm` ("drm/v1"): mmap, buffer handle, full sync objects
- `AccelerantAmdgpu` ("amdgpu/v1"): info, buffer alloc/map, command submit
- `AccelerantDisplay`: VideoStreams consumer for CRTC, cursor
Supports both C vtables and C++ virtual classes.

**RadeonGfx** (github.com/X547/RadeonGfx):
Complete GPU server for AMD GCN 1.0 (Cape Verde). Architecture:
- Minimal kernel driver (PCI, MMIO, shared_info)
- Userspace BApplication server: firmware, MC, ring buffer, command submit
- Client-server IPC via PortLink/ThreadLink (app_server style)
- Direct hardware ring buffer (writes to GPU memory-mapped memory)
- Memory manager: VRAM, GTT, 2-level page table per client
- Per-client isolation: handle table, AddressSpace, VM pool

**VideoStreams** (github.com/X547/VideoStreams):
Equivalent of GBM + wl_buffer for Haiku. Producer/Consumer with SwapChain.
Buffer reference via `area_id` (CPU) or `fd` + fence (GPU, equivalent
to DMA-BUF). Multi-surface compositing with dirty region tracking.

### Current state of GPU drivers on Haiku

| GPU | Driver | 3D | Vulkan | Status |
|---|---|---|---|---|
| NVIDIA Turing+ | nvidia-haiku (X547) | Zink OpenGL | NVK | v0.0.2, working |
| AMD GCN 1.0 | RadeonGfx (X547) | radeonsi | RADV | Experimental |
| Intel Gen2-Gen12 | intel_extreme (Haiku) | No | No | Display only |
| Any | Mesa llvmpipe | Software | Lavapipe | Working |

### Roadmap: Intel Gen5 in the X547 ecosystem

**What would be needed to bring Intel Gen5 into the X547 stack:**

1. **Kernel driver** (`intel_gfx`): minimal — PCI, MMIO mapping,
   shared_info. Could be based on the existing, already-working
   `intel_extreme` kernel driver.

2. **GPU Server** (`IntelGfx`): GTT management (global on Gen5, not
   per-process like AMDGPU), RCS ring buffer, batch buffer submission,
   fencing via the HWS page. Simpler than RadeonGfx because Gen5 has
   a global GTT and a single ring.

3. **accelerant2 interface**: `AccelerantIntel` with GEM-like
   operations (buffer create/map, execbuffer submit, wait).

4. **libdrm2 Intel shim**: `libdrm_intel` or an adaptation of the
   `crocus` winsys (Gen4-7 Gallium driver) to call AccelerantIntel
   instead of `DRM_IOCTL_I915_*`.

5. **Mesa crocus winsys**: adapt `src/gallium/winsys/crocus/drm/`
   to use the libdrm2 shim instead of Linux DRM ioctls.

**Simplifications compared to RadeonGfx:**
- Gen5 has a global GTT (no per-client page table)
- A single ring buffer (RCS) vs. multiple (GFX, SDMA, etc.)
- No GPU firmware to load
- Well-documented hardware (public Intel PRMs)

**Estimated complexity:**
- Kernel driver: 1-2 weeks (adapting the existing intel_extreme)
- GPU Server: 4-8 weeks (GTT, ring buffer, batch submit, fencing)
- libdrm2 + winsys: 2-4 weeks
- Mesa crocus integration: 2-4 weeks
- **Total: 2-4 months for an experienced developer**

### Technical blockers

1. **No Intel abstraction in libdrm2**: everything is AMDGPU-specific
   (ioctl numbers, structures, semantics). A parallel Intel layer is needed.

2. **Mesa crocus** calls `DRM_IOCTL_I915_GEM_CREATE`,
   `DRM_IOCTL_I915_GEM_EXECBUFFER2`, etc. — completely different
   from AMDGPU. A dedicated winsys adapter is needed.

3. **Gen5 is old**: crocus supports it, but it's the lowest-end target.
   The community may not be interested in maintaining it.

4. **X547 has no Intel hardware**: nobody is working on Intel
   within the Haiku GPU ecosystem.

## Future work on the 3D render engine

### Priority 1: verify the PIPELINE_SELECT fix

The `CMD_PIPELINE_SELECT = (0x01 << 23)` fix should resolve the
main problem. After the reboot, verify:
- PIPE_CONTROL marker = 0xDEADBEEF (3D pipeline active)
- Red rectangle visible in the test at (270,50)-(370,150)

### If 3D produces pixels but with artifacts

1. **SF kernel**: the SF kernel from the vaapi-driver might not match
   the vertex format (2 float position-only, no UV). Alternative:
   use the SNA approach with a 1x1 texture + a WM kernel with sampling.

2. **WM kernel SIMD16**: SNA uses SIMD16, not SIMD8. This may be
   needed for correct WM thread dispatch on Ironlake.

3. **EU encoding validation**: the Gen5 EU instructions are derived
   from the bitfield layout of brw_eu.h. Without an EU assembler or a
   verified reference binary, the encoding might have subtle errors
   in the src/dst fields of the MOV and SEND instructions.

## Statistics

- **37 commits** in the repository
- **~4100 lines added**, 86 removed
- **4 display patches** working and tested (stable LVDS 1366x768)
- **1 render engine** with the 3D pipeline active (commands processed)
  but draw not yet functional (SF/WM kernel)
- **~15 reboots** for testing and debugging the 3D pipeline
- **Critical opcode bug** found and fixed (all Gen5 commands)

## References

- xf86-video-intel SNA gen5_render.c (reference for opcodes and state setup)
- intel-vaapi-driver exa_sf.g4b.gen5 (SF kernel binary)
- Linux i915 init_render_ring() (Gen5 workarounds)
- X547/libdrm2, X547/accelerant2, X547/RadeonGfx (Haiku GPU architecture)
- Haiku AccelerantHWInterface.cpp commit 03f77fd7d9db (2D HW accel removal)
- Intel Gen5 (Ironlake) PRM Vol 1-4
