# Roadmap: Intel Gen5+ GPU Acceleration on Haiku

| | |
|---|---|
| **Status** | 🚧 Living document |
| **Category** | Roadmap |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](INDEX.md)

---

**Primary hardware:** Intel HD Graphics 0x0046 (Ironlake Mobile, Gen5), Sony Vaio VPCEB3K1E
**Modular architecture:** gen_ops vtable for multi-generation support (Gen5 tested, Gen6 ready)
**OS:** Haiku R1~beta5 (hrev59506+)
**Strategic direction:** Hardware video decode (MPEG-2 → H.264) as the primary goal,
compute/LLM as a later phase. See [`analysis/VIDEO_DECODE_PIVOT.md`](analysis/VIDEO_DECODE_PIVOT.md).

**Last updated:** 2026-05-18 (BLT 60fps, media 3.5x, EXECBUF2 test OK, ring health/test tools)

---

## Legend

- [x] Completed and verified on hardware
- [-] Partially completed / working with limitations
- [ ] To do

---

## Phase 0: Infrastructure and preparation — COMPLETE

- [x] Study of X547/RadeonGfx, libdrm2, accelerant2 architecture
- [x] Study of the Linux i915 driver (crocus winsys, GEM, execbuffer, DRM ioctl)
- [x] Gen5 hardware documentation (register map, ring buffer, PRM)
- [x] LVDS display patches (EDID fallback, dual/single channel, IBX watermark)
- [x] Working 1366x768 32bit 59.9Hz display
- [x] 2D BLT acceleration (XY_SOURCE_BLIT, XY_COLOR_BLIT, pattern fill)
- [x] Port of intel-gen4asm, Gen5 opcode fixes

---

## Phase 1: 3D Pipeline (Render Engine) — COMPLETE (base)

- [x] PIPELINE_SELECT 3D, STATE_BASE_ADDRESS, URB_FENCE, 3DPRIMITIVE
- [x] SF kernel (7 instr) + WM kernel (6 instr) — solid fill SIMD16
- [x] Ironlake workarounds (RC6 wake, MI_MODE, _3D_CHICKEN2)
- [x] render_fill_rect working with diagnostic markers
- [ ] Advanced 3D: alpha blending, texture sampling, compositing

---

## Phase 2: Media Pipeline (Compute) — COMPLETE

- [x] Infrastructure: gpu_bo allocator, gpu_debug, bench module
- [x] Phase I.B: first EU kernel (hello_world.g4a, 10-command batch)
- [x] Phase 1.2: 48-thread parallel dispatch
- [x] Phase 1.3: per-thread memory write (OWord Block Write, SURFTYPE_BUFFER)
- [x] Phase 2.1: bit-exact FP32 SAXPY (384 elements, MFLOPS benchmark)
- [x] Phase 2.2: MEDIA_BLOCK_READ sampler cache, CURBE 30 GRF, libva ABI
- [x] Phase 2.3b: MPEG-2 IQ kernel (inverse quantization, bit-exact)

---

## Phase 3: MPEG-2 Decoder — ACTIVE PHASE

### 3.1 Standalone IDCT kernel — COMPLETE
- [x] idct_single.g4a — 2-pass dp4 IDCT (109 instructions)
- [x] DO_IDCT subroutine with jmpi/ip, a0.0 dual-half addressing
- [x] Bit-exact CPU reference (idct_ref.h)

### 3.2 Combined IQ + IDCT kernel — COMPLETE
- [x] iq_idct_intra.g4a (149 instructions) with U8 output to SURFTYPE_2D
- [x] Media Block Write (msg_type=2) with (x,y) coordinates from inline data
- [x] No-level-shift version for real decoding (145 instructions)
- [x] Bugs discovered and fixed:
  - {compr} UB→UW widening writes only half the GRF (stale data)
  - gen4asm .N subregister is in BYTES without the -a flag
  - {compr} on W add/mov writes only half the GRF

### 3.3 MPEG-2 bitstream parser — COMPLETE
- [x] Bitstream reader, start code scanner
- [x] Parser: sequence_header, picture_header, picture_coding_extension, slice_header
- [x] DC VLC decoder (Table B-12 luma, B-13 chroma)
- [x] AC VLC decoder (full Table B-14, 112 entries + escape codes)
- [x] Table B-15 for intra_vlc_format=1 — implemented
- [x] Macroblock address increment (Table B-1, up to 33 + escape + stuffing)
- [x] Macroblock type for I-picture (Table B-2) and P-picture (Table B-3)
- [x] Complete Coded Block Pattern Table B-9 (64 entries)
- [x] Intra macroblock decoder (6 blocks: 4Y + Cb + Cr, DC prediction)
- [x] Non-intra macroblock decoder (P-frame inter blocks)
- [x] Motion vector decode (Table B-10 + f_code expansion)
- [x] Inline IQ in the parser (as in ffmpeg): sign applied AFTER >>4
- [x] Mismatch control §7.4.3: block[63] ^= 1
- [x] Error recovery: continue on MB failure (not resync_to_next_slice)
- [x] Fix EOB after a full block (idx>=64): consume the trailing EOB
- [-] IDCT precision: ±1-3 pixel vs ffmpeg (libva cosine table, not IEEE 1180)

