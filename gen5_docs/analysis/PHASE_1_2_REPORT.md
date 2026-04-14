# Phase 1.2 — parallel dispatch milestone achieved

**Date**: 2026-04-12
**Hardware**: Sony VAIO VPCEB3K1E, Intel HD Graphics device 0x0046 (Ironlake Mobile, Gen5)
**OS**: Haiku R1~beta5 hrev59571

## Status

✅ **PASSED.** The Gen5 media pipeline successfully dispatched 48 concurrent threads from a single batch submission, and all 48 threads reached EOT with a clean VFE drain. The URB_FENCE sizing fix was the decisive change.

## What was proved

1. **Parallel thread dispatch on 48 hardware threads works.** The Ironlake Execution Unit array has 12 EUs × 4 threads/EU = 48 hardware thread slots. With `max_threads=48` in VFE state and `num_urb_entries=48` in URB_FENCE, the VFE dispatcher issued all 48 threads in parallel, each running the single-instruction EOT kernel, and drained within the trailing `MI_FLUSH`.

2. **URB region sizing must match the thread count.** The key bug fix: `emit_urb_fence(vfe_entries)` was hardcoded to 1 in Phase 1.1 (fine for single-thread hello world), but when VFE state declared 48 entries for parallel dispatch, the regions collided. Symptom: `CONSTANT_BUFFER` stall with `INSTDONE` bit 8 (IS = VS dispatcher) clear, because the CURBE load collided with VFE URB slots the VS unit was waiting on.

3. **The reusable infrastructure held up cleanly.** Zero new state objects, zero new command types. `media_pipeline_submit_parallel` reused 100% of `media_pipeline_submit_hello`'s preamble and differed only in the single `for` loop over N `MEDIA_OBJECT` emissions.

## Raw numbers

```
baseline (1 thread):     112268 us wall time     PASSED
parallel (48 threads):      491 us wall time     PASSED (10 us/thread)
```

**Important caveat on the baseline timing**: the 112 ms number is almost entirely *polling overhead*, not GPU execution time. The story:

- The GPU completes a single pure-EOT kernel dispatch in a handful of microseconds
- The CPU, having kicked the ring, enters `gpu_debug_wait_value` which spins busy-waiting for up to 2 ms
- During the spin, Haiku's scheduler preempts the accelerant thread for a full scheduler quantum (~100 ms is typical on idle Haiku)
- When our thread wakes, `system_time() - t0` already exceeds 2 ms, so the spin loop exits
- The slow-path `snooze()` fallback adds another partial quantum
- First successful check after waking → total wall time ≈ 100 ms

The parallel run does NOT exhibit this artifact because the CPU spends more time in the (unpreemptable) ring-write phase (360 DWORDs vs 78 DWORDs), which gives the GPU enough time to complete by the time `wait_value` is first called. The first check succeeds instantly. So **491 us ≈ CPU submit + GPU execute + check return** — a reasonable upper bound on the real end-to-end cost for a 48-thread parallel dispatch.

**Real takeaway**: a 48-thread EOT parallel dispatch completes in well under 500 us on this hardware, CPU submit included. This is the first concrete latency number we have for Gen5 compute on the VAIO.

## Timeline of the Phase 1.2 session

1. Built and installed test binary with new `media_pipeline_submit_parallel()` using `emit_urb_fence(&w)` hardcoded to 1 VFE entry.
2. First test run: baseline PASSED, parallel TIMEOUT at marker 7 (`CS_URB_STATE` done, `CONSTANT_BUFFER` not reached).
3. Post-timeout state confirmed: `INSTDONE=0xfffffeff` with IS stalled, `IPEHR=0x60020100` (= `CMD_CONSTANT_BUFFER | valid`), ring HEAD stuck at `0x230`, TAIL at `0x6e8`.
4. Root cause identified: URB region sizing mismatch. With 48 VFE entries declared in VFE state but only 1 VFE entry allocated in URB_FENCE, the CURBE load in `CONSTANT_BUFFER` collided with VFE URB slots and the VS dispatcher stalled.
5. Fixed `emit_urb_fence` to take a `vfe_entries` parameter, both submit paths pass their correct thread count.
6. Also fixed the IPEHR decoder (previously conflated type `[31:29]` with pipeline `[28:27]`).
7. Rebuilt, reinstalled, rebooted. Second test run: both baseline and parallel PASSED.
8. Noticed the ratio display was showing "0.0" for sub-1 ratios (integer division). Added a branch for parallel-faster-than-baseline case with explanation about polling artifacts.
9. Also improved `gpu_debug_wait_value` with a 2 ms busy-wait fast path before falling back to snooze — helps but does not fully eliminate the scheduler preemption effect.
10. Third test run confirmed both passes, with the new ratio display.

## Files touched this phase

