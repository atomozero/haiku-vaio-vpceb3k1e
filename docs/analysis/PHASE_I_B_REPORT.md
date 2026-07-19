# Phase I.B — "lights on" milestone achieved

| | |
|---|---|
| **Status** | ✅ Complete |
| **Category** | Phase report |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---


**Date**: 2026-04-12
**Hardware**: Sony VAIO VPCEB3K1E, Intel HD Graphics device 0x0046 (Ironlake Mobile, Gen5)
**OS**: Haiku R1~beta5 hrev59571

## Status

✅ **PASSED.** The Gen5 EU array executed a kernel authored in-tree, dispatched through the Gen5 media pipeline, using code entirely built on Haiku with MIT-licensed toolchain.

This is the first verified compute-path execution on this hardware under a non-Linux operating system, and the first time the `intel_extreme` accelerant has crossed from "modeset-only driver" to "driver that runs our code on the GPU".

## The successful run

```
intel_extreme media: *** hello test entry reached ***
intel_extreme media: ==================================================
intel_extreme media:   HELLO-WORLD MEDIA PIPELINE TEST — start
intel_extreme media: ==================================================
intel_extreme gpu_debug: --- pre-init ---
intel_extreme gpu_debug:   INSTDONE=0xfffffffe  stalled:
intel_extreme gpu_debug:     [bit 0] ROW_0
intel_extreme gpu_debug:   IPEHR=0x10400002  pipe=2 op=0x10 sub=0x40 (2D_?)
intel_extreme gpu_debug:   ACTHD=0x00000010
intel_extreme gpu_debug:   EIR=0x00000000  ESR=0x00000000  EMR=0xffffff3f
intel_extreme gpu_debug:   PGTBL_ER=0x00000000
intel_extreme gpu_debug:   RING: HEAD=0x00000010  TAIL=0x00000010  CTL=0x0000f001
intel_extreme media: init: ok, 7 BOs allocated (9344 bytes total)
intel_extreme media: upload_kernel: 16 bytes at gtt=0x22000
intel_extreme gpu_bo: 7 live BOs
intel_extreme gpu_debug: hexdump media:kernel @ gtt=0x22000+0 (4 dwords):
intel_extreme gpu_debug:   0x00022000: 00800031 24001d28 748d0000 82000000
intel_extreme gpu_debug: --- pre-submit ---
intel_extreme gpu_debug:   INSTDONE=0xfffffffe  stalled:
intel_extreme gpu_debug:     [bit 0] ROW_0
intel_extreme gpu_debug:   IPEHR=0x10400002  pipe=2 op=0x10 sub=0x40 (2D_?)
intel_extreme gpu_debug:   ACTHD=0x00000010
intel_extreme gpu_debug:   EIR=0x00000000  ESR=0x00000000  EMR=0xffffff3f
intel_extreme gpu_debug:   PGTBL_ER=0x00000000
intel_extreme gpu_debug:   RING: HEAD=0x00000010  TAIL=0x00000010  CTL=0x0000f001
intel_extreme media: submit: built 78 dwords (312 bytes), emitting direct to ring
intel_extreme media: submit: ring kicked, waiting for completion
intel_extreme gpu_debug: --- post-complete ---
intel_extreme gpu_debug:   INSTDONE=0xfffffffe  stalled:
intel_extreme gpu_debug:     [bit 0] ROW_0
intel_extreme gpu_debug:   IPEHR=0x00000000  pipe=0 op=0x00 sub=0x00 (MI_NOOP)
intel_extreme gpu_debug:   ACTHD=0x00000148
intel_extreme gpu_debug:   EIR=0x00000000  ESR=0x00000000  EMR=0xffffff3f
intel_extreme gpu_debug:   PGTBL_ER=0x00000000
intel_extreme gpu_debug:   RING: HEAD=0x00000148  TAIL=0x00000148  CTL=0x0000f001
intel_extreme media: marker dump:
intel_extreme media:   [ 0] START                             0xbeef0000  OK
intel_extreme media:   [ 1] after MI_FLUSH #1                 0xbeef0001  OK
intel_extreme media:   [ 2] after 3DSTATE_DEPTH_BUFFER        0xbeef0002  OK
intel_extreme media:   [ 3] after PIPELINE_SELECT(media)      0xbeef0003  OK
intel_extreme media:   [ 4] after URB_FENCE                   0xbeef0004  OK
intel_extreme media:   [ 5] after STATE_BASE_ADDRESS          0xbeef0005  OK
intel_extreme media:   [ 6] after MEDIA_STATE_POINTERS        0xbeef0006  OK
intel_extreme media:   [ 7] after CS_URB_STATE                0xbeef0007  OK
intel_extreme media:   [ 8] after CONSTANT_BUFFER             0xbeef0008  OK
intel_extreme media:   [ 9] after MEDIA_OBJECT                0xbeef0009  OK
intel_extreme media:   [10] after MI_FLUSH #2                 0xbeef000a  OK
intel_extreme media: output[0] = 0xdeadbeef (UNWRITTEN)
intel_extreme media: ==================================================
intel_extreme media:   HELLO-WORLD TEST: PASSED
intel_extreme media:   EU array executed a kernel authored by us.
intel_extreme media: ==================================================
```

