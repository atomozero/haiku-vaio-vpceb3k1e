# Render Engine Analysis for Shader-Based 2D Acceleration

| | |
|---|---|
| **Status** | 📋 Plan |
| **Category** | Analysis |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

**Date:** March 21, 2026
**Prerequisite:** BLT acceleration work completed (HWS sync, optimizations)

---

## 1. CURRENT STATE: BLT Engine

### What We've Implemented
The current 2D acceleration uses the **BLT engine** (blitter) via the ring buffer:

```
app_server --> accelerant hook --> QueueCommands --> Ring Buffer --> BLT Engine
                                                        |
                                                MI_STORE_DWORD_INDEX
                                                        |
                                                   HWS Page (sync)
```

**Supported operations:**
| Operation | BLT Command | Performance vs. System |
|---|---|---|
| Color Fill | XY_COLOR_BLT | +6% |
| Screen Copy | XY_SRC_COPY_BLT | +7% |
| Invert | XY_COLOR_BLT (ROP 0x55) | +18% |
| Full Clear | XY_COLOR_BLT | +24% |
| Span Fill | XY_SCANLINE_BLIT | (untested) |

**BLT engine limitations:**
- No alpha blending (transparency)
- No scaling/rotation
- No pixel format conversion
- No antialiasing
- No gradients
- Only fixed raster operations (ROP)

These limitations mean that **all compositing operations**
(window transparency, shadows, antialiased text, scaled images) are
done by the CPU in software, even though the GPU has the hardware to do them.

---

## 2. RENDER ENGINE: WHAT IT CAN DO

The Ironlake Gen5 has a **complete 3D pipeline** with 12 programmable
Execution Units (EU). Linux uses it for 2D through the SNA backend of xf86-video-intel.

### 2.1 Hardware Available on Gen5

| Component | Quantity | Function |
|---|---|---|
| Execution Units (EU) | 12 | Programmable shaders (vertex + fragment) |
| Thread Dispatcher | 1 | Manages up to 60 simultaneous HW threads |
| Sampler | 2 | Texture reads with bilinear filtering |
| Data Port | 1 | Framebuffer writes |
| URB (Unified Return Buffer) | 64KB | Temporary shader data |
| L1 Texture Cache | 16KB per sampler | Texture cache |
| L2 Cache | 256KB | Shared cache |

### 2.2 2D Operations Possible via Render Engine

| Operation | BLT | Render | Render Advantage |
|---|---|---|---|
| Color fill | Yes | Yes | None (BLT is sufficient) |
| Screen copy | Yes | Yes | None for simple copies |
| **Alpha composite** | **No** | **Yes** | Window transparency, shadows |
| **Bilinear scaling** | **No** | **Yes** | Smooth image resizing |
| **Rotation/transform** | **No** | **Yes** | Rotation effects |
| **Gradients (linear/radial)** | **No** | **Yes** | Backgrounds, progress bars |
| **Format conversion** | Limited | **Yes** | YUV→RGB for video |
| **Text rendering AA** | **No** | **Yes** | Sharp subpixel text |
| **Porter-Duff blend** | **No** | **Yes** | All 12 compositing modes |
| **Pattern fill** | Limited | **Yes** | Arbitrary patterns |

### 2.3 How Linux SNA Implements It (gen5_render.c)

The 3D pipeline is configured with most stages **disabled**.
Only the **WM (Windower/Masker = fragment shader)** and the **CC (Color Calculator)**
are active for 2D compositing:

```
Vertex Input (rectangle coordinates)
    |
    v
VS (Vertex Shader) -- minimal passthrough
    |
    v
[GS disabled] [minimal Clipper]
    |
    v
WM (Fragment Shader) -- CORE: performs compositing
    |                    Reads source texture via Sampler
    |                    Applies blend operation
    |                    Writes result
    v
CC (Color Calculator) -- Porter-Duff blend mode
    |
    v
Framebuffer Output
```

The primitive type used is **RECTLIST** (3 vertices = 1 rectangle):
- Vertex 0: top-left (x0, y0, u0, v0)
- Vertex 1: bottom-left (x0, y1, u0, v1)
- Vertex 2: bottom-right (x1, y1, u1, v1)
The GPU infers the fourth vertex automatically.

---

## 3. PROPOSED ARCHITECTURE FOR HAIKU

### 3.1 Integration with Existing BLT Work

The render engine does NOT replace the BLT engine — it **complements** it:

