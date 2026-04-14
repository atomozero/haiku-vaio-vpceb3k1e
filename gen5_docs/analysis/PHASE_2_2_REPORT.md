# Phase 2.2 / 2.2b — sampler path milestone achieved

**Date**: 2026-04-14
**Hardware**: Sony VAIO VPCEB3K1E, Intel HD Graphics device 0x0046 (Ironlake Mobile, Gen5)
**OS**: Haiku R1~beta5

## Status

✅ **PASSED.** The Gen5 data port Media Block Read message via the sampler cache path is live on our hardware. Single-row and multi-row reads at arbitrary (X, Y) origins on a `SURFTYPE_2D` input surface produce bit-exact expected output through our own authored kernels. This is the decisive prerequisite for porting any libva-intel-driver video-decode kernel (all MPEG-2 VLD and H.264 MC kernels use exactly this path).

## What was proved

1. **SURFTYPE_2D surface state is consumed correctly by the data port Media Block Read message.** The PRM Vol 4 Part 1 §5.10.5 restriction — "the only surface type allowed is non-arrayed, non-mipmapped SURFTYPE_2D" — has to be followed strictly, *and* the surface state's Width/Height/Pitch bit positions must match what the 2D format expects (different from SURFTYPE_BUFFER which we learned about in Phase 2.1b).

2. **Sampler cache path (`target_cache = 2`) is operational on DevILK.** Our kernels route reads through the sampler cache and get valid data back. Render cache (`target_cache = 1`) is the other legal target per PRM; we did not exercise it.

3. **6-DWord message header layout for Media Block Read works as documented:**
   - M0.0 = X byte offset
   - M0.1 = Y row offset
   - M0.2 bits [4:0]   = Block Width - 1 in bytes (≤31)
   - M0.2 bits [21:16] = Block Height - 1 in rows
   - M0.5 [7:0] FFTID preserved from R0 (the `mov (8) g3 g0` header-clone pattern we use)

4. **Multi-row reads return contiguous GRFs with 1 GRF per row for 32-byte-wide blocks.** `rlen=4` delivers a 32×4 block into `g4..g7` with row N in `gN+4`. Matches PRM §5.10.5.3 ("register pitch equal to next power-of-2 ≥ Block Width").

5. **Non-zero (X, Y) origins are honored bit-exact:** tested at (0,0), (16,0), (0,2) and (16,2), all 128/128 bytes correct on each sub-test.

6. **Inline data delivery via MEDIA_OBJECT DW4/DW5 reaches g1.0/g1.1 of the thread payload.** Same mechanism as Phase 1.3 memset_indexed and Phase 2.1b SAXPY, now used to carry dynamic X/Y coordinates from the host into the kernel so we can issue different reads without recompiling.

7. **`msg_type = 2` is the correct Gen5 encoding for Media Block Read**, despite the PRM DevILK section listing it as a 3-bit value 4. Confirmed by both `gen4disasm -g5` of libva's production MPEG-2 MC kernels AND by our own kernels working bit-exact on the hardware. Saved as a persistent project memory so we don't re-hit this next time.

8. **Hybrid read/write paths in one kernel:** `sampler_read.g4a` and `sampler_read_4row.g4a` both use Media Block Read for input (via sampler cache on a SURFTYPE_2D surface) and OWord Block Write for output (via data cache on a SURFTYPE_BUFFER surface), validating that both surface types and both message paths can coexist in a single dispatch with a 2-entry binding table.

## Raw output from the passing runs

### Phase 2.2 (single-row, 32×1 at origin)

```
sampler: input[0..7] = 00 01 02 03 04 05 06 07
sampler: submit+complete: 94438 us
marker dump: [ 0] START ... [10] after MI_FLUSH #2  all OK
output[0..31] = 00 01 02 03 04 05 06 07
                08 09 0a 0b 0c 0d 0e 0f
                10 11 12 13 14 15 16 17
                18 19 1a 1b 1c 1d 1e 1f
PHASE 2.2 RESULT: PASSED — all 32/32 bytes correct
```

### Phase 2.2b (multi-row, four (X, Y) origins)

```
sub-test 1/4: origin (0,0)
sampler2b: first 8 bytes = 00 01 02 03 04 05 06 07
sampler2b: PASS (128/128 bytes)

sub-test 2/4: X offset (16,0)
sampler2b: first 8 bytes = 10 11 12 13 14 15 16 17
sampler2b: PASS (128/128 bytes)

sub-test 3/4: Y offset (0,2)
sampler2b: first 8 bytes = 80 81 82 83 84 85 86 87
sampler2b: PASS (128/128 bytes)

sub-test 4/4: XY offset (16,2)
sampler2b: first 8 bytes = 90 91 92 93 94 95 96 97
sampler2b: PASS (128/128 bytes)

PHASE 2.2b RESULT: PASSED — all 4/4 sub-tests
```