### 3.4 Parser → GPU decode integration — COMPLETE
- [x] Parse I-frame → dispatch GPU batch → verify pixels
- [x] 100% coverage on all test files (gray, dark, testsrc, real, mandel)

### 3.5 Visible output — COMPLETE
- [x] Decode Y on GPU, Cb/Cr on CPU, YCbCr→RGB32 (BT.601)
- [x] PPM output viewable with ShowImage
- [x] Final test results:
  | File | Resolution | MB | Coverage | PSNR vs ffmpeg |
  |------|------------|:---:|:---------:|:-----------:|
  | Gray q31 | 64×64 | 16/16 | **100%** | exact |
  | Dark q20 | 320×240 | 300/300 | **100%** | 44+ dB |
  | Real q31 | 640×480 | 1200/1200 | **100%** | 44.5 dB |
  | Testsrc q15 | 320×240 | 300/300 | **100%** | 44.3 dB |
  | Mandel q2 | 320×240 | 300/300 | **100%** | 50.0 dB |
  | Mandel q5 | 320×240 | 300/300 | **100%** | 49.4 dB |

### 3.6 IDCT-only GPU path — COMPLETE
- [x] Kernel idct_to_u8.g4a: IDCT + clamp + Media Block Write U8
- [x] Batch dispatch submit_blocks_batch_gpu() up to 400 blocks

### 3.7 P-frame motion compensation — CPU COMPLETE
- [x] test_mc_decode.cpp: multi-frame I+P decoder with CPU MC
- [x] Half-pel bilinear interpolation (4 cases)
- [x] Chroma MC with /2 MV scaling
- [x] Skipped MB handling, reference frame management

### 3.8 media_kit codec add-on — COMPLETE (I+P)
- [x] MPEG2DecoderPlugin + MPEG2Decoder plugin (DecoderPlugin API)
- [x] B_MPEG_2_VIDEO format registration via BMediaFormats
- [x] B_YCbCr420 output (Y + Cb + Cr planes)
- [x] I-frame decode via mpeg2_parser + compute_idct_reference
- [x] P-frame decode with motion compensation (half-pel bilinear)
- [x] Reference frame management (I/P → reference for subsequent Ps)
- [x] Skipped MB handling (copy from reference, zero MV)
- [x] Built as .so, installed to ~/config/non-packaged/add-ons/media/plugins/
- [x] Standalone viewer (mpeg2_viewer) with visual I+P playback
- [x] Multi-frame test: 10 frames (2 I + 8 P), 320x240, 100% MB coverage
- [ ] Test with MediaPlayer on a .m2v file
- [-] B-frame decode — implemented (Table B-4, backward MV, bidir MC)
      Decoding works: 293/300 MB on the first B-frame test.
      Coverage limited by upstream P-frame reference errors.

### 3.9 GPU Motion Compensation kernel — COMPLETE
- [x] mc_fullpel_only.g4b.gen5: full-pel MC kernel (13 instructions)
  - Media Block Read from reference surface (BTI 2)
  - Media Block Write to output surface (BTI 1)
- [x] Boot-time test: MC on a synthetic 32x32 gradient reference
  - Pixel-exact verification vs CPU mc_block_cpu(): 64/64 correct
- [x] GPU vs CPU benchmark (48 dispatch batch, single submission)
  - GPU: 17 µs / 48 blocks = 2.8M blocks/s
  - CPU: 7 µs / 48 blocks = 6.9M blocks/s
  - Ratio: 0.41x (CPU wins for a single 8×8 block, dispatch overhead dominates)
  - Gen5 limits discovered:
    - URB recycling broken: iterations ≤ num_urb_entries (max 48)
    - No second ring submission per media: IS stall after MI_FLUSH
    - Solution: everything in a single ring submission (state + N dispatch + flush)
- [ ] mc_idct_inter.g4a: combined MC + IDCT residual addition
- [ ] Batch dispatch integration for P-frames (MC per block)
- [x] IDCT benchmark, 400 blocks — **GPU WINS 4×**
      GPU: 114-126 µs, CPU: 458-475 µs → speedup **4.01×**
      Fix: pure busy-wait (no snooze), timing only from ring kick→marker.
      Standalone tool: tests/gpu_idct_bench (no reboot, clones the accelerant)

