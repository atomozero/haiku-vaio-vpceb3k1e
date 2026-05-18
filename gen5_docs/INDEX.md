# Gen5 / Ironlake Documentation Index

Local reference collection for Intel HD Graphics (Ironlake, device 0x0046) driver development on Haiku, VAIO VPCEB3K1E.

Target: move the `intel_extreme` accelerant from "modeset-only" to "EU array usable for GPGPU" so a small LLM can run with hardware acceleration.

---

## 1. Intel Official PRMs (`prm_ironlake/`)

Authoritative hardware reference. Source: `https://kiwitree.net/~lina/intel-gfx-docs/prm/ilk/` (mirror of Intel 01.org Open Source PRMs, 2010).

| File | Volume | Subject | Primary use for us |
|---|---|---|---|
| `ilk_ihd_os_vol1_part1r2.pdf` | 1.1 | Graphics Core overview | Architecture primer |
| `ilk_ihd_os_vol1_part2r2.pdf` | 1.2 | MMIO, Media Registers, Programming Environment | **Register map — essential** |
| `ilk_ihd_os_vol1_part3r2.pdf` | 1.3 | Memory Interface + **Render Engine commands** | **Ring/MI/PIPE_CONTROL — essential** |
| `ilk_ihd_os_vol1_part4r2.pdf` | 1.4 | Video Codec Engine | (not needed) |
| `ilk_ihd_os_vol1_part5r2.pdf` | 1.5 | **Blitter Engine** | 2D fallback path |
| `ilk_ihd_os_vol2_part1r2.pdf` | 2.1 | **3D Pipeline** (VF/VS/GS/CLIP/SF/WM/CC) | **Critical — current hang lives here** |
| `ilk_ihd_os_vol2_part2r2.pdf` | 2.2 | **Media Pipeline** (VFE/CURBE/thread dispatch) | **Critical for LLM — GPGPU path** |
| `ilk_ihd_os_vol3_part1r2.pdf` | 3.1 | VGA Registers | Legacy, not needed |
| `ilk_ihd_os_vol3_part2r2.pdf` | 3.2 | CPU Display Registers | Already handled by modeset |
| `ilk_ihd_os_vol3_part3r2.pdf` | 3.3 | PCH Display Registers | IBX watermarks (already patched) |
| `ilk_ihd_os_vol4_part1r2.pdf` | 4.1 | **Shared Functions** (sampler, DAP, URB) | **Critical — URB mgmt, sampler for matmul** |
| `ilk_ihd_os_vol4_part2_july_28_10.pdf` | 4.2 | **Message Gateway, URB, ISA (EU instruction set)** | **Critical — compute kernel ISA** |

Reading order for the current blocker (3DPRIMITIVE hang):
1. **Vol 1 Part 3** §1 (memory interface), §2 (Render Engine Command Streamer) — ring, MI_*, PIPE_CONTROL.
2. **Vol 2 Part 1** §1.7 (3D pipeline overview), §2 (3DSTATE_* ordering rules), the VS/SF/WM sections.
3. **Vol 4 Part 1** (URB allocation and shared functions).

Reading order for LLM / GPGPU path:
1. **Vol 2 Part 2** (media pipeline — MEDIA_VFE_STATE, MEDIA_CURBE_LOAD, MEDIA_OBJECT thread dispatch).
2. **Vol 4 Part 2** (EU ISA — instructions, register file, SIMD8/SIMD16 execution model).
3. **Vol 4 Part 1** sampler engine (for matrix load patterns).

---

## 2. Mesa i965 References (`mesa_refs/`)

Mesa's i965 classic driver lives on the `amber` branch (dropped from main). These are the reference implementations that actually drove Ironlake silicon in production for ~10 years — the ground truth for "what sequence of commands actually works".

### genxml hardware descriptions (authoritative bit layouts)
- `gen50.xml` — **Gen5 (Ironlake) packet/state definitions**. Every `3DSTATE_*`, `MI_*`, `PIPE_CONTROL` with exact bit positions. Use this to validate every DWORD we emit.
- `gen45.xml` — Gen4.5 (Ironlake predecessor, similar layout, useful cross-check)
- `gen40.xml` — Gen4 (original Broadwater)

### i965 classic driver sources
- `brw_urb.c` — **URB partitioning for Gen4/5**. This is the file to compare against our URB setup. Small (268 lines) and focused.
- `brw_vs.c` — VS state setup, kernel pointer, thread dispatch configuration
- `brw_sf.c`, `brw_wm.c`, `brw_gs.c`, `brw_clip.c` — pipeline stage state
- `brw_misc_state.c` — STATE_BASE_ADDRESS, depth buffer, drawing rect
- `brw_state_upload.c` — ordering of state uploads (what comes before what)
- `brw_curbe.c` — constant URB entries (push constants)
- `brw_draw_upload.c` — vertex buffer / vertex element setup
- `brw_vs_surface_state.c` — VS surface state / binding tables
- `brw_defines.h` — **full register/bitfield defines for all i965 generations**
- `genX_state_upload.c` — newer unified state upload (Gen6+, for cross-reference)