The expected first byte for each sub-test was computed from the input pattern (`byte[i] = i & 0xff`, 64-byte row pitch) and matches exactly: `Y*64 + X` = 0x00, 0x10, 0x80, 0x90 respectively.

## Timeline of the session

1. **Research step**. Read PRM Vol 4 Part 1 §5.10.5 (Media Block Read/Write) and §5.10.2.1.2 (message descriptor for DevILK). Found the `SURFTYPE_2D`, target cache, and 6-DWord header requirements. Cross-checked with `gen4disasm -g5 field_forward.g4b.gen5` from libva to resolve the msg_type=2 vs msg_type=4 ambiguity. Bottom line: the gen4asm struct uses a 2-bit msg_type field on Gen5, value 2 = Media Block Read, confirmed by every libva Gen5 kernel in production and by our own test binaries.

2. **Phase 2.2 kernel — `sampler_read.g4a`**. Single MEDIA_OBJECT, one thread, reads a 1-row × 32-byte block at (0,0) from a SURFTYPE_2D input surface via sampler cache, then writes the 32 bytes verbatim to a SURFTYPE_BUFFER output surface via OWord Block Write. 11 instructions. Passed on first hardware run — output bytes `00..1f` bit-exact, marker dump clean.

3. **Phase 2.2b kernel — `sampler_read_4row.g4a`**. Reads a 4-row × 32-byte block (rlen=4 GRFs, 128 bytes) at (X, Y) taken from the MEDIA_OBJECT inline data (g1.0 / g1.1). Block size hardcoded in the kernel. Writes the 128 bytes via OWord Block Write with `msg_control = 4` (8 OWords).

4. **Bug in first build**: I wrote `g1.1<0,1,0>UD` as the Y source, which gen4asm interpreted as byte offset 1 (non-DWord-aligned) and silently clamped to element 0 — both source reads ended up at g1.0. Caught by pre-install disasm (`gen4disasm -g5` showed `g1.0<0,1,0>UD` where I had written `g1.1`). Fixed to `g1.4<0,1,0>UD` (byte offset 4 = DW1) and the disasm then showed `g1.1<0,1,0>UD` (element index 1). Re-built. This is a pre-install check I should keep doing for every new kernel.

5. **Host refactor**: `emit_media_object_indexed` (single thread index value) was generalized to `emit_media_object_inline3(w, a, b, c)` that carries three uint32s in inline DW0/DW1/DW2. Existing callers now just pass `(thread_index, 0, 0)`. For the 2b path, we pass `(X, Y, 0)` and the kernel reads X from g1.0 and Y from g1.1.

6. **Phase 2.2b host**: `run_sampler_2b_test` runs 4 sub-tests (one dispatch each) at (0,0), (16,0), (0,2), (16,2). For each sub-test it memset's the output sentinel, submits a dedicated single-thread batch (not `submit_parallel_generic`, because we want the custom inline), waits on the AFTER_MI_FLUSH_2 marker, and CPU-verifies the 128 output bytes against the expected pattern `((Y+r)*64 + (X+c)) & 0xff`. All four sub-tests passed on the first hardware run.

## Files touched this phase

```
intel_extreme/accelerant/
├── kernels/sampler_read.g4a          (new, ~80 lines incl. comments)
├── kernels/sampler_read_4row.g4a     (new, ~80 lines)
├── media_pipeline.h                  (+15 lines: two new run_* decls)
├── media_pipeline.cpp                (+~320 lines total across 2.2 and 2.2b:
│                                      write_2d_surface_state_at helper,
│                                      emit_media_object_inline3 generalization,
│                                      setup_sampler_surfaces,
│                                      setup_sampler_2b_surfaces,
│                                      submit_sampler_2b,
│                                      verify_sampler_2b_block,
│                                      run_sampler_test,
│                                      run_sampler_2b_test)
└── Makefile                          (+2 kernels in KERNEL_SRCS)
```

## What was NOT proved

1. **Tiled surfaces.** Both tests use linear (`tile_walk = 0`, `tiled_surface = 0`) SURFTYPE_2D input surfaces. libva's production MPEG-2/H.264 kernels read from tiled reference frames via sampler cache. This will need a separate exercise, including getting the surface state tiling bits right, before we can port the MC kernels.

2. **Sub-32-byte block widths.** Our 2b kernel always reads 32×4. The PRM describes smaller block widths (1-4, 5-8, 9-16, 17-32 bytes) with different register pitch rules — "register pitch equal to next power-of-2 ≥ Block Width" — which we have not exercised. The first libva kernel that does this will hit any latent bug. For our own authored kernels we can keep using 32-byte width to stay on the easy path.