### 3.10 GPU 3D Cube Demo — WORKING
- [x] Accelerant clone from userspace (device open + shared_info + registers)
- [x] Compute rasterizer: 3D cube with 48 parallel EU threads
  - Rotation + perspective projection + backface culling + lighting
  - Painter's algorithm (Z-sort) for 6 colored faces
  - Tile fill kernel via the media pipeline
  - BWindow + BBitmap + Invalidate display loop
- [x] IDCT benchmark, 400 blocks — **GPU 4× faster than CPU**
  - GPU: 100-126 µs, CPU: 351-475 µs → speedup 3.5-4.0×
  - Standalone tool: tests/gpu_idct_bench (no reboot)
- [x] 3D cube demo: 480×480 raster, 720×720 window, 21 FPS
  - GPU tile fill: ~100 µs/frame (260 tiles)
  - Bottleneck: app_server Invalidate→Draw round-trip (~47 ms)
  - BDirectWindow attempted but VRAM write-combining was slower (11 FPS)
- [-] Userspace ring clone: ring reset works (HEAD/TAIL=0) but the CS
      does not restart after the boot-time media pipeline test.
      **Root cause found** (2026-05-09 session, i915 analysis):
      1. ILK_GDSR media domain reset needed (reg 0x12ca4) — a ring
         soft-reset is not enough for the IS stall from MEDIA_OBJECT+MI_FLUSH
      2. FORCEWAKE (reg 0xA18C) not asserted — GPU in RC6, TAIL drops
      3. Linux i915 NEVER allows a userspace RING_TAIL write —
         only the kernel via the execbuffer ioctl
- [x] **Phase A.1**: ILK_GDSR media domain reset in render_init_clone
- [x] **Phase A.2**: FORCEWAKE assert before ring operations
- [x] **Phase A.3**: GPU plasma demo visible on screen (177 FPS)
      GPU IDCT 0.1ms/frame, CPU blit 4.3ms/frame, 300 blocks/frame
      Discovery: graphics_memory is accessible from the clone, direct
      framebuffer write works. ILK_GDSR is not needed for the media
      pipeline (only for 3D pipeline ring recovery).
      Tool: tests/gpu_plasma_screen
- [x] **3D cube direct to framebuffer** — stable 60 FPS
      CPU raster (per-pixel edge test) → memcpy to graphics_memory.
      815 FPS raw, 60 FPS with frame limiter.
- [x] **BLT engine via kernel ioctl** — stable 60 FPS
      XY_SRC_COPY_BLT (0x54F00006) written into ring memory,
      TAIL kick via the INTEL_RING_WRITE_TAIL ioctl. The GPU executes the BLT,
      copying from a GTT buffer to the screen framebuffer. Tool: tests/gpu_triangle

### 3.10.1 Critical discovery: MMIO is read-only from userspace (2026-05-11)
MMIO writes are **silently ignored** both via `clone_area` and
via the `/dev/misc/poke` driver (direct BAR0 mapping at 0xF0000000). Verified:
- Scratch register: write 0xDEADBEEF, read back 0x0
- RING_BUFFER_HEAD stuck at 0x5134 — not resettable
- RING_BUFFER_CONTROL: write 0, read back 0xF001 — cannot be disabled
- Syslog confirms: `engine stalled, head 5134` (app_server fails too)
- Render ring HEAD identical across 5+ consecutive runs

**Implication:** the entire ring→GPU pipeline (BLT, media pipeline, 3D
TRILIST) is inaccessible from userspace. The kernel driver MUST perform the
TAIL/HEAD/CTL writes. This is the absolute prerequisite for:
- Userspace BLT engine
- Userspace media pipeline (post-boot)
- Mesa/crocus (GEM_EXECBUFFER2)
- Any GPU operation other than direct framebuffer access

### 3.11 Next steps
- [x] **Kernel ioctl for TAIL write** — WORKING (GPU executes commands)
- [x] **BLT blit to framebuffer** — WORKING (60 FPS, 3D cube visible)
- [x] **Media pipeline via ioctl** — WORKING (GPU IDCT 3.5x, 400 blocks)
      ring_submit_ioctl() in media_pipeline.cpp, falls back to QueueCommands.
      Ring reset via ioctl in media_pipeline_init for clones.
- [x] GPU IDCT in the plugin (_FlushBatch uses gpu_idct_process with CPU fallback)
      Fix: batch buffer overflow — dedicated GTT batch_base allocated (64KB).
      Removed redefined macros (RING_BUFFER_*, MI_FLUSH already in intel_extreme.h).
- [ ] Combined GPU MC+IDCT for full GPU P-frame decode
- [ ] Gouraud shading WM kernel (per-vertex color interpolation)
- [ ] IEEE 1180 IDCT (replace the cosine table)
- [-] B-frame support — parser + MC implemented, decode working
      Table B-4 (11 VLC), backward MV, bidirectional MC blend.
      Coverage 270-293/300 MB on the first B-frames (errors from upstream P-frame ref)

