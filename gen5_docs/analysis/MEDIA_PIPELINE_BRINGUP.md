# Gen5 media pipeline bring-up — concrete specification

Derived from reading:
- `libva_intel/src/i965_media.c` (398 lines)
- `libva_intel/src/i965_media.h`
- `libva_intel/src/i965_media_mpeg2.c` (1024 lines, first 1024 read)
- `libva_intel/src/i965_defines.h` (opcodes)
- `libva_intel/src/i965_structs.h` (VFE/interface descriptor/surface state layouts)

This document captures the complete command and state-object sequence required to dispatch a compute kernel on the Gen5 EU array via the media pipeline. It is what we need to implement in Haiku to reach the first "transistors doing what we say" milestone.

---

## 0. Big picture: 3D pipeline vs media pipeline on Gen5

The two pipelines share the same ring, the same EU array, and the same memory, but they are programmed through two almost entirely disjoint sets of commands. A `PIPELINE_SELECT` command at the start switches the command streamer between them.

| Concept | 3D pipeline | Media pipeline |
|---|---|---|
| Pipeline selector | `PIPELINE_SELECT_3D` (0) | `PIPELINE_SELECT_MEDIA` (1) |
| Thread dispatcher | VS/GS/SF/WM fixed-function | VFE (Video Front End) |
| State pointer command | `3DSTATE_PIPELINED_POINTERS` | `MEDIA_STATE_POINTERS` |
| Per-kernel descriptor | Separate VS/GS/SF/WM state | Single "interface descriptor" |
| Thread launch | `3DPRIMITIVE` (implicit via rasterizer) | `MEDIA_OBJECT` (explicit) |
| URB partitioning | VS/GS/CLIP/SF/CS fences | VFE + CS fences only |
| "Hello world" cost | ~15 state commands before first draw | ~8 state commands before first thread |

The media pipeline is **dramatically simpler**. No rasterizer setup, no depth buffer (well, almost — see §5), no vertex fetch, no clipping. You allocate URB space, upload one VFE state and one interface descriptor pointing to a kernel, and launch threads.

**This is why the pivot from the 3D path to the media path is not just a detour — it is a simpler target on its own merits, quite apart from being the right path for compute workloads.**

---

## 1. The 10-step bring-up sequence

This is the exact order used by `i965_media_pipeline_setup()` in `i965_media.c:185`. Every step is obligatory; skipping any one hangs the pipeline.

```
┌──────────────────────────────────────────────────────────────────┐
│ Phase A — reset pipeline (batch prologue)                        │
├──────────────────────────────────────────────────────────────────┤
│ 1. MI_FLUSH                                                      │
│ 2. 3DSTATE_DEPTH_BUFFER (NULL depth — required even for media)   │
│ 3. PIPELINE_SELECT (media mode)                                  │
├──────────────────────────────────────────────────────────────────┤
│ Phase B — allocate URB + instruction base                        │
├──────────────────────────────────────────────────────────────────┤
│ 4. URB_FENCE  (VFE entries + CS entries partitioning)            │
│ 5. STATE_BASE_ADDRESS  (general/surface/indirect/etc.)           │
├──────────────────────────────────────────────────────────────────┤
│ Phase C — attach compute state                                   │
├──────────────────────────────────────────────────────────────────┤
│ 6. MEDIA_STATE_POINTERS (-> VFE state BO, -> extended state BO)  │
│ 7. CS_URB_STATE (CURBE entry allocation within URB)              │
│ 8. CONSTANT_BUFFER (load CURBE contents from curbe BO)           │
├──────────────────────────────────────────────────────────────────┤
│ Phase D — launch kernels                                         │
├──────────────────────────────────────────────────────────────────┤
│ 9. MEDIA_OBJECT (one or more, one per work item)                 │
│10. MI_FLUSH + batch end                                          │
└──────────────────────────────────────────────────────────────────┘
```