---

## 3. Linux i915 References (`linux_i915_refs/`)

From Linux kernel v4.19 (last version with full Gen5 support in mainline style).

- `i915_reg.h` — **complete register map**, all MMIO offsets, PCH registers, etc.
- `intel_ringbuffer.c` — **ring init, Gen5 workarounds** (`init_render_ring()` contains the MI_MODE / _3D_CHICKEN2 / CACHE_MODE_0 workarounds we already partially apply)
- `i915_gem_render_state.c` — render state helper (Gen5 has no precanned null state; ILK is skipped — see note below)
- `intel_renderstate.h` + `intel_renderstate_gen{6,7,8,9}.c` — null render state blobs for Gen6+. Useful as shape reference, NOT directly applicable to Gen5.
- `intel_lrc.c` — logical ring contexts (Gen8+, reference only)

**Key finding**: there is no `intel_renderstate_gen5.c`. On Ironlake, Linux i915 does not use a precanned null-state batch — the first 3D draw has to bring its own complete state. This means we can't copy/paste a Linux golden batch for Gen5; the reference implementation we need is **Mesa i965** (which is what actually runs 3D on Gen5 under Linux).

---

## 4. libva-intel-driver (`libva_intel/`) — PRIMARY COMPUTE REFERENCE

Shallow clone of `github.com/intel/intel-vaapi-driver`, ~41 MB, MIT licensed.

This is the **single most important reference** for the media-pipeline work. It is the only MIT codebase that programs the Gen5 media pipeline end-to-end for real workloads (video decode).

Highlights:
- `src/shaders/mpeg2/vld/*.g4a` + `*.g4b.gen5` — **full MPEG-2 VLD kernel set for Gen5** in readable assembly source AND prebuilt binary form. This is the porting target for phase 2 of the video-decode-first roadmap.
- `src/shaders/h264/mc/*.gen5` — H.264 motion-compensation kernels for Gen5.
- `src/shaders/post_processing/*` — image scaling/conversion kernels.
- `src/i965_media.c`, `src/i965_media_mpeg2.c`, `src/i965_media_h264.c` — the CPU-side driver glue that builds `MEDIA_VFE_STATE` / `MEDIA_OBJECT` batches and dispatches kernels. Directly applicable to our bring-up.
- `src/gen6_mfd.c` and related — newer-gen video decode (for structural comparison).
- `src/i965_drv_video.{c,h}` — top-level VA-API entrypoints.

## 5. Mesa crocus (`mesa_crocus/`) — CLEAN STATE-PACKING REFERENCE

Targeted download (~1 MB) of `src/gallium/drivers/crocus/` from Mesa main branch. Gen4–Gen7 Gallium3D driver, replaces i965 classic. Cleaner bitfield handling via genxml packers.

Key files:
- `crocus_state.c` — all `3DSTATE_*` emission. The modern-style reference.
- `crocus_batch.{c,h}` — batch buffer management.
- `crocus_bufmgr.{c,h}` — BO manager (relocation-based, works on Gen5).
- `crocus_pipe_control.c` — PIPE_CONTROL use patterns.
- `crocus_program.c` — shader compilation and upload.
- `crocus_todo.txt` — known issues list, useful to avoid re-hitting them.
- `crocus_defines.h`, `crocus_genx_macros.h`, `crocus_genx_protos.h` — the macro glue that turns genxml into packers.

## 6. IGT GPU Tools (`igt/`) — DEBUGGING + EU ASSEMBLER

Shallow clone of `gitlab.freedesktop.org/drm/igt-gpu-tools` (~73 MB, MIT).

Two separable treasures:

### 6.1 `igt/lib/` — low-level GPU test helpers
- `intel_batchbuffer.c` — minimum-viable batch building + submission.
- `intel_reg.h` — comprehensive register definitions.
- `intel_chipset.c` — chipset detection patterns.
- `tests/gem_*.c` — hundreds of ~50-line test programs that each exercise one specific hardware feature. Unparalleled for "how do I do X on the ring" lookup.

### 6.2 `igt/assembler/` — **intel-gen4asm (THE EU ASSEMBLER)**
The full Gen4/5/6/7/8 EU assembler, ~17k lines of flex/bison + C. This is the single most valuable find in the whole documentation effort.

- `gram.y` (3398 lines) — full grammar for the Gen4+ EU ISA.
- `lex.l` — tokenizer.
- `brw_eu_emit.c` — instruction encoder.
- `brw_eu.{c,h}` — EU ISA constants and helpers.
- `brw_defines.h`, `brw_reg.h`, `brw_structs.h` — complete register and instruction definitions.
- `brw_disasm.c` + `disasm-main.c` — standalone disassembler for debugging compiled kernels.
- `main.c` — the `intel-gen4asm` command-line tool entry point.
- `test/` — regression tests (sample `.g4a` → expected binary output).

