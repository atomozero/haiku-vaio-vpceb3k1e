# Haiku Intel Ironlake GPU Driver Patches

Patched Haiku OS drivers for the **Sony Vaio VPCEB3K1E** laptop, targeting hardware-accelerated GPU operations on Intel Ironlake (Gen5).

## Hardware

| Component | Details |
|-----------|---------|
| CPU | Intel Core i3-M370 (Arrandale) |
| GPU | Intel HD Graphics, device 0x0046 (Ironlake Mobile, Gen5) |
| PCH | Intel Ibex Peak (IBX), device 0x3b09 |
| Display | 15.5" LVDS 1366x768 |
| Audio | Intel HDA, Realtek ALC269 codec |
| OS | Haiku R1~beta5 (hrev59506+) |

## What Works

| Feature | Status | Details |
|---------|--------|---------|
| LVDS display | Working | 1366x768 32-bit, EDID fallback chain |
| 2D BLT engine | Working | XY_SRC_COPY_BLT via kernel ioctl, 60 FPS |
| Media pipeline | Working | EU kernel dispatch (MEDIA_OBJECT), IDCT 3.5x GPU speedup |
| MPEG-2 I+P decode | Working | 100% macroblock coverage, CPU parser + GPU IDCT |
| 3D cube demo | Working | CPU rasterizer + BLT to screen, 60 FPS |
| GPU ring from userspace | Working | Kernel ioctl for TAIL write (MMIO is read-only from userspace) |
| HDA audio | Working | Realtek ALC269 with custom pin config |
| OpenGL (Mesa crocus) | In progress | GEM shim, DRM ioctl translation layer |

## Building

### GPU Accelerant

```sh
cd intel_extreme/accelerant
make                # builds ../intel_extreme.accelerant (+ EU kernel assembly)
make install        # copies to /boot/system/non-packaged/add-ons/accelerants/
make test           # builds with GPU probe test (runs at boot)
```

Requires reboot after install. The accelerant is a shared library loaded by app_server.

### Test Tools

After building the accelerant:

```sh
cd intel_extreme/accelerant/tests
# Example: build the 3D cube demo
g++ -Wall -O2 -I.. \
    -I/boot/system/develop/headers/os/add-ons/graphics \
    -I/boot/system/develop/headers/os/drivers \
    -I/boot/system/develop/headers/private/graphics \
    -I/boot/system/develop/headers/private/graphics/intel_extreme \
    -I/boot/system/develop/headers/private/graphics/common \
    -I/boot/system/develop/headers/private/shared \
    -I/boot/system/develop/headers/private/system \
    -I/boot/system/develop/headers/os \
    -I/boot/system/develop/headers/os/support \
    -I/boot/system/develop/headers/os/interface \
    -I/boot/system/develop/headers/os/kernel \
    -I/boot/system/develop/headers/os/storage \
    -I/boot/system/develop/headers/os/app \
    -I/boot/system/develop/headers/posix \
    -o gpu_triangle gpu_triangle.cpp \
    ../*.o ../../libaccelerantscommon.a -lbe -lstdc++
```

### Kernel Driver (for ring ioctl support)

The patched kernel driver adds `INTEL_RING_RESET` and `INTEL_RING_WRITE_TAIL` ioctls.
Build requires the Haiku source tree matching your running kernel revision.

### HDA Audio Driver

```sh
cd hda
make && make install    # kernel driver, requires reboot
```

## Directory Structure

```
intel_extreme/
  accelerant/          GPU accelerant (userspace, loaded by app_server)
    engine.cpp         Ring buffer, 2D BLT, hardware sync
    render.cpp         3D render engine (Gen5 solid fills, triangles)
    media_pipeline.cpp Media pipeline: EU kernel dispatch, MPEG-2 GPU decode
    gpu_bo.cpp         GPU buffer object allocator (GTT-mapped)
    mpeg2_parser.cpp   MPEG-2 bitstream parser (sequence/picture/slice/VLC)
    kernels/           EU kernel assembly (.g4a) and binaries (.g4b.gen5)
    tests/             Standalone test programs (no reboot needed)
  drm_shim/            DRM ioctl shim for Mesa crocus
  mpeg2_plugin/        Haiku media_kit MPEG-2 decoder plugin
  crocus_addon/        Mesa crocus kernel driver compatibility shims
hda/                   Patched HDA audio kernel driver
gen5_docs/             Analysis documents and notes
```

## Key Technical Findings

- **MMIO registers are read-only from userspace** on this hardware. All GPU command submission requires kernel ioctls (`INTEL_RING_WRITE_TAIL`). Verified with both `clone_area` and `/dev/misc/poke` BAR0 direct mapping.
- **Ring reset kills the command streamer.** After disable+re-enable, the CS never restarts. Only the boot-time `setup_ring_buffer` works. Userspace must sync with hardware TAIL and append, never reset.
- **MI_STORE_DATA_IMM silently fails in batch buffers** (non-secure mode). Direct ring emission works.
- **{compr} UB to UW widening is broken** on Gen5 EU. Must use explicit SIMD8 pairs.

## Documentation

- [DIFFERENZE_DRIVER.md](DIFFERENZE_DRIVER.md) - All patches vs stock Haiku R1~beta5
- [TODO_INTEL_GPU_HAIKU.md](TODO_INTEL_GPU_HAIKU.md) - Development roadmap and milestone tracking
- [CLAUDE.md](CLAUDE.md) - Build instructions and architecture reference

## License

MIT License. See [LICENSE](LICENSE).

Based on Haiku R1~beta5 (hrev59506) driver sources, copyright 2007-2024 Haiku, Inc.