```
app_server 2D request
    |
    +--- Simple operation (fill, copy, invert)?
    |       |
    |       v
    |    BLT Engine (current code, fast)
    |    [QueueCommands + HWS sync]
    |
    +--- Complex operation (alpha blend, scale, gradient)?
            |
            v
         Render Engine (new code)
         [BatchCommands + pre-compiled shaders]
```

The **BatchCommands** infrastructure we've already implemented (Phase 1,
currently disabled) becomes useful here: 3D state commands are many more
DWORDs than BLT commands (30-50 DWORDs for setup + 3 DWORDs per primitive),
so the batch buffer amortizes the overhead better.

### 3.2 Required 3D State (One-Time per Frame)

To initialize the 3D pipeline for 2D operations, the following commands
are needed (emitted once, then reused for multiple primitives):

```
1. PIPELINE_SELECT               -- Select 3D pipeline (1 DWORD)
2. STATE_BASE_ADDRESS             -- Base addresses for state heap (4+ DWORD)
3. 3DSTATE_PIPELINED_POINTERS     -- Pointers to VS/WM/CC state (3 DWORD)
4. 3DSTATE_BINDING_TABLE_PTRS     -- Pointer to binding table (2 DWORD)
5. 3DSTATE_VERTEX_ELEMENTS        -- Vertex format (3+ DWORD)
6. 3DSTATE_VERTEX_BUFFERS         -- Vertex buffer (5 DWORD)
```

Then for each rectangle:
```
7. 3DPRIMITIVE                    -- Draw RECTLIST (3 DWORD)
```

**Total setup:** ~25-30 DWORD (one time)
**Per rectangle:** ~3 DWORD + 3 vertices in the vertex buffer

### 3.3 State Structures in GPU Memory

GPU allocations are needed for:

| Structure | Size | Content |
|---|---|---|
| VS State | 32B | Vertex shader state (passthrough) |
| WM State | 64B | Fragment shader state + kernel pointer |
| CC State | 64B | Color calculator (blend mode) |
| Binding Table | 16B | Pointers to surface state (src, dst) |
| Surface State | 32B x 2 | Description of source and destination surface |
| WM Kernel | ~256B | Compiled shader program |
| Vertex Buffer | 4KB | Rectangle coordinates (reusable) |
| **Total** | **~5KB** | Allocated once at init |

### 3.4 Pre-compiled Shaders

SNA uses pre-compiled shaders (EU code in binary form). For basic 2D
operations, only a few kernels are needed:

**Kernel 1: Source Copy (format conversion)**
```
// EU shader pseudocode
sample(src_surface, texcoord)
write(dst_surface, position, sampled_color)
```

**Kernel 2: Alpha Composite (Porter-Duff SRC_OVER)**
```
sample(src_surface, texcoord) -> src_color
sample(dst_surface, position) -> dst_color  // only if needed
result = src_color + dst_color * (1 - src_alpha)
write(dst_surface, position, result)
```

**Kernel 3: Solid Color Fill (with alpha)**
```
// Color passed as a constant
result = blend(constant_color, dst_color, blend_mode)
write(dst_surface, position, result)
```

**Kernel 4: Linear Gradient**
```
t = dot(position, gradient_direction) / gradient_length
color = lerp(color0, color1, clamp(t, 0, 1))
write(dst_surface, position, color)
```

Gen5 EU kernels are in an Intel-specific binary format. They can be:
1. Extracted from SNA (embedded as uint32[] arrays)
2. Hand-written in EU assembly (documented in the PRM)
3. Compiled with Mesa's EU assembler (intel_eu_emit)

---

## 4. IMPLEMENTATION PLAN

### Phase R1: Infrastructure (2-3 weeks)

**Goal:** Allocate GPU structures, emit state commands, draw
a single colored rectangle via the render engine.

1. Allocate 5KB of GPU memory for state structures
2. Build VS/WM/CC state in memory
3. Write a minimal EU kernel (solid color fill)
4. Emit the 3D command sequence into the batch buffer
5. Draw a test rectangle
6. Verify that rendering is correct

**Files to create:**
- `render.cpp` — 3D pipeline and state management
- `render.h` — state structures and 3D command definitions
- `gen5_shader.h` — pre-compiled EU kernels (binary arrays)

**Files to modify:**
- `hooks.cpp` — add hook for overlay/composite
- `accelerant.cpp` — init/uninit render state
- `commands.h` — add 3DSTATE commands