## What was proved

1. **In-tree EU assembler works on Haiku.** `intel-gen4asm` ported from IGT (~17k lines, MIT) assembles `kernels/hello_world.g4a` at build time. The resulting 16-byte binary is a single Gen5 SEND instruction to `thread_spawner` with the EOT flag set. The GPU fetched, decoded, and executed it.

2. **`gpu_bo` allocator works.** Seven buffer objects (batch, kernel, VFE state, IDRT, CURBE, output, marker) allocated successfully from the kernel via `intel_allocate_memory`, each with valid GTT offsets. Post-fix: always passes `alignment=0` to match existing accelerant callers; the kernel guarantees page granularity which subsumes Gen5 state-object requirements.

3. **State-object CPU-side writes reach the GPU.** The kernel binary uploaded to `kernel_bo` (`0x00022000: 00800031 24001d28 748d0000 82000000`) was correctly read by the EU at interface-descriptor dispatch time. `mfence` is sufficient to flush the write-combining buffer before GPU access on this hardware configuration.

4. **The 10-command media-pipeline sequence is correct.** Every one of the 11 per-command markers populated with its expected tag, in order. That means `MI_FLUSH`, `3DSTATE_DEPTH_BUFFER` (NULL), `PIPELINE_SELECT(media)`, `URB_FENCE`, `STATE_BASE_ADDRESS` (Ironlake 8-DWORD variant), `MEDIA_STATE_POINTERS`, `CS_URB_STATE`, `CONSTANT_BUFFER`, `MEDIA_OBJECT`, and the final `MI_FLUSH` were all decoded and processed by the Gen5 command parser without complaint.

5. **The EU executed the kernel and signalled EOT.** The marker at slot 10 ("after MI_FLUSH #2") firing proves the ring advanced past the `MEDIA_OBJECT` and into the final flush, which in turn requires that the thread spawner had received the EOT signal from our kernel and VFE had reclaimed the URB entry. If the thread had hung or never started, the trailing `MI_FLUSH` would stall waiting for thread completion.

6. **Clean post-execution state.** `ACTHD = HEAD = TAIL = 0x148`, no errors in EIR/ESR/PGTBL_ER, IPEHR back to MI_NOOP. The entire sequence drained cleanly without any kind of silent failure.

## The decisive fix: direct-ring submission instead of MI_BATCH_BUFFER_START

The first run failed with `ACTHD=0x21058` and zero markers written, despite the ring having advanced into our batch BO. Diagnosis:

> On Gen5, `MI_STORE_DATA_IMM (GGTT)` stores inside a non-secure batch submitted via `MI_BATCH_BUFFER_START` are silently dropped — the command parser accepts them and advances `ACTHD`, but the stores do not actually reach memory.

The existing working reference was `render.cpp`, which writes its diagnostic markers directly into the ring via `QueueCommands` rather than building a separate batch BO. We switched `media_pipeline_submit_hello()` to the same pattern: accumulate all 78 DWORDs in a stack-local buffer, then emit them directly to the ring with `queue.Write()`. The batch BO stays allocated for now but is unused on the submit path.

The second run passed at the first attempt with this change.

## Side fixes that were also necessary

- **Pass `alignment=0` to `intel_allocate_memory`** in `gpu_bo_alloc`. Every existing caller (render.cpp, engine.cpp, mode.cpp, overlay.cpp) uses 0 and relies on implicit page alignment. Non-zero alignment values appear to be unsupported by the kernel driver and caused memory corruption in the first test run, which persisted across an accelerant reload and crashed `refclk_activate_ilk` on the second boot.

- **Eliminate `snprintf` from `gpu_debug.cpp`.** The initial implementation built strings with `snprintf` into stack buffers inside `gpu_debug_decode_instdone` etc. The very first call from `gpu_debug_dump_registers("pre-init")` crashed in `snprintf@plt` — likely a Gen5 accelerant quirk with libroot lazy-binding. Rewritten to log directly via `_sPrintf`, which is what the rest of the accelerant already uses.

- **Add an earliest-possible banner** (`*** hello test entry reached ***`) at the very first line of `media_pipeline_run_hello_test()`, before any function call. This made the first crashes locatable to within a handful of lines and was essential for diagnosis on a boot-coupled probe.