### 3.12 Modular multi-generation architecture — COMPLETE
- [x] **gpu_ring.h/cpp** — Generic ring submission via kernel ioctl.
      Self-contained, no dependency on gInfo. Works on any Intel
      gen with the INTEL_RING_WRITE_TAIL ioctl.
- [x] **gen_ops.h** — Vtable interface: batch_writer + gen_ops struct
      with function pointers for pipeline select, state base address,
      URB, BLT, markers. One gen_ops per generation.
- [x] **gen5_ops.cpp** — Gen5 (Ironlake) implementation. TESTED.
- [x] **gen6_ops.cpp** — Gen6 (Sandy Bridge) implementation. NOT TESTED.
      Differences: STATE_BASE_ADDRESS 10DW, PIPE_CONTROL, MEDIA_VFE_STATE
      inline, 3DSTATE_URB, MEDIA_INTERFACE_DESCRIPTOR_LOAD, 60 threads.
- [x] **gen7_ops.cpp** — Gen7 (Ivy Bridge / Haswell) implementation. NOT TESTED.
      Similar to Gen6: DEPTH_BUFFER 8DW, IVB 64 threads, HSW 112 threads.
- [x] **init_gen_ops()** — Auto-detects the generation from Generation() → vtable.
- [x] **PORTING.md** — Contributor guide: how to add support for a new generation.
- [ ] gen8_ops.cpp — Broadwell (last generation with classic ring submission)

---

## Phase 4: H.264 Decoder — after MPEG-2

- [ ] H.264 parser (NAL, SPS, PPS, slice, CAVLC/CABAC)
- [ ] GPU H.264 kernel (quarter-pel MC, IDCT 4x4/8x8, deblocking)
- [ ] Reference frame management (DPB, MMCO)
- [ ] Profiles Baseline → Main → High

---

## Phase 5: Compute / LLM — after video decode

- [ ] Compute kernels (matmul, softmax, RMSNorm, RoPE)
- [ ] Inference runtime (GPT-2 small / TinyLlama)

---

## Phase 6: OpenGL via Mesa crocus — ROADMAP

### Phase A: Working ring clone (prerequisite for everything)
- [-] A.1: ILK_GDSR media domain reset — not needed (discovered 2026-05-10)
- [-] A.2: FORCEWAKE — doesn't exist on ILK Gen5 (SNB+ only)
- [-] A.3: MMIO write from userspace impossible — HEAD stuck, TAIL ignored
      **Root cause 2026-05-11:** all MMIO writes (clone_area + poke
      BAR0 0xF0000000) silently ignored. A kernel ioctl is required.
- [x] A.4: 3D cube visible via direct framebuffer access (CPU raster, 60 FPS)

### Phase B: Kernel ioctl for ring submission — COMPLETE
- [x] B.1: INTEL_RING_RESET + INTEL_RING_WRITE_TAIL ioctl in the kernel driver
      Kernel writes TAIL/HEAD/CTL via MMIO (userspace is R/O).
      Manual build with -fPIC + mutex ABI shim (_mutex→mutex) for hrev59669.
      System driver blacklisted via /boot/system/settings/packages.
      **TEST PASS**: HEAD advances after TAIL write → GPU executes MI_NOOP!
      **CRITICAL**: RING_RESET (disable→re-enable) kills the CS after the
      first use. Solution: ring sync (read HW TAIL, never reset).
- [x] B.2: BLT via ioctl — 3D cube at 480×480 at 60 FPS, GPU BLT to screen
- [x] B.3: INTEL_RING_INIT_3D ioctl — Gen5 workaround via kernel MMIO
      MI_MODE (0x209C), _3D_CHICKEN2 (0x208C), CACHE_MODE_0 (0x2120).
      Confirmed working via syslog.
      **LRI (MI_LOAD_REGISTER_IMM) does NOT work on Gen5**: hangs the CS
      after 2 DW. Masked registers (MI_MODE, CACHE_MODE_0) get corrupted
      by raw LRI writes. Only direct kernel MMIO works.
- [x] B.4: Media pipeline via ioctl — GPU IDCT **5.8x**, 400 blocks (155µs vs 905µs)
- [ ] B.5: GPU hang detection + ILK_GDSR recovery in the kernel

