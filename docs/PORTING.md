# Porting Guide: Adding Support for a New Intel GPU Generation

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Guide |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](INDEX.md)

---


This guide explains how to add support for a new Intel GPU generation
to the Haiku intel_extreme accelerated driver. The codebase uses a
modular architecture where generation-specific code is isolated in
`gen*_ops.cpp` files implementing a common `gen_ops` vtable.

**Currently supported:**
- Gen5 (Ironlake) — tested on hardware
- Gen6 (Sandy Bridge) — implemented, needs testing
- Gen7 (Ivy Bridge / Haswell) — implemented, needs testing

**Architecture overview:**
```
Application / Test tool
       │
  gen_ops vtable ← init_gen_ops(generation)
       │
  ┌────┴────┐
  │ gpu_ring │  Ring buffer submission via kernel ioctl
  └────┬────┘
       │
  Kernel driver (INTEL_RING_WRITE_TAIL ioctl)
       │
  GPU Hardware
```

---

## Step 1: Create gen*_ops.cpp

Copy `gen6_ops.cpp` as a starting point and modify the command
encodings for your generation. The key differences between
generations are documented in the Intel Programmer's Reference
Manuals (PRMs), available at:
https://www.intel.com/content/www/us/en/docs/graphics-for-linux/

### What to change per generation:

| Command | What changes | Where to find info |
|---------|-------------|-------------------|
| STATE_BASE_ADDRESS | DWord count (Gen5=8, Gen6+=10, Gen8=16) | PRM Vol2 Part1 |
| PIPE_CONTROL / MI_FLUSH | Gen5 uses MI_FLUSH, Gen6+ uses PIPE_CONTROL | PRM Vol1 Part2 |
| 3DSTATE_DEPTH_BUFFER | DWord count (Gen5=6, Gen6=7, Gen7=8) | PRM Vol2 Part1 |
| URB allocation | Gen5: URB_FENCE, Gen6+: 3DSTATE_URB | PRM Vol2 Part1 |
| MEDIA_VFE_STATE | Gen5: state object, Gen6+: inline command (8 DW) | PRM Vol2 Part2 |
| CS_URB_STATE | Gen5: explicit, Gen6+: folded into VFE_STATE | PRM Vol2 Part1 |
| CONSTANT_BUFFER | Gen5: CMD_CONSTANT_BUFFER, Gen6+: MEDIA_CURBE_LOAD | PRM Vol2 Part2 |
| Max media threads | Gen5=48, Gen6=60, Gen7=64/112 | PRM Vol2 Part2 |

### Template:

```cpp
#include "gen_ops.h"
#include "intel_extreme.h"

// Your command opcodes here (use CMD_GFX macro)
#define CMD_GFX(p, o, s) \
    ((3u << 29) | ((uint32)(p) << 27) | ((uint32)(o) << 24) \
        | ((uint32)(s) << 16))

// Implement each gen_ops function:
static void genN_emit_mi_flush(batch_writer* w) { ... }
static void genN_emit_pipeline_select_media(batch_writer* w) { ... }
// ... etc ...

void genN_init_ops(gen_ops* ops) {
    ops->info.generation = N;
    ops->info.max_media_threads = ...;
    ops->emit_mi_flush = genN_emit_mi_flush;
    // ... fill all function pointers ...
}
```

## Step 2: Register in gen_ops.h

Add your `genN_init_ops()` declaration and add a case to `init_gen_ops()`:

```cpp
void genN_init_ops(gen_ops* ops);

static inline status_t init_gen_ops(gen_ops* ops, int generation) {
    switch (generation) {
        ...
        case N: genN_init_ops(ops); return B_OK;
    }
}
```

## Step 3: Add to Makefile

Add `genN_ops.cpp` to `ACCEL_SRCS` in `intel_extreme/accelerant/Makefile`.

## Step 4: Kernel Driver Ioctl

The `INTEL_RING_WRITE_TAIL` ioctl in the kernel driver writes the
TAIL register to kick the GPU. This is needed because userspace MMIO
writes are silently ignored (the kernel maps registers with
`B_KERNEL_WRITE_AREA` only).

