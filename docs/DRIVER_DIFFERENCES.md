# Driver Differences vs. Stock Haiku

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Reference |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](INDEX.md)

---

This document lists all the changes made to the `intel_extreme` (GPU) and
`hda` (audio) drivers relative to the original version included in Haiku
R1~beta5 (hrev59506).

---

## 1. GPU — intel_extreme accelerant

The original accelerant driver is located at:
`/boot/system/add-ons/accelerants/intel_extreme.accelerant` (194179 bytes)

Our patched driver is located at:
`/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`

### 1.1 Ports.cpp — LVDSPort::IsConnected()

**Problem:** On PCH platforms (Ironlake+), EDID reads over GMBUS DDC fail
on many LVDS panels. The driver treats the port as "not connected" and
falls back to VESA at 1024x768.

**Fix:** Added a fallback chain after HasEDID() fails:
1. Try VESA EDID from the bootloader (`has_vesa_edid_info`)
2. Fall back to VBT data (`got_vbt`) — contains 1366x768 from the BIOS
3. Last resort: port enabled by the BIOS (bit `LVDS_PORT_EN`)

The same logic already existed for Gen<=4 but was missing on the PCH path.

### 1.2 Ports.cpp — LVDSPort::SetDisplayMode()

**Problem:** The dual/single channel LVDS configuration was derived from
the PLL's P2 divider, potentially overwriting the correct BIOS
configuration and causing a black screen.

**Fix:** Preserve the BIOS configuration by reading the `LVDS_CLKB_POWER_UP`
bit from the current LVDS register. Reference: Michael Forney's fix for
the 9front igfx driver.

### 1.3 Ports.cpp — LVDSPort::SetDisplayMode()

**Problem:** The panel fitter was enabled unconditionally, even when the
requested resolution matched the panel's native resolution. With 1366
pixels (not evenly divisible), the 1:1 panel fitter introduced visual
artifacts.

**Fix:** Disable the panel fitter when `needsScaling` is false (native
resolution).

### 1.4 Pipes.cpp — Pipe::Enable()

**Problem:** Display watermarks were only configured for PCH Cougar Point
(CPT/Sandy Bridge), not for Ibex Peak (IBX/Ironlake).

**Fix:** Added `INTEL_PCH_IBX` to the watermark condition.

### 1.5 engine.cpp — QueueCommands (HWS Sync)

**Problem:** `intel_wait_engine_idle()` used MMIO polling of the ring
buffer's HEAD register (~100-500ns per read), causing excessive latency
during GPU synchronization.

**Fix:** Implemented sync via the Hardware Status Page:
- Every 8 commands, it emits `MI_STORE_DWORD_INDEX`, which writes a
  sequence number into the HWS page
- `intel_wait_engine_idle()` reads the HWS page (cached memory, ~1-2ns)
  and checks whether the sequence number has reached the target
- Automatic fallback to HEAD polling if the HWS is not available

### 1.6 engine.cpp — BLT Optimizations

- BLT command construction moved out of the loop (opcode/mode/stride are
  constant)
- Fast `memcpy` path in `QueueCommands::Put()` for commands with no wrap
- Fixed uninitialized `flush` variable in the destructor
- Fixed `intel_fill_span`: added missing `queue.Put()` call in the loop

### 1.7 engine.cpp — Batch Buffer (infrastructure, disabled)

Implemented the `BatchCommands` class and the `init_batch_buffer()`/
`uninit_batch_buffer()` functions for command submission via
`MI_BATCH_BUFFER_START`. Currently disabled in the BLT functions because
the overhead for single operations (count=1) outweighs the benefit.

### 1.8 render.cpp/render.h — Gen5 3D Render Engine

Implemented 2D rendering via the Gen5 (Ironlake) 3D pipeline:
- Ring buffer re-initialization (disable/reset HEAD/re-enable) matching
  Linux i915's `init_ring_common()` — required for Type 3 commands
- Gen5 workarounds: MI_MODE, _3D_CHICKEN2, CACHE_MODE_0
- `GEN5_3D(pipeline, opcode, subopcode)` macro for command encoding
- SF kernel from intel-vaapi-driver (7 instructions: attribute delta +
  URB_WRITE)
- WM solid-fill kernel (6 instructions: immediate RGBA MOVs + FB_WRITE
  SIMD8)
- Color patching: converts BGRA uint32 to 4 floats and patches the kernel
  binary