**Dependency on BLT work:**
- Uses the already-implemented BatchCommands (re-enable it for render)
- Uses HWS sync for synchronization
- Uses init_batch_buffer() for GPU memory allocation

### Phase R2: Compositing (2-3 weeks)

**Goal:** Implement alpha blending for window compositing.

1. Add EU kernel for SRC_OVER blend
2. Implement surface state for source texture
3. Handle multiple pixel formats (B_RGBA32, B_RGB32)
4. Hook B_COMPOSITE into the accelerant
5. Test with transparent windows

**Visual impact:** Smooth transparency, drop shadows under windows.

### Phase R3: Scaling and Transform (1-2 weeks)

**Goal:** Bilinear scaling for image resizing.

1. Configure the sampler for bilinear filtering
2. Implement texture coordinate transformation
3. Hook for scaled blit
4. Test with image resizing in ShowImage

**Visual impact:** Image resizing without visible pixelation.

### Phase R4: Gradients and Text (2-3 weeks)

**Goal:** Linear/radial gradients and improved text rendering.

1. EU kernel for linear gradient
2. EU kernel for radial gradient
3. Integration with app_server text rendering
4. Subpixel antialiasing via shader

**Visual impact:** More modern UI, crisp text.

---

## 5. REQUIRED GEN5 3D COMMANDS

### 5.1 Command Opcodes (from Linux's intel_gpu_commands.h)

```cpp
// Pipeline setup
#define CMD_PIPELINE_SELECT          (0x6104)  // 3D vs Media
#define CMD_STATE_BASE_ADDRESS       (0x6101)  // Base addresses
#define CMD_PIPELINED_POINTERS       (0x6100)  // VS/WM/CC state ptrs

// Binding tables
#define CMD_BINDING_TABLE_PTRS       (0x6801)  // Surface binding

// Vertex setup
#define CMD_VERTEX_BUFFERS           (0x6808)  // VB base/size/stride
#define CMD_VERTEX_ELEMENTS          (0x6809)  // Vertex format

// Drawing
#define CMD_3DPRIMITIVE              (0x7b00)  // Draw call
#define PRIM_RECTLIST                3         // Rectangle list primitive

// Primitive format
// DWORD 0: opcode
// DWORD 1: vertex count, start vertex, instance count
// DWORD 2: start instance, base vertex
```

### 5.2 Gen5 Surface State Registers

```cpp
struct gen5_surface_state {
    uint32 dw0;  // surface type, format, tiling
    uint32 dw1;  // base address (GTT offset)
    uint32 dw2;  // width, height
    uint32 dw3;  // pitch, depth
    uint32 dw4;  // multisampling (0 for 2D)
    uint32 dw5;  // reserved
};

// Surface types
#define SURFACE_2D    1
// Surface formats (BGRA for Haiku)
#define FORMAT_B8G8R8A8_UNORM  0x0C0
#define FORMAT_B8G8R8X8_UNORM  0x0C8
```

### 5.3 Gen5 WM State Registers

```cpp
struct gen5_wm_state {
    uint32 dw0;  // kernel pointer 0
    uint32 dw1;  // single program flow, binding table count
    uint32 dw2;  // scratch space
    uint32 dw3;  // URB entry size, thread count
    uint32 dw4;  // max threads, stats
    uint32 dw5;  // dispatch mode (16-pixel wide), kernel pointer 1
    uint32 dw6;  // kernel pointer 2
    uint32 dw7;  // reserved
    uint32 dw8;  // kernel start pointer (main entry)
};
```

---

## 6. EXPECTED PERFORMANCE COMPARISON

### Operations the Render Engine Improves

| Operation | CPU Software | BLT Engine | Render Engine |
|---|---|---|---|
| Alpha composite 800x600 | ~5 ms | N/A | ~0.5 ms |
| Scale 1920→800 bilinear | ~15 ms | N/A | ~1 ms |
| Linear gradient 800x600 | ~3 ms | N/A | ~0.3 ms |
| Glyph composite (text) | ~0.1 ms/glyph | N/A | ~0.01 ms/glyph |

**Estimated impact:** Haiku desktop compositing makes heavy use of alpha
blending (menus, tooltips, notifications). Moving from CPU to GPU reduces
CPU load by 30-50% during UI-intensive operations.

### Operations Where BLT Remains Better