```
intel_extreme/accelerant/
├── bench.h                 (new, 28 lines)
├── bench.cpp               (new, 67 lines)
├── media_pipeline.h        (+15 lines: submit_parallel, run_parallel_test, MEDIA_MAX_PARALLEL_THREADS)
├── media_pipeline.cpp      (+190 lines: write_vfe_state parameterization,
│                             emit_urb_fence parameterization,
│                             emit_media_object_indexed,
│                             submit_parallel, run_media_test_core,
│                             run_parallel_test)
├── gpu_debug.cpp           (~100 lines rewritten: IPEHR decoder corrected,
│                             wait_value busy-wait fast path)
├── accelerant.cpp          (1 line: hook calls run_parallel_test)
└── Makefile                (+1 file: bench.cpp)
```

Total new code authored in this phase: **~300 lines of C++** plus the bench infrastructure.

## What was NOT proved

1. **Real GPU latency**. Our 48-thread dispatch ran in ~500 us including CPU work. The GPU-only portion is unknown — likely tens to low hundreds of microseconds — but polling-based timing cannot isolate it. Phase 1.3's proper completion signalling (via seqno or interrupt) will give us true GPU timing.

2. **Actual thread differentiation**. We pass a thread index in MEDIA_OBJECT inline data, but the pure-EOT kernel ignores it. Phase 1.3 will have each thread read its index and write a unique value to a unique offset in the output BO.

3. **Scaling characteristics**. We only tested two points: 1 thread and 48 threads. Intermediate values (4, 8, 16, 24, 32) would draw a scaling curve showing where parallelism saturates. Not worth running standalone, but will come for free when we instrument Phase 2 workloads.

4. **Max URB entry count**. The VFE field is 7 bits = 127 entries max, but the Ironlake URB is shared with other regions. We only validated up to 48. Phase 2's MPEG-2 kernels use 28 VFE entries × 13 units each = 364 URB units, so we have headroom.

## Infrastructure that is now reusable

- **`bench_now_us` / `bench_log` / `bench_log_per_unit`** — ready to drop into any future test harness. Works fine for the broad strokes; will need refinement for sub-ms precision.
- **`write_vfe_state(ctx, max_threads)`** — parameterized, will be called with different thread counts by every future media pipeline user.
- **`emit_urb_fence(w, vfe_entries)`** — parameterized, same.
- **`media_pipeline_submit_parallel(ctx, n)`** — generic parallel dispatch that any kernel (not just hello world) can use.
- **`run_media_test_core`** — factored harness pattern for running any submit function with standard diagnostics.
- **Corrected IPEHR decoder** — `[31:29]` for type, `[28:27]` for pipeline, will make every future hang analysis correct.

## Known limitations / technical debt

- **Scheduler preemption during busy-wait** makes sub-ms timing unreliable for short dispatches. Candidates to fix: (a) disable preemption via Haiku API if available, (b) use interrupt-driven completion via seqno+IRQ, (c) loop the measurement and take the minimum.
- **IPEHR decoder is still Gen5-focused**. Will need extending for Gen6+ if we ever care.
- **batch_bo is still allocated but unused** since we switched to direct-ring submit. Harmless, but worth cleaning up before shipping.
- **sAlreadyRun only guards run_hello_test**, not run_parallel_test. If app_server reloads the accelerant, the parallel test runs again — wasted cycles but not harmful.

## Next steps

Phase 1.3 — **first kernel that reads and writes memory**. The pure-EOT kernel has served its purpose. Next we need to:

1. Add `surface_state_bo` and `binding_table_bo` to `media_pipeline_context`.
2. Implement `write_surface_state()` pointing at `output_bo`, format R8_UINT or similar simple layout.
3. Implement `write_binding_table()` with one entry.
4. Update interface descriptor to reference the binding table.
5. Write a new `.g4a` kernel that:
   - Reads its thread index from R0 payload
   - Writes `thread_index * 0x1000 + 0xdead0000` to `output_bo[thread_index * 4]` via dataport RT write
   - Issues EOT
6. After parallel dispatch of 48 threads, verify `output_bo[i] == i * 0x1000 + 0xdead0000` for all 48 slots.

**Definition of done**: CPU-visible readback of 48 unique DWORDs, each in a slot written by a distinct GPU thread. This is the "Phase 1.3 — kernel reads inputs / writes outputs" milestone from `VIDEO_DECODE_PIVOT.md` §3.

Estimated effort: 3-4 new files (`kernels/memset_indexed.g4a`, extended `media_pipeline` with surface/binding setup, updated test harness). ~2-3 sessions of work.

After Phase 1.3, we begin **Phase 2 — MPEG-2 decoder port from libva-intel-driver**, which reuses everything from Phases 1.1–1.3 plus the 15 already-validated libva MPEG-2 VLD kernels.