### Phase C: Minimal DRM interface for Mesa crocus
- [x] C.1: GEM_CREATE / GEM_CLOSE — wrapper over INTEL_ALLOCATE_GRAPHICS_MEMORY ✅
- [x] C.2: GEM_MMAP — already mapped by allocator, just return addr ✅
- [x] C.2b: GETPARAM + GET_APERTURE + GEM_BUSY + SET_DOMAIN ✅
- [x] C.2c: GEM_CONTEXT_CREATE/DESTROY (stub) ✅
- [x] C.3: **GEM_EXECBUFFER2** — WORKING! (2026-05-13, ioctl path 2026-05-16)
      DRM shim: inline batch in the ring + TAIL kick via the INTEL_RING_WRITE_TAIL ioctl.
      Ring sync (not reset!) with the hardware TAIL — RING_RESET kills the CS.
      Relocation patching, EXEC_HANDLE_LUT, EXEC_BATCH_FIRST supported.
      Completion marker via MI_STORE_DATA_IMM in the ring (not in the batch).
      **gl_test 2026-05-16**: OpenGL 2.1 Mesa Intel(R) HD Graphics (ILK),
      GLSL 1.20. EXECBUF2 #1 (state setup) completed by the GPU!
      EXECBUF2 #2+ (3D render) hangs on MI_FLUSH (IPEHR=0x02000000) —
      a 3D pipeline problem, not EXECBUF2.
      **CRITICAL**: RING_RESET (disable→re-enable) kills the CS after the
      first use. Fix: sync with the HW TAIL without resetting (render_init_clone).
      **gl_test 2026-05-16** (with the kernel ioctl TAIL):
      - Ring sync OK: marker=0xBEEF0001, GPU WORKS!
      - Mesa crocus init OK: screen, context, resource_create, textures
      - EXECBUF2 #1 (state setup, 9 DW): GPU COMPLETED!
      - EXECBUF2 #2+ (glClear/3D render): hangs on MI_FLUSH
        IPEHR=0x02000000, INSTDONE=0xFFFFFFFF, HEAD stuck at 0x1a8
      - Cause: 3D pipeline state issue, not ring/EXECBUF2
      - Next: debug the MI_FLUSH stall after the 3DSTATE commands
      EXECBUF2 #2+ (glClear/3D render) hang: IPEHR=0x02000000 (MI_FLUSH
      after batch return), INSTDONE=0xFFFFFFFF, EIR=0x0.
      HEAD advances through MI_BATCH_BUFFER_START but stalls on
      post-batch MI_FLUSH. The 3D pipeline state from the batch
      leaves the CS unable to flush. Need to debug pipeline state.
      **Critical discovery:** RING_RESET (disable→re-enable) kills the CS
      permanently. Solution: ring sync (read HW TAIL, never reset).
- [x] C.4: GET_RESET_STATS, SET_TILING, GET_TILING, SET_CACHING, GEM_WAIT,
      CONTEXT_GETPARAM/SETPARAM, MADVISE — all implemented in the dispatcher

### Phase D: Mesa crocus winsys for Haiku — IN PROGRESS
- [x] D.1: libdrm shim (xf86drm.h, _IOC compat, drmDevice, libdrm_shim.so)
- [x] D.2: crocus_bufmgr → Haiku GEM shim via haiku_drm_intel
- [x] D.3: Mesa 25.3.3 compiled with crocus (libcrocus.a OK)
- [-] D.4: CrocusRenderer + "Crocus Pipe" add-on (128MB, statically linked)
      **Status 2026-05-14:**
      - GLInfo/gl_test loads the Crocus Pipe OK, crocus_screen_create OK
      - Ring sync (no reset): GPU WORKS, ring test marker OK
      - GEM_EXECBUFFER2 #1 (state setup): **completed by the GPU!**
      - GEM_EXECBUFFER2 #2+ (glClear/render): hangs on post-batch MI_FLUSH
        IPEHR=0x02000000, INSTDONE=0xFFFFFFFF. CS stall after 3DSTATE.
        HEAD advances through the batch but stalls on MI_FLUSH in the ring.
      - Next: replace MI_FLUSH with PIPE_CONTROL or a workaround
      - OpenGL 2.1, GLSL 1.20, Mesa Intel(R) HD Graphics (ILK)
      - Next: debug the crash after validate_textures (ralloc/syncobj)
      **Next blocker:** 3D pipeline hang at batch #3 (see E.2)

### Phase E: GLInfo and visible rendering — PLANNING

#### E.1: GLInfo shows correct GL data — COMPLETE (2026-05-11)
- [x] Fix `st_api_make_current`: declared as bool (was int, ABI mismatch)
- [x] glapi dispatch bridge: Mesa 25 `_mesa_glapi` → system `_glapi`
      Find the ALREADY loaded libglapi.so (get_next_image_info, not load_add_on)
- [x] `hgl_buffer->newWidth/Height` initialized from BGLView bounds
- [x] GLInfo shows: **Mesa Intel(R) HD Graphics (ILK), OpenGL 2.1 Mesa 25.3.3**
      Texture 2D max: 8192, all capabilities populated

