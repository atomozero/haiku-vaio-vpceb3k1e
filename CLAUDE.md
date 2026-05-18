# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Patched Haiku OS drivers for the Sony Vaio VPCEB3K1E laptop (Intel Ironlake/Arrandale, Gen5). Contains modified versions of the `intel_extreme` GPU accelerant and `hda` audio kernel driver, forked from Haiku R1~beta5 (hrev59506).

Target hardware: Intel HD Graphics device 0x0046 (Ironlake Mobile), PCH Ibex Peak (IBX), 15.5" LVDS 1366x768, Realtek ALC269 audio codec.

## Build Commands

### GPU Accelerant (intel_extreme)
```sh
cd intel_extreme/accelerant
make            # builds ../intel_extreme.accelerant (+ assembles EU kernels)
make clean      # removes .o files, accelerant binary, and .g4b.gen5 kernel binaries
make install    # copies to /boot/system/non-packaged/add-ons/accelerants/
make test       # builds with -DMEDIA_PIPELINE_HELLO_TEST (runs GPU probe at boot)
make test-install  # installs test build with safety backup
make revert-test   # restores pre-test accelerant
```
Requires reboot after install. The accelerant is a shared library loaded by app_server.

### EU Kernel Assembly
```sh
cd intel_extreme/accelerant
../tools/gen4asm/gen4asm -g5 -o kernels/foo.g4b.gen5 kernels/foo.g4a
```
The Makefile handles this automatically. Kernel binaries (.g4b.gen5) are C array initializers `#include`d by media_pipeline.cpp.

### HDA Audio Driver
```sh
cd hda
make && make install    # kernel driver, requires reboot
```

## Architecture

### intel_extreme/accelerant/ — GPU accelerant (userspace, loaded by app_server)

#### Generic layer (generation-independent)
- **gpu_ring.cpp / gpu_ring.h** — Ring buffer submission via kernel ioctl. Handles position tracking, TAIL kicks, idle waits. Works on any Intel gen where the kernel provides `INTEL_RING_WRITE_TAIL` ioctl.
- **gen_ops.h** — Generation abstraction: `gen_ops` vtable with function pointers for gen-specific command emission (pipeline select, state base address, URB fence, BLT, markers). `batch_writer` DWORD accumulator.
- **gpu_bo.cpp / gpu_bo.h** — GPU buffer object allocator (GTT-mapped). Generation-independent.
- **gpu_debug.cpp / gpu_debug.h** — GPU register dumps (INSTDONE, IPEHR, ACTHD, EIR). Marker helpers are generic, register decode is Gen5-specific.

