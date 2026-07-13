/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Gen5 (Ironlake) media-pipeline bring-up.
 *
 * Implements the 10-command sequence that hands control of the EU array
 * over to a compute kernel via MEDIA_OBJECT. Source of truth for the
 * sequence is gen5_docs/analysis/MEDIA_PIPELINE_BRINGUP.md, itself derived
 * from libva-intel-driver i965_media.c and the Ironlake PRM Vol 2 Part 2.
 *
 * This is the path we will use for video decode (phase 2) and eventually
 * compute/LLM workloads (phase 4). It is completely independent of the
 * (frozen) 3D render path in render.cpp.
 */


#ifndef MEDIA_PIPELINE_H
#define MEDIA_PIPELINE_H


#include <SupportDefs.h>

#include "gpu_bo.h"


// Slot indices in the marker area of ctx->marker_bo. One slot per
// media-pipeline command: the kernel ring emits MI_STORE_DATA_IMM writing
// a distinct tag into each slot immediately after the corresponding
// command completes. Post-hang readback tells us exactly which command
// the pipeline failed to drain.
enum media_marker_slot {
	MEDIA_MARKER_START = 0,
	MEDIA_MARKER_AFTER_MI_FLUSH_1,
	MEDIA_MARKER_AFTER_DEPTH_BUFFER,
	MEDIA_MARKER_AFTER_PIPELINE_SELECT,
	MEDIA_MARKER_AFTER_URB_FENCE,
	MEDIA_MARKER_AFTER_STATE_BASE,
	MEDIA_MARKER_AFTER_MEDIA_STATE_POINTERS,
	MEDIA_MARKER_AFTER_CS_URB,
	MEDIA_MARKER_AFTER_CONSTANT_BUFFER,
	MEDIA_MARKER_AFTER_MEDIA_OBJECT,
	MEDIA_MARKER_AFTER_MI_FLUSH_2,
	MEDIA_MARKER_COUNT
};


// Distinct tag values written into the marker slots so we can tell at
// readback time whether a slot was populated by this run or is stale.
// 0xbeef* = "beef this particular slot", low byte = slot index.
#define MEDIA_MARKER_TAG(slot) (0xbeef0000u | (uint32)(slot))

// Sentinel the CPU writes into every marker slot before submission so we
// can distinguish "GPU wrote 0" from "GPU never wrote anything".
#define MEDIA_MARKER_SENTINEL 0xdeadbeefu


struct media_pipeline_context {
	gpu_bo batch_bo;			// batch buffer (legacy, unused since direct-ring)
	gpu_bo kernel_bo;			// EU kernel instruction binary
	gpu_bo vfe_state_bo;		// struct i965_vfe_state (6 DWORDs)
	gpu_bo idrt_bo;				// interface descriptor (4 DWORDs)
	gpu_bo curbe_bo;			// constant URB entry contents
	gpu_bo output_bo;			// kernel output destination (also SAXPY c)
	gpu_bo marker_bo;			// per-command completion markers
	gpu_bo surface_state_bo;	// Phase 1.3+: struct i965_surface_state(s)
	gpu_bo binding_table_bo;	// Phase 1.3+: 1+ binding table entries
	gpu_bo input_x_bo;			// Phase 2.1: SAXPY x input
	gpu_bo input_y_bo;			// Phase 2.1: SAXPY y input
	bool initialized;
};


// One-time initialization: allocates all the BOs above.
// Caller is expected to populate kernel_bo contents via
// media_pipeline_upload_kernel() before submitting.
status_t media_pipeline_init(media_pipeline_context* ctx);

// Release all BOs and clear the context.
void media_pipeline_uninit(media_pipeline_context* ctx);

// Copy an EU instruction binary (from intel-gen4asm -g5 output) into the
// kernel BO. The binary must fit within the kernel BO size.
status_t media_pipeline_upload_kernel(media_pipeline_context* ctx,
	const uint32* kernel_dwords, uint32 kernel_bytes);

// Upload the embedded hello-world kernel (single send-EOT instruction,
// assembled from kernels/hello_world.g4a). Equivalent to calling
// media_pipeline_upload_kernel() with the built-in binary.
status_t media_pipeline_upload_hello_kernel(media_pipeline_context* ctx);

// Upload the embedded memset-indexed kernel (Phase 1.3 memory-write
// proof, assembled from kernels/memset_indexed.g4a). Each thread writes
// a 32-byte row of 0xdead0000|thread_index pattern to the output surface.
status_t media_pipeline_upload_memset_kernel(media_pipeline_context* ctx);