Three notes:
- **Step 2 is surprising and easy to miss.** `3DSTATE_DEPTH_BUFFER` belongs to the 3D pipeline, yet libva-intel-driver issues it *before* `PIPELINE_SELECT` to media mode. This is a Gen4/5 quirk: the depth buffer pointer leaks across pipeline selects and must be neutralized with a NULL depth buffer to avoid phantom reads. Omitting this causes hangs on some hardware revisions.
- **Step 6 on Gen5 is called `MEDIA_STATE_POINTERS`, not `MEDIA_VFE_STATE`.** The opcode is the same (`(3<<29) | (2<<27) | (0<<24) | (0<<16) = 0x70000000`), but on Gen5 it is an *indirection* — the command carries two pointers (VFE state BO, extended state BO), not the inline VFE configuration that Gen6+ puts in-line as `MEDIA_VFE_STATE`. This means on Gen5 the VFE state lives in a separate buffer object written by the CPU.
- **Step 8 uses `CONSTANT_BUFFER` (the Gen4/5 constant-buffer load)**, not `3DSTATE_CONSTANT_*` (which is 3D-only) and not `MEDIA_CURBE_LOAD` (which arrives on Gen6+). Another Gen5-specific wrinkle.

---

## 2. Opcode reference

All Gen4/5 command opcodes follow the `CMD(pipeline, op, sub_op)` pattern:

```c
#define CMD(pipeline, op, sub_op)  ((3 << 29) | ((pipeline) << 27) | \
                                    ((op) << 24)  | ((sub_op) << 16))
```

The 10 commands we need:

| Step | Command | CMD(p,o,s) | Raw DWORD0 |
|---|---|---|---|
| 1 | `MI_FLUSH` | n/a (MI category) | `0x04000000` |
| 2 | `3DSTATE_DEPTH_BUFFER` | `CMD(3, 1, 5)` | `0x79050000` |
| 3 | `PIPELINE_SELECT` | `CMD(1, 1, 4)` | `0x69040000` (+ `\|1` for media) |
| 4 | `URB_FENCE` | `CMD(0, 0, 0)` | `0x60000000` |
| 5 | `STATE_BASE_ADDRESS` | `CMD(0, 1, 1)` | `0x61010000` |
| 6 | `MEDIA_STATE_POINTERS` | `CMD(2, 0, 0)` | `0x70000000` |
| 7 | `CS_URB_STATE` | `CMD(0, 0, 1)` | `0x60010000` |
| 8 | `CONSTANT_BUFFER` | `CMD(0, 0, 2)` | `0x60020000` |
| 9 | `MEDIA_OBJECT` | `CMD(2, 1, 0)` | `0x71000000` |
| 10 | `MI_FLUSH` | (same as step 1) | `0x04000000` |

The low bits of DWORD0 carry a *length minus 2* field (the DWORD count following DWORD0, minus 2) except for fixed-length MI commands.

---

## 3. Command details, DWORD-by-DWORD

### 3.1 `PIPELINE_SELECT` (step 3) — 1 DWORD total

```
DWORD0:  0x69040000 | 0x1    // CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA
```

Source: `i965_media.c:51`.

### 3.2 `URB_FENCE` (step 4) — 3 DWORDs

On Gen5 the URB is 1024 entries total (per `i965_defines.h`). The fence command partitions it into regions. For the media pipeline only **VFE** and **CS** regions exist — the 3D-pipeline regions (VS, GS, CLIP, SF) are not allocated and their `_REALLOC` bits are left at 0.

```
DWORD0:  0x60000000                      // CMD_URB_FENCE
         | (1 << 12)                     // UF0_VFE_REALLOC
         | (1 << 13)                     // UF0_CS_REALLOC
         | 1                             // length - 2
DWORD1:  0                               // VS/GS/CLIP fences = 0 (unused)
DWORD2:  (vfe_fence << 10)               // UF2_VFE_FENCE_SHIFT = top of VFE region
         | (cs_fence  << 20)             // UF2_CS_FENCE_SHIFT  = top of CS region
```

Where `vfe_fence = vfe_end_index`, `cs_fence = cs_end_index = total_urb_size`.

For the MPEG-2 VLD decoder, libva uses:
- `num_vfe_entries = 28`, `size_vfe_entry = 13 × 512 bits = 832 bytes per entry`
- `num_cs_entries  = 1`,  `size_cs_entry  = 16 × 512 bits = 1024 bytes per entry`
- `vfe_start = 0`
- `cs_start  = 28 × 13 = 364`
- `cs_fence  = urb_size (total)`

Source: `i965_media_mpeg2.c:1006–1017` and `i965_media.c:62–72`.