#### E.1.1: ISL format table fix (2026-05-11)
- [x] **ISL format table corrupted in the linked binary** — 170 valid entries out of 918.
      Entry 233 (B8G8R8X8_UNORM) had bpb=0 → crash `bs > 0` in isl_tiling_get_info.
      Root cause: the linker did not correctly write the sparse C99 array.
      Fix: binary patch of the Crocus Pipe with the correct table from the .o (36720 bytes).
- [x] **gl_test no longer crashes** — black window (glClear not visible, needs SwapBuffers)

#### E.2: SwapBuffers GPU→Screen (visible rendering) + 3D pipeline debug
- [-] **gl_test session 2026-05-13 (new, with the ring sync fix):**
      OpenGL 2.1 Mesa 25.3.3 Intel(R) HD Graphics (ILK) — init OK
      EXECBUF2 #1: 9 cmds state setup → **GPU completed!** (seq=1)
      EXECBUF2 #2: 65 cmds glClear → **GPU completed!** (seq=2)
      EXECBUF2 #3: 67 cmds readback → **GPU HANG** HEAD=0x254, TAIL=0x3e0
      EXECBUF2 #4+: all timing out, HEAD stuck at 0x254
      **Hang #3 diagnostics:**
      - IPEHR=0x79000002 (3DSTATE_GLOBAL_DEPTH_OFFSET_CLAMP)
      - INSTDONE=0xFFFFFFFF (pipeline completed, no stage blocked)
      - EIR=0x0 ESR=0x0 (no HW error)
      - ACTHD=0x254 (GPU stopped in the ring, not in the batch)
      - Batch #3 starts with: 79000002 00000000 00000000 00000000
        then 61010006 (STATE_BASE_ADDRESS), 78080007 (PIPELINED_POINTERS)
      - Batch #2 (which works) has nearly identical commands — what's the difference?
      **Batch #2 vs #3 analysis:**
      Batch #2 (OK):  79000002 00000000 **00c7012b** 00000000 61010006...
      Batch #3 (HANG): 79000002 00000000 **00000000** 00000000 61010006...
      DW2 of 3DSTATE_GLOBAL_DEPTH_OFFSET_CLAMP: 0x00c7012b vs 0x00000000
      The difference could be significant (depth buffer config).
- [ ] **Debug the 3D pipeline hang** — ACTIVE BLOCKER
      **Session 2026-05-14 (evening):**
      - Workaround registers confirmed applied (syslog: INIT_3D 5 times)
      - LRI in the ring HANGS the CS on Gen5 (after 2 DW) — masked registers get corrupted
      - EXECBUF2 #1 hangs with a dirty ring (from a previous LRI) — invalid
      - A clean boot is needed for a reliable test
      **Session 2026-05-13 (reference, clean ring):**
      - EXECBUF2 #1 (state): OK, EXECBUF2 #2 (glClear): OK
      - EXECBUF2 #3 (readback): HANG on 3DSTATE_GLOBAL_DEPTH_OFFSET_CLAMP
      - Difference batch #2 vs #3: DW2 = 0x00c7012b vs 0x00000000
      Next step (after reboot):
      1. Compare batch #2 (OK) and #3 (hang) DW by DW
      2. Verify the surface state encoding (0x42b040, 0x42b080 refs)
      3. Check whether a PIPE_CONTROL/MI_FLUSH is missing between batches
      4. Test: force the same commands from batch #2 into batch #3
      5. Try adding a PIPE_CONTROL with a CS stall before batch #3
- [ ] Option A (quick): readback GPU surface → memcpy to BBitmap (CPU, slow)
- [ ] Option B (fast): BLT from the GPU surface to the direct framebuffer
- [ ] Option C (correct): DRI-style direct rendering

#### E.3: DRM shim robustness
- [ ] Ring wrap: handle wrap-around without RING_RESET (sync HEAD, wait drain)
- [ ] GEM_BUSY: check whether the BO has pending commands in the ring
- [ ] SYNCOBJ: implement real wait (poll on the completion marker)
- [ ] Multiple EXECBUFFER2 in sequence without ring overflow

#### E.4: Mesa crocus completeness
- [ ] Verify the crocus batch submit path (crocus_batch.c) end-to-end
- [ ] Test with a simple GL program (glClear + glFinish)
- [ ] Test with glxgears or a Haiku equivalent
- [ ] Profile performance: GPU batch throughput

### Preliminary discoveries
- **(2026-05-08)** Ring buffer accessible from a userspace clone, but do NOT reset it.
  render_init_clone(): allocates GPU state, syncs the ring position.
- **(2026-05-11)** **MMIO write broken**: writes to GPU registers are ignored
  from userspace (clone_area + poke). Root cause: the kernel maps with
  B_KERNEL_WRITE_AREA without B_WRITE_AREA. Solved with a kernel ioctl.