// Build and submit the hello-world 10-command batch. Configures the VFE
// state for a single-thread generic-mode dispatch, the interface descriptor
// to point at the uploaded kernel, initializes the output BO to a sentinel
// value, emits per-command markers, and fires the batch via the primary
// render ring. Does not wait for completion — use
// media_pipeline_wait_output() afterward.
status_t media_pipeline_submit_hello(media_pipeline_context* ctx);

// Submit 'thread_count' back-to-back MEDIA_OBJECT dispatches of the same
// kernel, with the VFE configured for 'thread_count' concurrent threads
// and enough URB entries to feed them in parallel. Used by Phase 1.2 to
// validate parallel dispatch — each thread reaches EOT and VFE has to
// drain all of them before the trailing MI_FLUSH/marker completes.
// thread_count must be in [1, MEDIA_MAX_PARALLEL_THREADS].
status_t media_pipeline_submit_parallel(media_pipeline_context* ctx,
	uint32 thread_count);

#define MEDIA_MAX_PARALLEL_THREADS 48

// Phase 1.3: surface state / binding table configuration. Configures the
// output BO as an R8_UINT 2D surface of 'row_bytes' wide × 'row_count'
// tall, writes the surface state into surface_state_bo, and puts its
// GTT offset into binding_table_bo[0]. Must be called before submitting
// a kernel that uses binding_table index 0 for writes.
status_t media_pipeline_setup_output_surface(media_pipeline_context* ctx,
	uint32 row_bytes, uint32 row_count);

// Phase 1.3 row byte size: each thread writes 1 OWord (16 bytes) via
// OWord Block Write with the thread index as the OWord offset.
#define MEDIA_MEMSET_ROW_BYTES 16

// Wait up to timeout_us microseconds for output_bo[0] to differ from the
// MEDIA_MARKER_SENTINEL value, i.e. for the kernel to have written
// something. Returns true if a write was observed.
bool media_pipeline_wait_output(media_pipeline_context* ctx,
	uint32 timeout_us);

// Diagnostic: print the marker slot contents to the syslog. A normal
// completion shows every slot populated with its expected tag
// (MEDIA_MARKER_TAG(slot)); a hang shows the first N populated and the
// rest still at MEDIA_MARKER_SENTINEL.
void media_pipeline_dump_markers(const media_pipeline_context* ctx);

// One-shot end-to-end smoke test: init context, upload the embedded
// hello-world kernel, submit the 10-command batch, wait for all markers
// to fire, dump full diagnostics on either success or timeout, tear
// everything back down. Returns B_OK if every marker slot was populated
// with the expected tag, B_TIMED_OUT if the pipeline stalled, or another
// error code for earlier setup failures.
//
// This is the Phase I.B "lights on" milestone — a successful run is
// definitive proof that the EU array executed a kernel we authored.
status_t media_pipeline_run_hello_test(void);

// Phase 1.2: parallel-dispatch smoke test. Runs the hello-world test
// first (baseline = 1 thread), then dispatches
// MEDIA_MAX_PARALLEL_THREADS identical EOT kernels via back-to-back
// MEDIA_OBJECT commands. Both runs are timed via the bench module and
// their per-thread cost is compared. Returns B_OK if both runs complete
// successfully, error otherwise.
status_t media_pipeline_run_parallel_test(void);

// Phase 1.3: memory-write proof test. Configures the output BO as an
// R8_UINT surface, uploads the memset_indexed kernel, dispatches 48
// parallel threads (each writing a 32-byte row of their unique marker),
// and verifies CPU-side that every row contains the expected pattern.
// Returns B_OK if all 48 rows are correct, B_ERROR otherwise.
status_t media_pipeline_run_memwrite_test(void);

// Phase 2.1: SAXPY per-thread block size. 48 threads × 8 FP32/thread
// = 384 elements = 1536 bytes per buffer. The kernel hardcodes alpha
// and computes c[i] = 2.0f * x[i] + y[i] for its slice.
#define MEDIA_SAXPY_ELEMENTS 384

// Phase 2.1: upload the embedded saxpy_simd8 kernel into kernel_bo.
status_t media_pipeline_upload_saxpy_kernel(media_pipeline_context* ctx);

// Phase 2.1: pre-fill input_x_bo and input_y_bo with `n_elements`
// deterministic FP32 values each, configure 3 surface states (x, y,
// c) at distinct offsets in surface_state_bo, and write a 3-entry
// binding table. Must be called before submitting the saxpy kernel.
status_t media_pipeline_setup_saxpy_buffers(media_pipeline_context* ctx,
	uint32 n_elements);

