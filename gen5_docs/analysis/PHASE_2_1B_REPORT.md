# Phase 2.1b — SAXPY scaling and throughput benchmark

**Date**: 2026-04-14
**Hardware**: Sony VAIO VPCEB3K1E, Intel HD Graphics device 0x0046 (Ironlake Mobile, Gen5)
**OS**: Haiku R1~beta5

## Status

✅ **Milestone 1 PASSED** — saxpy kernel scales cleanly from 16 to 2048 FP32 elements across 10 dispatch sizes, bit-exact against a CPU reference.

✅ **Milestone 2 COMPLETED** — throughput benchmark produced real GPU vs CPU numbers. GPU was ~18x slower than CPU on this workload, dominated by synchronous submit+polling overhead. The numbers are real but the benchmark is not representative of workloads where the GPU path will actually be used (video decode, LLM).

🐛 **Bug discovered and fixed during M1** that had been latent through every prior phase: our surface state used `SURFTYPE_2D` where Gen5 OWord Block Read/Write requires `SURFTYPE_BUFFER`. The bug was hidden through Phase 1.3 / 2.1a because the specific row-byte values we used happened to fall on a `row_bytes mod 128 == 127` equivalence class that produced "correct" bounds by accident.

## Milestone 1 — scaling

### Background

After Phase 2.1a passed with a single 48-thread dispatch, Milestone 1's goal was to verify the saxpy kernel at dispatch counts beyond the hardware's 48 concurrent-thread limit (12 EUs × 4 threads). The VFE dispatcher is documented to recycle URB entries as in-flight threads retire, so nothing in theory prevents dispatching 1000+ MEDIA_OBJECTs in a single batch. M1 was the first time we'd actually tested that.

### The bug chase

M1 went through several false hypotheses before landing on the real cause. Every intermediate "fix" was compiled, installed, rebooted, measured, and rejected as not matching the observed failure pattern. The full list:

1. **CS URB size too small** (bumped from 1 unit to 16, matching libva) — no effect.
2. **MEDIA_OBJECT inline data not filling the URB entry** (expanded from 6 DW to 20 DW padded with zeros) — no effect. Still a correct change in its own right; kept.
3. **Missing MI_FLUSH between waves of 16 MEDIA_OBJECTs** — no effect. Reverted.
4. **grf_reg_blocks too small for the kernel's register usage** (bumped from 0 → 2, giving 24 regs instead of 8) — no effect on the failure pattern. Kept as a real correctness fix independently.
5. **num_urb_entries too high** (reduced from 48 → 32) — failure pattern unchanged, which told us the bug was not tied to URB pool size.

### The actual bug

The dispatch-dependent failure pattern was:

| dispatch | correct rows | notes |
|---|---|---|
| 16 | 16 | OK |
| 17 | 16 | wrong — row 16 at sentinel |
| 32 | 32 | OK |
| 33 | 16 | |
| 48 | 48 | OK |
| 49 | 16 | |
| 56 | 56 | OK |
| 63 | 48 | |
| 64 | 64 | OK |
| 65 | 16 | |
| 66 | 32 | |
| 68 | 64 | |
| 72 | 64 | |
| 80..256 | 64 | capped |

At dispatch counts where `(row_bytes - 1) & 0x7F == 127`, the test passed. At other values it produced a deterministic smaller number of correct rows. The numbers were reproducible bit-for-bit across runs.

Going back to the PRM (Vol 4 Part 1 §5.10.3 "OWord Block Read/Write") exposed one sentence at the top of the restrictions list:

> the only surface type allowed is SURFTYPE_BUFFER

We were using `SURFTYPE_2D = 1`, not `SURFTYPE_BUFFER = 4`. The hardware did not reject the surface state, but it also did not read our Width field from the SURFTYPE_2D bit positions. It read Width from the **SURFTYPE_BUFFER** bit positions, where the same 13-bit slot in DW2 bits [12:6] encodes only the **low 7 bits** of `num_entries - 1`. With element size fixed at 16 bytes for OWord Block Read/Write, this effectively produced a buffer sized `((row_bytes - 1) & 0x7F + 1) * 16` bytes, depending on `row_bytes mod 128`.