**This removes the entire "how do we produce EU machine code without a Linux toolchain" question.** Port this subtree into the Haiku build and we can write kernels in human-readable `.g4a` syntax and assemble them at build time. No precompiled blobs ever ship.

## 7. xf86-video-intel (`xf86_video_intel/`) — BLT ENGINE REFERENCE

Shallow clone of `gitlab.freedesktop.org/xorg/driver/xf86-video-intel` (~13 MB, MIT).

Scope: the historical X.Org 2D acceleration driver. Reference for the BLT / 2D engine track.

Key files:
- `src/sna/gen5_render.c` + `.h` — **Gen5-specific SNA render path**. Production-hardened 2D acceleration for Ironlake.
- `src/sna/kgem_debug_gen5.c` — Gen5 batch buffer debugging helpers.
- `src/sna/brw/brw_test_gen5.c` — Gen5 unit tests from the SNA tree.
- `src/sna/kgem.{c,h}` — kernel GEM abstraction layer (BO management).

Use this as the canonical reference if we keep the BLT engine track alive for 2D app_server acceleration in parallel with the media-pipeline work.

## 8. Analysis (`analysis/`)

Our own documentation:
- `LLM_FEASIBILITY.md` — can a small LLM actually run on this GPU, and via which pipeline.
- `REFERENCE_PROJECTS.md` — external reference projects and clean-room license analysis.
- `VIDEO_DECODE_PIVOT.md` — **strategic pivot: video decode as primary goal, LLM as phase 2.**
- `MEDIA_PIPELINE_BRINGUP.md` — **concrete 10-step command sequence, DWORD-level details, and phase 1 plan extracted from libva-intel-driver.**
- `GEN4ASM_PORT.md` — **feasibility analysis and integration plan for porting `intel-gen4asm` into the accelerant tree as a build-time tool.**
- `PHASE_I_A_REPORT.md` — **session report: gen4asm ported to Haiku, all 15 libva MPEG-2 Gen5 kernels validated byte-identical.**
- `PHASE_I_B_TEST_PROTOCOL.md` — **hardware test protocol: safety, build, install, interpretation, rollback for the media-pipeline hello-world probe.**
- `PHASE_I_B_REPORT.md` — **✅ PASSED: first on-GPU run of an in-tree authored kernel via the Gen5 media pipeline, all 11 markers OK, full session dump.**
- `PHASE_1_2_REPORT.md` — **✅ PASSED: parallel dispatch of 48 hardware threads via 48 back-to-back MEDIA_OBJECTs, URB_FENCE scaling fix, first real benchmark numbers.**
- `PHASE_1_3_REPORT.md` — **✅ PASSED: first kernel that reads and writes memory via surface state + binding table (memset_indexed, 48 rows bit-exact).**
- `PHASE_2_1B_REPORT.md` — **✅ M1 PASSED / M2 COMPLETED: SAXPY scaling (16..2048 elem bit-exact) + throughput benchmark (GPU vs CPU MB/s/MFLOPS). Discovered SURFTYPE_BUFFER requirement for OWord Block Read/Write after a 10-reboot bug chase.**
- `PHASE_2_2_REPORT.md` — **✅ PASSED: first kernel using Media Block Read via sampler cache on SURFTYPE_2D surface. Phase 2.2 single-row + Phase 2.2b multi-row with arbitrary (X, Y) origins. Sampler path ready for libva kernel port.**
- (future) `PHASE_2_3_REPORT.md` — port of first libva MPEG-2 VLD kernel (iq_intra).
- (future) `RENDER_HANG_DEBUG.md` — step-by-step diagnosis of the (now-parked) 3DPRIMITIVE hang.

---

## Quick reference — key numbers for Ironlake 0x0046 (Arrandale mobile)

| Parameter | Value | Source |
|---|---|---|
| Generation | Gen5 | PRM Vol 1 Part 1 |
| EUs (Execution Units) | **12** | PRM Vol 4 Part 2 |
| Threads per EU | 4 | PRM Vol 4 Part 2 |
| Max concurrent threads | 48 | 12 × 4 |
| SIMD width | 8 / 16 (per instruction) | EU ISA |
| GPU clock (typical) | ~500-900 MHz | varies, no fixed PRM value |
| Peak FP32 FLOPS (estimate) | ~40-80 GFLOPS | 12 EU × 8 SIMD × 2 (MAD) × ~0.5 GHz |
| GTT size | 2 MB (512 entries) | syslog |
| Stolen memory | 128 MB | syslog |
| Aperture | 256 MB | syslog |
| L3 cache | 256 KB (shared) | PRM Vol 1 Part 1 |
| URB total size | 256 entries × 1 KB = 256 KB | PRM Vol 2 Part 1 §1.7 |
| Memory | System DRAM via GTT (no VRAM) | integrated |
| Memory BW (system) | ~8.5 GB/s (DDR3-1066 dual ch) | platform spec |