// Phase 2.1: first-arithmetic-kernel test. Initializes the pipeline,
// uploads the saxpy kernel, populates the two input buffers, dispatches
// 48 parallel threads (each computing 8 FP32 elements of
// c = 2.0f*x + y), and CPU-side verifies every output element against
// a bit-exact reference. Returns B_OK on full match, B_ERROR otherwise.
status_t media_pipeline_run_saxpy_test(void);

// Phase 2.1b — Milestone 1: run the saxpy kernel at 3 dispatch sizes
// (48, 256, 1024) to prove that batch dispatch can exceed the VFE
// concurrent thread pool (48). Each size sets up buffers for
// dispatch_count * 8 FP32 elements, dispatches, waits, and verifies
// bit-exact. Returns B_OK if all sizes pass.
status_t media_pipeline_run_saxpy_bench_test(void);

// Phase 2.1b — Milestone 2: SAXPY performance test. Fixes dispatch
// count at 400 threads (3200 FP32 elements per iteration), repeats
// N iterations GPU-side and CPU-side on the same input/output data,
// reports MB/s and MFLOPS for both paths and the GPU/CPU ratio.
// A warmup dispatch is verified bit-exact before timing starts.
status_t media_pipeline_run_saxpy_perf_test(void);

// Level 2 POC: GEMV (matrix x vector) on the Gen5 EUs, verified vs CPU.
status_t media_pipeline_run_gemv_test(void);

// Prefill POC: multi-token GEMM (W @ X for T token columns) in one dispatch.
status_t media_pipeline_run_prefill_test(void);

// Public persistent GEMV (LLM matmul). N=288 only; d must be a multiple
// of 8. Caller falls back to CPU on B_BAD_VALUE.
status_t media_pipeline_gemv_open(void);
status_t media_pipeline_gemv(const float* w, const float* x, float* out,
	int n, int d);
// Batched: `count` weights sharing x, in one dispatch. N=288, count*d/8<=1024.
status_t media_pipeline_gemv_stacked(const float* const* w, int count,
	const float* x, float* const* out, int n, int d);
void media_pipeline_gemv_close(void);

// Phase 2.2: first kernel using the data port Media Block Read
// message via sampler cache. Sets up a SURFTYPE_2D input surface
// pre-filled with the pattern byte[i] = i & 0xff, a SURFTYPE_BUFFER
// output surface, dispatches one thread running sampler_read.g4a,
// and verifies CPU-side that the first 32 output bytes match the
// input pattern. Returns B_OK if the read/write round-trip is
// bit-exact.
status_t media_pipeline_run_sampler_test(void);

// Phase 2.2b: multi-row Media Block Read with variable (X, Y) origin.
// Input surface is 64 bytes wide × 8 rows tall, pre-filled with
// byte[i] = i & 0xff. For each of four (X, Y) sub-tests, dispatches
// sampler_read_4row.g4a which reads a 32×4 block and writes its
// 128 bytes to the output buffer. CPU verifier checks each block
// against the expected pattern at the given origin. Returns B_OK
// if all sub-tests pass.
status_t media_pipeline_run_sampler_2b_test(void);

// Phase 2.2c: prove the CURBE mechanism end-to-end. Fills curbe_bo
// with an 8-DWORD known pattern, sets the interface descriptor's
// const_urb_entry_read_len to 1 (push one GRF from CS URB into the
// thread payload at R1), dispatches curbe_read.g4a which copies
// g1 to the output, and verifies CPU-side that the first 8 DWORDs
// of output match the pattern. Returns B_OK on match.
status_t media_pipeline_run_curbe_test(void);

// Phase 2.3a: libva ABI discovery probe. Pushes 30 GRFs of recognizable
// pattern as CURBE (matching libva MPEG-2's const_urb_entry_read_len=30
// configuration), dispatches a single thread that dumps g1, g15, g30,
// and g31 to the output BO, and verifies that the 30-GRF push landed
// correctly AND that MEDIA_OBJECT inline data lands at g31 (immediately
// after the CURBE region). Returns B_OK on full 32/32 DWORD match.
status_t media_pipeline_run_libva_probe_test(void);

// Phase 2.3b: first MPEG-2-style integer arithmetic kernel. Runs an IQ
// intra pass on one 8x8 block of S16 DCT coefficients:
//   out[i] = (coeff[i] * quant_matrix[i] * quant_scale) >> 4
//   out[0] = coeff[0] * intra_dc_mult     (DC override)
// Uses CURBE to deliver the 64-byte quant matrix + scalars (read_len=3),
// OWord Block Read for input, OWord Block Write for output, 2-entry
// binding table. Inputs are chosen so no saturation is needed; output
// is verified bit-exact against an in-process CPU reference computing
// the same algorithm.
status_t media_pipeline_run_iq_intra_test(void);