- URB_FENCE + CS_URB_STATE for URB partitioning (VS:256, SF:64)
- State setup: VS/SF/WM/CC, binding table, surface state, CC viewport
- `render_fill_rect()` function with a complete command sequence:
  MI_FLUSH → MI_LRI workarounds → PIPELINE_SELECT → STATE_BASE_ADDRESS →
  URB_FENCE → PIPELINED_POINTERS → DRAWING_RECTANGLE → BINDING_TABLE_PTRS →
  VERTEX_BUFFERS → VERTEX_ELEMENTS → 3DPRIMITIVE → PIPE_CONTROL → MI_FLUSH
- GPU diagnostics: INSTDONE/IPEIR/IPEHR/EIR dump, PIPE_CONTROL marker

**Critical bugs fixed:**
- `CMD_STATE_BASE_ADDRESS` was `0x69000006` (DRAWING_RECTANGLE) instead of
  `0x61010006` — the address bases were never being set
- `CMD_PIPELINE_SELECT` had `(0x1<<29)` setting type=001 instead of
  type=000 (MI) — the GPU never entered 3D mode
- All 3D opcodes had the SubOpcode in the Opcode field (wrong bit
  positions)
- SEND `msg_reg_nr` was 0 in the SF and WM kernels (should have been 1)

### 1.9 hooks.cpp — Fill Span enabled

The `B_FILL_SPAN` hook was disabled (`return NULL`). Re-enabled now that
the bug in the `intel_fill_span` loop has been fixed.

### 1.10 hooks.cpp — Overlay enabled for Ironlake

`INTEL_GROUP_ILK` removed from the overlay blacklist. The legacy hardware
overlay (MI_OVERLAY_FLIP) is supported on Gen3-Gen5. The overlay registers
are correctly allocated by the kernel driver.

### 1.11 intel_extreme.h — New definitions

- `MI_STORE_DWORD_INDEX`, `MI_BATCH_BUFFER_START`, `MI_BATCH_BUFFER_END`
- `HWS_SYNC_SEQUENCE_INDEX`
- `DISPLAY_CONTROL_TILED` (bit 10 of DSPCNTR)
- `INTEL_FENCE_BASE_965` (0x03000), `INTEL_FENCE_BASE_GEN6` (0x100000)
- Fence register constants (FENCE_REG_VALID, FENCE_REG_PITCH_SHIFT)

### 1.12 memory.cpp — Allocation with alignment

Added an `intel_allocate_memory(size, alignment, flags, base)` overload
that propagates the alignment parameter to the kernel GART allocator.

### 1.13 accelerant.h — Tiling state

Added `frame_buffer_tiled` and `fence_register_index` fields to the
`accelerant_info` struct (private to the accelerant, NOT in
`intel_shared_info`, to avoid an ABI break with the kernel driver).

### 1.14 media_pipeline.cpp/h — Media Pipeline & EU Kernel Dispatch

Implemented a complete infrastructure for the Gen5 compute engine via the
MEDIA_OBJECT command:
- **gpu_bo allocator** (gpu_bo.cpp/h): GTT buffers with alloc/free/write/clear
- **Batch writer** (batch_writer in gen_ops.h): accumulates DWORD commands
  in a static array, then submits them via the ring
- **10-command preamble**: MI_FLUSH → DEPTH_BUFFER_NULL → PIPELINE_SELECT →
  URB_FENCE → STATE_BASE_ADDRESS → MEDIA_STATE_POINTERS → CS_URB_STATE →
  CONSTANT_BUFFER → N × MEDIA_OBJECT → MI_FLUSH
- **Surface state**: SURFTYPE_BUFFER for OWord R/W, SURFTYPE_2D for Media
  Block
- **CURBE**: up to 30 GRF push-to-thread via CONSTANT_BUFFER
- **Marker system**: MI_STORE_DATA_IMM writes a tag into marker_bo for
  debug/sync
- **Kernel dispatch**: up to 48 parallel EU threads, URB recycling

**EU kernels implemented** (kernels/):
- `idct_single.g4a` — standalone IDCT S16→S16 (109 instructions)
- `idct_to_u8.g4a` — IDCT + clamp + Media Block Write U8
- `iq_idct_intra.g4a` — combined IQ + IDCT for I-frames
- `mc_forward.g4a` — P-frame forward MC (Media Block Read/Write)

**Benchmark**: GPU IDCT 4× faster than CPU on 400 8×8 blocks.

