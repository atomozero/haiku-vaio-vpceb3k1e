# Phase 1.3 — per-thread memory write via surface state + binding table

**Date**: 2026-04-13
**Hardware**: Sony VAIO VPCEB3K1E, Intel HD Graphics device 0x0046 (Ironlake Mobile, Gen5)
**OS**: Haiku R1~beta5

## Status

✅ **PASSED.** 48 concurrent threads, each reading its own thread index from the MEDIA_OBJECT inline payload and writing a distinct per-thread value to a distinct per-thread offset in the output BO via the data port. 48/48 rows correct on readback. This is the first time on this hardware that an in-tree authored Gen5 compute kernel has done something *observably different per thread*.

## What was proved

1. **The full data port write path is live.** `BTI 0 → binding_table[0] → surface_state[ss1] → output_bo` resolves correctly inside the EU. OWord Block Write message type 0 with `mlen=2`, `rlen=0`, inline header DW[2] = global byte offset is the working encoding on Gen5.

2. **Global offset unit on Gen5 is bytes, not OWords.** The `offset /= 16` division that Mesa applies in `brw_oword_block_write_scratch` is guarded by `intel->gen >= 6`; on Gen5 the value in the message header DW[2] is consumed as a raw byte offset added to the surface base address. Confirmed empirically: header offset 80 → bytes 80..95 of the surface.

3. **MEDIA_OBJECT inline data lands at g(1 + const_urb_entry_read_len) on Gen5.** With `const_urb_entry_read_len = 0` in the interface descriptor (our config), MEDIA_OBJECT DW4 is delivered to the child thread at **g1.0**. Libva's MPEG-2 kernels use `read_len = 30` and therefore read their inline data from **g31**, matching `shaders/mpeg2/vld/null.g4a`.

4. **`urb_entry_alloc_size = 0` = 1 unit = 32 B = 1 GRF** is sufficient to carry up to 8 DWORDs of inline state. The 2 DWORDs we emit (thread index + unused) fit comfortably.

5. **Phase 1.2's parallel dispatch was real, not a no-op.** Before Phase 1.3, we could not distinguish "48 threads all executed an identical EOT kernel" from "1 thread executed and 47 were silently drained". Phase 1.3 closes this gap: each of the 48 slots produced a distinct memory side-effect. The parallel path is genuinely parallel.

6. **The safety clamp (`and idx, 0x3f`) was a no-op on the happy path** but prevented any chance of a garbage offset escaping the 4 KB output BO. Keep this pattern for future kernels whose payload source is not yet fully trusted.

## The two-kernel journey

This phase took two kernel revisions to complete.

**v9** (hardcoded write) — proved the data port path end-to-end but wrote the same value (`0xcafebabe`) at the same offset (80) regardless of thread. Idempotent: indistinguishable from "1 wrote / 47 no-oped". Correctly interpreted as a **step 1 of 2** result.

**v10** (per-thread differentiation) — reads the thread index from g1.0 (MEDIA_OBJECT inline data), computes per-thread offset and value, writes via the same OWord Block Write path. 48/48 rows correct.

Both kernels went to hardware; the v9 intermediate was important because it isolated the data port question from the per-thread question. If v10 had failed, we would not have known whether the problem was the write path or the payload delivery.

## Raw output from the passing run