For a **"hello world" single-thread kernel** we can use much smaller numbers:
- `num_vfe_entries = 1`, `size_vfe_entry = 1`
- `num_cs_entries  = 1`, `size_cs_entry  = 1`
- Total URB footprint: 2 entries.

### 3.3 `STATE_BASE_ADDRESS` (step 5, Ironlake variant) — 8 DWORDs

Gen5 expanded the command from 6 DWORDs (Gen4) to 8 DWORDs by adding instruction-base-address fields. The `IS_IRONLAKE()` branch in `i965_media.c:80` handles this.

```
DWORD0:  0x61010000 | 6                  // length - 2
DWORD1:  0 | BASE_ADDRESS_MODIFY         // general state base = 0
DWORD2:  0 | BASE_ADDRESS_MODIFY         // surface state base = 0
DWORD3:  indirect_object_bo_reloc | BASE_ADDRESS_MODIFY   // or 0 if none
DWORD4:  0 | BASE_ADDRESS_MODIFY         // (Gen5 extra: instruction base = 0)
DWORD5:  0 | BASE_ADDRESS_MODIFY         // general state upper bound
DWORD6:  0 | BASE_ADDRESS_MODIFY         // (Gen5 extra: upper bound)
DWORD7:  0 | BASE_ADDRESS_MODIFY         // indirect object upper bound
```

`BASE_ADDRESS_MODIFY = 1` is the "this field is valid, apply it" bit.

The indirect object address is the base for bitstream data pointers in `MEDIA_OBJECT` commands; for a hello-world compute kernel we pass 0 and use absolute addresses instead.

Source: `i965_media.c:80–97`.

### 3.4 `MEDIA_STATE_POINTERS` (step 6) — 3 DWORDs

```
DWORD0:  0x70000000 | 1                  // length - 2
DWORD1:  extended_state_bo_reloc | 1     // low bit = extended state present
         (or 0 if no extended state)
DWORD2:  vfe_state_bo_reloc              // pointer to i965_vfe_state (6 DWORDs)
```

Source: `i965_media.c:118–132`.

### 3.5 `CS_URB_STATE` (step 7) — 2 DWORDs

```
DWORD0:  0x60010000 | 0                  // length - 2
DWORD1:  ((size_cs_entry - 1) << 4)      // entry size in 512-bit units, -1 encoded
         | num_cs_entries                // number of CS entries
```

Source: `i965_media.c:135–145`.

### 3.6 `CONSTANT_BUFFER` (step 8) — 2 DWORDs

```
DWORD0:  0x60020000 | (1 << 8) | 0       // bit 8 = valid, length - 2 = 0
DWORD1:  curbe_bo_reloc | (size_cs_entry - 1)   // low bits = size - 1
```

Source: `i965_media.c:156–166`.

### 3.7 `MEDIA_OBJECT` (step 9) — 6 DWORDs

This is the thread launch. One MEDIA_OBJECT = one thread (or one group of SIMD lanes, depending on kernel mode). Multiple MEDIA_OBJECTs are queued back-to-back in the batch.

```
DWORD0:  0x71000000 | 4                  // length - 2 = 4
DWORD1:  0                               // interface descriptor index (0..15)
                                         // -> picks desc from IDRT
DWORD2:  indirect_data_length            // bytes of payload (0 for hello world)
DWORD3:  indirect_data_start_reloc       // pointer to per-thread data (0 ok)
DWORD4:  thread_parameters               // (hpos<<24)|(vpos<<16)|(mask<<8)|bits
DWORD5:  per_thread_dword                // application-defined (e.g. scale code)
```

DWORD4 and DWORD5 are passed as inline GRF payload to the kernel as R0 / R1 register contents. For a hello-world kernel DWORD4 can encode a destination offset and DWORD5 the constant to write.

Source: `i965_media_mpeg2.c:915–928`.

---

## 4. State objects — what the CPU writes to the referenced BOs

The commands above carry GPU-address *pointers* to state objects. Those objects are ordinary buffer objects whose contents the CPU writes before submitting the batch.

### 4.1 VFE state BO — `struct i965_vfe_state` (6 DWORDs = 24 bytes)

Layout from `i965_structs.h:4`:

```
DWORD0 (vfe0):
  [ 3: 0] per_thread_scratch_space       // 0 for hello world
  [ 6: 4] pad
  [   7 ] extend_vfe_state_present       // 1 if extended_state BO is attached
  [ 9: 8] pad
  [31:10] scratch_base                   // 0 for hello world

DWORD1 (vfe1):
  [ 1: 0] debug_counter_control          // 0
  [   2 ] children_present               // 0
  [ 6: 3] vfe_mode                       // 0 = GENERIC, 1 = VLD, ... (see below)
  [ 8: 7] pad
  [15: 9] num_urb_entries                // = num_vfe_entries from URB allocation
  [24:16] urb_entry_alloc_size           // = size_vfe_entry - 1
  [31:25] max_threads                    // number of threads that can run - 1

DWORD2 (vfe2):
  [ 3: 0] pad
  [31: 4] interface_descriptor_base      // IDRT base >> 4 (16-byte aligned)
```

VFE modes from `i965_defines.h`:
- `VFE_GENERIC_MODE    = 0x0`  ← use this for hello world and for compute/LLM
- `VFE_VLD_MODE        = 0x1`  ← MPEG-2 / H.264 VLD decode
- `VFE_IS_MODE         = 0x2`  ← image stabilization
- `VFE_AVC_MC_MODE     = 0x4`  ← H.264 motion compensation
- `VFE_AVC_IT_MODE     = 0x7`  ← H.264 intra transform

**For hello world**: set `vfe_mode = 0` (generic), `num_urb_entries = 1`, `urb_entry_alloc_size = 0`, `max_threads = 0`, everything else zero. Then point `interface_descriptor_base` at the IDRT BO shifted right by 4.

Source: `i965_media_mpeg2.c:666–691`.

### 4.2 Interface Descriptor (IDRT) — `struct i965_interface_descriptor` (4 DWORDs = 16 bytes)

One descriptor per kernel. The MEDIA_OBJECT `interface descriptor index` in DWORD1 selects among up to 16 descriptors in the IDRT BO.

```
DWORD0 (desc0):
  [ 3: 0] grf_reg_blocks                 // GRF allocation for kernel, 0..15
  [ 5: 4] pad
  [31: 6] kernel_start_pointer           // kernel BO offset >> 6

DWORD1 (desc1):
  [ 6: 0] pad
  [   7 ] software_exception             // 0
  [10: 8] pad
  [  11 ] maskstack_exception
  [  12 ] pad
  [  13 ] illegal_opcode_exception
  [15:14] pad
  [  16 ] floating_point_mode            // 0 = IEEE 754, 1 = alt
  [  17 ] thread_priority                // 0 = normal
  [  18 ] single_program_flow            // 1 for SIMD kernels without branching
  [  19 ] pad
  [25:20] const_urb_entry_read_offset    // CURBE read offset in owords
  [31:26] const_urb_entry_read_len       // CURBE read length in owords (30 for MPEG-2)

DWORD2 (desc2):
  [ 1: 0] pad
  [ 4: 2] sampler_count                  // 0 for hello world, 1+ if using sampler
  [31: 5] sampler_state_pointer          // sampler state BO >> 5

DWORD3 (desc3):
  [ 4: 0] binding_table_entry_count      // number of surfaces, 0 for hello world
  [31: 5] binding_table_pointer          // binding table BO >> 5
```

**For hello world**: only `kernel_start_pointer` (to a tiny kernel BO) and some minimal `grf_reg_blocks` value. Binding table pointer can be 0 if the kernel writes through a raw address without sampler/surface access.

Source: `i965_media_mpeg2.c:694–731`, struct `i965_structs.h:150`.

### 4.3 Surface State — `struct i965_surface_state` (~6 DWORDs)

Only needed when the kernel reads/writes through the sampler or surface-write path. Skipped entirely for the first hello-world kernel (which writes directly to a known GPU address).

### 4.4 Binding table — array of DWORDs

Same: only needed when surface state is in use.

---

## 5. The minimum viable "hello world" for Haiku

Here is the irreducible setup to see the first sign of life from the EUs. Everything is as small as the hardware will accept.

### 5.1 What we allocate (CPU-side)

1. **Batch buffer** — a BO in the existing ring path. ~1 KB is plenty.
2. **Kernel BO** — 128 bytes, contains a few EU instructions written by hand (see §5.3).
3. **VFE state BO** — 24 bytes.
4. **IDRT BO** — 16 bytes (one interface descriptor).
5. **CURBE BO** — 32 bytes (minimum aligned, unused content).
6. **Output BO** — 4 KB, initialized to a sentinel value (e.g. `0xdeadbeef`) by the CPU before submission.

