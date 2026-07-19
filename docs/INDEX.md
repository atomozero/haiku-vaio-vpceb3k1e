# Documentation Index

| | |
|---|---|
| **Status** | 🚧 Living document |
| **Category** | Index |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Project README](../README.md)

---

All project documentation lives under `docs/`. Each document carries a metadata
block (status, category, target hardware, last update) right below its title.

**Status legend:** ✅ Complete · ⚠️ WIP · 🧪 Experiment · 📋 Plan · 📄 Reference ·
🚧 Living document · ❌ Abandoned approach

---

## Guides & project-wide docs

| Document | Status | Summary |
|---|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 📄 Reference | Layered design of the Gen5–Gen8 GPU driver stack for Haiku. |
| [PORTING.md](PORTING.md) | 📄 Reference | How to add support for a new Intel GPU generation. |
| [TODO_INTEL_GPU_HAIKU.md](TODO_INTEL_GPU_HAIKU.md) | 🚧 Living | Master roadmap and milestone tracker. |
| [DRIVER_DIFFERENCES.md](DRIVER_DIFFERENCES.md) | 📄 Reference | Every patch applied to `intel_extreme` and `hda` vs stock Haiku R1~beta5. |

## Hardware (`hardware/`)

| Document | Status | Summary |
|---|---|---|
| [HARDWARE_WIKI.md](hardware/HARDWARE_WIKI.md) | 📄 Reference | Empirical Gen5–Gen8 hardware knowledge: bugs, workarounds, register quirks. |
| [HARDWARE_ACCELERATION_DEMO.md](hardware/HARDWARE_ACCELERATION_DEMO.md) | ✅ Complete | Walkthrough of the working GPU-acceleration demos. |
| [VPCEB3K1E_Haiku_Driver_Report.md](hardware/VPCEB3K1E_Haiku_Driver_Report.md) | ✅ Complete | Haiku driver compatibility report for the VPCEB3K1E. |

## Reports (`reports/`)

| Document | Status | Summary |
|---|---|---|
| [REPORT_RENDER_ENGINE.md](reports/REPORT_RENDER_ENGINE.md) | ⚠️ WIP | Gen5 (Ironlake) 3D render-engine bring-up report. |

## Analysis & design (`analysis/`)

### Strategy & feasibility

| Document | Status | Summary |
|---|---|---|
| [VIDEO_DECODE_PIVOT.md](analysis/VIDEO_DECODE_PIVOT.md) | 📄 Reference | Strategic pivot: video decode as the primary goal, LLM as phase 2. |
| [LLM_FEASIBILITY.md](analysis/LLM_FEASIBILITY.md) | 📄 Reference | Can a small LLM run on the Ironlake GPU, and through which pipeline? |
| [LLM_MATMUL_ON_GEN5_RESULTS.md](analysis/LLM_MATMUL_ON_GEN5_RESULTS.md) | 🧪 Experiment | Running `stories15M` matmuls on the EUs end-to-end — results and honest limits. |
| [REFERENCE_PROJECTS.md](analysis/REFERENCE_PROJECTS.md) | 📄 Reference | External reference projects and clean-room license analysis. |
| [GEN5_REFERENCES.md](analysis/GEN5_REFERENCES.md) | 📄 Reference | Catalog of Intel PRMs, Mesa/i915/libva references, and key Ironlake numbers. |

### Media pipeline & EU compute

| Document | Status | Summary |
|---|---|---|
| [MEDIA_PIPELINE_BRINGUP.md](analysis/MEDIA_PIPELINE_BRINGUP.md) | 📄 Reference | Concrete 10-command media-pipeline specification, DWORD-level. |
| [GEN4ASM_PORT.md](analysis/GEN4ASM_PORT.md) | ✅ Complete | Feasibility and plan for porting `intel-gen4asm` into the tree. |
| [GPU_IDCT_DESIGN.md](analysis/GPU_IDCT_DESIGN.md) | 📄 Reference | Design of the standalone GPU IDCT used by the MPEG-2 plugin. |

### Phase reports

| Document | Status | Summary |
|---|---|---|
| [PHASE_I_A_REPORT.md](analysis/PHASE_I_A_REPORT.md) | ✅ Complete | gen4asm ported to Haiku; all 15 libva MPEG-2 Gen5 kernels byte-identical. |
| [PHASE_I_B_TEST_PROTOCOL.md](analysis/PHASE_I_B_TEST_PROTOCOL.md) | 📄 Reference | Hardware test protocol for the media-pipeline hello-world probe. |
| [PHASE_I_B_REPORT.md](analysis/PHASE_I_B_REPORT.md) | ✅ Complete | First on-GPU run of an in-tree kernel via the Gen5 media pipeline. |
| [PHASE_1_2_REPORT.md](analysis/PHASE_1_2_REPORT.md) | ✅ Complete | Parallel dispatch of 48 hardware threads; URB_FENCE scaling fix. |
| [PHASE_1_3_REPORT.md](analysis/PHASE_1_3_REPORT.md) | ✅ Complete | Per-thread memory write via surface state + binding table. |
| [PHASE_2_1B_REPORT.md](analysis/PHASE_2_1B_REPORT.md) | ✅ Complete | SAXPY scaling + throughput benchmark; SURFTYPE_BUFFER requirement found. |
| [PHASE_2_2_REPORT.md](analysis/PHASE_2_2_REPORT.md) | ✅ Complete | Sampler path via Media Block Read on a SURFTYPE_2D surface. |

### 2D & render engine

| Document | Status | Summary |
|---|---|---|
| [DRIVER_TECHNICAL_ANALYSIS.md](analysis/DRIVER_TECHNICAL_ANALYSIS.md) | 📄 Reference | Technical comparison: `intel_extreme` (Haiku) vs i915 (Linux). |
| [RENDER_ENGINE_2D_ANALYSIS.md](analysis/RENDER_ENGINE_2D_ANALYSIS.md) | 📋 Plan | Render-engine analysis for shader-based 2D acceleration. |
| [2D_ACCELERATION_PLAN.md](analysis/2D_ACCELERATION_PLAN.md) | 📋 Plan | Plan for 2D BLT acceleration on Ironlake. |
| [CROCUS_DRM_IOCTL_ANALYSIS.md](analysis/CROCUS_DRM_IOCTL_ANALYSIS.md) | 📄 Reference | Complete i915 DRM ioctl analysis for the Crocus Gallium driver. |
| [XTILING_KERNEL_PLAN.md](analysis/XTILING_KERNEL_PLAN.md) | 📋 Plan | Plan to implement X-tiling via the kernel driver. |
| [X_TILING_BUG_ANALYSIS.md](analysis/X_TILING_BUG_ANALYSIS.md) | 📄 Reference | Root-cause analysis of the X-tiling display-corruption bug. |
| [X_TILING_NOTE.md](analysis/X_TILING_NOTE.md) | ❌ Abandoned | Notes on the failed accelerant-only X-tiling attempt. |

### Audio

| Document | Status | Summary |
|---|---|---|
| [HDA_ALC269_ANALYSIS.md](analysis/HDA_ALC269_ANALYSIS.md) | 📄 Reference | HDA driver analysis for the Realtek ALC269 codec. |