Computed against each tested dispatch size, this formula produces the exact observed failure pattern for every single data point we measured. Cross-checked once more against sweeps at multiple `num_urb_entries` values to confirm.

The fix was to change `write_linear_surface_state_at` to emit a proper SURFTYPE_BUFFER encoding, with `num_entries - 1` split across the Width/Height/Depth bit fields per PRM, and an element size of 16 bytes in the surface pitch slot (which OWord Block Read/Write ignores in practice, but we set it to be explicit).

### M1 passing run

After the SURFTYPE_BUFFER fix, the same 10-dispatch sweep that had been failing now passed cleanly:

```
dispatch=16  →  128/128 OK        (was 128/128 before the bug)
dispatch=17  →  136/136 OK        (was 128/136 broken)
dispatch=32  →  256/256 OK
dispatch=33  →  264/264 OK        (was 128/264 broken)
dispatch=48  →  384/384 OK
dispatch=49  →  392/392 OK        (was 128/392 broken)
dispatch=64  →  512/512 OK
dispatch=65  →  520/520 OK        (was 128/520 broken)
dispatch=128 → 1024/1024 OK       (was capped at 64 rows before)
dispatch=256 → 2048/2048 OK       (was capped at 64 rows before)
PHASE 2.1b M1 RESULT: PASSED — all 10 sizes correct
```

This also explains retroactively why Phase 1.3 memset_indexed (48 × 16-byte rows) and Phase 2.1a saxpy (48 × 32-byte rows) had been working: `row_bytes - 1` = 767 and 1535 respectively, both have `& 0x7F == 127`, so both fell into the "accidentally OK" equivalence class. The bug was latent — it never manifested until we deliberately tried dispatch counts that moved `row_bytes` off that lucky boundary.

The finding is saved to persistent memory as `gen5_oword_block_surftype_buffer.md` so we don't hit it again.

## Milestone 2 — throughput benchmark

### Design

With correctness proven, M2 ran a throughput comparison. Dispatch = 400 threads × 8 FP32 = 3200 elements per iteration, 2000 iterations, submitted synchronously (each iteration: submit + wait-for-marker + next). CPU reference computed the same operation on malloc'd arrays holding copies of the same inputs, 2000 iterations, asm-volatile memory barrier between outer loop iterations to defeat compiler hoisting.

Element[0] cross-check verified GPU and CPU produced the same first output (`0x40000000 = 2.0f`) before reporting.

### Results

```
                  wall (us)   MB/s    MFLOPS
GPU (400 × 2000)   1294224    59      9
CPU (same work)      71630    1072    178
                              ──────
  GPU / CPU ratio              0.05x
```

- Correctness: `element[0] GPU=0x40000000 CPU=0x40000000 MATCH` for every size we ever ran.
- GPU 18x slower than CPU **for this specific workload and submission pattern**.

### Why the GPU is so slow here

Time per GPU iteration = 1294224 / 2000 ≈ 647 µs.

Actual GPU work: 3200 elements × 4 bytes × 3 buffers = 38.4 KB moved + 6400 FLOPs. At even 1 GB/s the hardware could do that in about 40 µs.

The remaining ~600 µs per iteration is **submit + poll overhead**:
- CPU builds a 1032-DW batch and writes it to the ring (~10 µs).
- CPU spins on the AFTER_MI_FLUSH_2 marker BO until the GPU updates it.
- Haiku's scheduler preempts our thread during the spin; wake-up latency is a significant fraction of a scheduler quantum.

Dividing 76.8 MB of moved data by 647 µs per iteration gives the ~60 MB/s figure we see, which is NOT the GPU's bandwidth — it's the effective bandwidth you get when you bottleneck on synchronous single-dispatch latency.

### Why this matters (and why it doesn't)

The 0.05x number is NOT a property of the GPU for saxpy. It's a property of (small workload + synchronous submission + polling-based wait + tiny per-dispatch work) *together*. Removing any of those factors would move the result significantly:

- **Large single dispatch, one wait**: a single batch with thousands of dispatches amortizes the polling overhead over all of them. This is how libva decodes video.
- **In-kernel looping** so each thread does 4x to 100x the compute per dispatch — arithmetic intensity goes up, dispatches-per-wall-µs goes down, fewer overhead dominates.
- **GPU timeline with IRQ completion**: no spinning at all; wake on a fence.
- **Workload with more arithmetic intensity**: saxpy is 2 FLOPs per 12 bytes moved (0.17 FLOP/B). A matmul has 10-100x more. LLM inference even more. The GPU's advantage scales with intensity.

The real value of M2 is having **measured** numbers instead of extrapolated ones. When we eventually hit Phase 2.3 and port the first libva kernel, we'll have a known-good reference point to compare against.

## Files touched this phase

```
intel_extreme/accelerant/
├── kernels/saxpy_simd8.g4a           (unchanged from Phase 2.1a)
├── media_pipeline.h                  (+10 lines: two run_saxpy decls)
├── media_pipeline.cpp                (+~450 lines total:
│                                      SURFTYPE_BUFFER surface encoding,
│                                      expanded MEDIA_OBJECT emission,
│                                      bigger BATCH_CAPACITY_DW,
│                                      grf_reg_blocks = 2 in IDRT,
│                                      run_saxpy_bench_test sweep harness,
│                                      run_saxpy_perf_test timing harness,
│                                      CPU reference loop with asm barrier)
└── Makefile                          (unchanged)
```

## What was NOT proved

1. **GPU asymptotic throughput on saxpy.** We measured the wrong thing. The 59 MB/s figure does not represent a saturation throughput on memory bandwidth. To do that we'd need single-submit large-dispatch mode, or in-kernel looping, or async completion.

2. **Performance at larger dispatch counts.** Our kernel mask is `0x3ff` (1024), so even a single batch is currently capped there. libva uses values up to ~2000 in practice. We didn't hit that wall but we're close.

3. **Any non-saxpy workload.** This phase was saxpy-only. The real question — how does Gen5 do on the MC/IDCT kernels of MPEG-2 — is Phase 2.3 territory.

4. **Multi-kernel pipeline.** Phase 2.1 runs one kernel per dispatch. Real video decode runs a sequence: VLD → IQ → IDCT → MC → IDB. That composition is not exercised yet.

## Infrastructure that is now reusable

- **SURFTYPE_BUFFER surface state encoder** `write_linear_surface_state_at` — correct PRM-compliant BUFFER encoding with the 27-bit num_entries-1 split across Width/Height/Depth. Drop-in for any future OWord Block Read/Write access.
- **MEDIA_OBJECT expanded emission** (`emit_media_object_inline3`, 20 DWORDs total, 16-DW inline payload filling one 1-unit URB entry) — reusable for any generic-mode Gen5 kernel.
- **`submit_parallel_generic(ctx, dispatch_count, bt_entry_count)`** — generic dispatch submission with arbitrary count and binding table size. Used by saxpy, memset, and any future kernel.
- **`run_saxpy_bench_test`** — can be used as-is to regression-check correctness after any media pipeline change.
- **`run_saxpy_perf_test`** with the CPU reference loop — template for any future GPU vs CPU benchmark harness. Remember the asm volatile barrier.

## Known limitations / technical debt

- **Batch capacity is still stack-allocated** (`uint32 dwords[BATCH_CAPACITY_DW]` inside `batch_writer` struct). At 8192 we use 32 KB of stack per submit; bumping to 65536 would need heap allocation.
- **`gpu_debug_wait_value` is a mixed busy-spin + snooze** that is the main source of per-iteration wake-up latency in M2. Switching to an IRQ-driven or seqno-based sync would improve throughput numbers significantly but is more driver-level work than we needed for M2's correctness purposes.
- **The kernel mask clamps dispatch to 1023**, which is a safety value (`idx & 0x3ff`) rather than a hardware limit. Increase if we want to benchmark at larger scales.
- **The SAXPY kernel is memory-bound** and will never win against a well-tuned CPU on this specific operation. This is expected — the GPU's strength is in shaders/kernels with higher arithmetic intensity. Saxpy is a sanity benchmark, not a showcase.

## Next steps

Phase 2.1b is done. Phase 2.2 (sampler path) started immediately after and is covered in PHASE_2_2_REPORT.md. The overall video-decode roadmap continues from there to Phase 2.3 (first libva kernel port).