3. **Boundary clamping.** The PRM specifies clamp-to-edge behaviour when reads go outside the surface (byte replication for 8bpp, word replication for 16bpp, etc). We never read outside the surface in our tests. If we do this later, the expected-value computation in the verifier needs to account for the clamp.

4. **Block height > 4.** We tested `rlen=4`. The max useful is 8 GRFs (32×8 or larger block) which libva uses for some paths. Should work identically but we have no direct evidence.

5. **Render cache target (`target_cache = 1`).** We only tested sampler cache. Render cache is the other legal path per PRM and might be needed for some workloads or for write coherence against render target access.

6. **Real coordinate arithmetic inside the kernel.** Our 2b kernel receives X and Y pre-baked from the CPU and uses them as-is. A real MPEG-2 MC kernel would compute X/Y from a macroblock index, include a motion vector offset, etc. That is a Phase 2.3 concern — we have enough to handle it when we cross that bridge.

## Infrastructure that is now reusable

- **`write_2d_surface_state_at(ctx, off, base, w, h, pitch)`** — builds a linear SURFTYPE_2D surface state entry in the shared surface_state_bo at any offset. Works for the kind of input surfaces MEDIA_BLOCK_READ expects.
- **`write_linear_surface_state_at` (existing from Phase 2.1b)** — writes a SURFTYPE_BUFFER entry. A single binding table can mix SURFTYPE_2D and SURFTYPE_BUFFER entries at different slots, as sampler_read.g4a and sampler_read_4row.g4a both do.
- **`emit_media_object_inline3`** — generalized MEDIA_OBJECT emitter that carries three arbitrary uint32s as inline data, reaching the thread at g1.0 / g1.1 / g1.2. Back-compat wrapper `emit_media_object_indexed` is a one-liner.
- **`submit_sampler_2b` pattern** — a dedicated single-thread submit that builds the full 10-command preamble and emits one MEDIA_OBJECT with custom inline. Any follow-up sampler test can be a thin variant of this.
- **gen4asm + pre-install disasm** — the disasm caught the `g1.1` / `g1.4` bug before the first hardware run. It takes 1 second per kernel. Keep doing it.
- **Persistent memory entries** gen5_oword_block_surftype_buffer.md and gen5_media_block_read_encoding.md document the two surface-type / message-type gotchas we've hit so far.

## Known limitations / technical debt

- **The `sampler_read.g4a` kernel's block-size DW is overwritten by a hardcoded constant** (0x0000001F). It currently serves only as a correctness proof; production kernels would parameterize block size via inline data or compile-time specialization.
- **Only one dispatch per test** in 2.2b. Parallel dispatch across multiple (X, Y) origins (e.g. one thread per macroblock) is in scope for Phase 2.3 but not exercised yet.
- **No perf numbers.** Both 2.2 and 2.2b are correctness-first. A benchmark over the sampler path is meaningless at this stage (same polling-artifact problem we discussed in the 2.1b M2 report).
- **The `sampler_read` and `sampler_read_4row` kernels reuse input_x_bo** (a saxpy-era alias) as the input surface. The BO name is carried over from Phase 2.1 and is slightly misleading in this context but correct functionally — it's just the first 4 KB input allocation. Worth renaming to something neutral like `input_a_bo` if we ever clean up.

## Next steps

**Phase 2.3 — port first libva kernel (`iq_intra.g4a`).** This is the simplest of the 15 MPEG-2 VLD kernels in libva-intel-driver's `shaders/mpeg2/vld/`. It performs inverse quantization on an intra macroblock: reads a block of DCT coefficients (via indirect data or sampler cache, TBD), applies the quantization matrix, and writes the result back. Porting it exercises:

1. m4 preprocessing of `.g4a` + `.g4i` includes (gen4asm accepts the raw `.g4a` but libva kernels are split across files)
2. Uploading the quantization matrices as a CURBE or an indirect-data payload
3. Setting `const_urb_entry_read_len` to a non-zero value if the kernel expects CURBE data pushed into its GRFs — until now we've always used 0
4. Validating the output bit-exact against a CPU-side MPEG-2 inverse-quantization reference

Effort estimate: two sessions. The first session should just get `iq_intra.g4a` to assemble with our gen4asm and run without hanging; the second focuses on correctness of the output values.

After Phase 2.3 passes, we can start stringing the VLD kernels together to decode a real MPEG-2 slice. That's the end-to-end video-decode goal we've been working toward since the VIDEO_DECODE_PIVOT decision.