#### Gen5 (Ironlake) specific
- **gen5_ops.cpp** — Gen5 implementation of `gen_ops`: command encodings (CMD_GFX macro), ILK state base address (8 DWORDs), URB fence, CS URB state, BLT (XY_SRC_COPY_BLT), MI_STORE_DATA_IMM markers.
- **engine.cpp** — Ring buffer command submission (QueueCommands), 2D BLT engine, HWS sync.
- **render.cpp / render.h** — 3D render engine (Gen5 solid fills via RECTLIST, TRILIST triangles). `render_init_clone()` for userspace access.
- **media_pipeline.cpp / media_pipeline.h** — Gen5 media pipeline: EU kernel dispatch via MEDIA_OBJECT, compute tests, MPEG-2 decode path. Uses `gpu_ring` for ioctl-based submission.
- **idct_ref.h** — CPU reference for IDCT (cosine table + 2-pass algorithm).
- **iq_intra_ref.h** — CPU reference for MPEG-2 inverse quantization.
- **mpeg2_parser.cpp / mpeg2_parser.h** — MPEG-2 bitstream parser (sequence/picture/slice headers, VLC coefficient decode, macroblock decode).
- **kernels/** — EU kernel sources (.g4a) and assembled binaries (.g4b.gen5):
  - **idct_to_u8.g4a** — Production I-frame IDCT: S16→dp4→clamp→U8, Media Block Write.
  - **mc_forward.g4a** — P-frame forward MC: Media Block Read from reference, half-pel bilinear interpolation (4 cases), Media Block Write to output.
  - **iq_idct_intra.g4a** — Combined IQ+IDCT for intra blocks.
  - **idct_single.g4a** — Standalone IDCT outputting S16 (Phase 3.1 test).
- **tests/** — Standalone test programs (no reboot needed): parser test, parse+decode integration, MC decode test, GPU benchmarks.
- **tests/gpu_idct_bench.cpp** — GPU vs CPU IDCT benchmark (400 blocks). GPU 4× faster. No reboot.
- **tests/gpu_plasma_demo.cpp** — Animated plasma via GPU IDCT, BWindow display.
- **tests/gpu_triangle.cpp** — 3D TRILIST triangle test (ring clone diagnostic).
- **tests/test_mc_decode.cpp** — Multi-frame I+P decoder with half-pel MC. Outputs PPM frames.
- **Ports.cpp** — Display port implementations. Ironlake LVDS fixes.
- **Pipes.cpp** — Display pipe configuration. IBX watermark fix.
- **commands.h** — Hardware command definitions (MI_*, 3DSTATE_*, MEDIA_*).
- **mode.cpp** — Mode setting, display mode creation/validation.

### intel_extreme/mpeg2_plugin/ — MPEG-2 media_kit decoder plugin
- **mpeg2_decoder_plugin.cpp** — Haiku DecoderPlugin for MPEG-2 video. Decodes I and P frames with CPU-side IDCT and half-pel motion compensation. Outputs B_YCbCr420 planar. Reference frame management for P-frame chains.
- **mpeg2_viewer.cpp** — Standalone BWindow-based MPEG-2 viewer. Decodes I+P frames from .m2v files and displays them as RGB32 via BT.601 YCbCr→RGB conversion.
- **Makefile** — Builds mpeg2_decoder.so plugin and mpeg2_viewer app.

### Key Patterns

- The accelerant communicates with the kernel driver through a `shared_info` structure mapped into both address spaces. Register access goes through `shared_info->registers`.
- Intel GPU generations are checked via `gInfo->shared_info->device_type` (`.InGroup(INTEL_GROUP_ILK)`, `.Generation()`).
- PCH type: `gInfo->shared_info->pch_info` — `INTEL_PCH_IBX` for Ironlake.
- Trace output uses `_sPrintf()`. Enable per-file with `#define TRACE_MODE`.
- Haiku coding style: BSD-style braces, tabs for indentation, CamelCase for classes.

## Gen5 EU Kernel Development (CRITICAL)

### gen4asm subregister addressing (without -a flag)

**Subregister `.N` is in BYTES, not element units.** This is the #1 source of bugs.

```
g90.0<1>UW   → byte 0  = UW element 0   ✓
g90.8<1>UW   → byte 8  = UW element 4   (NOT element 8!)
g90.16<1>UW  → byte 16 = UW element 8   ✓ (second half of GRF)

g80.0<1>D    → byte 0  = D element 0    ✓
g80.8<1>D    → byte 8  = D element 2    ✓
g80.16<1>D   → byte 16 = D element 4    ✓
g80.24<1>D   → byte 24 = D element 6    ✓
```

To address the second half of a 32-byte GRF:
- For UW (2 bytes): use `.16` (byte 16 = element 8)
- For D (4 bytes): use `.8` (byte 8 = element 2)

### {compr} widening UB→UW is broken

`mov (16) g70<1>UW g1.0<16,16,1>UB {compr}` writes ONLY bytes 0-15 of g70. Bytes 16-31 remain **stale** (uninitialized data from previous GPU threads).

**Fix:** Use explicit SIMD8 pairs:
```asm
mov (8) g70.0<1>UW  g1.0<8,8,1>UB    { align1 };   // bytes 0-15
mov (8) g70.16<1>UW g1.8<8,8,1>UB    { align1 };   // bytes 16-31
```

### {compr} on D destination DOES increment register

`mul (16) g80<1>D ... {compr}` writes 8 D to g80 (first half) + 8 D to g81 (second half). This is correct — D destination fills the whole GRF (32 bytes), so {compr} increments.

### MEDIA_OBJECT inline data position

With `const_urb_entry_read_len = N`, inline data from MEDIA_OBJECT lands at `g(1 + N)`, not g1. CURBE occupies g1..gN.

### OWord Block Read/Write requires SURFTYPE_BUFFER

Using SURFTYPE_2D for OWord Block operations causes silent OOB drops. Always use SURFTYPE_BUFFER (type 4).

### MEDIA_BLOCK_READ encoding

Gen5 uses `msg_type=2` (2-bit gen4asm encoding), despite the PRM DevILK table showing it as 3-bit value 4.

### Send message payload

On Gen4/5, the send instruction reads the **header from the GRF source register** and the **payload from MRF** (m1, m2, ...). The GRF source is NOT used for payload data.

### MPEG-2 DC level shift is in dc_pred, NOT added after IDCT

The DC predictor init value `2^(7 + intra_dc_precision)` = 128 for dc_prec=0 already embeds the +128 level shift. After IQ → IDCT, clamp to [0,255] WITHOUT adding +128. The kernel for GPU decode should NOT have the +128 add — the dc_pred handles it.

### a0.0 dual-half addressing for {compr} dp4

The address register a0.0 has two 16-bit halves: low (channels 0-7) and high (channels 8-15). For processing 2 rows of an 8×8 block simultaneously, set the high half 16 bytes (0x10) ahead of the low half, then increment both by 0x20 (32 bytes = 1 GRF) per iteration:
```asm
mov (1) a0.0<1>UD 0x03300320UD   // low=g26.0, high=g26.16 (minus 0x20 pre-decrement)
```

### MPEG-2 Table B-14 AC VLC codes

The VLC code-to-(run,level) mapping in Table B-14 is NOT sequential. Codes are assigned by statistical frequency. The correct mapping must be derived from the ISO/IEC 13818-2 standard or from ffmpeg's `ff_mpeg1_vlc_table` in `libavcodec/mpeg12data.c`. The 12-bit codes in particular have a non-obvious order that differs completely from a linear scan-index mapping.

### MPEG-2 quantiser_scale

Must be computed from `q_scale_code` (from slice header or MB quant override) using `mpeg2_compute_quantiser_scale()`. For linear type: `q_scale = 2 * q_scale_code`. The scale must be updated per-slice and per-MB (when MB has quant flag).

### MPEG-2 error recovery: do NOT resync_to_next_slice on MB failure

When `mpeg2_decode_intra_macroblock()` fails, **skip the MB and continue** (`mbx++; continue;`). Do NOT call `mpeg2_resync_to_next_slice()` — that loses all remaining MBs in the current slice. The internal resync within `decode_intra_macroblock` is sufficient. This fix raised coverage from 76-82% to 100% on real content.

### MPEG-2 Coded Block Pattern (CBP) — Table B-9 is critical for P-frames

The CBP VLC table (Table B-9) has 64 entries mapping VLC codes (3-9 bits) to 6-bit block patterns. An incomplete CBP decoder causes **silent corruption** in P-frame inter MBs: the parser either doesn't consume bits (wrong bit position) or decodes wrong blocks. Always use the full 64-entry table from ISO 13818-2 / ffmpeg `ff_mpeg12_mbPatTable`.

### MPEG-2 EOB after full 64-coefficient block (CRITICAL BUG FIX)

When all 64 coefficients of a DCT block are coded (idx reaches 63 after the last run+level), the bitstream STILL contains an EOB marker (`10`, 2 bits). The parser MUST consume this EOB even though the block is "full". Failing to consume it causes a **2-bit drift per full block**, which accumulates across MBs and eventually corrupts the entire slice. This is the #1 cause of "75% coverage" failures on high-detail content. Fix: after the AC decode loop, if `idx >= 64`, read and discard one more VLC (the EOB).

## Userspace GPU Access (Ring Clone)

Standalone programs can access the GPU by cloning the accelerant's shared_info:

```cpp
int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data));
clone_area("shared", &gInfo->shared_info, ..., data.shared_info_area);
clone_area("regs", &gInfo->registers, ..., shared_info->registers_area);
```

### CRITICAL: Ring buffer rules for clones

1. **NEVER reset the ring** from userspace. `render_init_clone()` syncs `sw_pos` with hardware TAIL — does NOT disable/re-enable the ring. Resetting kills the CS permanently.
2. **NEVER RING_RESET after boot** — the kernel's `setup_ring_buffer` initializes the ring at boot. Calling `INTEL_RING_RESET` ioctl (disable→re-enable) kills the CS permanently. It works exactly ONCE after boot, then all subsequent resets fail (HEAD stuck at 0). The CS cannot be restarted after disable.
3. **`ring.base` is valid** in clones — it equals `graphics_memory` (shared mapping).
4. **`QueueCommands` works** from clones when the CS is alive.
5. **Benchmark tool**: `tests/gpu_idct_bench` — runs 400 IDCT blocks, GPU 4× faster than CPU. No reboot needed on a clean ring.

### CRITICAL: MI_LOAD_REGISTER_IMM (LRI) does NOT work on Gen5

LRI (opcode 0x22) in the ring hangs the CS after 2 DWORDs on Ironlake. Masked registers (MI_MODE, _3D_CHICKEN2, CACHE_MODE_0) get corrupted by raw LRI writes. The CS reads the LRI header + first register address, then stops permanently.

**Use INTEL_RING_INIT_3D kernel ioctl instead** — the kernel writes workaround registers via direct MMIO, which handles masked register semantics correctly.

### CRITICAL: MMIO writes are READ-ONLY from userspace

All MMIO register writes via `clone_area` are silently ignored (kernel maps with `B_KERNEL_WRITE_AREA` only, no `B_WRITE_AREA`). Verified with `/dev/misc/poke` BAR0 mapping too. Only the kernel driver can write MMIO registers.

**Kernel ioctls for ring access** (added to intel_extreme kernel driver):
- `INTEL_RING_RESET` — DO NOT USE (kills CS, see below)
- `INTEL_RING_WRITE_TAIL` — writes TAIL register, kicks GPU. This is the ONLY way to submit ring commands from userspace.

### CRITICAL: never RING_RESET after boot

Calling `INTEL_RING_RESET` ioctl (disable→re-enable) kills the CS permanently. It works exactly ONCE after boot, then all subsequent resets fail (HEAD stuck at 0). The CS cannot be restarted after disable.

**Correct pattern for ring submission from userspace:**
```cpp
// 1. Sync ring position with hardware TAIL (reads work)
ring.position = read32(ring.register_base + RING_BUFFER_TAIL) & (ring.size - 1);
// 2. Write commands to ring memory (graphics_memory IS writable)
uint32* cmd = (uint32*)(ring.base + ring.position);
cmd[0] = ...; cmd[1] = ...;
// 3. Kick GPU via kernel ioctl
intel_ring_tail data = { INTEL_PRIVATE_DATA_MAGIC, new_position };
ioctl(fd, INTEL_RING_WRITE_TAIL, &data, sizeof(data));
```

### Build for userspace GPU tools

```sh
cd intel_extreme/accelerant && make    # build accelerant .o files
cd tests
g++ -Wall -O2 -I.. [include flags] -o tool tool.cpp ../*.o ../../libaccelerantscommon.a -lbe -lstdc++
```

## Media Pipeline Test Protocol

Tests run synchronously inside `intel_init_accelerant` (app_server is blocked, giving exclusive ring access). Enable with `make test`.

```sh
make test && sudo make test-install   # build + install
# reboot
grep 'PHASE\|intel_extreme media' /boot/system/var/log/syslog
sudo make revert-test                 # rollback if needed
```

Each test phase has its own `media_pipeline_run_*_test()` function called from `accelerant.cpp`. Change which test runs by editing the call site (around line 554).

## Documentation

- `TODO_INTEL_GPU_HAIKU.md` — Master TODO with milestone tracking
- `gen5_docs/analysis/MEDIA_PIPELINE_BRINGUP.md` — 10-command media pipeline specification
- `gen5_docs/analysis/VIDEO_DECODE_PIVOT.md` — Strategic direction (MPEG-2 → H.264 → LLM)
- `gen5_docs/analysis/PHASE_*.md` — Per-phase test reports
- `DIFFERENZE_DRIVER.md` — All patches vs stock Haiku