Everything must be placed in the GTT-mapped area so the GPU can reach it.

### 5.2 The batch buffer contents

Twelve commands (in this exact order), totalling about 30 DWORDs:

1. `MI_FLUSH`  (0x04000000)
2. `3DSTATE_DEPTH_BUFFER` (NULL) — 6 DWORDs
3. `PIPELINE_SELECT` media mode
4. `URB_FENCE` — vfe=1, cs=1
5. `STATE_BASE_ADDRESS` Ironlake variant — 8 DWORDs, all zero + MODIFY
6. `MEDIA_STATE_POINTERS` — no extended state, pointer to VFE state BO
7. `CS_URB_STATE` — 1 CS entry, size 1
8. `CONSTANT_BUFFER` — pointer to CURBE BO
9. `MEDIA_OBJECT` — interface descriptor index 0, no payload
10. `MI_FLUSH` — 0x04000000
11. `MI_BATCH_BUFFER_END` — 0x05000000
12. `MI_NOOP` padding to 8-DWORD alignment

Total: well under 256 bytes of batch.

### 5.3 The kernel

The simplest possible EU kernel does two things: write a constant to a known memory address, then signal thread end. In EU assembly this is roughly:

```
mov (8) g2<1>UD  0xcafef00dUD        { align1 };   // load constant into GRF
send (8) null g2 0x0 <DATA-PORT-WRITE-HEADER>      // store to memory
send (8) null g0 0x0 <EOT-HEADER>    { eot };      // end of thread
```

Three instructions, 48 bytes in binary form, trivial to hand-assemble from `brw_eu_emit.c` or to compile with the in-tree `intel-gen4asm` once we port it.

**Crucial detail**: the destination address for the write is passed through the payload registers that MEDIA_OBJECT fills in (DWORD4 / DWORD5 become part of R0, which the kernel reads). So the kernel learns *where* to write from its dispatch parameters, not from CURBE. This keeps the hello-world kernel self-contained with zero binding table entries.

### 5.4 Success criterion

After submitting the batch and waiting for completion (polled via seqno at first, interrupt-driven later):

1. The output BO contains `0xcafef00d` instead of the sentinel `0xdeadbeef`.
2. `IPEHR` / `ACTHD` registers show the ring advanced past the `MI_BATCH_BUFFER_END`.
3. `INSTDONE` shows every pipeline stage "done" (all bits set).
4. `EIR` is 0.

**This is the moment the project crosses from "modeset-only driver" to "GPU doing actual computation on our command".** Nothing else we build after matters if this step doesn't work, and once it works, the rest is incremental extension.

---

## 6. Why this maps cleanly onto the existing accelerant

Looking at what we already have in `intel_extreme/accelerant/`:

| Need | Already exists? | Notes |
|---|---|---|
| Ring buffer submission | ✅ `engine.cpp` | Works for BLT; same ring for render. |
| GTT mapping | ✅ | Via kernel driver, shared via `shared_info`. |
| MMIO register access | ✅ | `shared_info->registers`. |
| `PIPE_CONTROL` emission | ✅ partial | Currently used for render 3D path. |
| Batch buffer atomic start/end | ✅ | Exists for BLT. |
| Relocation / BO address writing | ⚠ ad-hoc | Currently hard-coded GTT offsets. Needs a proper allocator. |
| Seqno-based completion wait | ⚠ polling | OK for bring-up. |

**The only blocker for "run a MEDIA_OBJECT batch" is the memory allocator.** Everything else is already in place or a minor extension. The current accelerant hand-places things at fixed GTT offsets; that works for a handful of fixed buffers but falls apart once we have dynamic BO lifetimes (kernels, state, CURBE, output). We need a simple bump-allocator or freelist over a chunk of GTT space before phase 1 proper.

That allocator is **the single work item that stands between us and the hello-world milestone**. Everything after it is reading specs and emitting DWORDs.

---

## 7. What phase 1 concretely looks like

Three sub-milestones, each independently verifiable.

### 1.1 — "Lights on"
Allocator + batch emission + MEDIA_OBJECT hello world kernel.

**Definition of done**: the output BO contains `0xcafef00d` after ring submission. Everything else optional.