### 1.15 gpu_ring.cpp/h — Ring Submission via Kernel Ioctl

Generation-independent ring submission layer. Handles:
- Device open, shared_info and register cloning
- Sync with hardware TAIL (never RING_RESET — kills the CS)
- `gpu_ring_begin/emit/advance`: command writes + TAIL kick via the
  `INTEL_RING_WRITE_TAIL` ioctl (the only way to write MMIO from
  userspace)
- `gpu_ring_submit`: submits a pre-built command array
- `gpu_ring_wait_idle`: waits for HEAD to reach TAIL

Used by media_pipeline.cpp, standalone tests, and gpu_idct in the plugin.

### 1.16 gen_ops.h, gen5/6/7_ops.cpp — Multi-Generation Abstraction

`gen_ops` vtable with function pointers for generation-specific command
emission:
- `emit_pipeline_select_media/3d`, `emit_state_base_address`,
  `emit_urb_fence`, `emit_mi_flush`, `emit_constant_buffer`,
  `emit_media_state_pointers`, `emit_cs_urb_state`, markers
- `init_gen_ops(ops, generation)` selects automatically
- **Gen5** (Ironlake): tested, STATE_BASE_ADDRESS 8DW, MI_FLUSH
- **Gen6** (Sandy Bridge): untested, STATE_BASE_ADDRESS 10DW, PIPE_CONTROL
- **Gen7** (Ivy Bridge/Haswell): untested

Contributor guide: `PORTING.md`.

### 1.17 gpu_debug.cpp/h — GPU Diagnostics

- Register dumps: INSTDONE, IPEHR, ACTHD, EIR, ESR
- `gpu_debug_wait_value()`: busy-wait on a GTT address for marker sync
- Ring health check: HEAD/TAIL comparison, stall detection

### 1.18 DRM Shim for Mesa crocus

`libdrm_shim.so` library that translates Linux DRM ioctls into
intel_extreme Haiku ioctls:
- **GEM**: CREATE/CLOSE/MMAP/BUSY/WAIT, SET_DOMAIN, MADVISE
- **GEM_EXECBUFFER2**: inline batch into the ring + MI_STORE_DATA_IMM
  marker + TAIL ioctl. Relocation patching, EXEC_HANDLE_LUT,
  EXEC_BATCH_FIRST.
- **GETPARAM/GET_APERTURE**: chipset_id=0x0046, aperture size
- **SET_TILING/GET_TILING**: tiling mode tracking
- **CONTEXT**: CREATE/DESTROY stub, GETPARAM/SETPARAM

Mesa 25.3.3 built with the crocus (Gallium) driver, CrocusRenderer addon
for the Haiku GL stack. OpenGL 2.1, GLSL 1.20.

### GPU Performance vs. Original Driver

| Test | Original | Patched | Difference |
|------|-----------|----------|------------|
| FillRect | 57,294/s | 67,316/s | **+18%** |
| CopyBits | 9,614/s | 9,648/s | = |
| InvertRect | 56,715/s | 61,943/s | **+9%** |
| FullClear | 70,827/s | 71,412/s | +1% |
| SyncLatency | 22.4 us | 19.2 us | **+14%** |
| SmallRect 16x16 | 52,093/s | 48,902/s | -6% |
| LargeRect | 176 MB/s | 162 MB/s | -8% |

---

## 2. Audio — HDA kernel driver

The original driver is located at:
`/boot/system/add-ons/kernel/drivers/bin/hda` (from the haiku package)

Our patched driver is installed via HPKG:
`/boot/system/packages/hda_patched-1.0-1-x86_64.hpkg`

With a `BlockedEntries` entry in `/boot/system/settings/packages` to
override the system driver.

### 2.1 hda_codec.cpp — Sony Vaio ALC269 Quirk (VREF fix)

**Problem:** The global `HDA_QUIRK_IVREF` quirk sets VREF_80 on all pins
with VREF capability. The ALC269's pin 0x19 is logically disabled but
electrically connected to the audio path. VREF_80 causes crosstalk/
distortion on the internal speakers.

**Fix:** Added a Sony-specific quirk (vendor 0x104d) for the ALC269:
```cpp
{ SONY_VENDORID, HDA_ALL, REALTEK_VENDORID, 0x0269,
    HDA_QUIRK_SONY_VAIO, HDA_QUIRK_IVREF },
```
After the tree build, it sends `SET_PIN_WIDGET_CONTROL(0x19, VREF_GRD)`
to set the pin to VREF Ground. Equivalent to the Linux
`ALC269_FIXUP_SONY_VAIO` fix in `patch_realtek.c`.

