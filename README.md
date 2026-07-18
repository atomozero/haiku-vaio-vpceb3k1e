# Haiku on a 2010 Sony Vaio — Intel Ironlake (Gen5) GPU & audio drivers

Patched [Haiku OS](https://www.haiku-os.org/) drivers and GPU experiments for the
**Sony Vaio VPCEB3K1E** laptop (2010). The goal: take the Intel HD Graphics
(Ironlake, Gen5) from "framebuffer only" to **real hardware acceleration** —
2D BLT, the media/EU compute pipeline, hardware OpenGL via Mesa, MPEG-2 GPU
decode — and then push the compute pipeline as far as it will go, including
running a small LLM's matrix multiplies on the GPU's execution units.

> This is a personal research fork against one specific laptop. It is **not**
> upstream Haiku, and some pieces are experimental. But everything here runs on
> real hardware and the findings are measured, not hand-waved.

## Highlights

- **Hardware OpenGL** on Gen5 via a patched **Mesa 22 crocus** stack + a DRM/GEM
  compatibility shim — GLTeapot and friends render on the GPU, not in software.
- **GPU media/EU pipeline** brought up from scratch: `MEDIA_OBJECT` thread
  dispatch, hand-written Gen5 EU kernels (`.g4a` → `.g4b.gen5`), and an in-tree
  port of the `intel-gen4asm` assembler.
- **MPEG-2 GPU decode**: CPU bitstream parser + GPU IDCT (3.5× vs CPU), 100%
  macroblock coverage on real content, exposed as a `media_kit` decoder plugin.
- **LLM matmuls on a 2010 GPU** (Level 2): a Llama-2 model (`llama2.c`
  `stories15M`) runs its matrix multiplies on the Ironlake EUs end-to-end
  (decode **and** batched prefill), **byte-identical** to the CPU. A complete,
  honest study of where the GPU wins (batched compute) and why it can't beat the
  CPU at token-by-token inference — see
  [`gen5_docs/analysis/LLM_MATMUL_ON_GEN5_RESULTS.md`](gen5_docs/analysis/LLM_MATMUL_ON_GEN5_RESULTS.md).
- **Tools**: a live GPU-utilisation graph (`tools/gpu_monitor`, works as a
  Desktop replicant), a regression test suite, and ring-health diagnostics.
- Patched **HDA audio** driver for the Realtek ALC269.

## Hardware

| Component | Details |
|-----------|---------|
| CPU | Intel Core i3-M370 (Arrandale), 2 cores / 4 threads |
| GPU | Intel HD Graphics, device `0x0046` (Ironlake Mobile, Gen5), ~12 EUs |
| PCH | Intel Ibex Peak (IBX), device `0x3b09` |
| Display | 15.5" LVDS 1366×768 |
| Audio | Intel HDA, Realtek ALC269 codec |
| OS | Haiku R1~beta5 (hrev59506+) |

## Status

| Feature | Status | Notes |
|---------|--------|-------|
| LVDS display | ✅ Working | 1366×768 32-bit, EDID fallback chain |
| 2D BLT engine | ✅ Working | `XY_SRC_COPY_BLT` via kernel ioctl |
| Media/EU pipeline | ✅ Working | `MEDIA_OBJECT` dispatch; IDCT 3.5× vs CPU |
| MPEG-2 I/P/B decode | ✅ Working | CPU parser + GPU IDCT, `media_kit` plugin |
| Hardware OpenGL | ✅ Working | Mesa crocus (default renderer); `HGL_SOFTWARE=1` to opt out |
| GPU ring from userspace | ✅ Working | via kernel `INTEL_RING_WRITE_TAIL` ioctl (MMIO is read-only from userspace) |
| HDA audio (playback) | ✅ Working | ALC269 with custom pin config |
| ALC269 microphone | ⚠️ WIP | capture path not yet complete |
| LLM matmul on GPU | 🧪 Experiment | correct end-to-end; a measured negative on beating CPU wall-clock |

## Repository layout

```
intel_extreme/
  accelerant/          GPU accelerant (userspace, loaded by app_server)
    engine.cpp         Ring submission, 2D BLT, hardware sync
    render.cpp         Gen5 3D render engine (solid fills, triangles)
    media_pipeline.cpp EU kernel dispatch, MPEG-2 GPU decode, GEMV/GEMM
    gpu_ring.* gpu_bo.* gen_ops.*   Generation-independent GPU submission layer
    gen5_ops.cpp / gen6_ops.cpp / gen7_ops.cpp   Per-generation command emission
    mpeg2_parser.cpp   MPEG-2 bitstream parser (sequence/picture/slice/VLC)
    kernels/           Gen5 EU kernels (.g4a source, .g4b.gen5 assembled) + gen_gemv.py
    tests/             Standalone test/benchmark programs (no reboot needed)
  mpeg2_plugin/        media_kit MPEG-2 decoder plugin (GPU IDCT)
  tools/gen4asm/       In-tree port of intel-gen4asm (Gen5 EU assembler)
hda/                   Patched HDA audio kernel driver
tools/                 gpu_monitor, test suite, ring health, GEM/DRM probes
gen5_docs/analysis/    Design notes, phase reports, and result write-ups
```

The `llama2.c` GPU integration (`gpu_llm.cpp`, `forward_prefill`) lives in a
separate tree; the driver side of it is the public `media_pipeline_gemv*` API in
`intel_extreme/accelerant/media_pipeline.cpp`.

## Building

### GPU accelerant

```sh
cd intel_extreme/accelerant
make            # builds ../intel_extreme.accelerant (+ assembles EU kernels)
make install    # copies to /boot/system/non-packaged/add-ons/accelerants/
make test       # build with the boot-time media-pipeline probe
```

Requires a reboot after install (the accelerant is loaded by app_server).

### Standalone GPU tests (no reboot)

```sh
cd intel_extreme/accelerant && make
cd tests
g++ -O2 -I.. <private graphics -I flags> -o gpu_gemv gpu_gemv.cpp \
    ../*.o ../../libaccelerantscommon.a -lbe -lstdc++
./gpu_gemv        # GEMV on the EUs, GPU vs CPU
```

The exact `-I` set is documented in the test source headers and `CLAUDE.md`.

### GPU utilisation monitor

```sh
cd tools
g++ -O2 -o gpu_monitor gpu_monitor.cpp \
    -I/boot/system/develop/headers/private/graphics/intel_extreme \
    -I/boot/system/develop/headers/private/graphics \
    -I/boot/system/develop/headers/private/graphics/common \
    -I/boot/system/develop/headers/private/shared -lbe
./gpu_monitor          # window; drag the graph onto the Desktop as a replicant
./gpu_monitor test     # headless, prints busy %
```

### HDA audio driver

```sh
cd hda
make && make install    # kernel driver, requires reboot
```

## Documentation

- [`gen5_docs/analysis/LLM_MATMUL_ON_GEN5_RESULTS.md`](gen5_docs/analysis/LLM_MATMUL_ON_GEN5_RESULTS.md)
  — the LLM-on-GPU study: what was built, the full results, and the root-cause of
  the CPU/GPU wall-clock verdict.
- [`gen5_docs/INDEX.md`](gen5_docs/INDEX.md) — index of all analysis notes and phase reports.
- [`PORTING.md`](PORTING.md) — how to add support for another Intel GPU generation.
- [`DIFFERENZE_DRIVER.md`](DIFFERENZE_DRIVER.md) — every patch vs stock Haiku R1~beta5.
- [`TODO_INTEL_GPU_HAIKU.md`](TODO_INTEL_GPU_HAIKU.md) — roadmap and milestone tracking.
- [`CLAUDE.md`](CLAUDE.md) — build commands, architecture, and the hard-won Gen5 gotchas.

## Notable hardware findings

- **MMIO registers are read-only from userspace** here. All GPU command
  submission goes through kernel ioctls (`INTEL_RING_WRITE_TAIL`); userspace only
  writes command *data* into ring memory.
- **Never reset the ring after boot** — disable+re-enable permanently kills the
  command streamer. Sync with the hardware TAIL and append instead.
- **`MI_LOAD_REGISTER_IMM` hangs the CS on Gen5** in the ring; use the kernel's
  direct-MMIO workaround path instead.
- **`{compr}` UB→UW widening is broken** on the Gen5 EU — use explicit SIMD8 pairs.
- **The GPU is a batch machine**: it wins big on heavily-batched compute (video
  IDCT, large one-shot GEMV/GEMM) but the per-thread `MEDIA_OBJECT` dispatch
  model (no thread-grid `WALKER` until Gen6) caps light, latency-bound workloads
  like token-by-token LLM decode below the CPU.

## License

MIT. See [`LICENSE`](LICENSE). Based on Haiku R1~beta5 (hrev59506) driver
sources, © 2007–2024 Haiku, Inc. The `intel-gen4asm` port and Mesa references
retain their upstream licenses.