Estimated work: ~1–2 weeks once we start, mostly on the allocator and on getting the `intel-gen4asm` ported. Emitting the 12-command batch is mechanical once the supporting pieces are there.

### 1.2 — "Parallelism proven"
Same setup, but the kernel is a 48-way SIMD memset: each hardware thread writes a different DWORD to a different offset in the output BO, all launched via a single MEDIA_OBJECT_WALKER (`CMD(2, 1, 3)`) or by queuing 48 MEDIA_OBJECTs back-to-back.

**Definition of done**: output BO contains a deterministic sequence (e.g. each DWORD equals its index × 0x1000). Validates that thread dispatch, URB partitioning, and inter-thread independence all work.

Estimated work: a few days after 1.1, mostly tweaking URB numbers and verifying the walker command encoding.

### 1.3 — "Kernel reads inputs"
Kernel reads from a source BO through a binding table + surface state, applies a trivial transform (e.g. byte-swap), writes to a destination BO.

**Definition of done**: source BO `[0x00,0x01,0x02,...]` produces dest BO `[0x00,0x01,0x02,...]` (or any verifiable transform).

Validates: surface state layout, binding table population, sampler/data-port access, the full state graph.

Estimated work: 1–2 weeks, mostly spent writing the surface-state constructor and debugging binding-table relocation.

At the end of phase 1.3, **we are three ports away from MPEG-2 decode**: port the MPEG-2 VLD kernels (already compiled binaries in `libva_intel/src/shaders/mpeg2/vld/*.g4b.gen5`), port the IQ-matrix + IDCT table upload (already written in `i965_media_mpeg2.c:800–865`), port the slice parser (bounded work, CPU-side). That is phase 2.

---

## 8. Risks and open questions

**What is still unclear and needs PRM reading before phase 1:**

1. **Relocation vs absolute addresses on Haiku.** libva relies on `drm_i915_gem_execbuffer` ioctl for relocation fixups — we won't have that. Our kernel driver either pins BOs at known GTT offsets (simple, wastes GTT) or we write a tiny relocation pass in the accelerant. Small design decision, but needs to be made before we start.

2. **Coherency of CPU-written state BOs.** Does Gen5 on Ironlake snoop CPU caches for these GTT-mapped BOs, or do we need explicit `clflush` on the CPU side before each submission? Vol 1 Part 2 (MMIO/programming environment) has the definitive answer. On Linux libva leans on GEM domain tracking, which we don't have.

3. **Kernel instruction cache flushing.** When we update the kernel BO contents between submissions, does Gen5 EU L1 instruction cache hold stale copies? `PIPE_CONTROL` with instruction-cache-invalidate is usually the answer; need to verify the Gen5 form.

4. **Minimum URB sizes.** We assumed `num_vfe_entries = 1, size_vfe_entry = 1` works. The PRM may have minimums. Check Vol 4 Part 1 (URB) before relying on this.

5. **`3DSTATE_DEPTH_BUFFER` before `PIPELINE_SELECT media`.** libva does it. The PRM should say explicitly whether this is necessary — worth confirming, as it feels fragile.

6. **Context switching.** Haiku's accelerant runs in multiple app_server contexts. On Gen5 before hardware contexts (which arrive with Ironlake but are complex), we may need to re-submit all state at every batch. libva does exactly that. Acceptable for bring-up; reconsider for performance later.

All six are small, bounded questions. None is a showstopper.

---

## 9. Summary

The Gen5 media pipeline is programmed by a short, rigid sequence: **flush → NULL depth → select media → fence URB → set bases → point at VFE state → allocate CURBE → load CURBE → launch MEDIA_OBJECT**. Each command is a few DWORDs, total setup is under 256 bytes of batch, and the backing state objects (VFE state, interface descriptor) are small structs the CPU writes directly into BOs.

The work between where we are today and "first EU thread runs our code":
1. Port `intel-gen4asm` to the tree.
2. Write a minimal GTT BO allocator.
3. Emit the 12-command batch with hand-assembled 3-instruction kernel.
4. Verify output BO contains the expected constant.

libva-intel-driver has every piece of this working for Gen5 and licensed MIT. The porting effort is real but bounded, and every risk on the list in §8 is a question with a known place to find the answer (one of our local PRMs).

This is the concrete plan for phase 1.
