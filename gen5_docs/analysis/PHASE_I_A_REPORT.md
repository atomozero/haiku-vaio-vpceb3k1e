# Phase I.A — gen4asm port to Haiku — SESSION REPORT

**Status**: ✅ **Complete. All goals met, zero blockers encountered.**

**Duration**: ~30 minutes (vs. 1–3 days estimated in `GEN4ASM_PORT.md`).

---

## What was done

### Tree layout created
```
intel_extreme/tools/gen4asm/
├── Makefile                  (Haiku-specific, written in this session)
├── *.c, *.h, *.l, *.y        (26 files, copied from gen5_docs/igt/assembler/)
├── test/                     (reference tests + run-test.sh)
├── README, TODO               (upstream docs)
└── gen4asm, gen4disasm       (built binaries, 753 KB + 452 KB)
```

Total source footprint in tree: **~780 KB** before build, adds ~1.2 MB of build artefacts that are regenerated on every `make`.

### Makefile (hand-written for Haiku)
Path: `intel_extreme/tools/gen4asm/Makefile`
- Uses `/boot/system/develop/tools/bin/gcc`, `/bin/flex`, `/bin/bison` — all present on target.
- Generates `gram.c`/`gram.h` from `gram.y` via bison, `lex.c` from `lex.l` via flex.
- Compiles 13 `.c` files + 3 generated → `gen4asm` and `gen4disasm` binaries.
- Targets: `all`, `clean`, `test`.

### Build result
**Zero errors. Zero warnings suppressed beyond the upstream set in `meson.build`. First-try success.**

This was the single most pleasant surprise of the session. My Phase I.A plan budgeted 0.5–1 day and expected "0–3 small issues, most likely around `alloca.h` or YY stack". Zero materialized. Haiku's POSIX compliance for this slice of the toolchain is exact; the gen4asm code is sufficiently portable that nothing needed to be touched.

---

## Validation results

### Baseline tests (11 cases from upstream `test/`)
```
PASS mov          PASS rnde          PASS lzd
PASS frc          PASS rnde-intsrc   PASS not
PASS regtype      PASS rndz          PASS immediate
PASS rndd         PASS rndu
```
**11 / 11 byte-identical.** No endianness issues, no bison/flex version drift, no libc quirks.

### Real-world validation: libva MPEG-2 Gen5 kernels
This is the strong test — taking the actual production kernels that libva-intel-driver has been shipping to real Ironlake silicon for 15 years, preprocessing with m4, running through our port at `-g5`, and binary-comparing the output against libva's committed `.g4b.gen5` files.

```
Kernel                              Status
─────────────────────────────────── ──────────────
field_backward                      BYTE-IDENTICAL ✓
field_backward_16x8                 BYTE-IDENTICAL ✓
field_bidirect                      BYTE-IDENTICAL ✓
field_bidirect_16x8                 BYTE-IDENTICAL ✓
field_forward                       BYTE-IDENTICAL ✓
field_forward_16x8                  BYTE-IDENTICAL ✓
field_intra                         BYTE-IDENTICAL ✓
frame_field_pred_backward           BYTE-IDENTICAL ✓
frame_field_pred_bidirect           BYTE-IDENTICAL ✓
frame_field_pred_forward            BYTE-IDENTICAL ✓
frame_frame_pred_backward           BYTE-IDENTICAL ✓
frame_frame_pred_bidirect           BYTE-IDENTICAL ✓
frame_frame_pred_forward            BYTE-IDENTICAL ✓
frame_intra                         BYTE-IDENTICAL ✓
lib                                 BYTE-IDENTICAL ✓
──────────────────────────────────────────────────
Total: 15 / 15 byte-identical
```
(The 16th `.g4a` in that directory is `null.g4a`, a stub with no committed `.g4b.gen5` reference.)

**Our Haiku-hosted assembler produces byte-for-byte identical Gen5 machine code to what the libva build chain has produced on Linux for 15 years.** This is the strongest possible pre-GPU validation — it means when we get to phase 2 (MPEG-2 decoder bring-up), we can rebuild every kernel from source in-tree, with confidence that the binaries match what is known to work on real silicon.

### m4 dependency confirmed
`m4` is available at `/bin/m4` on the target Haiku system, so the libva kernel preprocessing flow (`include()`, `define()` macros) works out of the box. We noted in `GEN4ASM_PORT.md` that we should not depend on m4 for *our own* kernels, and that remains the plan — but being able to rebuild libva kernels from source on Haiku is a useful bonus.

### Full-rebuild test
```
make clean && make
```
Completes cleanly, regenerates `gram.c`/`gram.h`/`lex.c` from scratch, rebuilds all object files and binaries. No hidden incremental-build dependencies.

---

## Files not needed / not used on Gen5

Not removed (kept for future generations / debug tooling), but worth documenting as dead code for our target:

- `gen8_disasm.c` (990 lines), `gen8_instruction.c` (445 lines), `gen8_instruction.h` (362 lines) — Gen8 instruction encoding, never exercised with `-g5`.
- `brw_eu_compact.c` (810 lines) — early-returns at line 682 when `gen < 6`. No effect on Gen5 output.

