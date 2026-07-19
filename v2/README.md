# Intel Gen5-Gen8 GPU Driver Stack for Haiku OS — v2

Clean rewrite of the complete driver stack. See `../docs/ARCHITECTURE.md` for the
full plan, `../docs/hardware/HARDWARE_WIKI.md` for empirical hardware knowledge.

## Directory Layout

```
intel_gen_gpu/          Kernel driver (GEM, EXECBUF2, ring, reset, power)
intel_gen_accelerant/   Accelerant (display, BLT, 3D, media pipeline)
libdrm_haiku/           DRM compatibility library for Mesa
media_plugins/          media_kit decoder plugins (MPEG-2, H.264)
mesa_addon/             Mesa GL renderer addon
tests/                  Unified test suite
```

## Current Phase

**Phase 0**: Fix batch #3 3D pipeline hang + ring wrap-around.

## Build

Each component has its own Makefile. Build order:

1. `intel_gen_gpu` (kernel driver, requires haiku build tree)
2. `intel_gen_accelerant` (accelerant library)
3. `libdrm_haiku` (shared library)
4. `media_plugins` (shared libraries)
5. `mesa_addon` (shared library, requires Mesa source)
6. `tests` (standalone executables)

## Reference

The v1 code in `../intel_extreme/` is the working reference implementation.
All 24 documented Gen5 bugs and workarounds are in `../docs/hardware/HARDWARE_WIKI.md`.