### 2.2 hda_codec.cpp — Widget type override for ALC269 input selectors

**Problem:** Widgets NID 0x23 and 0x24 (input selectors for the ADC) are
reported by the hardware as "Vendor Defined" (type 15). The
`hda_widget_find_input_path()` function ignores them in the
`default: return false` branch, causing `build input tree failed` and a
non-functional microphone.

**Fix:** Added a case in the `switch(codec_id)` for the ALC269:
```cpp
case 0x10ec0269:
    if (nodeID == 0x23 || nodeID == 0x24)
        widget.type = WT_AUDIO_SELECTOR;
    break;
```

### HDA Result

- Quirks: 0x4000 (HDA_QUIRK_SONY_VAIO, IVREF removed)
- Internal mic (NID 18) and external mic (NID 24) detected
- Headphone/speaker automute already present in the base driver
- Audio working without distortion

---

## 3. MPEG-2 Decoder Plugin (media_kit)

`mpeg2_decoder.so` plugin for Haiku's media_kit. Decodes MPEG-2 video
with optional GPU acceleration.

Installed at: `~/config/non-packaged/add-ons/media/plugins/`

### 3.1 mpeg2_decoder_plugin.cpp — DecoderPlugin Plugin

- Registers the `B_MPEG_2_VIDEO` format via BMediaFormats
- `B_YCbCr420` output (separate Y + Cb + Cr planes)
- Decodes I, P, and B frames with half-pel motion compensation
- **Batch IDCT**: accumulates 8×8 blocks, flushes via GPU (CPU fallback)
- Reference frame management: I/P → forward ref, shift to backward

### 3.2 gpu_idct.cpp/h — GPU IDCT for the Plugin

Standalone reimplementation of the media pipeline for use by the plugin:
- Opens `/dev/graphics/intel_extreme_000200`, clones shared_info and
  registers
- Allocates GTT buffers: kernel, CURBE, input (S16 coeff), output
  (S16 IDCT), batch (commands), VFE state, IDRT, surface state, binding
  table
- GPU batch: 10-command preamble + N MEDIA_OBJECT, submitted via
  MI_BATCH_BUFFER_START into the ring + TAIL ioctl
- `idct_single.g4b.gen5` EU kernel: 2-pass dp4 IDCT, S16 output
- Automatic CPU fallback if the GPU is unavailable
- Thread-safe: uses the ring buffer's benaphore (shared with app_server)

### 3.3 mpeg2_parser.cpp/h — MPEG-2 Bitstream Parser

Complete ISO/IEC 13818-2 parser:
- Sequence header, picture header, picture coding extension, slice header
- DC VLC (Table B-12/B-13), AC VLC (full Table B-14, 112 entries)
- Table B-15 (intra_vlc_format=1), Table B-9 CBP (64 entries)
- Macroblock decode: I, P, B frame (Table B-2/B-3/B-4)
- Motion vector decode (Table B-10 + f_code expansion)
- Inline IQ, mismatch control, error recovery (skip+continue)

### 3.4 mpeg2_viewer — Standalone Viewer

BWindow viewer for .m2v files. Decodes I+P frames, YCbCr→RGB32 (BT.601).

---

## 4. Added Files (not present in the original)