| Operation | BLT | Render | Reason |
|---|---|---|---|
| Solid rectangle fill | 60K/s | ~40K/s | BLT has less setup overhead |
| Screen copy | 9.5K/s | ~7K/s | BLT has an optimized DMA path |
| Invert | 66K/s | N/A | Only BLT supports ROP |

This is why the two engines must **coexist**: BLT for simple operations,
Render for complex operations.

---

## 7. TECHNICAL CHALLENGES

### 7.1 BLT ↔ Render Context Switching

On Gen5 the ring buffer is shared between BLT and Render. When switching
from BLT commands to 3D commands, an `MI_FLUSH` is needed to avoid conflicts:

```
[BLT commands]
MI_FLUSH          <-- flush BLT pipeline
PIPELINE_SELECT   <-- switch to 3D
[3D state setup commands]
3DPRIMITIVE       <-- draw
MI_FLUSH          <-- flush 3D pipeline
[BLT commands]     <-- back to BLT
```

The context switch has a cost (~5-10 us). To minimize it, render
operations should be grouped (batched).

### 7.2 Gen5 EU Shaders — Binary Format

EU kernels are in Intel's proprietary binary format. Each instruction is
128 bits (16 bytes). The instruction set includes:

- `send` — sends a message to a functional unit (sampler, data port)
- `mov` — register copy
- `add/mul/mad` — arithmetic
- `sel` — conditional select
- `cmp` — comparison
- `jmp/if/else/endif` — flow control

A minimal SRC_OVER kernel requires ~10-15 instructions (~200 bytes).
Kernels can be extracted from SNA (`brw_wm_kernels.h` file) or
written with the EU assembler.

### 7.3 Haiku app_server — Accelerant Hook

Haiku doesn't have a standard accelerant hook for compositing. The app_server
uses `ServerBitmap::HandleComposite()`, which is entirely software-based. To
accelerate it requires:

1. Add a `B_COMPOSITE` hook to the accelerant interface
2. Modify app_server to use it when available
3. Software fallback for unsupported hardware

This requires changes to app_server (upstream Haiku code), not
just to the accelerant. It's the most critical point of the integration.

### 7.4 Alternative: Video Overlay

A simpler approach for video: use the display engine's **hardware overlay**
(currently disabled for Ironlake in hooks.cpp, line 131).

The overlay doesn't require the 3D pipeline — it's a separate hardware
plane that performs YUV→RGB scaling and compositing with the framebuffer.
For video playback this would be sufficient and much simpler to implement.

---

## 8. INTEGRATED ROADMAP

```
COMPLETED (March 2026):
  [x] BLT Engine working
  [x] HWS page sync (+18-24% vs system)
  [x] Optimizations (memcpy, out-of-loop construction)
  [x] Batch buffer infrastructure (ready, disabled)
  [x] Benchmark suite (7 tests)

NEXT (accelerant-only):
  [ ] Phase R1: Render infrastructure (state, first rectangle)
  [ ] Hardware video overlay (re-enable for Ironlake)
  [ ] Phase R2: Alpha compositing via shader

FUTURE (requires app_server/kernel changes):
  [ ] B_COMPOSITE hook in app_server
  [ ] X-Tiled framebuffer (kernel driver modification)
  [ ] Phase R3-R4: Scaling, gradients, text rendering
  [ ] 2MB ring buffer (kernel driver modification)

LONG TERM (OS-level project):
  [ ] Mesa/Gallium port (DRM equivalent for Haiku)
  [ ] OpenGL 2.1 via i915g driver
```

---

## 9. CONCLUSION

2D acceleration via the Render Engine is **feasible** on the Ironlake
Gen5 hardware, but requires:

1. **Knowledge of the Execution Units ISA** — documented in the Intel PRM
2. **Significant development time** — 2-3 months for Phases R1-R2
3. **Changes to Haiku's app_server** — for the compositing hook
4. **Thorough testing** — the 3D pipeline has many more states than the BLT

The BLT work already done provides the **foundation**:
- The batch buffer is the ideal infrastructure for 3D commands (many DWORDs)
- HWS sync also works for 3D commands
- GPU memory allocation (intel_allocate_memory) is already working
- The benchmark framework allows measuring improvements

The most useful and immediate next step (without modifying app_server)
would be to **re-enable the hardware overlay for Ironlake** for video
playback, and then proceed with Phase R1 (first rectangle via render
engine) as a proof of concept.
