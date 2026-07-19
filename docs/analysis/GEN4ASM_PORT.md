# Porting intel-gen4asm to Haiku — feasibility and plan

| | |
|---|---|
| **Status** | ✅ Complete |
| **Category** | Analysis |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---


Based on reading:
- `igt/assembler/` (17,295 lines total, MIT)
- `igt/assembler/meson.build`, `main.c`, `brw_compat.h`, `gen4asm.h`, `README`, `TODO`
- `igt/assembler/test/mov.g4a` / `test/mov.expected` (output format)
- `libva_intel/src/shaders/mpeg2/vld/field_intra.g4a` (real-world Gen5 kernel syntax)
- Haiku environment: `flex`, `bison`, `yacc` all present at `/bin/`.

Short answer: **straightforward port. No blockers, no runtime dependencies, no Linux-specific headers. Expected effort: 1–3 days of focused work to get a working `haiku-gen4asm` binary that produces byte-identical output to upstream for Gen5 targets.**

The rest of this document walks the dependency analysis, the changes required, and the phased integration plan into our driver tree.

---

## 1. What `intel-gen4asm` is

A standalone command-line assembler that reads Intel EU ISA source (`.g4a`, text) and writes binary or C-array output (one instruction = 4 × 32-bit DWORDs = 16 bytes on Gen4 through Gen7; compact instructions exist only from Gen6+ and are a no-op for our target).

Input syntax example (from `test/mov.g4a`):
```
mov (1) g0<1>UD g1<0,1,0>UD { align1 };
```

Output (with default `-b` flag, ready to `#include` in C code — from `test/mov.expected`):
```
   { 0x00000001, 0x20000021, 0x00000020, 0x00000000 },
```

**This is exactly the format libva-intel-driver uses** in `libva_intel/src/shaders/mpeg2/vld/*.g4b.gen5`. Porting gen4asm gives us the exact build-time flow libva uses: write kernel as `.g4a`, run assembler, `#include` the `.g4b.gen5` output as a `uint32_t kernel[][4]` array in C. **Zero precompiled blobs ever ship; binaries are regenerated on every build.**

Invocation: `intel-gen4asm -g5 -o kernel.g4b.gen5 kernel.g4a` (`-g5` selects the target generation).

---

## 2. Composition of the codebase

Seventeen thousand lines is a lot, but it decomposes cleanly.

### 2.1 Core assembler (must keep)
| File | Lines | Role |
|---|---:|---|
| `gram.y` | 3398 | Bison grammar for the EU assembly language |
| `lex.l` | 487 | Flex tokenizer |
| `main.c` | 478 | Entry point, argument parsing, I/O |
| `gen4asm.h` | 231 | Parser/AST data structures |
| `brw_eu_emit.c` | 2627 | Instruction-to-DWORD encoder (the real engine) |
| `brw_eu.c` | 268 | Instruction list / compile context |
| `brw_eu.h` | 427 | Encoder API |
| `brw_eu_util.c` | 125 | Small helpers |
| `brw_reg.h` | 808 | Register-class data types |
| `brw_structs.h` | 1493 | `union brw_instruction` — the 128-bit instruction bitfields |
| `brw_defines.h` | 1652 | Opcode constants, enum values |
| `brw_context.{c,h}` | 122 | Target-gen context |
| `brw_compat.h` | 69 | Mesa-style helper macros (`likely`, `ARRAY_SIZE`, …) |
| `ralloc.{c,h}` | 886 | Hierarchical arena allocator (self-contained, no deps) |
| **Core total** | **~13,000** | |

### 2.2 Optional (can be removed for Haiku)
| File | Lines | Role | Needed for Gen5? |
|---|---:|---|---|
| `brw_disasm.c` | 1348 | Gen4–7 disassembler | No — used only by `intel-gen4disasm` tool; useful for debug |
| `disasm-main.c` | 177 | Disassembler CLI entry | No |
| `brw_eu_compact.c` | 810 | Gen6+ instruction compression | No — early-returns at `brw_eu_compact.c:682` when `gen < 6` |
| `brw_eu_debug.c` | 92 | Debug printer | Optional |
| `gen8_disasm.c` | 990 | Gen8 disassembler | No |
| `gen8_instruction.{c,h}` | 807 | Gen8 instruction type (different encoding) | No |
| **Optional total** | **~4,200** | | |

