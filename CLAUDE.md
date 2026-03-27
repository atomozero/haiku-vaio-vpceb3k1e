# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Patched Haiku OS drivers for the Sony Vaio VPCEB3K1E laptop (Intel Ironlake/Arrandale, Gen5). Contains modified versions of the `intel_extreme` GPU accelerant and `hda` audio kernel driver, forked from Haiku R1~beta5 (hrev59506), plus a 2D benchmark tool.

Target hardware: Intel HD Graphics device 0x0046 (Ironlake Mobile), PCH Ibex Peak (IBX), 15.5" LVDS 1366x768, Realtek ALC269 audio codec.

## Build Commands

### GPU Accelerant (intel_extreme)
```sh
cd intel_extreme/accelerant
make            # builds ../intel_extreme.accelerant
make clean      # removes all .o files and the accelerant binary
make install    # copies to /boot/system/non-packaged/add-ons/accelerants/
```
Requires reboot after install. The accelerant is a shared library loaded by app_server.

### HDA Audio Driver
```sh
cd hda
make            # builds kernel driver binary
make clean
make install    # copies to /boot/system/non-packaged/add-ons/kernel/drivers/
```
This is a kernel driver (`-D_KERNEL_MODE`, `-nostdlib`, no exceptions/RTTI). Requires reboot after install.

### 2D Benchmark
```sh
cd bench
g++ -o bench_2d bench_2d.cpp -lbe
./bench_2d
```

## Architecture

### intel_extreme/accelerant/ — GPU accelerant (userspace, loaded by app_server)
- **Ports.cpp** — Display port implementations (LVDS, HDMI, DP, etc.). Most Ironlake fixes live here: EDID fallback chain, BIOS channel preservation, panel fitter bypass at native resolution.
- **Pipes.cpp** — Display pipe configuration including watermarks. IBX watermark fix is here.
- **mode.cpp** — Mode setting, display mode creation/validation.
- **engine.cpp** — Ring buffer command submission, 2D BLT engine, HWS sync.
- **render.cpp / render.h** — 2D render engine acceleration (Gen5 BLT commands).
- **commands.h** — Hardware command definitions (MI_*, XY_* opcodes for ring buffer).
- **pll.cpp** — PLL (clock) configuration for different Intel generations.
- **PanelFitter.cpp** — Panel fitter control for scaling.
- **accelerant.h / intel_extreme.h** — Shared info structures between kernel driver and accelerant.

### intel_extreme/common_src/ — Shared graphics library
Display mode computation, EDID parsing, DDC/I2C, DP aux channel. Built into `libaccelerantscommon.a` and linked into the accelerant.

### intel_extreme/common_headers/ — Headers shared with kernel driver
Register definitions, EDID structures, display timing types.

### hda/ — HDA audio kernel driver
- **hda_codec.cpp** — Codec initialization, verb sending, widget parsing. Contains Sony VAIO ALC269 quirks (VREF_GRD, widget type overrides).
- **hda_controller.cpp** — HDA controller setup, DMA, interrupt handling.
- **driver.cpp / device.cpp** — Haiku kernel driver entry points.

### gpu_pkg/ — HPKG packaging structure for the patched accelerant.

## Key Patterns

- The accelerant communicates with the kernel driver through a `shared_info` structure mapped into both address spaces. Register access from the accelerant goes through memory-mapped I/O via `shared_info->registers`.
- Intel GPU generations are checked via `gInfo->shared_info->device_type` which provides generation checks like `.InGroup(INTEL_GROUP_ILK)` or `.Generation()`.
- PCH (Platform Controller Hub) type is in `gInfo->shared_info->pch_info` — key values: `INTEL_PCH_IBX` (Ironlake), `INTEL_PCH_CPT` (Sandy Bridge).
- Trace output uses `_sPrintf()` (kernel debug output). Enable per-file with `#define TRACE_MODE` before the TRACE macro definition.
- Haiku coding style: BSD-style braces, tabs for indentation, CamelCase for classes, camelCase for methods.

## Documentation

Technical analysis docs are in Italian. Key files:
- `DIFFERENZE_DRIVER.md` — Complete list of all patches vs stock Haiku
- `VPCEB3K1E_Haiku_Driver_Report.md` — Full hardware compatibility report
- `intel_extreme/ANALISI_TECNICA_DRIVER.md` — Technical driver architecture analysis
- `intel_extreme/ANALISI_RENDER_ENGINE_2D.md` — Gen5 2D render engine documentation