- **(2026-05-11)** **Ring reset kills the CS**: disable+re-enable of the ring
  kills the Command Streamer after the first use. Works only once
  after boot, then the CS never restarts. Use sync-only (read the HW TAIL).
- **(2026-05-11)** **GEM_EXECBUFFER2 working**: the DRM shim sends batches
  via MI_BATCH_BUFFER_START in the ring + a TAIL kick ioctl. Mesa crocus
  submits batches and the GPU executes them. Ring sync without reset.
- **(2026-05-11)** **GLInfo analysis**: all 18 ioctls are handled.
  `st_api_make_current` returns true (ABI: declared as int, is actually bool).
  Missing GPU surface → BBitmap bridge for visible rendering.

### Mesa roadmap (current status)
- [x] **Step 1**: GETPARAM + GET_APERTURE (chipset_id=0x0046) ✅
- [x] **Step 2**: GEM_CREATE/CLOSE/MMAP ✅
- [x] **Step 3**: GEM_CONTEXT_CREATE/DESTROY (stub) ✅
- [x] **Step 4**: **GEM_EXECBUFFER2** ✅ (clflush + INTEL_EXEC_BATCH ioctl)
- [x] **Step 5**: SET_TILING, GET_TILING, GEM_WAIT, CONTEXT_PARAM, RESET_STATS ✅
- [x] **Step 6**: libdrm shim for Haiku — xf86drm.h with _IOC compat, drmDevice,
      syncobj stubs, drmPrime, drmAuth. libdrm_shim.so with extern "C" exports.
- [x] **Step 7**: **Mesa 25.3.3 compiled with crocus!**
      `meson -Dgallium-drivers=softpipe,crocus -Dplatforms=haiku -Dwerror=false`
      Patches: system_has_kms_drm += haiku, intel_perf MIN/MAX guard,
      build_id stub, libsync _IOC compat. 1339 files compiled.
      Output: libgallium-25.3.3.so (134MB) with softpipe + crocus.
- [-] **Step 8**: CrocusRenderer add-on for the Haiku GL stack
      Add-on installed and working (GLInfo loads, crocus screen + EXECBUF OK).
      Missing GPU surface → BBitmap bridge for visible display.
      See Phase E for detailed planning.

### 2026-05-10 session discoveries

**IDCT benchmark fixed with busy-wait:**
- snooze(500) in the wait loop skewed the GPU timing (60ms measured vs ~100µs real)
- Fix: pure busy-wait (spin loop without snooze) in the benchmark
- Result: **GPU 3.5-4.0× faster than the CPU** on 400 8×8 IDCT blocks
  - GPU: 100-126 µs (400 blocks in parallel, 48 EU threads + URB recycling)
  - CPU: 351-475 µs (400 blocks sequential, compute_idct_reference -O2)
- Standalone tool: tests/gpu_idct_bench (no reboot, output to terminal)

**Ring clone — full analysis:**
- The ring works from the clone for the media pipeline + BLT (cube demo OK)
- The ring does NOT work after the cube demo (or any media pipeline)
  has filled the ring → CS blocks on MI_FLUSH after MEDIA_OBJECT
  (the same IS stall bug documented in a previous session)
- Diagnostics: HEAD=0x5134 fixed with TAIL=0xCA20 (31KB of pending commands)
- MMIO register writes work from the clone (TAIL updates)
- ring.base == graphics_memory (VA correct, no remap needed)
- HWS_PGA=0x1ffff000 (HW Status Page configured by the kernel)
- Fundamental problem: **the CS blocks after N media dispatches** because
  MI_FLUSH doesn't complete (IS stall). Once blocked, the ring is dead.
  Only a reboot or an ILK_GDSR reset can bring it back to life.

**3D TRILIST triangle — status:**
- render_draw_triangle implemented with CLIP ACCEPT_ALL + VUE header
- Not testable while the ring is dead (needs a fresh boot without the cube demo)
- Approach: clean boot → test TRILIST before any media pipeline

### Immediate next step
- [ ] Fresh boot (no test accelerant, no cube demo) → test _tri_test
      to verify TRILIST on a clean ring
- [ ] If TRILIST works: integrate into the visual demo (cube with the 3D pipeline)
- [ ] MI_BATCH_BUFFER_START from the clone — prerequisite for EXECBUFFER2
- [ ] Fix the CS hang: find a way to reset the CS after the media pipeline
      without rebooting (ILK_GDSR or a kernel ioctl)

---

## Discovered and documented Gen5 bugs