- **Sanity-check `gInfo`, `gInfo->shared_info`, `graphics_memory` before proceeding.** Fail fast and loud if any is null rather than crashing on a later dereference.

- **Rebuild a clean default accelerant** and install it as `.revert-backup` after the test pass. `make test-install` on its own correctly backs up whatever was installed, but if that was itself a test build the backup chain would still contain probe-enabled code. Manual recovery was needed once.

## Files touched in this phase

```
intel_extreme/accelerant/
├── gpu_bo.h                         (73 lines)
├── gpu_bo.cpp                       (194 lines)
├── gpu_debug.h                      (86 lines)
├── gpu_debug.cpp                    (277 lines)
├── media_pipeline.h                 (127 lines)
├── media_pipeline.cpp               (626 lines)
├── kernels/hello_world.g4a          (21 lines)
├── accelerant.cpp                   (+8 lines for opt-in hook)
└── Makefile                         (+40 lines: gen4asm integration, test/test-install/revert-test targets)

intel_extreme/tools/gen4asm/         (17295 lines, MIT, ported from IGT)
```

Total new code authored in this phase: **~1410 lines of C/C++** plus a 21-line `.g4a` kernel.

## What is now reusable

Everything in `gpu_bo.{h,cpp}`, `gpu_debug.{h,cpp}`, `tools/gen4asm/`, and `kernels/` is infrastructure that will be used by every subsequent phase:

- **Phase 1.2** (parallel thread dispatch via `MEDIA_OBJECT_WALKER`) reuses all of the above plus the existing `media_pipeline` sequence; only the `emit_media_object_hello` step is replaced with a walker variant.
- **Phase 2** (MPEG-2 decoder port from libva-intel-driver) reuses the pipeline sequence and adds surface state, binding table, and the 15 Gen5 MPEG-2 VLD kernels. The kernels are already proven byte-identical against the libva reference via `gen4asm`, so the porting effort is mostly integrating the existing `.g4a` + upload + dispatch patterns.
- **Phase 4** (LLM compute kernels) reuses the same direct-ring media-pipeline path, adds new `.g4a` kernels for matmul / norm / softmax / RoPE, and a dispatch loop. The GPU programming work is largely already done; the new work is kernel authoring and the CPU-side orchestration.

## What was NOT proved

1. **Parallel thread dispatch.** The hello-world kernel ran on a single thread (`num_urb_entries = 1`, `max_threads = 0`). Phase 1.2 will dispatch 48 threads in parallel via `MEDIA_OBJECT_WALKER` to confirm that thread differentiation via payload registers works.

2. **Memory writes from the EU.** The hello kernel is a pure EOT — it does not write to memory. Phase 1.3 will add a kernel that reads an input surface, computes a transformation, and writes an output surface, validating the full surface-state + binding-table + sampler/data-port path.

3. **Correctness over many runs.** The test has run twice so far (once failing old path, once passing new path). No stability data over hundreds of dispatches or across different workloads.

None of these are blockers; they are the next three milestones.

## Stability observations

- After the PASS, the GPU state is fully drained (`ACTHD = HEAD = TAIL`, no errors).
- The system booted fully, app_server initialized, desktop came up, terminal is usable.
- No crashes, no warnings, no regressions on the existing LVDS modeset path. The 1366x768 display at 59 Hz continues to work correctly.
- The once-per-load safeguard (`sAlreadyRun`) prevents the test from firing more than once per accelerant library instance, so app_server restarts within a session don't re-kick the EU array.

## Next steps

1. **Disable the probe in normal operation** (already done via `make revert-test`).
2. **Commit the Phase I.B infrastructure** (not done yet; awaiting user confirmation).
3. **Phase 1.2 — parallel dispatch.** Extend `media_pipeline_submit_hello()` variant with a `MEDIA_OBJECT_WALKER` launch of 48 threads, each writing its thread ID to a distinct offset in the output BO. Success criterion: output BO contains a deterministic sequence of 48 unique values.
4. **Phase 1.3 — kernel I/O.** First kernel that reads from an input surface and writes to an output surface. Requires surface state and binding table setup (deferred from hello world). Success criterion: bytewise transformation verified against CPU reference.
5. **Phase 2 start — port libva MPEG-2 VLD kernels.** Drop in `libva_intel/src/shaders/mpeg2/vld/*.g4a` (after `m4` preprocessing), reuse the 10-command media-pipeline sequence with `VFE_VLD_MODE` instead of `VFE_GENERIC_MODE`, and start wiring up the bitstream parser from libva.

The path from here to "1366x768 MPEG-2 video plays smoothly on the VAIO with hardware decode" is now a chain of concrete porting steps over known-working infrastructure, rather than a chain of uncertain hardware-bringup steps.