```
upload_kernel: 96 bytes at gtt=0x22000
setup_output_surface: 16x48 R8_UINT @ output gtt=0x26000, ss gtt=0x28000, bt gtt=0x29000
hexdump media:kernel @ gtt=0x22000+0 (24 dwords):
  0x00022000: 00600001 20600021 008d0000 00000000   ; mov(8)  g3=g0
  0x00022010: 00000005 20800c21 00000020 0000003f   ; and(1)  g4=g1&0x3f
  0x00022020: 00000041 20680c21 00000080 00000010   ; mul(1)  g3.2=g4*16
  0x00022030: 00400006 20200c22 00000080 dead0000   ; or(4)   m1=g4|0xdead0000
  0x00022040: 00800031 24001d28 508d0060 04080000   ; send    write(0,0,0,0) mlen 2
  0x00022050: 00800031 24001d28 748d0000 82000000   ; send    thread_spawner EOT
hexdump media:surface @ gtt=0x28000+0 (6 dwords):
  0x00028000: 250c0000 00026000 017803c0 00000078
hexdump media:bindtbl @ gtt=0x29000+0 (1 dwords):
  0x00029000: 00028000
submit_parallel: 360 dwords (1440 bytes) for 48 thread(s), emitting direct to ring
submit_parallel: ring kicked, 48 thread(s) dispatched
marker dump:
  [ 0] START                             0xbeef0000  OK
  [ 1] after MI_FLUSH #1                 0xbeef0001  OK
  ...
  [10] after MI_FLUSH #2                 0xbeef000a  OK
--- memwrite post-complete ---
  INSTDONE=0xfffffffe  stalled:
    [bit 0] ROW_0
  RING: HEAD=0x000005b0  TAIL=0x000005b0  CTL=0x0000f001
output surface dump (first 8 of 48 rows, 16 bytes each):
  row  0 (exp 0xdead0000): dead0000 dead0000 dead0000 dead0000 OK
  row  1 (exp 0xdead0001): dead0001 dead0001 dead0001 dead0001 OK
  row  2 (exp 0xdead0002): dead0002 dead0002 dead0002 dead0002 OK
  row  3 (exp 0xdead0003): dead0003 dead0003 dead0003 dead0003 OK
  row  4 (exp 0xdead0004): dead0004 dead0004 dead0004 dead0004 OK
  row  5 (exp 0xdead0005): dead0005 dead0005 dead0005 dead0005 OK
  row  6 (exp 0xdead0006): dead0006 dead0006 dead0006 dead0006 OK
  row  7 (exp 0xdead0007): dead0007 dead0007 dead0007 dead0007 OK
  PHASE 1.3 RESULT: PASSED — all 48/48 rows correct
```