// Phase 3.1: standalone 2-pass IDCT on a single 8x8 block. Loads the
// MPEG-2 cosine table into CURBE (g5-g20), reads 64 S16 DCT coefficients
// from BTI 0, applies the row transform (dp4 × 8 cosine rows, round,
// shift, narrow) then the column transform (same algorithm on the
// transposed intermediate), and writes 64 S16 results to BTI 1.
// Runs two sub-tests: DC-only (flat output) and mixed-AC (varied output).
// Both are verified bit-exact against a CPU reference implementing the
// same algorithm. Returns B_OK if both pass.
status_t media_pipeline_run_idct_test(void);

// Phase 3.2: combined IQ + IDCT for one intra 8x8 block. Chains inverse
// quantization and 2-pass IDCT in a single GPU dispatch, then adds the
// DC offset (+128) and clamps to [0,255] via mov.sat, producing 64 U8
// pixel values. CURBE carries the IQ matrix, scalars, and IDCT cosine
// table (same layout as the separate tests). Verified bit-exact against
// a CPU reference that chains compute_iq_intra_reference + compute_idct_
// reference + offset + clamp.
status_t media_pipeline_run_iq_idct_test(void);

// Phase 3.4: parse embedded MPEG-2 I-frame, dispatch all Y blocks to
// GPU via IQ+IDCT kernel, and verify output pixels against CPU decode.
status_t media_pipeline_run_parse_gpu_test(void);

// Phase 3.6: verify the IDCT-only kernel (idct_to_u8.g4a) against the
// CPU reference. Uses synthetic IQ'd coefficients (DC=1024, some AC),
// dispatches one block at (0,0) via Media Block Write to SURFTYPE_2D,
// and compares the 64 U8 output pixels bit-exact against
// compute_idct_reference + clamp[0,255]. This isolates the GPU IDCT +
// clamp + 2D-write path from the parser, validating Phase 3.6 readiness.
status_t media_pipeline_run_idct_to_u8_test(void);

// Phase 3.9: GPU forward motion compensation test. Dispatches the
// mc_forward.g4a kernel with 4 half-pel interpolation cases on a
// synthetic reference frame. Verifies pixel-exact against CPU reference.
status_t media_pipeline_run_mc_test(void);

// Phase 3.9 benchmark: measures IDCT throughput on GPU vs CPU.
status_t media_pipeline_run_mc_bench(void);

// Set up the IDCT-to-U8 pipeline: upload kernel, configure surfaces
// for 'block_count' stacked 8x8 blocks (output 8 x block_count*8),
// CURBE cosine table, binding table. After this, call
// submit_blocks_batch_gpu() per frame.
status_t media_pipeline_setup_idct_to_u8(media_pipeline_context* ctx,
	uint32 block_count);

// One block for GPU batch dispatch: 64 S16 coefficients + (x,y) position.
struct gpu_block_entry {
	int16	coeffs[64];
	uint32	x;
	uint32	y;
};

// Submit up to 400 IDCT blocks to the GPU in a single ring submission.
// ctx must have been set up with media_pipeline_setup_idct_to_u8().
// Output pixels are in ctx->output_bo at y-stacked 8-wide blocks.
status_t submit_blocks_batch_gpu(media_pipeline_context* ctx,
	const gpu_block_entry* blocks, uint32 count);

// Tile fill entry: coordinates + fill color.
struct gpu_tile_entry {
	uint32	x;		// pixel X of tile top-left
	uint32	y;		// pixel Y of tile top-left
	uint32	color;	// fill value: U8 for grayscale, ARGB for RGBA
};

// Set up the tile fill pipeline (U8 grayscale output).
status_t media_pipeline_setup_tile_fill(media_pipeline_context* ctx,
	uint32 width, uint32 height);
status_t submit_tile_fill_batch(media_pipeline_context* ctx,
	const gpu_tile_entry* tiles, uint32 count);

// RGBA tile fill: writes 8×8 tiles as 32-bit ARGB directly to output_bo.
status_t media_pipeline_setup_tile_fill_rgba(media_pipeline_context* ctx,
	uint32 width, uint32 height);
status_t submit_tile_fill_rgba_batch(media_pipeline_context* ctx,
	const gpu_tile_entry* tiles, uint32 count);

// BLT output_bo to screen framebuffer via XY_SRC_COPY_BLT.
status_t media_pipeline_blt_to_screen(media_pipeline_context* ctx,
	uint32 src_width, uint32 src_height,
	uint32 dst_x, uint32 dst_y);


#endif // MEDIA_PIPELINE_H