### 2.3 Conclusion on scope
- **If we cut Gen8 + compact + disasm**: ~13,000 lines. All essential, all required.
- **If we also keep the disassembler** (strongly recommended for debugging compiled kernels when we hit hangs): ~14,500 lines.
- **If we keep everything** as a straight port, including Gen8 paths we'll never exercise: ~17,300 lines, but the extra code is dead on Gen5 targets so the complexity cost is low.

Recommendation: **start with everything**, remove Gen8 later only if maintenance friction appears. The Gen8 code is cleanly isolated and doesn't complicate the Gen5 paths.

---

## 3. Dependency analysis — what gen4asm needs to build

### 3.1 System headers used (from grep of all `.c` / `.h` files)
```
<assert.h>   <inttypes.h>  <stdarg.h>   <stdbool.h>
<stddef.h>   <stdint.h>    <stdio.h>    <stdlib.h>
<string.h>   <getopt.h>    <unistd.h>
```

**All of these are ISO C99 or POSIX, available on Haiku out of the box.**

Nothing Linux-specific:
- No `<sys/mman.h>`, `<sys/ioctl.h>`, `<linux/*>`
- No `<endian.h>`, `<byteswap.h>`
- No `<pthread.h>`
- No libdrm, no Mesa, no X11
- No `pkg-config` linkage at runtime (the `intel-gen4asm.pc.in` is only for downstream consumers of the assembler as a library, which we don't care about)

### 3.2 Build-time tools
- **flex** — ✅ present at `/bin/flex` on the user's Haiku system
- **bison** — ✅ present at `/bin/bison`
- **A C compiler** — ✅ gcc/clang, already in use for the accelerant

### 3.3 Runtime dependencies
**None.** It's a pure compile-time tool. The output is a text file with C array initializers. After running `gen4asm` at build time, the resulting `.g4b.gen5` is `#include`d into our driver source like any ordinary header.

This is the best possible situation. The assembler is entirely decoupled from the runtime driver:

```
      build time                         runtime
  ┌──────────────────┐              ┌─────────────────┐
  │ hello.g4a  ─┐    │              │                 │
  │             │    │              │                 │
  │  gen4asm ───┤    │              │                 │
  │   (Haiku) ──┤    │              │                 │
  │             ↓    │              │                 │
  │ hello.g4b.gen5 ──┼────#include──→ intel_extreme   │
  │                  │              │ .accelerant    │
  └──────────────────┘              └─────────────────┘
```

---

## 4. Haiku-specific porting changes

This is the honest punch list of what needs to be touched. Shorter than expected.

### 4.1 Build system
- Meson is available on Haiku but we're not using it for the accelerant (pure `Makefile`). Port the short `meson.build` to a makefile fragment under `intel_extreme/accelerant/tools/gen4asm/`.
- Need rules for flex/bison code generation: `lex.l → lex.c`, `gram.y → gram.c gram.h`.
- Typical bmake / GNU make rules, ~30 lines.

### 4.2 Source changes
Based on the dependency analysis above, expected touches are minimal:

1. **Check for `#include <sys/types.h>` / `<unistd.h>` portability**. Haiku's `unistd.h` is POSIX-compliant; `getopt_long` is present. Expect zero changes, but verify.

2. **`ralloc.c` expects a standard malloc**. Haiku's libroot malloc is fine. No change.

3. **Bison grammar may emit `YYSTACK_USE_ALLOCA`**. On Haiku, `alloca` lives in `<alloca.h>` rather than `<stdlib.h>` in some configurations. May need a `%{ #include <alloca.h> %}` in `gram.y`'s prologue, or set `%define api.pure` + explicit stack allocation. Quick fix if it comes up.

4. **Integer types**. The code uses `uint32_t` everywhere (from `<stdint.h>`). No `u_int32_t` or BSD types. Clean.

5. **Endianness**. The EU instruction encoding is little-endian in memory, matching the GPU. The encoder writes bytes via `uint32_t` stores, so on an x86 host (little-endian) it is a direct memory layout match. On any other host you'd need byteswaps. **We are x86, so no change.**

6. **Locale / printf formats**. `PRIx32` / `PRIu32` from `<inttypes.h>` are used consistently. Should work unchanged.

### 4.3 Assembler bugs we should know about upfront
`TODO` file says:
- "support labels for branch/jump instruction destinations" — partial. The test suite's broken tests (`test/jmpi`, `test/if`, `test/while`, …) use **numeric offsets**. Labels via `reloc_target` in `gen4asm.h:145` and the relocation pass in `main.c:431–448` **do work** — libva's real-world MPEG-2 kernels (`field_intra.g4a` et al.) use `jmpi LABEL;` successfully. So:
  - Label-based branches: ✅ work
  - Numeric-offset branches: ❌ known broken

**Impact on us**:
- Phase 1 hello-world kernel: **no branches at all**, irrelevant.
- Phase 2 MPEG-2 kernels: use labels — ✅ fine.
- Phase 4 LLM kernels (matmul loops): use labels — ✅ fine.

The "broken tests" list is not blocking for our roadmap. We note it and move on.

### 4.4 m4 preprocessor dependency (unexpected finding)
Reading `libva_intel/src/shaders/mpeg2/vld/field_intra.g4a` revealed that libva runs `.g4a` files through **m4** before feeding them to gen4asm:
```
include(`iq_intra.g4i')
define(`ROW_SHIFT', `11UD')
```

This is a libva build-system convention, not a gen4asm feature. gen4asm itself knows nothing about `include` or `define`.

**Impact**: if we want to rebuild libva's MPEG-2 kernels from source on Haiku (phase 2 goal), we need m4. Haiku has m4 available via haikuports. For our own kernels (hello world, matmul, etc.) we can simply **not use m4** and keep `.g4a` files flat — it's optional.

Decision: **don't depend on m4 for our own kernels.** If/when we rebuild libva MPEG-2 kernels from source, add m4 as a build-time dep then. For phase 1 and simple phase 4 kernels, pure gen4asm is enough.

---

## 5. Verification strategy — how we know the port is correct

This is the part where the `test/` directory saves us. The upstream ships reference `.g4a` inputs and `.expected` output files. Our port is correct iff `haiku-gen4asm -g5 test/mov.g4a` produces byte-identical output to `test/mov.expected`.

### 5.1 Baseline verification (must pass before declaring the port done)
Run all tests listed as passing in `meson.build`:
```
mov, frc, regtype, rndd, rndu, rnde, rnde-intsrc, rndz, lzd, not, immediate
```
These use no branches, no labels, no declarations — basic arithmetic and move instructions. Getting all 11 to produce byte-identical output is the definition of "port works".

### 5.2 Gen5-specific verification
The tests target the default gen (Gen4). We need a second pass:
1. Pick a simple test (e.g. `mov.g4a`).
2. Run `haiku-gen4asm -g5 -o mov.gen5.out mov.g4a`.
3. Compare against a reference Gen5 output. If we don't have one, run the same on a Linux machine with upstream intel-gen4asm and commit the result to our tree as the Gen5 golden output. One-time cost.

### 5.3 Round-trip verification against libva
Higher-confidence check: take one of libva's existing `.g4a` files (say `libva_intel/src/shaders/mpeg2/vld/field_intra.g4a` after m4 expansion), run it through our ported assembler with `-g5`, compare the output byte-for-byte against the committed `libva_intel/src/shaders/mpeg2/vld/field_intra.g4b.gen5`. If they match, our assembler **produces byte-identical binaries to the ones libva actually runs on real Ironlake hardware in production**. That is the strongest possible validation short of executing on-GPU.

### 5.4 On-GPU verification (the real proof)
The moment of truth is of course phase 1.1 from `MEDIA_PIPELINE_BRINGUP.md`: assemble a three-instruction hello-world kernel with our port, submit it via MEDIA_OBJECT, watch the output BO change from `0xdeadbeef` to `0xcafef00d`. Until that happens we don't know for certain that any part of the chain is correct. But §5.3 is the best pre-GPU check we can do.

---

## 6. Integration plan

Phased so each step has an independently checkable outcome. Estimate: **total 1–3 days of focused work.**

### Phase I.A — Copy, isolate, build (0.5–1 day)
1. Create `intel_extreme/accelerant/tools/gen4asm/` directory in our tree.
2. Copy the ~13 core files from `gen5_docs/igt/assembler/` into it. Keep file-level SPDX/MIT headers intact.
3. Write a small Makefile that:
   - Runs `bison -d gram.y -o gram.c`
   - Runs `flex -o lex.c lex.l`
   - Compiles all `.c` files into an executable `gen4asm` (or `haiku-gen4asm`).
4. Fix any Haiku-specific compile errors that surface (expect: 0–3 small issues, most likely around `alloca.h` or YY stack).
5. Verify: `gen4asm --version` runs, `gen4asm test/mov.g4a` produces output.

**Done when**: binary builds and accepts input without crashing.

### Phase I.B — Correctness (0.5 day)
1. Copy `test/` directory into tree (or symlink from reference).
2. Write a one-line shell script / make target that runs all 11 passing test cases and diffs against `.expected`.
3. Fix any byte-level discrepancies. Most likely: none on x86 Haiku since endianness and C semantics match upstream.
4. Generate Gen5 golden outputs for the 11 cases as a committed reference (cross-validated against upstream on Linux once, then permanently in-tree).

**Done when**: all 11 baseline tests pass byte-identical on Haiku, plus Gen5 golden files committed.

### Phase I.C — Real-world validation (0.5–1 day)
1. Take `libva_intel/src/shaders/mpeg2/vld/field_intra.g4a`.
2. Run m4 preprocess (or flatten manually for this one test).
3. Assemble with our port at `-g5`.
4. `cmp` against `libva_intel/src/shaders/mpeg2/vld/field_intra.g4b.gen5`.
5. If byte-identical: the assembler is production-quality for our target. If not: debug the single failing encoding.

**Done when**: at least one libva Gen5 kernel round-trips through our port byte-identical.

### Phase I.D — Integration into accelerant build (0.5 day)
1. Add make rule: `%.g4b.gen5: %.g4a → tools/gen4asm/gen4asm -g5 -o $@ $<`
2. Create `intel_extreme/accelerant/kernels/` directory for our `.g4a` sources.
3. Drop in a `hello_world.g4a` (3 instructions) and verify it assembles cleanly as part of `make`.
4. Reference the resulting `hello_world.g4b.gen5` in an `#include` in a new `render_compute.cpp` (stub for now).

**Done when**: `make clean && make` rebuilds the accelerant including the hello-world kernel binary, with zero external toolchain beyond flex/bison.

---

## 7. What we ship, what we don't

After this port:

**In our driver tree** (all MIT, with attribution, authored by Eric Anholt / Keith Packard / various Intel):
- `tools/gen4asm/*.c *.h *.l *.y` — the assembler itself
- `tools/gen4asm/test/*` — baseline correctness tests
- `kernels/*.g4a` — kernel source we write ourselves

**Generated at build time, never committed as binary**:
- `tools/gen4asm/lex.c`, `gram.c`, `gram.h` — flex/bison outputs
- `tools/gen4asm/gen4asm` — the assembler binary
- `kernels/*.g4b.gen5` — the assembled EU machine code

**Build-time dependencies** (on the developer's Haiku machine):
- `flex`, `bison` — already installed per the check in §3.2
- `gcc` (already required by the accelerant)
- `m4` — **only if** we later choose to rebuild libva MPEG-2 kernels from source rather than using libva's committed `.g4b.gen5`. Not required for phase 1.

**Runtime dependencies on the end-user machine**: none. The user-visible `intel_extreme.accelerant` binary has zero new runtime links.

**Linux dependencies at any point**: none. Nothing here ever touches a Linux machine, a Linux kernel, or a Linux binary. The only time we'd read Linux code is if we needed to check an obscure quirk in i915 — strictly read-only, understanding-only.

This delivers exactly what the earlier conversation asked for: **a fully MIT, firmware-free, blob-free, Linux-free driver stack**, with the kernel toolchain self-hosted in-tree.

---

## 8. Risks and unknowns

Honest list. None is a showstopper.

**8.1 Flex/bison version drift.** The grammar was written against flex 2.5.x and bison 2.x. Modern distributions ship flex 2.6 and bison 3.8, which have minor incompatibilities (`%define api.pure` syntax changed, reentrant scanner macros, etc.). Mesa-side files typically compile fine with modern versions; IGT-side gen4asm has not been actively maintained, so there may be small warnings. Fixable within an hour if they surface.

**8.2 The "broken tests" are more broken than they look.** Specifically, if matmul kernels turn out to need numeric-offset branches (unlikely but possible), we'd have to fix that path in the assembler. Looking at `main.c:431–448`, the relocation logic is clean and the issue is probably just the parser accepting the numeric form. Worst case: small grammar fix.

**8.3 m4 dependency for rebuilding libva MPEG-2 kernels from source.** Deferred to phase 2. If/when we want to rebuild rather than use libva's committed `.g4b.gen5`, install m4 via haikuports. One-line decision, not a design problem.

**8.4 bison-generated code may use `YYMALLOC` / `YYFREE` in unexpected ways.** If the generated parser on Haiku behaves differently, we may need a `%define api.pure` and manual memory management. Typical workaround takes ~30 min.

**8.5 No Gen5 golden output in the upstream test suite.** The 11 passing tests target the default generation (Gen4). We have to either (a) cross-validate against Linux once, (b) trust §5.3's round-trip against libva's committed binaries, or (c) both. Recommend both.

**8.6 Large-scale assembly (thousands of instructions) untested.** Upstream intel-gen4asm has been used almost exclusively for small video-codec kernels (hundreds of instructions max). If phase 4 LLM kernels get larger (matmul inner loops, softmax reductions), we may hit scaling issues. I'd expect none, but we'd find out in phase 4, not phase 1.

---

## 9. Where this sits in the overall plan

Cross-referencing `VIDEO_DECODE_PIVOT.md` and `MEDIA_PIPELINE_BRINGUP.md`:

> **Phase 0 — Infrastructure**
>  1. Freeze the 3D pipeline debugging.
>  2. Debug instrumentation.
>  3. GTT memory allocator.
>  4. **Port `intel-gen4asm` into the tree. ← this document describes step 4.**

This is literally the step we're on. The prerequisites upstream (frozen 3D work, instrumentation, allocator) are independent workstreams — the gen4asm port can start immediately because it's a build-time tool with no runtime dependencies on the driver itself. We can even build and validate the assembler **before** the allocator is ready, using off-line tests against libva's committed `.g4b.gen5` binaries.

Practically that means: **this is the work item with the lowest coupling to anything else in the project, and therefore the safest first concrete move.** If it succeeds, we have the toolchain ready and waiting for phase 1.A/B/C from `MEDIA_PIPELINE_BRINGUP.md`. If it fails in some unexpected way, we learn early and re-plan without having wasted effort on allocator or bring-up work that would have assumed it.

---

## 10. Summary

- **Total port effort**: 1–3 days of focused work.
- **Dependencies**: flex, bison (already installed), standard C library (already used).
- **Runtime cost**: zero — it's a build-time tool that produces C header-like output.
- **License**: MIT throughout. Attribution required, compatible with Haiku.
- **Validation**: 11 upstream test cases + round-trip against libva's own Gen5 binaries + final on-GPU test in phase 1.1.
- **Known limitations**: numeric-offset branches broken (labels work), m4 preprocessing required only for rebuilding libva's kernels from source (optional), Gen8 code paths included but unused.
- **What it unlocks**: the ability to write new EU kernels in human-readable assembly, assemble them in-tree with zero Linux involvement, and regenerate them deterministically on every build. This is the foundation for phases 1 (hello world), 2 (video decode kernels), and 4 (LLM kernels).

**Recommendation**: proceed with the port as the first concrete implementation work. It's the lowest-risk, highest-leverage step available to us right now. Everything downstream depends on it, nothing upstream blocks it.