| # | Bug | Impact | Fix |
|---|-----|---------|-----|
| 1 | {compr} UB→UW widening writes only half the GRF | Nondeterministic stale data | Explicit 2×SIMD8 widening |
| 2 | gen4asm .N subreg is in BYTES (without -a) | D→W pack at the wrong position | Use .16 for UW elem 8 |
| 3 | {compr} on W add/mov writes only half the GRF | +128 applied to only half the block | 4× add(16) without {compr} |
| 4 | dc_pred init already includes the level shift | +128 after IDCT doubles the offset | Do NOT add +128 |
| 5 | GOP code 0xB8 > SLICE_START_MAX | Parser exits the header loop | Check <= SLICE_MAX |
| 6 | EOB '10' not checked for the first AC | DC-only blocks don't terminate | EOB check before '1s' |
| 7 | Stale URB entry data beyond 48 dispatches | Threads read wrong data | 16 DW padding for inline data |
| 8 | SURFTYPE_2D for OWord Block R/W | Silent OOB drops | Use SURFTYPE_BUFFER |
| 9 | MEDIA_BLOCK_READ msg_type=4 (PRM) | Silently fails | Use msg_type=2 |
| 10 | Send payload from GRF (not MRF) | Corrupted output | Header from GRF, payload from MRF |
| 11 | Table B-14 12-bit VLC codes WRONG | Incorrectly decoded AC coefficients | Correct codes from ffmpeg ff_mpeg1_vlc_table |
| 12 | Start code 00 00 01 inside a macroblock | Parser reads the start code as a DC VLC | Check peek(23)==0 before every block |
| 13 | quantiser_scale never updated | AC IQ with q_scale=1 (hardcoded) | Compute from q_scale_code per-slice |
| 14 | EOB not consumed after a full block | 2-bit drift per block with 64 coeff | Read trailing EOB when idx>=64 |
| 15 | CBP Table B-9 incomplete | Silent corruption of P-frame inter MB | Full 64-entry table implemented |
| 16 | resync_to_next_slice on MB failure | Loses all remaining MBs in the slice | Replaced with continue (skip+retry) |
| 17 | VFE num_urb_entries=1, >6 dispatches | IS stall at the 7th MEDIA_OBJECT | num_urb_entries = dispatch_count (max 48) |
| 18 | Second ring submission for media after MI_FLUSH | IS stall, EU threads don't dispatch | Everything in a single ring submission |
| 19 | 3D pipeline from the clone: 0 pixel output | State block clone ignored by the 3D pipeline | Use the media pipeline (compute) for rasterization |
| 20 | 3D commands in the ring (not a batch): parsed but not dispatched | 3DPRIMITIVE in the ring produces no pixels | Always use MI_BATCH_BUFFER_START |
| 21 | MC kernel EOT doesn't release the URB entry | URB recycling broken for the MC kernel | IDCT kernel works, MC doesn't |
| 22 | gpu_debug_wait_value snooze(500) | Skewed benchmark timing (60ms vs µs) | Pure busy-wait for benchmarking |
| 23 | Media pipeline hung → CS dead for everyone | Cube demo fills the ring, HEAD stuck, ring dead | Fresh boot, or ILK_GDSR / kernel ioctl |
| 24 | Userspace ring reset doesn't restore the CS | HEAD=0 but CS doesn't restart after disable/re-enable | Only the kernel driver can perform a GPU reset |

---

## Key milestones

| # | Milestone | Status |
|---|---|---|
| M1 | Working 1366x768 display | ✅ |
| M2 | 2D BLT acceleration | ✅ |
| M3 | 3D solid fill via the render engine | ✅ |
| M4 | First EU kernel via the media pipeline | ✅ |
| M5 | 48-thread parallel dispatch | ✅ |
| M6 | Bit-exact FP32 arithmetic (SAXPY) | ✅ |
| M7 | Sampler cache + CURBE + libva ABI | ✅ |
| M8 | MPEG-2 IQ kernel | ✅ |
| M9 | MPEG-2 IDCT kernel (2-pass dp4) | ✅ |
| M10 | IQ+IDCT+clamp+U8+Media Block Write | ✅ |
| M11 | MPEG-2 parser + GPU I-frame decode | ✅ |
| M12 | First visible MPEG-2 frame (PPM) | ✅ |
| M13 | 100% I-frame decode (all files) | ✅ 100%, PSNR 44-50 dB vs ffmpeg |
| M14 | P-frame motion compensation (CPU) | ✅ MC + parser EOB/CBP fixes |
| M14b | media_kit I+P decode plugin | ✅ 10 frames, 100% MB coverage |
| M14c | GPU IDCT batch benchmark | ✅ 400 blocks, GPU 4× faster than CPU |
| M14d | GPU 3D cube demo (compute rasterizer) | ✅ 480×480, 21 FPS, 48 EU threads |
| M15 | BLT framebuffer blit (bypassing app_server) | ⬜ |
| M16 | Full GPU MC+IDCT P-frame decode | ⬜ |
| M17 | MPEG-2 playback in MediaPlayer | ⬜ |
| M18 | H.264 decode | ⬜ |
| M17 | LLM inference on GPU | ⬜ |