Total dead code on Gen5 target: ~2,600 lines out of ~17,300. **Kept for now** because:
1. Isolation is clean (no cross-references into live paths from Gen5).
2. No maintenance burden yet.
3. Saves a future decision if we ever add `-g6` / `-g7` targets (e.g. for Sandybridge laptops).

If maintenance friction appears later, the cut is straightforward: remove the four files from `LIB_BRW_SRC` in the Makefile and delete unused references. Not doing it preemptively.

---

## What we can now do that we couldn't before

This port unlocks every downstream step in the roadmap. Specifically:

1. **Write new EU kernels as human-readable `.g4a` source in-tree.** No Linux machine required, no precompiled blobs shipped.
2. **Regenerate libva's MPEG-2 kernels from source on every build**, if we choose to (alternative: use the committed `.g4b.gen5` binaries directly — now we have both options).
3. **Disassemble any Gen5 EU binary** via the `gen4disasm` tool that was built alongside `gen4asm`. This becomes critical when we're debugging kernel hangs and need to read back what the GPU is actually executing.
4. **Move to Phase I.B of `MEDIA_PIPELINE_BRINGUP.md`** — the "hello world" three-instruction kernel. We can write it as `kernels/hello_world.g4a` and assemble it immediately; it's no longer a theoretical step.

---

## What still blocks the first on-GPU test

From `MEDIA_PIPELINE_BRINGUP.md` §1.1 ("Lights on" milestone), the remaining prerequisites are:

1. **GTT memory allocator** in the accelerant — replaces the current fixed-offset BO placement. This is the one real piece of non-trivial C code still to write. Est. ~200 lines.
2. **Instrumentation** (ACTHD, INSTDONE, IPEHR readback, per-command markers) — mostly a small library on top of existing MMIO access in `shared_info->registers`. Est. ~150 lines.
3. **Media-pipeline batch builder** — emits the 10-command sequence documented in `MEDIA_PIPELINE_BRINGUP.md` §1. Est. ~300 lines of mostly mechanical DWORD emission.
4. **Hello-world `.g4a` kernel** — 3 instructions, trivial now that we have the assembler.
5. **Test harness that submits the batch, polls for completion, reads back the output BO, and reports success/failure.** Est. ~100 lines.

Total estimated remaining work for Phase I.B ("hello world"): **~750 lines of code + 3 instructions of EU assembly**. Bounded, no unknowns.

---

## Risks that did not materialize

For the record, here are the risks I flagged in `GEN4ASM_PORT.md` §8 and what actually happened:

| Risk | Actually hit? | Notes |
|---|---|---|
| flex/bison version drift | ❌ | Built cleanly with Haiku's flex/bison versions |
| `alloca.h` include missing | ❌ | Bison didn't emit `alloca` references |
| YYSTACK / YYMALLOC quirks | ❌ | Default parser stack worked |
| Endianness issues | ❌ | Expected (x86 Haiku = little-endian, matching Intel GPU) |
| Haiku-specific header renames | ❌ | Nothing needed |
| "Broken tests" biting us | ❌ | We only ran the known-passing 11 tests; the broken ones are numeric-offset branches which we don't use |
| Gen5-specific encoding gaps | ❌ | 15/15 real-world MPEG-2 kernels matched byte-for-byte |

The risk analysis was more conservative than necessary. The upstream code is more portable than the TODO and known-brokenness list suggested.

---

## Invocation reference

For future use of the assembler:

```sh
# From intel_extreme/tools/gen4asm/
./gen4asm -g5 -o output.g4b.gen5 input.g4a    # assemble for Gen5
./gen4asm --gen=5 -o output.g4b.gen5 input.g4a  # long form
./gen4asm                                       # prints usage
./gen4disasm < binary.bin                       # disassemble
```

The output file is a C-style array initializer block:
```c
   { 0x00000001, 0x20000021, 0x00000020, 0x00000000 },
   ...
```
which is `#include`d into a `uint32_t kernel[][4]` declaration, exactly as libva does:
```c
static const uint32_t hello_world_kernel[][4] = {
#include "hello_world.g4b.gen5"
};
```

---

## Recommended next step

**Proceed to Phase I.B step 1: GTT memory allocator.**

The allocator is the single piece of actual driver code still on the critical path to the first EU-thread execution. Everything else downstream (batch builder, kernel, test harness) is either mechanical emission of known DWORDs or, in the case of the hello-world kernel, three assembly instructions.

Suggested focus for the next session:
1. Read the current BO placement logic in `intel_extreme/accelerant/engine.cpp` and `memory.cpp`.
2. Decide on scope: bump allocator over a reserved GTT region, or freelist-based reclaimable. For phase 1 purposes, a bump allocator is enough.
3. Implement and unit-test with existing infrastructure.

After that, Phase I.B completion (hello world running on the GPU) is a handful of days of mechanical work, and we cross the "GPU is executing our code" threshold.