The kernel driver patch is in the haiku-build source tree:
- `src/add-ons/kernel/drivers/graphics/intel_extreme/device.cpp`

The ioctl handler is generation-independent (same ring TAIL register
offset 0x2030 on Gen4-Gen8). For Gen9+ this needs to change to
GuC submission.

### Building the kernel driver

```sh
cd haiku-build
# Edit device.cpp to add INTEL_RING_RESET + INTEL_RING_WRITE_TAIL
jam intel_extreme
```

If the build tree's kernel ABI doesn't match the running system
(e.g. `_mutex_unlock` vs `mutex_unlock`), you need to compile
manually with a mutex compatibility shim. See the commit history
for the exact procedure.

### Installing

1. Blacklist the system driver:
   Edit `/boot/system/settings/packages`:
   ```
   Package haiku {
       BlockedEntries {
           add-ons/kernel/drivers/bin/intel_extreme
           add-ons/kernel/drivers/dev/graphics/intel_extreme
       }
   }
   ```

2. Install the patched driver:
   ```sh
   cp intel_extreme /boot/system/non-packaged/add-ons/kernel/drivers/bin/
   ln -sf ../../bin/intel_extreme \
       /boot/system/non-packaged/add-ons/kernel/drivers/dev/graphics/intel_extreme
   ```

3. Reboot.

## Step 5: EU Kernel Assembly (optional)

EU (Execution Unit) kernels are written in gen4asm assembly and
compiled per generation:

```sh
# Gen5 (Ironlake)
gen4asm -g5 -o kernel.g4b.gen5 kernel.g4a

# Gen6 (Sandy Bridge)
gen4asm -g6 -o kernel.g4b.gen6 kernel.g4a

# Gen7 (Ivy Bridge)
gen4asm -g7 -o kernel.g4b.gen7 kernel.g4a
```

The ISA is similar across Gen4-Gen7 but register file sizes,
instruction encoding details, and send message formats differ.
Refer to the PRMs for your generation.

For Gen8+ the ISA changes significantly (new register file layout,
different control flow). The gen4asm tool may not support Gen8+.

## Step 6: Testing

### Basic ring test
```sh
cd intel_extreme/accelerant/tests
./test_ring_ioctl
```
Expected: `GPU EXECUTED the NOOPs!`

### Media pipeline benchmark
```sh
./gpu_idct_bench
```
Expected: GPU vs CPU IDCT comparison with GPU speedup.
Note: requires Gen5 EU kernels. For other gens, the IDCT kernel
must be recompiled with the appropriate `-gN` flag.

### BLT to screen
```sh
./gpu_triangle
```
Expected: rotating 3D cube at 60 FPS.
Note: Gen6+ BLT is on separate BCS ring (0x22000). The gpu_ring
layer may need to target BCS instead of RCS for BLT commands.

### GEM_EXECBUFFER2
```sh
cd intel_extreme/drm_shim
./test_execbuf
```
Expected: `GPU WROTE CORRECTLY!` (MI_STORE_DATA_IMM via batch).

## Generation Quick Reference

| Gen | Codename | Years | Ring TAIL | BLT | MI_FLUSH | Max Threads |
|-----|----------|-------|-----------|-----|----------|-------------|
| 5 | Ironlake | 2010 | 0x2030 | RCS | MI_FLUSH | 48 |
| 6 | Sandy Bridge | 2011 | 0x2030 | BCS 0x22000 | PIPE_CONTROL | 60 |
| 7 | Ivy Bridge | 2012 | 0x2030 | BCS 0x22000 | PIPE_CONTROL | 64 |
| 7.5 | Haswell | 2013 | 0x2030 | BCS 0x22000 | PIPE_CONTROL | 112 |
| 8 | Broadwell | 2014 | 0x2030 | BCS 0x22000 | PIPE_CONTROL | 112 |
| 9 | Skylake | 2015 | GuC | none (render) | PIPE_CONTROL | varies |