The post-complete state is clean: INSTDONE bit 0 is `ROW_0` (a bit *name*, not an error flag — it's the "row 0 done" indicator in the EU bitmap), ring HEAD==TAIL, no EIR/ESR/PGTBL_ER.

## Timeline of the Phase 1.3 session

1. First test run with a v8 kernel that passed a raw OWord offset (5): wrote at bytes 0..15 instead of 80..95, disproving the v8 hypothesis that global offset was in OWord units. Concluded Gen5 global offset unit is bytes.
2. v9 kernel: hardcoded 80-byte offset, hardcoded `0xcafebabe` value, no per-thread logic. Wrote exactly at row 5. Proved the data port path works but left the per-thread question open.
3. **Re-analysis of the v9 log** (during the current session): the prior session's summary misread the dump and assumed "nothing worked"; reading the actual syslog showed row 5 correctly set. v9 was a step 1 success, not a failure.
4. Cross-referenced `libva_intel/src/i965_media_mpeg2.c` and `shaders/mpeg2/vld/null.g4a` to locate where MEDIA_OBJECT inline data lands on Gen5 with different IDRT settings. Derived the `g(1 + const_urb_entry_read_len)` rule.
5. Wrote v10: read g1.0, mask to 0..63, compute offset = idx*16 and value = 0xdead0000|idx, OWord Block Write via the same encoding as v9.
6. Built with gen4asm, disassembled with gen4disasm to verify the instruction encoding and in particular the write-send descriptor bit-for-bit against v9.
7. Installed, rebooted, ran, read the new syslog: 48/48 rows correct on first try.
8. Saved the `g(1 + const_urb_entry_read_len)` finding to persistent memory.

Phase 1.3 took roughly one session once the v9-log re-reading step had corrected the framing.

## Files touched this phase

```
intel_extreme/accelerant/
├── kernels/memset_indexed.g4a   (rewritten: v9 → v10, +60 lines of commentary)
├── kernels/memset_indexed.g4b.gen5 (regenerated by gen4asm, 6 instructions)
└── media_pipeline.cpp            (1 line: hexdump kernel 8 → 24 dwords)
```

Minimal diff. The heavy lifting (surface state, binding table, IDRT with BT reference, run harness, verifier) was already in place from the Phase 1.3 step-1 session.

## What was NOT proved

1. **Arithmetic on EU ALUs.** The v10 kernel still doesn't exercise FP MAD, nor any significant ALU pipeline. `and`/`mul`/`or` on scalars is not a stress test — it barely touches the EU shared functions. Nothing yet about real throughput.

2. **Surface input reads.** The kernel only writes. A kernel that reads from one bound surface and writes to another (the standard compute pattern) is the next step up. Needed for any real workload including MPEG-2 IDCT.

3. **Larger-than-OWord writes.** Each thread wrote exactly one OWord (16 B). OWord Block Write supports up to 8 OWords per message on Gen5; we never exercised that. Bandwidth measurements are premature until we do.

4. **Multi-binding-table kernels.** IDRT has `binding_table_entry_count` up to 31. We used exactly 1. Any kernel touching multiple surfaces (input tex + output RT, or A+B→C matmul) needs this validated.

5. **R0 dispatch header layout.** We sidestepped the question of what's in R0 on Gen5 media by routing the thread index through MEDIA_OBJECT inline data. R0 does contain a hardware-assigned dispatch ID that could replace our CPU-provided index — eventually worth learning, but not blocking.

6. **Scaling curve.** Still only two data points (1 thread and 48 threads) and still dominated by polling artifacts in the 1-thread case. No scaling inference possible yet.

## Infrastructure that is now reusable

- **`media_pipeline_setup_output_surface(ctx, row_bytes, row_count)`** — generic R8_UINT linear 2D surface + binding table setup. Any future kernel writing to a single output BO can use this verbatim.
- **`write_interface_descriptor_ex(ctx, entry_count, bt_gtt_offset)`** — parameterized IDRT emission, handles both the "no binding table" (Phase 1.1/1.2) and "binding table referenced" (Phase 1.3+) cases.
- **`submit_parallel_generic(ctx, n, with_binding_table)`** — single shared implementation for both parallel-EOT and parallel-memwrite dispatches, differing only in the IDRT form.
- **`verify_memwrite_output` / `dump_memwrite_output`** — CPU-side readback and per-row diff reporting pattern. Adaptable to any per-thread output scheme.
- **gen4asm → gen4disasm round-trip as a pre-install step.** Disassembling the built binary before committing to a test install was low-effort and gave confidence in the message descriptor encoding. Keep doing this for every new kernel.

## Known limitations / technical debt

- **v10 uses MRF m1 for payload** following the v9 pattern. The exact MRF↔GRF mapping convention on Gen5 that makes `send g3 mlen 2` pull `g3` as header and `m1` as data is still empirically understood, not fully documented in our own notes. Works reliably but a proper writeup would help future kernels.
- **The accelerant hook `MEDIA_PIPELINE_HELLO_TEST` still points at `run_memwrite_test`.** On every app_server reload the Phase 1.3 test runs again, wasting a few ms of boot time. Worth flipping off for "normal" installs, keeping on for regression checks.
- **No bench numbers worth reporting.** The test writes 768 B total; the CPU+setup overhead dwarfs any GPU work. A useful benchmark waits for a Phase 2 kernel with a real payload (see below).

## Next steps

Phase 1.3 is genuinely closed. The next milestone is **Phase 2.1 — first arithmetic kernel**. Target:

- Input: two linear FP32 buffers `a[N]`, `b[N]`, `N = 64 * 1024` (256 KB each).
- Kernel per thread: SAXPY or straight `c[i] = a*x[i] + y[i]` on a SIMD8 block (32 elements per thread across 4 threads per EU).
- Output: one linear FP32 buffer `c[N]`.
- Binding table: 3 entries (a, b, c).
- Kernel: read 2 OWord-block inputs, one MAD (or mul+add), one OWord-block write, EOT. ~10 instructions.
- Verifier: CPU-side `fabs(c[i] - (a*x[i] + y[i])) < epsilon` for all N.

**Definition of done**: 48 threads each process a disjoint slice of the 64 K element array, producing a bit-exact (or epsilon-correct) result versus the CPU reference in under some target µs-per-element figure.

This phase finally produces **a meaningful benchmark**: GFLOPS-measured EU throughput for one specific pattern, compared against a straight CPU SSE2 loop on the i3 M370. That is the number worth reporting.

After Phase 2.1, **Phase 2.2 — kernel with input texture reads via sampler** covers the shared function we'll need for MPEG-2 MC. Then **Phase 2.3** starts the port of `libva_intel/src/shaders/mpeg2/vld/iq_intra.g4a` (the simplest of the 15 already-validated MPEG-2 kernels).