| File | Description |
|------|-------------|
| **Accelerant — Render Engine** | |
| `intel_extreme/accelerant/render.h` | 3D-for-2D render engine header |
| `intel_extreme/accelerant/render.cpp` | Render engine infrastructure |
| **Accelerant — Media Pipeline & Compute** | |
| `intel_extreme/accelerant/media_pipeline.cpp/h` | Media pipeline: EU dispatch, IDCT, compute |
| `intel_extreme/accelerant/gpu_bo.cpp/h` | GPU buffer object allocator (GTT) |
| `intel_extreme/accelerant/gpu_ring.cpp/h` | Ring submission via kernel ioctl |
| `intel_extreme/accelerant/gpu_debug.cpp/h` | GPU diagnostics (registers, markers) |
| `intel_extreme/accelerant/gen_ops.h` | Multi-generation vtable |
| `intel_extreme/accelerant/gen5_ops.cpp` | Gen5 implementation (tested) |
| `intel_extreme/accelerant/gen6_ops.cpp` | Gen6 implementation (untested) |
| `intel_extreme/accelerant/gen7_ops.cpp` | Gen7 implementation (untested) |
| `intel_extreme/accelerant/idct_ref.h` | IDCT CPU reference + GPU cosine table |
| `intel_extreme/accelerant/iq_intra_ref.h` | IQ CPU reference for MPEG-2 |
| `intel_extreme/accelerant/mpeg2_parser.cpp/h` | MPEG-2 bitstream parser |
| `intel_extreme/accelerant/commands.h` | HW command definitions (MI, 3D, MEDIA) |
| **Accelerant — EU Kernels** | |
| `intel_extreme/accelerant/kernels/idct_single.g4a` | Standalone IDCT S16→S16 |
| `intel_extreme/accelerant/kernels/idct_to_u8.g4a` | IDCT + clamp → U8 |
| `intel_extreme/accelerant/kernels/iq_idct_intra.g4a` | Combined IQ + IDCT |
| `intel_extreme/accelerant/kernels/mc_forward.g4a` | Forward MC (half-pel) |
| **MPEG-2 Plugin** | |
| `intel_extreme/mpeg2_plugin/mpeg2_decoder_plugin.cpp` | I+P+B media_kit plugin |
| `intel_extreme/mpeg2_plugin/gpu_idct.cpp/h` | Standalone GPU IDCT for the plugin |
| `intel_extreme/mpeg2_plugin/mpeg2_viewer.cpp` | Standalone .m2v viewer |
| **Test & Tools** | |
| `intel_extreme/accelerant/tests/gpu_idct_bench.cpp` | GPU vs CPU IDCT benchmark |
| `intel_extreme/accelerant/tests/gpu_plasma_demo.cpp` | Animated plasma via GPU IDCT |
| `intel_extreme/accelerant/tests/gpu_triangle.cpp` | 3D TRILIST + BLT demo |
| `intel_extreme/accelerant/tests/test_mc_decode.cpp` | Multi-frame I+P decoder |
| `intel_extreme/tools/gen4asm/` | Port of the gen4asm EU assembler |
| `tools/ring_health.sh` | GPU ring buffer diagnostics |
| `tools/test_suite.sh` | Test suite & regression runner |
| **DRM Shim / Mesa** | |
| `intel_extreme/accelerant/libdrm_shim.cpp` | DRM ioctl shim for Mesa crocus |
| **Documentation** | |
| `analysis/DRIVER_TECHNICAL_ANALYSIS.md` | Comparative analysis of Haiku vs Linux |
| `analysis/RENDER_ENGINE_2D_ANALYSIS.md` | Render engine via shader plan |
| `analysis/2D_ACCELERATION_PLAN.md` | BLT optimization plan |
| `analysis/X_TILING_NOTE.md` | X-Tiling implementation notes |
| `analysis/X_TILING_BUG_ANALYSIS.md` | X-Tiling bug analysis (ABI break) |
| `analysis/HDA_ALC269_ANALYSIS.md` | ALC269 audio codec analysis |
| `bench/bench_2d.cpp` | 2D benchmark (7 tests) |
| `hardware/VPCEB3K1E_Haiku_Driver_Report.md` | Hardware compatibility report |
| `reports/REPORT_RENDER_ENGINE.md` | Gen5 3D render engine report |
| `TODO_INTEL_GPU_HAIKU.md` | Master roadmap with milestone tracking |
| `PORTING.md` | Guide: adding support for a new GPU generation |
| `DRIVER_DIFFERENCES.md` | This document |
| `INDEX.md` | Master documentation index |

---

## 5. X-Tiling Work (not completed)

X-Tiling was attempted and documented but not completed:
- Fence register at 0x03000 works (read/write verified)
- GART allocation with 8MB alignment works
- The corruption is caused by a race condition: `program_pipe_color_modes()`
  clears the DSPCNTR tiled bit, creating a window in which the display
  engine reads linearly from tiled data
- The fix requires integrating the tiled bit into
  `program_pipe_color_modes`, based on `gInfo->frame_buffer_tiled`

---

## 6. How to Restore the Original Drivers

### GPU
```bash
rm /boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant
# The system will load the driver from the haiku package
```

### HDA
```bash
rm /boot/system/packages/hda_patched-1.0-1-x86_64.hpkg
# Remove the hda lines from /boot/system/settings/packages
# The system will load the driver from the haiku package
```
