/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Gen5 media-pipeline bring-up — implementation.
 */


#include "media_pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "accelerant.h"
#include "bench.h"
#include "commands.h"
#include "gpu_debug.h"
#include "intel_extreme.h"


#define LOG(x...)	_sPrintf("intel_extreme media: " x)


// ---------------------------------------------------------------------------
// Command opcodes (Gen4/5 CMD(pipeline, op, sub_op) encoding)
// ---------------------------------------------------------------------------

// 3 << 29 = common command prefix
#define CMD_GFX(p, o, s) \
	((3u << 29) | ((uint32)(p) << 27) | ((uint32)(o) << 24) \
		| ((uint32)(s) << 16))

#define CMD_URB_FENCE				CMD_GFX(0, 0, 0)	// 0x60000000
#define CMD_CS_URB_STATE			CMD_GFX(0, 0, 1)	// 0x60010000
#define CMD_CONSTANT_BUFFER			CMD_GFX(0, 0, 2)	// 0x60020000
#define CMD_STATE_BASE_ADDRESS		CMD_GFX(0, 1, 1)	// 0x61010000
#define CMD_PIPELINE_SELECT			CMD_GFX(1, 1, 4)	// 0x69040000
#define CMD_MEDIA_STATE_POINTERS	CMD_GFX(2, 0, 0)	// 0x70000000
#define CMD_MEDIA_OBJECT			CMD_GFX(2, 1, 0)	// 0x71000000
#define CMD_3DSTATE_DEPTH_BUFFER	CMD_GFX(3, 1, 5)	// 0x79050000

#define PIPELINE_SELECT_MEDIA		1u

#define UF0_VFE_REALLOC				(1u << 12)
#define UF0_CS_REALLOC				(1u << 13)
#define UF2_VFE_FENCE_SHIFT			10
#define UF2_CS_FENCE_SHIFT			20

#define BASE_ADDRESS_MODIFY			1u

#define VFE_GENERIC_MODE			0u

#define I965_DEPTHFORMAT_D32_FLOAT	1u
#define I965_SURFACE_NULL			7u

// CS URB region size in URB units. Matches libva-intel-driver's
// i965_media_mpeg2.c size_cs_entry=16. Previously this was 1 (our
// naive "we don't use CURBE data so one placeholder unit is enough"
// assumption). 1-unit caused a deterministic failure pattern when
// emitting >48 back-to-back MEDIA_OBJECTs: the hardware command
// streamer appears to need at least 16 units of CS URB backing for
// internal state, and under-sized allocation corrupts the adjacent
// VFE region in a dispatch-count-dependent way.
#define CS_URB_SIZE_UNITS			16


// ---------------------------------------------------------------------------
// Embedded EU kernels. Assembled at build time from kernels/*.g4a by the
// in-tree intel-gen4asm (tools/gen4asm/), emitted as C array initializer
// fragments and included here. Never shipped as precompiled blobs.
// ---------------------------------------------------------------------------

static const uint32 kHelloWorldKernel[][4] = {
#include "kernels/hello_world.g4b.gen5"
};

static const uint32 kMemsetIndexedKernel[][4] = {
#include "kernels/memset_indexed.g4b.gen5"
};

static const uint32 kSaxpyKernel[][4] = {
#include "kernels/saxpy_simd8.g4b.gen5"
};

static const uint32 kSamplerReadKernel[][4] = {
#include "kernels/sampler_read.g4b.gen5"
};

static const uint32 kSamplerRead4RowKernel[][4] = {
#include "kernels/sampler_read_4row.g4b.gen5"
};


// ---------------------------------------------------------------------------
// Gen5 surface format / type constants (subset of i965_defines.h).
// ---------------------------------------------------------------------------

#define I965_SURFACE_2D					1
#define I965_SURFACE_BUFFER				4
#define I965_SURFACEFORMAT_R8_UINT		0x143
#define I965_SURFACEFORMAT_RAW			0x1FF


// ---------------------------------------------------------------------------
// BO sizes (Gen5 hello-world minimums, rounded up to kernel allocator grain)
// ---------------------------------------------------------------------------

#define BATCH_BO_SIZE		4096	// batch DWORDs + markers, huge safety margin
#define KERNEL_BO_SIZE		512		// tiny kernels (3-12 instructions, 48-192 bytes)
#define VFE_STATE_BO_SIZE	64		// struct i965_vfe_state = 24 bytes, pad to 64
#define IDRT_BO_SIZE		64		// one interface descriptor = 16 bytes, pad
#define CURBE_BO_SIZE		256		// CURBE contents (1 entry × 1 oword for hello)
#define OUTPUT_BO_SIZE		65536	// where the kernel writes its result (up to 16K FP32)
#define MARKER_BO_SIZE		256		// 32 DWORDs of marker slots
#define SURFACE_STATE_BO_SIZE	256	// up to 8 surface states × 32 bytes stride
#define BINDING_TABLE_BO_SIZE	64	// 1 DWORD per entry, 32-byte aligned
#define INPUT_BO_SIZE			65536	// SAXPY input buffers (x, y), up to 16K FP32


// ---------------------------------------------------------------------------
// Lightweight batch writer: accumulate DWORDs into a stack-local buffer,
// then hand them off to either the ring buffer (via QueueCommands) or a
// batch BO at submit time. Having the accumulator be pipeline-agnostic
// lets us use the same emit_* functions for both submission modes.
//
// We use direct-ring emission for the hello-world probe because Gen5's
// MI_STORE_DATA_IMM (GGTT) stores inside a non-secure MI_BATCH_BUFFER_START
// batch silently drop their writes — observed on this hardware: every
// marker slot remained at its sentinel even though ACTHD advanced past
// them. Direct ring emission avoids the batch altogether and matches the
// pattern already proven working by render.cpp's 3D marker diagnostics.
// ---------------------------------------------------------------------------

#define BATCH_CAPACITY_DW 8192

struct batch_writer {
	uint32	dwords[BATCH_CAPACITY_DW];
	uint32	count;
	bool	overflow;
};


static void
bw_init(batch_writer* w)
{
	w->count = 0;
	w->overflow = false;
}


static void
bw_emit(batch_writer* w, uint32 dword)
{
	if (w->count >= BATCH_CAPACITY_DW) {
		w->overflow = true;
		return;
	}
	w->dwords[w->count++] = dword;
}


// Emit an MI_STORE_DATA_IMM marker that writes the tag for the given
// marker slot to the marker BO at slot * 4 bytes.
static void
bw_emit_marker(batch_writer* w, const media_pipeline_context* ctx,
	media_marker_slot slot)
{
	uint32 dw[4];
	uint32 target_gtt = ctx->marker_bo.gtt_offset + (uint32)slot * 4;
	gpu_debug_marker_dwords(dw, target_gtt, MEDIA_MARKER_TAG(slot));
	for (int i = 0; i < 4; i++)
		bw_emit(w, dw[i]);
}


// ---------------------------------------------------------------------------
// Per-command emitters — each corresponds to one step in
// MEDIA_PIPELINE_BRINGUP.md §1.
// ---------------------------------------------------------------------------

static void
emit_mi_flush(batch_writer* w)
{
	bw_emit(w, MI_FLUSH);
}


static void
emit_3dstate_depth_buffer_null(batch_writer* w)
{
	// 6 DWORDs total. Length field = 4 (= 6 - 2).
	// NULL surface_type, D32_FLOAT format (ignored because NULL).
	bw_emit(w, CMD_3DSTATE_DEPTH_BUFFER | 4);
	bw_emit(w, (I965_DEPTHFORMAT_D32_FLOAT << 18) | (I965_SURFACE_NULL << 29));
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
}


static void
emit_pipeline_select_media(batch_writer* w)
{
	bw_emit(w, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
}


// URB partitioning for the media pipeline.
// 'vfe_entries' is the number of VFE URB entries we want (one per
// concurrent thread). Each VFE entry is 1 unit (urb_entry_alloc_size=0
// in VFE state), so the VFE region is vfe_entries units long. The CS
// region immediately follows, sized CS_URB_SIZE_UNITS units.
//
// Fences are authoritative: VFE state MUST declare num_urb_entries
// consistent with the VFE fence, and CS_URB_STATE MUST declare a size
// consistent with the CS fence, or pipeline internals stomp each
// other. Pattern matches i965_media.c urb_layout in libva-intel-driver.
static void
emit_urb_fence(batch_writer* w, uint32 vfe_entries)
{
	if (vfe_entries == 0)
		vfe_entries = 1;

	const uint32 vfe_fence = vfe_entries;				// end of VFE region
	const uint32 cs_fence = vfe_fence + CS_URB_SIZE_UNITS;	// end of CS region

	bw_emit(w, CMD_URB_FENCE | UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
	bw_emit(w, 0);	// VS/GS/CLIP fences (unused in media mode)
	bw_emit(w, (vfe_fence << UF2_VFE_FENCE_SHIFT)
		| (cs_fence << UF2_CS_FENCE_SHIFT));
}


static void
emit_state_base_address_ironlake(batch_writer* w)
{
	// Ironlake variant: 8 DWORDs (Gen4 was 6). All bases = 0 with the
	// MODIFY bit set, meaning "use this value, which is zero".
	bw_emit(w, CMD_STATE_BASE_ADDRESS | 6);	// length - 2 = 6
	bw_emit(w, BASE_ADDRESS_MODIFY);	// general state base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// surface state base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// indirect object base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// instruction base (Gen5 extra)
	bw_emit(w, BASE_ADDRESS_MODIFY);	// general state upper bound
	bw_emit(w, BASE_ADDRESS_MODIFY);	// dynamic state upper bound (Gen5)
	bw_emit(w, BASE_ADDRESS_MODIFY);	// indirect object upper bound
}


static void
emit_media_state_pointers(batch_writer* w,
	const media_pipeline_context* ctx)
{
	// 3 DWORDs. No extended state for hello world.
	bw_emit(w, CMD_MEDIA_STATE_POINTERS | 1);	// length - 2 = 1
	bw_emit(w, 0);								// extended_state: none
	bw_emit(w, ctx->vfe_state_bo.gtt_offset);	// vfe_state pointer
}


static void
emit_cs_urb_state(batch_writer* w)
{
	// 1 CS entry of size CS_URB_SIZE_UNITS. DW1: (size-1) << 4 | num.
	// Size must match what URB_FENCE reserved for the CS region, or
	// the hardware corrupts state when it writes internal CS tracking
	// data beyond the declared region.
	bw_emit(w, CMD_CS_URB_STATE | 0);	// length - 2 = 0
	bw_emit(w, ((CS_URB_SIZE_UNITS - 1u) << 4) | 1u);
}


static void
emit_constant_buffer(batch_writer* w,
	const media_pipeline_context* ctx)
{
	// Bit 8 = CURBE valid. Low bits of DW1 encode size - 1.
	bw_emit(w, CMD_CONSTANT_BUFFER | (1u << 8) | 0);
	bw_emit(w, ctx->curbe_bo.gtt_offset | 0);
}


static void
emit_media_object_hello(batch_writer* w,
	const media_pipeline_context* ctx)
{
	// 6 DWORDs total. Single thread, interface descriptor 0, no indirect
	// payload. The kernel learns its output address from DW4/DW5 which
	// become part of R0/R1 when the thread starts.
	bw_emit(w, CMD_MEDIA_OBJECT | 4);	// length - 2 = 4
	bw_emit(w, 0);						// interface descriptor index 0
	bw_emit(w, 0);						// indirect data length
	bw_emit(w, 0);						// indirect data pointer
	bw_emit(w, ctx->output_bo.gtt_offset);	// parameter 0: output GTT addr
	bw_emit(w, 0);						// parameter 1: unused
}


// Indexed variant used by parallel dispatch. Emits a MEDIA_OBJECT
// command whose inline data exactly fills the VFE URB entry (64 bytes
// = 16 DWORDs for urb_entry_alloc_size=0 which maps to 1 unit of 512
// bits). This is ESSENTIAL: the VFE dispatcher uses the URB entry as
// backing storage for the thread's R1+ payload, and if the inline
// data doesn't fill the entire entry, the remaining bytes contain
// whatever was there from the previous occupant of that URB slot.
// With a 48-entry pool recycling across >48 MEDIA_OBJECTs, stale
// inline data from previous threads leaks into new dispatches and
// corrupts their thread_index in a deterministic dispatch-count-
// dependent pattern (observed: 48 OK, 49→16, 56 OK, 63→48, 64 OK,
// 65→16 etc.).
//
// Reference: libva-intel-driver i965_post_processing.c uses
// CMD_MEDIA_OBJECT | 18 (20 DWORDs total) with size_vfe_entry=1,
// exactly filling one URB unit.
//
// Payload layout after header (DW0-DW3):
//   DW4 = thread_index (kernel reads g1.0)
//   DW5..DW19 = zero padding
// Emit a MEDIA_OBJECT command with arbitrary inline data. Three
// caller-supplied uint32s go into DW4/DW5/DW6 (which the thread sees
// as g1.0/g1.1/g1.2 of its payload with our const_urb_entry_read_len=0
// config); the remaining 13 inline DWORDs are zero-padded so the full
// URB unit is filled — essential to avoid stale-data leakage in
// recycled URB entries (see Phase 2.1b M1 debugging).
static void
emit_media_object_inline3(batch_writer* w,
	uint32 inline0, uint32 inline1, uint32 inline2)
{
	// Length = 18 → command is 20 DWORDs total (header is 2 DW, so
	// 20 - 2 = 18). 4 header DWs (DW0-DW3) + 16 inline data DWs
	// (DW4-DW19 = 64 bytes = 1 URB unit).
	bw_emit(w, CMD_MEDIA_OBJECT | 18);
	bw_emit(w, 0);						// interface descriptor index 0
	bw_emit(w, 0);						// indirect data length = 0
	bw_emit(w, 0);						// indirect data pointer = 0
	bw_emit(w, inline0);				// inline DW0
	bw_emit(w, inline1);				// inline DW1
	bw_emit(w, inline2);				// inline DW2
	for (uint32 i = 3; i < 16; i++)
		bw_emit(w, 0);					// inline DW3..15: zero pad
}


// Indexed variant used by parallel dispatch: single-value inline
// carrying the thread index at g1.0.
static void
emit_media_object_indexed(batch_writer* w, uint32 thread_index)
{
	emit_media_object_inline3(w, thread_index, 0, 0);
}


// ---------------------------------------------------------------------------
// CPU-side setup of state BOs (VFE state, interface descriptor, CURBE,
// output sentinel, marker sentinels). Called before building the batch.
// ---------------------------------------------------------------------------

static void
write_vfe_state(media_pipeline_context* ctx, uint32 max_threads)
{
	// struct i965_vfe_state, 6 DWORDs. Layout per i965_structs.h:4.
	// max_threads is the ACTUAL thread count (1..48), we encode it as
	// count-1 in the register. One URB entry per thread; each URB entry
	// is 1 unit (16 bytes), plenty for a payload-less EOT kernel.
	if (max_threads == 0)
		max_threads = 1;
	if (max_threads > MEDIA_MAX_PARALLEL_THREADS)
		max_threads = MEDIA_MAX_PARALLEL_THREADS;

	gpu_bo_clear(&ctx->vfe_state_bo);

	// DWORD0 (vfe0): extend_vfe_state_present = 0 (no extended state),
	//                scratch_base = 0, per_thread_scratch_space = 0.
	gpu_bo_write32(&ctx->vfe_state_bo, 0, 0);

	// DWORD1 (vfe1):
	//   [1:0]   debug_counter_control = 0
	//   [2]     children_present = 0
	//   [6:3]   vfe_mode = 0 (GENERIC)
	//   [8:7]   pad
	//   [15:9]  num_urb_entries = max_threads (one entry per thread)
	//   [24:16] urb_entry_alloc_size = 0 (= 1 unit - 1)
	//   [31:25] max_threads = count - 1
	uint32 vfe1 = (VFE_GENERIC_MODE << 3)
		| ((max_threads & 0x7f) << 9)	// num_urb_entries
		| (0u << 16)					// urb_entry_alloc_size = 0 → 1 unit
		| (((max_threads - 1) & 0x7f) << 25);	// max_threads = count - 1
	gpu_bo_write32(&ctx->vfe_state_bo, 4, vfe1);

	// DWORD2 (vfe2):
	//   [3:0]  pad
	//   [31:4] interface_descriptor_base = IDRT gtt >> 4
	gpu_bo_write32(&ctx->vfe_state_bo, 8,
		ctx->idrt_bo.gtt_offset & ~0xfu);

	// DWORDs 3-5 unused on Gen5 (struct is 6 DW including pad) — zeros.
	gpu_bo_write32(&ctx->vfe_state_bo, 12, 0);
	gpu_bo_write32(&ctx->vfe_state_bo, 16, 0);
	gpu_bo_write32(&ctx->vfe_state_bo, 20, 0);
}


static void
write_interface_descriptor_ex(media_pipeline_context* ctx,
	uint32 binding_table_entry_count, uint32 binding_table_gtt_offset)
{
	// struct i965_interface_descriptor, 4 DWORDs, per i965_structs.h:150.
	// For hello world and Phase 1.2: no binding table (entry_count=0).
	// For Phase 1.3+: binding_table_entry_count > 0 and
	//                 binding_table_gtt_offset points at binding_table_bo.
	gpu_bo_clear(&ctx->idrt_bo);

	// DWORD0 (desc0):
	//   [3:0]   grf_reg_blocks = count-1, granularity of 16 regs (LSB=0)
	//   [5:4]   pad
	//   [31:6]  kernel_start_pointer = kernel_bo >> 6 (64-byte aligned)
	//
	// PRM Vol 2 Part 2 (Ironlake) IDRT desc0 bits [3:0]:
	//   "Range = [0,15] corresponding to [1,16] 8-register blocks.
	//    Restriction: LSB must be zero, indicating that GRF assignment
	//    is in granularity of 16 GRF registers."
	//
	// We set 2 (= 3 blocks = 24 registers). Must be at least 1 block=1
	// (16 regs) to cover g10 which the saxpy kernel uses; our earlier
	// value of 0 (1 block = 8 regs) was out of range for g10 and caused
	// a deterministic cross-thread corruption visible as the "dispatch
	// = 16k+1 → only 16 correct rows" failure pattern. libva uses 7
	// (AVC/H264), 10 (post-processing) or 15 (MPEG-2) — all larger
	// than our minimal 2.
	uint32 desc0 = 2u	// grf_reg_blocks = 2 → 24 register allocation
		| (ctx->kernel_bo.gtt_offset & ~0x3fu);
	gpu_bo_write32(&ctx->idrt_bo, 0, desc0);

	// DWORD1 (desc1):
	//   [15:0]  exception/flag bits = 0
	//   [16]    floating_point_mode = 0 (IEEE 754)
	//   [18]    single_program_flow = 1 (no branching)
	//   [25:20] const_urb_entry_read_offset = 0
	//   [31:26] const_urb_entry_read_len = 0 (no CURBE consumed)
	uint32 desc1 = (1u << 18);
	gpu_bo_write32(&ctx->idrt_bo, 4, desc1);

	// DWORD2 (desc2): sampler_count = 0, no sampler state.
	gpu_bo_write32(&ctx->idrt_bo, 8, 0);

	// DWORD3 (desc3):
	//   [4:0]   binding_table_entry_count (limit; 0 = none allowed)
	//   [31:5]  binding_table_pointer (32-byte aligned)
	uint32 desc3 = (binding_table_entry_count & 0x1fu)
		| (binding_table_gtt_offset & ~0x1fu);
	gpu_bo_write32(&ctx->idrt_bo, 12, desc3);
}


static void
write_interface_descriptor(media_pipeline_context* ctx)
{
	// Phase 1.1/1.2 default: no binding table.
	write_interface_descriptor_ex(ctx, 0, 0);
}


// Phase 1.3+: configure a linear SURFTYPE_BUFFER surface state at a
// given byte offset within surface_state_bo. `base_gtt` is the GTT
// address of the backing buffer for this surface. `buffer_bytes` is
// the total size of the buffer to expose.
//
// PRM Vol 4 Part 1 §5.10.3 (OWord Block Read/Write) REQUIRES
// `SURFTYPE_BUFFER`. Our earlier SURFTYPE_2D caused the hardware to
// read the Width bit field [18:6] as the low 7 bits of
// "number_of_entries - 1" (since SURFTYPE_BUFFER packs the 27-bit
// num_entries-1 into scattered Width/Height/Depth fields), producing
// a tiny buffer that varied with row_bytes modulo 128 — which matches
// the observed dispatch-count-dependent failure pattern exactly
// (49→16, 56→56, 63→48, 65→16, 66→32, 68+→64 etc).
//
// SURFTYPE_BUFFER layout for num_entries - 1 (27 bits):
//   bits [6:0]  → DW2 "Width"  bits [12:6] of surface state
//   bits [19:7] → DW2 "Height" bits [31:19]
//   bits [26:20]→ DW3 "Depth"  bits [27:21]
// Element size (pitch) is ignored by OWord Block Read/Write — the
// hardware assumes a fixed 16-byte pitch for bounds checking, so
// num_entries = buffer_bytes / 16.
static void
write_linear_surface_state_at(media_pipeline_context* ctx,
	uint32 ss_byte_offset, uint32 base_gtt, uint32 buffer_bytes)
{
	if (buffer_bytes < 16)
		buffer_bytes = 16;
	const uint32 num_entries = buffer_bytes / 16;
	const uint32 ne_m1 = num_entries - 1;

	// DW0: surface_type = BUFFER. Surface format is ignored for
	// OWord Block Read/Write but set to RAW to be explicit.
	uint32 ss0 = ((uint32)I965_SURFACE_BUFFER << 29)
		| ((uint32)I965_SURFACEFORMAT_RAW << 18);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 0, ss0);

	// DW1: base address = backing buffer GTT offset.
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 4, base_gtt);

	// DW2: Width [12:6] = ne_m1[6:0]; Height [31:19] = ne_m1[19:7].
	uint32 width_bits  = ne_m1 & 0x7fu;
	uint32 height_bits = (ne_m1 >> 7) & 0x1fffu;
	uint32 ss2 = (width_bits << 6) | (height_bits << 19);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 8, ss2);

	// DW3: Depth [27:21] = ne_m1[26:20], Surface Pitch [19:3] = 15
	// (element size 16 bytes - 1), though OWord Block Write ignores
	// this field and uses a fixed 16.
	uint32 depth_bits = (ne_m1 >> 20) & 0x7fu;
	uint32 ss3 = (depth_bits << 21) | (15u << 3);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 12, ss3);

	// DW4, DW5: zero (no min_array_elt, no offsets).
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 16, 0);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 20, 0);
}


// Phase 2.2: configure a linear SURFTYPE_2D surface state at a given
// byte offset within surface_state_bo. Used for surfaces accessed by
// the data port Media Block Read message (required surface type per
// PRM Vol 4 Part 1 §5.10.5). The OWord Block Read/Write path uses
// write_linear_surface_state_at (SURFTYPE_BUFFER) instead.
//
// Layout (PRM Vol 4 Part 1 SURFACE_STATE):
//   ss0 [31:29]  surface_type = I965_SURFACE_2D
//   ss0 [26:18]  format
//   ss1 [31:0]   base address (32-byte aligned for Media Block Read)
//   ss2 [18:6]   width - 1 (bytes for 8bpp)
//   ss2 [31:19]  height - 1 (rows)
//   ss3 [20:3]   pitch - 1 (bytes)
//   ss3 [1:0]    tile walk (0 = x-major, linear when tiled_surface=0)
//   ss3 [1]      tiled surface (0 = linear)
static void
write_2d_surface_state_at(media_pipeline_context* ctx,
	uint32 ss_byte_offset, uint32 base_gtt,
	uint32 width_bytes, uint32 height_rows, uint32 pitch_bytes)
{
	const uint32 w_m1 = (width_bytes > 0) ? (width_bytes - 1) : 0;
	const uint32 h_m1 = (height_rows > 0) ? (height_rows - 1) : 0;
	const uint32 p_m1 = (pitch_bytes > 0) ? (pitch_bytes - 1) : 0;

	// ss0: SURFTYPE_2D, format R8_UINT (format content is returned
	// unconverted but it affects boundary clamp semantics).
	uint32 ss0 = ((uint32)I965_SURFACE_2D << 29)
		| ((uint32)I965_SURFACEFORMAT_R8_UINT << 18);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 0, ss0);

	// ss1: base address.
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 4, base_gtt);

	// ss2: width and height.
	uint32 ss2 = ((w_m1 & 0x1fffu) << 6)
		| ((h_m1 & 0x1fffu) << 19);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 8, ss2);

	// ss3: pitch at [20:3], tile_walk/tiled_surface = 0 (linear).
	uint32 ss3 = ((p_m1 & 0x3ffffu) << 3);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 12, ss3);

	// ss4, ss5: zero.
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 16, 0);
	gpu_bo_write32(&ctx->surface_state_bo, ss_byte_offset + 20, 0);
}


// Phase 1.3: configure the output BO as a single surface at offset 0
// of surface_state_bo.
static void
write_output_surface_state(media_pipeline_context* ctx,
	uint32 row_bytes, uint32 row_count)
{
	gpu_bo_clear(&ctx->surface_state_bo);
	write_linear_surface_state_at(ctx, 0, ctx->output_bo.gtt_offset,
		row_bytes * row_count);
}


static void
write_binding_table_one(media_pipeline_context* ctx)
{
	gpu_bo_clear(&ctx->binding_table_bo);
	// Entry 0 = surface state GTT offset. The hardware reads it as a
	// 32-byte-aligned pointer; low 5 bits are ignored.
	gpu_bo_write32(&ctx->binding_table_bo, 0,
		ctx->surface_state_bo.gtt_offset);
}


// Phase 2.1: configure 3 surface states at 32-byte stride within
// surface_state_bo: [0]=input_x, [32]=input_y, [64]=output_c. Also
// populate binding_table_bo with the 3 entries the saxpy kernel reads
// via read(bti=0,1) and write(bti=2). Each surface covers the entire
// backing BO allocation so OWord Block bounds cover any dispatch
// count we throw at it.
static void
write_saxpy_surfaces(media_pipeline_context* ctx, uint32 n_elements)
{
	(void)n_elements;
	gpu_bo_clear(&ctx->surface_state_bo);
	write_linear_surface_state_at(ctx,  0, ctx->input_x_bo.gtt_offset,
		ctx->input_x_bo.size);
	write_linear_surface_state_at(ctx, 32, ctx->input_y_bo.gtt_offset,
		ctx->input_y_bo.size);
	write_linear_surface_state_at(ctx, 64, ctx->output_bo.gtt_offset,
		ctx->output_bo.size);

	gpu_bo_clear(&ctx->binding_table_bo);
	const uint32 ss_base = ctx->surface_state_bo.gtt_offset;
	gpu_bo_write32(&ctx->binding_table_bo, 0, ss_base +  0);
	gpu_bo_write32(&ctx->binding_table_bo, 4, ss_base + 32);
	gpu_bo_write32(&ctx->binding_table_bo, 8, ss_base + 64);
}


static void
prime_output_and_markers(media_pipeline_context* ctx)
{
	// Output sentinel. Kernel will overwrite offset 0 with its result.
	gpu_bo_clear(&ctx->output_bo);
	gpu_bo_write32(&ctx->output_bo, 0, MEDIA_MARKER_SENTINEL);

	// Marker sentinels. Each slot gets a distinctive pre-fill so post-hang
	// readback can distinguish "GPU wrote 0" from "GPU never touched it".
	gpu_bo_clear(&ctx->marker_bo);
	for (uint32 i = 0; i < MEDIA_MARKER_COUNT; i++)
		gpu_bo_write32(&ctx->marker_bo, i * 4, MEDIA_MARKER_SENTINEL);
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

status_t
media_pipeline_init(media_pipeline_context* ctx)
{
	if (ctx == NULL)
		return B_BAD_VALUE;

	memset(ctx, 0, sizeof(*ctx));

	struct bo_spec {
		gpu_bo*		bo;
		const char*	name;
		uint32		size;
		uint32		align;
	};

	bo_spec specs[] = {
		{ &ctx->batch_bo,			"media:batch",		BATCH_BO_SIZE,		4096 },
		{ &ctx->kernel_bo,			"media:kernel",		KERNEL_BO_SIZE,		64 },
		{ &ctx->vfe_state_bo,		"media:vfe_state",	VFE_STATE_BO_SIZE,	32 },
		{ &ctx->idrt_bo,			"media:idrt",		IDRT_BO_SIZE,		16 },
		{ &ctx->curbe_bo,			"media:curbe",		CURBE_BO_SIZE,		64 },
		{ &ctx->output_bo,			"media:output",		OUTPUT_BO_SIZE,		4096 },
		{ &ctx->marker_bo,			"media:marker",		MARKER_BO_SIZE,		64 },
		{ &ctx->surface_state_bo,	"media:surface",	SURFACE_STATE_BO_SIZE, 32 },
		{ &ctx->binding_table_bo,	"media:bindtbl",	BINDING_TABLE_BO_SIZE, 32 },
		{ &ctx->input_x_bo,			"media:input_x",	INPUT_BO_SIZE,		4096 },
		{ &ctx->input_y_bo,			"media:input_y",	INPUT_BO_SIZE,		4096 },
	};

	for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
		status_t status = gpu_bo_alloc(specs[i].bo, specs[i].name,
			specs[i].size, specs[i].align);
		if (status != B_OK) {
			LOG("init: alloc of %s failed, rolling back\n", specs[i].name);
			media_pipeline_uninit(ctx);
			return status;
		}
	}

	// Clear everything to a known state.
	gpu_bo_clear(&ctx->batch_bo);
	gpu_bo_clear(&ctx->kernel_bo);
	gpu_bo_clear(&ctx->vfe_state_bo);
	gpu_bo_clear(&ctx->idrt_bo);
	gpu_bo_clear(&ctx->curbe_bo);
	gpu_bo_clear(&ctx->output_bo);
	gpu_bo_clear(&ctx->marker_bo);
	gpu_bo_clear(&ctx->surface_state_bo);
	gpu_bo_clear(&ctx->binding_table_bo);
	gpu_bo_clear(&ctx->input_x_bo);
	gpu_bo_clear(&ctx->input_y_bo);

	ctx->initialized = true;
	LOG("init: ok, 11 BOs allocated (%u bytes total)\n",
		(unsigned)(BATCH_BO_SIZE + KERNEL_BO_SIZE + VFE_STATE_BO_SIZE
			+ IDRT_BO_SIZE + CURBE_BO_SIZE + OUTPUT_BO_SIZE + MARKER_BO_SIZE
			+ SURFACE_STATE_BO_SIZE + BINDING_TABLE_BO_SIZE
			+ 2 * INPUT_BO_SIZE));
	return B_OK;
}


void
media_pipeline_uninit(media_pipeline_context* ctx)
{
	if (ctx == NULL)
		return;

	gpu_bo_free(&ctx->batch_bo);
	gpu_bo_free(&ctx->kernel_bo);
	gpu_bo_free(&ctx->vfe_state_bo);
	gpu_bo_free(&ctx->idrt_bo);
	gpu_bo_free(&ctx->curbe_bo);
	gpu_bo_free(&ctx->output_bo);
	gpu_bo_free(&ctx->marker_bo);
	gpu_bo_free(&ctx->surface_state_bo);
	gpu_bo_free(&ctx->binding_table_bo);
	gpu_bo_free(&ctx->input_x_bo);
	gpu_bo_free(&ctx->input_y_bo);
	ctx->initialized = false;
}


status_t
media_pipeline_upload_kernel(media_pipeline_context* ctx,
	const uint32* kernel_dwords, uint32 kernel_bytes)
{
	if (ctx == NULL || !ctx->initialized || kernel_dwords == NULL)
		return B_BAD_VALUE;
	if (kernel_bytes == 0 || kernel_bytes > ctx->kernel_bo.size) {
		LOG("upload_kernel: bad size %u (BO is %u)\n",
			kernel_bytes, ctx->kernel_bo.size);
		return B_BAD_VALUE;
	}

	gpu_bo_clear(&ctx->kernel_bo);
	gpu_bo_write(&ctx->kernel_bo, 0, kernel_dwords, kernel_bytes);
	LOG("upload_kernel: %u bytes at gtt=0x%x\n",
		kernel_bytes, ctx->kernel_bo.gtt_offset);
	return B_OK;
}


status_t
media_pipeline_upload_hello_kernel(media_pipeline_context* ctx)
{
	return media_pipeline_upload_kernel(ctx,
		(const uint32*)kHelloWorldKernel, sizeof(kHelloWorldKernel));
}


status_t
media_pipeline_upload_memset_kernel(media_pipeline_context* ctx)
{
	return media_pipeline_upload_kernel(ctx,
		(const uint32*)kMemsetIndexedKernel, sizeof(kMemsetIndexedKernel));
}


status_t
media_pipeline_setup_output_surface(media_pipeline_context* ctx,
	uint32 row_bytes, uint32 row_count)
{
	if (ctx == NULL || !ctx->initialized)
		return B_NOT_INITIALIZED;
	if (row_bytes == 0 || row_count == 0)
		return B_BAD_VALUE;
	if (row_bytes * row_count > ctx->output_bo.size) {
		LOG("setup_output_surface: surface %ux%u = %u bytes too large "
			"for output BO (%u bytes)\n",
			row_bytes, row_count, row_bytes * row_count,
			ctx->output_bo.size);
		return B_BAD_VALUE;
	}

	// Clear output to a distinct sentinel so we can tell 'written 0' from
	// 'never touched'. We use 0xcc per byte which is unlikely to arise
	// naturally from the memset kernel (which writes 0xde, 0xad, or low
	// index bytes).
	memset((void*)ctx->output_bo.cpu_addr, 0xcc, row_bytes * row_count);

	write_output_surface_state(ctx, row_bytes, row_count);
	write_binding_table_one(ctx);

	LOG("setup_output_surface: %ux%u R8_UINT @ output gtt=0x%x, "
		"ss gtt=0x%x, bt gtt=0x%x\n",
		row_bytes, row_count,
		ctx->output_bo.gtt_offset,
		ctx->surface_state_bo.gtt_offset,
		ctx->binding_table_bo.gtt_offset);
	return B_OK;
}


status_t
media_pipeline_submit_hello(media_pipeline_context* ctx)
{
	if (ctx == NULL || !ctx->initialized)
		return B_NOT_INITIALIZED;

	// 1. Populate CPU-written state BOs (single-thread VFE config).
	write_vfe_state(ctx, 1);
	write_interface_descriptor(ctx);
	prime_output_and_markers(ctx);

	// 2. Build the 10-command sequence (plus per-command markers) in
	//    a stack-local DWORD buffer. This is pipeline-agnostic; we copy
	//    to the ring at submit time.
	batch_writer w;
	bw_init(&w);

	bw_emit_marker(&w, ctx, MEDIA_MARKER_START);

	emit_mi_flush(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MI_FLUSH_1);

	emit_3dstate_depth_buffer_null(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_DEPTH_BUFFER);

	emit_pipeline_select_media(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_PIPELINE_SELECT);

	emit_urb_fence(&w, 1);	// hello world = 1 VFE entry
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_URB_FENCE);

	emit_state_base_address_ironlake(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_STATE_BASE);

	emit_media_state_pointers(&w, ctx);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MEDIA_STATE_POINTERS);

	emit_cs_urb_state(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_CS_URB);

	emit_constant_buffer(&w, ctx);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_CONSTANT_BUFFER);

	emit_media_object_hello(&w, ctx);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MEDIA_OBJECT);

	emit_mi_flush(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MI_FLUSH_2);

	// QWORD-pad to 2-DWORD alignment. The ring expects this; an odd
	// DWORD count would leave the ring TAIL mid-QWORD.
	if ((w.count & 1) != 0)
		bw_emit(&w, MI_NOOP);

	if (w.overflow) {
		LOG("submit: BATCH OVERFLOW — increase BATCH_CAPACITY_DW\n");
		return B_NO_MEMORY;
	}

	LOG("submit: built %u dwords (%u bytes), emitting direct to ring\n",
		w.count, w.count * 4);

	// 3. Ensure all CPU writes to state BOs (VFE state, IDRT, kernel,
	//    marker sentinels) are flushed to memory before the GPU reads
	//    them. mfence drains the write-combining buffer for x86 WC
	//    mappings, matching the pattern used in render.cpp.
	gpu_bo_flush_cpu_writes();

	// 4. Emit the accumulated DWORDs directly into the primary render
	//    ring via QueueCommands. This avoids MI_BATCH_BUFFER_START
	//    entirely — we observed that GGTT MI_STORE_DATA_IMM stores
	//    inside a non-secure batch are silently dropped on Gen5 even
	//    though ACTHD advances past them. Direct ring emission runs
	//    in the ring's privilege context and works reliably, as
	//    demonstrated by render.cpp's existing 3D marker diagnostics.
	{
		ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
		QueueCommands queue(ring);
		queue.MakeSpace(w.count);
		for (uint32 i = 0; i < w.count; i++)
			queue.Write(w.dwords[i]);
	}

	LOG("submit: ring kicked, waiting for completion\n");
	return B_OK;
}


// Shared implementation for parallel dispatches across Phases 1.2,
// 1.3, and 2.1. `dispatch_count` is the number of MEDIA_OBJECTs we
// emit; it can exceed the hardware's 48-thread in-flight limit
// because the VFE recycles URB entries as threads reach EOT.
// `bt_entry_count` controls how the interface descriptor is built:
//    0 = no binding table (Phase 1.2 pure-EOT kernels)
//    N = N valid binding table entries, pointing at binding_table_bo
//        which the caller must have pre-populated
// The kernel reads/writes via specific BTI values; the count here is
// only the "limit" field in the IDRT.
static status_t
submit_parallel_generic(media_pipeline_context* ctx, uint32 dispatch_count,
	uint32 bt_entry_count)
{
	if (ctx == NULL || !ctx->initialized)
		return B_NOT_INITIALIZED;
	if (dispatch_count == 0)
		dispatch_count = 1;

	// The VFE pool (in-flight threads) is capped at the hardware max;
	// the dispatch count is NOT capped — extra MEDIA_OBJECTs queue up
	// and run as earlier ones EOT and free URB entries.
	uint32 vfe_pool = dispatch_count;
	if (vfe_pool > MEDIA_MAX_PARALLEL_THREADS)
		vfe_pool = MEDIA_MAX_PARALLEL_THREADS;

	// 1. Configure VFE for 'vfe_pool' concurrent threads and that
	//    many URB entries.
	write_vfe_state(ctx, vfe_pool);
	if (bt_entry_count > 0) {
		write_interface_descriptor_ex(ctx, bt_entry_count,
			ctx->binding_table_bo.gtt_offset);
	} else {
		write_interface_descriptor(ctx);
	}
	// Preserve any pre-written output (surface state setup writes its
	// own sentinel pattern); only reset markers.
	gpu_bo_clear(&ctx->marker_bo);
	for (uint32 i = 0; i < MEDIA_MARKER_COUNT; i++)
		gpu_bo_write32(&ctx->marker_bo, i * 4, MEDIA_MARKER_SENTINEL);

	// 2. Build the command sequence. The preamble is identical to the
	//    hello-world path; we only differ at the MEDIA_OBJECT emission,
	//    where we queue 'thread_count' back-to-back dispatches instead
	//    of one. Each one carries its thread index as inline data so
	//    parallel consumers can differentiate themselves later.
	batch_writer w;
	bw_init(&w);

	bw_emit_marker(&w, ctx, MEDIA_MARKER_START);

	emit_mi_flush(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MI_FLUSH_1);

	emit_3dstate_depth_buffer_null(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_DEPTH_BUFFER);

	emit_pipeline_select_media(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_PIPELINE_SELECT);

	// URB region scaled to the VFE pool size. Under-sizing this is what
	// caused the Phase 1.2 first run to stall at CONSTANT_BUFFER with
	// the VS unit (INSTDONE bit 8) waiting for VFE URB entries that
	// were never allocated.
	emit_urb_fence(&w, vfe_pool);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_URB_FENCE);

	emit_state_base_address_ironlake(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_STATE_BASE);

	emit_media_state_pointers(&w, ctx);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MEDIA_STATE_POINTERS);

	emit_cs_urb_state(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_CS_URB);

	emit_constant_buffer(&w, ctx);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_CONSTANT_BUFFER);

	// Parallel dispatch: N MEDIA_OBJECTs with distinct thread indices.
	// dispatch_count may exceed vfe_pool (48) — VFE recycles URB
	// entries as in-flight threads reach EOT.
	for (uint32 i = 0; i < dispatch_count; i++)
		emit_media_object_indexed(&w, i);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MEDIA_OBJECT);

	emit_mi_flush(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MI_FLUSH_2);

	if ((w.count & 1) != 0)
		bw_emit(&w, MI_NOOP);

	if (w.overflow) {
		LOG("submit_parallel: BATCH OVERFLOW — increase BATCH_CAPACITY_DW "
			"(dispatch_count=%u)\n", (unsigned)dispatch_count);
		return B_NO_MEMORY;
	}

	LOG("submit_parallel: %u dwords (%u bytes) for %u dispatch(es), "
		"vfe_pool=%u, emitting direct to ring\n",
		w.count, w.count * 4, (unsigned)dispatch_count, (unsigned)vfe_pool);

	gpu_bo_flush_cpu_writes();

	{
		ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
		QueueCommands queue(ring);
		queue.MakeSpace(w.count);
		for (uint32 i = 0; i < w.count; i++)
			queue.Write(w.dwords[i]);
	}

	LOG("submit_parallel: ring kicked, %u dispatch(es) queued\n",
		(unsigned)dispatch_count);
	return B_OK;
}


status_t
media_pipeline_submit_parallel(media_pipeline_context* ctx,
	uint32 thread_count)
{
	// Phase 1.2 path: pure-EOT kernels, no binding table required.
	return submit_parallel_generic(ctx, thread_count, 0);
}


// Internal helper used by the Phase 1.3 run harness. Not publicly
// exported because the memwrite test hooks its own run function that
// calls this directly. 1 binding table entry.
static status_t
submit_parallel_memwrite(media_pipeline_context* ctx, uint32 thread_count)
{
	return submit_parallel_generic(ctx, thread_count, 1);
}


// Phase 2.1: submit the SAXPY kernel with 3 binding table entries
// (input_x, input_y, output_c).
static status_t
submit_parallel_saxpy(media_pipeline_context* ctx, uint32 thread_count)
{
	return submit_parallel_generic(ctx, thread_count, 3);
}


bool
media_pipeline_wait_output(media_pipeline_context* ctx, uint32 timeout_us)
{
	if (ctx == NULL || !ctx->initialized)
		return false;
	volatile uint32* slot =
		(volatile uint32*)(ctx->output_bo.cpu_addr + 0);
	const uint32 poll_interval_us = 500;
	uint32 elapsed = 0;
	while (elapsed < timeout_us) {
		if (*slot != MEDIA_MARKER_SENTINEL)
			return true;
		snooze(poll_interval_us);
		elapsed += poll_interval_us;
	}
	return *slot != MEDIA_MARKER_SENTINEL;
}


// Shared run core: init, upload kernel, submit via the provided submit
// function, wait for final marker, dump on outcome, tear down. Returns
// the elapsed time (in microseconds) for the CPU-side submit + GPU
// completion pair via 'out_us' if non-null.
static status_t
run_media_test_core(const char* label,
	status_t (*submit_fn)(media_pipeline_context*, uint32),
	uint32 submit_arg,
	bigtime_t* out_us)
{
	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("%s: init failed %s\n", label, strerror(status));
		return status;
	}

	status = media_pipeline_upload_hello_kernel(&ctx);
	if (status != B_OK) {
		LOG("%s: kernel upload failed %s\n", label, strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	bigtime_t t_start = bench_now_us();
	status = submit_fn(&ctx, submit_arg);
	if (status != B_OK) {
		LOG("%s: submit failed %s\n", label, strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	const uint32 expected_tag =
		MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
	volatile uint32* last_slot = (volatile uint32*)(ctx.marker_bo.cpu_addr
		+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);
	bool completed = gpu_debug_wait_value(last_slot, expected_tag, 500000);
	bigtime_t t_end = bench_now_us();

	bigtime_t elapsed = t_end - t_start;
	if (out_us != NULL)
		*out_us = elapsed;

	media_pipeline_dump_markers(&ctx);

	if (completed) {
		LOG("%s: PASSED\n", label);
	} else {
		LOG("%s: TIMEOUT after 500 ms\n", label);
		gpu_debug_dump_registers("post-timeout");
	}

	media_pipeline_uninit(&ctx);
	return completed ? B_OK : B_TIMED_OUT;
}


// Adapter lambdas-alike to let run_media_test_core treat submit_hello and
// submit_parallel with a uniform signature.
static status_t
submit_hello_adapter(media_pipeline_context* ctx, uint32 ignored)
{
	(void)ignored;
	return media_pipeline_submit_hello(ctx);
}


static status_t
submit_parallel_adapter(media_pipeline_context* ctx, uint32 thread_count)
{
	return media_pipeline_submit_parallel(ctx, thread_count);
}


// Phase 1.3 verification: each row of the output surface should contain
// (0xdead0000 | row_index) replicated across 8 DWORDs. Returns the number
// of correctly-written rows (0..row_count). A correct run returns
// row_count.
static uint32
verify_memwrite_output(media_pipeline_context* ctx,
	uint32 row_bytes, uint32 row_count)
{
	uint32 correct = 0;
	uint32 dwords_per_row = row_bytes / 4;
	uint8* base = (uint8*)ctx->output_bo.cpu_addr;

	for (uint32 row = 0; row < row_count; row++) {
		uint32 expected = 0xdead0000u | row;
		uint32* p = (uint32*)(base + row * row_bytes);
		bool row_ok = true;
		for (uint32 i = 0; i < dwords_per_row; i++) {
			if (p[i] != expected) {
				row_ok = false;
				break;
			}
		}
		if (row_ok)
			correct++;
	}
	return correct;
}


// Dump the first N rows of the output surface for debugging. Row 0 gets
// a full hexdump (all 8 DWORDs); subsequent rows show only the first 4
// for compactness.
static void
dump_memwrite_output(media_pipeline_context* ctx,
	uint32 row_bytes, uint32 row_count, uint32 max_rows)
{
	uint8* base = (uint8*)ctx->output_bo.cpu_addr;
	if (max_rows > row_count)
		max_rows = row_count;

	LOG("output surface dump (first %u of %u rows, %u bytes each):\n",
		max_rows, row_count, row_bytes);

	// Full hexdump of rows 0 and 1 (16 or 32 bytes each depending on
	// row_bytes). Helps tell 'write landed where I expected' from
	// 'write landed somewhere unexpected'.
	if (row_count > 0) {
		uint32 show_dwords = row_bytes / 4;
		if (show_dwords > 8)
			show_dwords = 8;
		uint32* p0 = (uint32*)base;
		if (show_dwords >= 8) {
			LOG("  row  0 full: %08" B_PRIx32 " %08" B_PRIx32
				" %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32
				" %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32 "\n",
				p0[0], p0[1], p0[2], p0[3], p0[4], p0[5], p0[6], p0[7]);
		} else {
			LOG("  row  0 full: %08" B_PRIx32 " %08" B_PRIx32
				" %08" B_PRIx32 " %08" B_PRIx32 "\n",
				p0[0], p0[1], p0[2], p0[3]);
		}
	}

	for (uint32 row = 0; row < max_rows; row++) {
		uint32* p = (uint32*)(base + row * row_bytes);
		uint32 expected = 0xdead0000u | row;
		LOG("  row %2u (exp 0x%08" B_PRIx32 "): %08" B_PRIx32
			" %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32 " %s\n",
			row, expected, p[0], p[1], p[2], p[3],
			(p[0] == expected && p[1] == expected
			&& p[2] == expected && p[3] == expected) ? "OK" : "MISMATCH");
	}
}


status_t
media_pipeline_run_memwrite_test(void)
{
	_sPrintf("intel_extreme media: *** memwrite test entry reached ***\n");

	if (gInfo == NULL || gInfo->shared_info == NULL
		|| gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT memwrite test prerequisites "
			"not met\n");
		return B_NOT_INITIALIZED;
	}

	LOG("==================================================\n");
	LOG("  PHASE 1.3 — MEMORY WRITE TEST\n");
	LOG("==================================================\n");

	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("memwrite: init failed %s\n", strerror(status));
		return status;
	}

	status = media_pipeline_upload_memset_kernel(&ctx);
	if (status != B_OK) {
		LOG("memwrite: kernel upload failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	const uint32 row_bytes = MEDIA_MEMSET_ROW_BYTES;
	const uint32 row_count = MEDIA_MAX_PARALLEL_THREADS;

	status = media_pipeline_setup_output_surface(&ctx, row_bytes, row_count);
	if (status != B_OK) {
		LOG("memwrite: surface setup failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	gpu_debug_hexdump_bo(&ctx.kernel_bo, 0, 24);
	gpu_debug_hexdump_bo(&ctx.surface_state_bo, 0, 6);
	gpu_debug_hexdump_bo(&ctx.binding_table_bo, 0, 1);

	bigtime_t t_start = bench_now_us();
	status = submit_parallel_memwrite(&ctx, row_count);
	if (status != B_OK) {
		LOG("memwrite: submit failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	const uint32 expected_tag =
		MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
	volatile uint32* last_slot = (volatile uint32*)(ctx.marker_bo.cpu_addr
		+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);
	bool completed = gpu_debug_wait_value(last_slot, expected_tag, 1000000);
	bigtime_t t_end = bench_now_us();
	bench_log("memwrite: submit+complete", t_end - t_start);

	media_pipeline_dump_markers(&ctx);

	if (!completed) {
		LOG("memwrite: TIMEOUT — pipeline did not drain\n");
		gpu_debug_dump_registers("memwrite post-timeout");
	} else {
		gpu_debug_dump_registers("memwrite post-complete");
	}

	// Verify CPU-side that every row got its expected marker.
	uint32 correct = verify_memwrite_output(&ctx, row_bytes, row_count);
	dump_memwrite_output(&ctx, row_bytes, row_count, 8);

	LOG("==================================================\n");
	if (correct == row_count) {
		LOG("  PHASE 1.3 RESULT: PASSED — all %u/%u rows correct\n",
			correct, row_count);
		LOG("  Gen5 EU array wrote memory via surface state + binding\n"
			"  table, each thread landed in a distinct output row.\n");
	} else {
		LOG("  PHASE 1.3 RESULT: FAILURE — %u/%u rows correct\n",
			correct, row_count);
	}
	LOG("==================================================\n");

	media_pipeline_uninit(&ctx);
	return (correct == row_count) ? B_OK : B_ERROR;
}


// ---------------------------------------------------------------------------
// Phase 2.1 — first arithmetic kernel (SAXPY)
// ---------------------------------------------------------------------------

status_t
media_pipeline_upload_saxpy_kernel(media_pipeline_context* ctx)
{
	return media_pipeline_upload_kernel(ctx,
		(const uint32*)kSaxpyKernel, sizeof(kSaxpyKernel));
}


status_t
media_pipeline_setup_saxpy_buffers(media_pipeline_context* ctx,
	uint32 n_elements)
{
	if (ctx == NULL || !ctx->initialized)
		return B_NOT_INITIALIZED;
	if (n_elements == 0)
		return B_BAD_VALUE;

	const uint32 bytes = n_elements * (uint32)sizeof(float);
	if (bytes > ctx->input_x_bo.size || bytes > ctx->input_y_bo.size
		|| bytes > ctx->output_bo.size) {
		LOG("saxpy setup: %u bytes exceeds one of the buffers\n", bytes);
		return B_BAD_VALUE;
	}

	// Pre-fill x and y with deterministic FP32 inputs chosen so that
	// 2.0f*x[i] + y[i] is bit-exact representable in IEEE-754:
	//    x[i] = (float)(i + 1)       — integers, exact up to 2^24
	//    y[i] = (float)i * 0.25f     — i*2^-2, all exact
	// Expected output: 2*(i+1) + 0.25*i, also bit-exact.
	float* xp = (float*)ctx->input_x_bo.cpu_addr;
	float* yp = (float*)ctx->input_y_bo.cpu_addr;
	for (uint32 i = 0; i < n_elements; i++) {
		xp[i] = (float)(i + 1);
		yp[i] = (float)i * 0.25f;
	}

	// Pre-fill output with an obvious sentinel so that unwritten slots
	// stand out in the CPU-side dump.
	memset((void*)ctx->output_bo.cpu_addr, 0xcc, bytes);

	write_saxpy_surfaces(ctx, n_elements);

	LOG("saxpy setup: n=%u, x @ gtt=0x%x, y @ gtt=0x%x, c @ gtt=0x%x, "
		"ss @ gtt=0x%x, bt @ gtt=0x%x\n",
		n_elements,
		ctx->input_x_bo.gtt_offset,
		ctx->input_y_bo.gtt_offset,
		ctx->output_bo.gtt_offset,
		ctx->surface_state_bo.gtt_offset,
		ctx->binding_table_bo.gtt_offset);
	return B_OK;
}


// Verify that the first `n_elements` output elements match
// 2.0f*x[i] + y[i] bit-exact. Returns the count of correct elements
// and writes the first mismatching index into `*out_first_mismatch`
// (or n_elements if everything matches).
static uint32
verify_saxpy_output(media_pipeline_context* ctx, uint32 n_elements,
	uint32* out_first_mismatch)
{
	const float* xp = (const float*)ctx->input_x_bo.cpu_addr;
	const float* yp = (const float*)ctx->input_y_bo.cpu_addr;
	const float* cp = (const float*)ctx->output_bo.cpu_addr;

	uint32 correct = 0;
	uint32 first_wrong = n_elements;
	for (uint32 i = 0; i < n_elements; i++) {
		float expected = 2.0f * xp[i] + yp[i];
		uint32 got_bits, exp_bits;
		memcpy(&got_bits, &cp[i], 4);
		memcpy(&exp_bits, &expected, 4);
		if (got_bits == exp_bits)
			correct++;
		else if (first_wrong == n_elements)
			first_wrong = i;
	}
	if (out_first_mismatch != NULL)
		*out_first_mismatch = first_wrong;
	return correct;
}


// Dump a window of output elements. Starts at `start` and shows
// `count` entries. Useful to inspect the boundary between OK and
// MISMATCH regions identified by verify_saxpy_output.
static void
dump_saxpy_window(media_pipeline_context* ctx, uint32 n_elements,
	uint32 start, uint32 count)
{
	if (start >= n_elements)
		return;
	if (start + count > n_elements)
		count = n_elements - start;

	const float* xp = (const float*)ctx->input_x_bo.cpu_addr;
	const float* yp = (const float*)ctx->input_y_bo.cpu_addr;
	const uint32* cp_bits = (const uint32*)ctx->output_bo.cpu_addr;

	LOG("saxpy window dump: elements [%u..%u] of %u\n",
		start, start + count - 1, n_elements);
	LOG("  idx      x        y   exp_bits   got_bits  status\n");
	for (uint32 k = 0; k < count; k++) {
		uint32 i = start + k;
		float expected = 2.0f * xp[i] + yp[i];
		uint32 exp_bits;
		memcpy(&exp_bits, &expected, 4);
		uint32 got_bits = cp_bits[i];
		LOG("  %4u  %8d %8d  0x%08" B_PRIx32 "  0x%08" B_PRIx32 "  %s\n",
			i, (int)xp[i], (int)(yp[i] * 4.0f),
			exp_bits, got_bits,
			(got_bits == exp_bits) ? "OK" : "MISMATCH");
	}
}


status_t
media_pipeline_run_saxpy_test(void)
{
	_sPrintf("intel_extreme media: *** saxpy test entry reached ***\n");

	if (gInfo == NULL || gInfo->shared_info == NULL
		|| gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT saxpy test prerequisites "
			"not met\n");
		return B_NOT_INITIALIZED;
	}

	LOG("==================================================\n");
	LOG("  PHASE 2.1 — SAXPY ARITHMETIC TEST\n");
	LOG("==================================================\n");

	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("saxpy: init failed %s\n", strerror(status));
		return status;
	}

	status = media_pipeline_upload_saxpy_kernel(&ctx);
	if (status != B_OK) {
		LOG("saxpy: kernel upload failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	status = media_pipeline_setup_saxpy_buffers(&ctx, MEDIA_SAXPY_ELEMENTS);
	if (status != B_OK) {
		LOG("saxpy: buffer setup failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	// Dump the full kernel and the 3 surface states before dispatch so
	// the syslog shows exactly what the GPU was asked to do.
	gpu_debug_hexdump_bo(&ctx.kernel_bo, 0, 44);			// 11 instructions
	gpu_debug_hexdump_bo(&ctx.surface_state_bo, 0, 18);		// 3 surfaces × 6 DW
	gpu_debug_hexdump_bo(&ctx.binding_table_bo, 0, 3);		// 3 BT entries

	bigtime_t t_start = bench_now_us();
	status = submit_parallel_saxpy(&ctx, MEDIA_MAX_PARALLEL_THREADS);
	if (status != B_OK) {
		LOG("saxpy: submit failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	const uint32 expected_tag =
		MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
	volatile uint32* last_slot = (volatile uint32*)(ctx.marker_bo.cpu_addr
		+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);
	bool completed = gpu_debug_wait_value(last_slot, expected_tag, 1000000);
	bigtime_t t_end = bench_now_us();
	bench_log("saxpy: submit+complete", t_end - t_start);

	media_pipeline_dump_markers(&ctx);

	if (!completed) {
		LOG("saxpy: TIMEOUT — pipeline did not drain\n");
		gpu_debug_dump_registers("saxpy post-timeout");
	} else {
		gpu_debug_dump_registers("saxpy post-complete");
	}

	uint32 first_mismatch = 0;
	uint32 correct = verify_saxpy_output(&ctx, MEDIA_SAXPY_ELEMENTS,
		&first_mismatch);
	dump_saxpy_window(&ctx, MEDIA_SAXPY_ELEMENTS, 0, 16);

	LOG("==================================================\n");
	if (correct == MEDIA_SAXPY_ELEMENTS) {
		LOG("  PHASE 2.1 RESULT: PASSED — all %u/%u elements correct\n",
			correct, (unsigned)MEDIA_SAXPY_ELEMENTS);
		LOG("  Gen5 EU executed SIMD8 FP32 multiply+add across 3 bound\n"
			"  surfaces, bit-exact vs CPU reference.\n");
	} else {
		LOG("  PHASE 2.1 RESULT: FAILURE — %u/%u elements correct\n",
			correct, (unsigned)MEDIA_SAXPY_ELEMENTS);
	}
	LOG("==================================================\n");

	media_pipeline_uninit(&ctx);
	return (correct == MEDIA_SAXPY_ELEMENTS) ? B_OK : B_ERROR;
}


// ---------------------------------------------------------------------------
// Phase 2.1b Milestone 1 — scaling SAXPY beyond the VFE pool
// ---------------------------------------------------------------------------

// One pass of the scaled saxpy run: (re)initializes the output sentinel,
// populates x/y inputs, dispatches `dispatch_count` MEDIA_OBJECTs, waits
// for the final MI_FLUSH marker, verifies bit-exact, logs wall time.
// Returns the number of correct elements (expected: dispatch_count*8).
static uint32
run_saxpy_size(media_pipeline_context* ctx, uint32 dispatch_count,
	bigtime_t* out_wall_us, bool* out_completed)
{
	const uint32 n_elements = dispatch_count * 8;

	LOG("----------------------------------------\n");
	LOG("  saxpy bench: dispatch=%u, elements=%u\n",
		dispatch_count, n_elements);
	LOG("----------------------------------------\n");

	status_t status = media_pipeline_setup_saxpy_buffers(ctx, n_elements);
	if (status != B_OK) {
		LOG("saxpy bench: setup failed %s\n", strerror(status));
		if (out_wall_us != NULL)
			*out_wall_us = 0;
		if (out_completed != NULL)
			*out_completed = false;
		return 0;
	}

	bigtime_t t_start = bench_now_us();
	status = submit_parallel_saxpy(ctx, dispatch_count);
	if (status != B_OK) {
		LOG("saxpy bench: submit failed %s\n", strerror(status));
		if (out_wall_us != NULL)
			*out_wall_us = 0;
		if (out_completed != NULL)
			*out_completed = false;
		return 0;
	}

	const uint32 expected_tag =
		MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
	volatile uint32* last_slot = (volatile uint32*)(ctx->marker_bo.cpu_addr
		+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);
	// Bigger dispatches need more time. Allow up to 2 seconds per size.
	bool completed = gpu_debug_wait_value(last_slot, expected_tag, 2000000);
	bigtime_t t_end = bench_now_us();

	bigtime_t wall_us = t_end - t_start;
	if (out_wall_us != NULL)
		*out_wall_us = wall_us;
	if (out_completed != NULL)
		*out_completed = completed;

	bench_log("saxpy bench: submit+complete", wall_us);

	if (!completed) {
		LOG("saxpy bench: TIMEOUT at dispatch=%u — pipeline did not drain\n",
			dispatch_count);
		media_pipeline_dump_markers(ctx);
		gpu_debug_dump_registers("saxpy bench post-timeout");
		return 0;
	}

	uint32 first_mismatch = 0;
	uint32 correct = verify_saxpy_output(ctx, n_elements, &first_mismatch);

	if (correct == n_elements) {
		LOG("saxpy bench: %u/%u OK\n", correct, n_elements);
	} else {
		// Mismatch: dump markers and a window around the boundary so
		// we can tell "threads stopped executing" (0xcccccccc sentinel
		// in got_bits) from "threads executed but wrote wrong values"
		// (plausible but incorrect FP32 bits).
		LOG("saxpy bench: %u/%u MISMATCH, first_mismatch_idx=%u\n",
			correct, n_elements, first_mismatch);
		media_pipeline_dump_markers(ctx);
		uint32 win_start = (first_mismatch >= 8) ? first_mismatch - 8 : 0;
		dump_saxpy_window(ctx, n_elements, win_start, 24);
	}

	return correct;
}


status_t
media_pipeline_run_saxpy_bench_test(void)
{
	_sPrintf("intel_extreme media: *** saxpy bench entry reached ***\n");

	if (gInfo == NULL || gInfo->shared_info == NULL
		|| gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT saxpy bench prerequisites "
			"not met\n");
		return B_NOT_INITIALIZED;
	}

	LOG("==================================================\n");
	LOG("  PHASE 2.1b M1 — SAXPY SCALING TEST\n");
	LOG("==================================================\n");

	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("saxpy bench: init failed %s\n", strerror(status));
		return status;
	}

	status = media_pipeline_upload_saxpy_kernel(&ctx);
	if (status != B_OK) {
		LOG("saxpy bench: kernel upload failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	// Dump the kernel once up front — it's identical across all sizes.
	gpu_debug_hexdump_bo(&ctx.kernel_bo, 0, 44);

	// Validation sweep for the grf_reg_blocks fix (0 → 2). Before the
	// fix this pattern showed 16/32/48/64 OK and 17/33/49/65 failing
	// with exactly 16 correct rows because the saxpy kernel uses g10
	// which was out of the allocated 8-register window. After bumping
	// grf_reg_blocks to 2 (24-register allocation) every dispatch
	// count should pass up to the 1024-clamp limit of the kernel
	// mask (0x3ff).
	const uint32 sizes[] = {
		16, 17, 32, 33, 48, 49, 64, 65, 128, 256
	};
	const uint32 num_sizes = sizeof(sizes) / sizeof(sizes[0]);

	uint32 passes = 0;
	bigtime_t wall_us[num_sizes];
	bool complete[num_sizes];
	uint32 correct[num_sizes];

	for (uint32 i = 0; i < num_sizes; i++) {
		wall_us[i] = 0;
		complete[i] = false;
		correct[i] = run_saxpy_size(&ctx, sizes[i],
			&wall_us[i], &complete[i]);
		if (correct[i] == sizes[i] * 8)
			passes++;
	}

	LOG("==================================================\n");
	LOG("  PHASE 2.1b M1 SUMMARY\n");
	LOG("  dispatch    elements       wall_us  ns/elem  result\n");
	for (uint32 i = 0; i < num_sizes; i++) {
		uint32 n_elem = sizes[i] * 8;
		long long ns_per_elem = (wall_us[i] > 0)
			? (long long)((wall_us[i] * 1000) / n_elem) : 0;
		const char* result = !complete[i] ? "TIMEOUT"
			: (correct[i] == n_elem) ? "PASSED" : "MISMATCH";
		LOG("  %8u  %10u  %12lld  %7lld  %s\n",
			sizes[i], n_elem, (long long)wall_us[i],
			ns_per_elem, result);
	}

	if (passes == num_sizes) {
		LOG("  PHASE 2.1b M1 RESULT: PASSED — all %u sizes correct\n",
			num_sizes);
		LOG("  VFE pool of 48 successfully recycled URB entries for\n"
			"  dispatches up to %u MEDIA_OBJECTs per batch.\n",
			sizes[num_sizes - 1]);
	} else {
		LOG("  PHASE 2.1b M1 RESULT: FAILURE — %u/%u sizes passed\n",
			passes, num_sizes);
	}
	LOG("==================================================\n");

	media_pipeline_uninit(&ctx);
	return (passes == num_sizes) ? B_OK : B_ERROR;
}


// ---------------------------------------------------------------------------
// Phase 2.1b Milestone 2 — SAXPY performance test (throughput benchmark)
// ---------------------------------------------------------------------------

// CPU-side reference SAXPY loop. Non-static so the optimiser cannot
// collapse it with the caller; the empty-clobber asm barrier inside
// the outer loop prevents hoisting per-iteration writes.
static void
cpu_saxpy_loop(float* __restrict cp, const float* __restrict xp,
	const float* __restrict yp, uint32 n, uint32 iterations)
{
	for (uint32 iter = 0; iter < iterations; iter++) {
		for (uint32 i = 0; i < n; i++)
			cp[i] = 2.0f * xp[i] + yp[i];
		asm volatile("" : : "r"(cp) : "memory");
	}
}


status_t
media_pipeline_run_saxpy_perf_test(void)
{
	_sPrintf("intel_extreme media: *** saxpy perf test entry reached ***\n");

	if (gInfo == NULL || gInfo->shared_info == NULL
		|| gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT saxpy perf test prerequisites "
			"not met\n");
		return B_NOT_INITIALIZED;
	}

	LOG("==================================================\n");
	LOG("  PHASE 2.1b M2 — SAXPY PERFORMANCE TEST\n");
	LOG("==================================================\n");

	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("perf: init failed %s\n", strerror(status));
		return status;
	}

	status = media_pipeline_upload_saxpy_kernel(&ctx);
	if (status != B_OK) {
		LOG("perf: kernel upload failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	// Dispatch sizing: 400 threads * 8 FP32/thread = 3200 elements per
	// iteration. 400 MEDIA_OBJECTs * 20 DW + ~80 DW preamble = 8080 DW,
	// comfortably under BATCH_CAPACITY_DW (8192). Iterations set high
	// enough that polling overhead doesn't dominate.
	const uint32 dispatch_count = 400;
	const uint32 n_elements = dispatch_count * 8;
	const uint32 iterations = 2000;

	status = media_pipeline_setup_saxpy_buffers(&ctx, n_elements);
	if (status != B_OK) {
		LOG("perf: buffer setup failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	// Warm-up dispatch + bit-exact verify so we catch any regression
	// BEFORE the timing loop produces meaningless numbers.
	LOG("perf: warm-up dispatch (%u threads, %u elements)\n",
		dispatch_count, n_elements);
	status = submit_parallel_saxpy(&ctx, dispatch_count);
	if (status != B_OK) {
		LOG("perf: warmup submit failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	const uint32 expected_tag =
		MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
	volatile uint32* last_slot = (volatile uint32*)(ctx.marker_bo.cpu_addr
		+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);

	if (!gpu_debug_wait_value(last_slot, expected_tag, 1000000)) {
		LOG("perf: warmup TIMEOUT — pipeline did not drain\n");
		gpu_debug_dump_registers("perf warmup post-timeout");
		media_pipeline_uninit(&ctx);
		return B_ERROR;
	}

	uint32 first_mismatch = 0;
	uint32 warm_correct = verify_saxpy_output(&ctx, n_elements,
		&first_mismatch);
	if (warm_correct != n_elements) {
		LOG("perf: warmup VERIFY FAILED %u/%u, first_wrong=%u\n",
			warm_correct, n_elements, first_mismatch);
		media_pipeline_uninit(&ctx);
		return B_ERROR;
	}
	LOG("perf: warmup OK, %u/%u elements correct\n",
		warm_correct, n_elements);

	// --- GPU timing loop ---
	LOG("perf: running GPU loop, %u iterations\n", iterations);
	bigtime_t t_gpu_start = bench_now_us();
	for (uint32 iter = 0; iter < iterations; iter++) {
		status_t s = submit_parallel_saxpy(&ctx, dispatch_count);
		if (s != B_OK) {
			LOG("perf: GPU iter %u submit failed %s\n", iter, strerror(s));
			media_pipeline_uninit(&ctx);
			return s;
		}
		if (!gpu_debug_wait_value(last_slot, expected_tag, 100000)) {
			LOG("perf: GPU iter %u TIMEOUT\n", iter);
			gpu_debug_dump_registers("perf GPU iter timeout");
			media_pipeline_uninit(&ctx);
			return B_ERROR;
		}
	}
	bigtime_t t_gpu_end = bench_now_us();
	bigtime_t gpu_wall_us = t_gpu_end - t_gpu_start;
	LOG("perf: GPU loop done in %lld us\n", (long long)gpu_wall_us);

	// --- CPU reference loop ---
	// Copy inputs from WC-mapped GTT into plain cacheable heap arrays so
	// the CPU reference reflects best-case cache-resident performance,
	// not WC-read latency. Output goes to a cached buffer as well.
	float* cpu_x = (float*)malloc(n_elements * sizeof(float));
	float* cpu_y = (float*)malloc(n_elements * sizeof(float));
	float* cpu_c = (float*)malloc(n_elements * sizeof(float));
	if (cpu_x == NULL || cpu_y == NULL || cpu_c == NULL) {
		LOG("perf: CPU buffer alloc failed\n");
		free(cpu_x); free(cpu_y); free(cpu_c);
		media_pipeline_uninit(&ctx);
		return B_NO_MEMORY;
	}
	memcpy(cpu_x, (const void*)ctx.input_x_bo.cpu_addr,
		n_elements * sizeof(float));
	memcpy(cpu_y, (const void*)ctx.input_y_bo.cpu_addr,
		n_elements * sizeof(float));
	memset(cpu_c, 0, n_elements * sizeof(float));

	LOG("perf: running CPU loop, %u iterations\n", iterations);
	bigtime_t t_cpu_start = bench_now_us();
	cpu_saxpy_loop(cpu_c, cpu_x, cpu_y, n_elements, iterations);
	bigtime_t t_cpu_end = bench_now_us();
	bigtime_t cpu_wall_us = t_cpu_end - t_cpu_start;
	LOG("perf: CPU loop done in %lld us\n", (long long)cpu_wall_us);

	// Spot check that CPU and GPU produced the same first element so
	// we didn't accidentally benchmark two different computations.
	uint32 gpu_bits, cpu_bits;
	memcpy(&gpu_bits, (const void*)ctx.output_bo.cpu_addr, 4);
	memcpy(&cpu_bits, &cpu_c[0], 4);
	LOG("perf: element[0] GPU=0x%08" B_PRIx32 " CPU=0x%08" B_PRIx32 " %s\n",
		gpu_bits, cpu_bits, (gpu_bits == cpu_bits) ? "MATCH" : "DIFFER");

	free(cpu_x); free(cpu_y); free(cpu_c);

	// --- Metrics ---
	// Per iteration: 3200 elements * 4 bytes * 3 buffers = 38400 B moved,
	// 3200 elements * 2 FLOPs = 6400 FLOPs.
	const uint64 per_iter_bytes = (uint64)n_elements * 4 * 3;
	const uint64 per_iter_flops = (uint64)n_elements * 2;
	const uint64 total_bytes = per_iter_bytes * iterations;
	const uint64 total_flops = per_iter_flops * iterations;

	// Integer-only formatting: bytes/us = MB/s, flops/us = MFLOPS.
	long long gpu_mbps   = (gpu_wall_us > 0) ? (long long)(total_bytes / gpu_wall_us) : 0;
	long long gpu_mflops = (gpu_wall_us > 0) ? (long long)(total_flops / gpu_wall_us) : 0;
	long long cpu_mbps   = (cpu_wall_us > 0) ? (long long)(total_bytes / cpu_wall_us) : 0;
	long long cpu_mflops = (cpu_wall_us > 0) ? (long long)(total_flops / cpu_wall_us) : 0;

	// Ratio GPU/CPU in hundredths (integer): so 125 = 1.25x.
	long long ratio_bps_x100  = (cpu_wall_us > 0 && gpu_wall_us > 0)
		? (long long)((cpu_wall_us * 100) / gpu_wall_us) : 0;

	LOG("==================================================\n");
	LOG("  PHASE 2.1b M2 RESULTS\n");
	LOG("  dispatch=%u, n_elements=%u, iterations=%u\n",
		dispatch_count, n_elements, iterations);
	LOG("  per iter: %llu bytes moved, %llu FLOPs\n",
		(unsigned long long)per_iter_bytes,
		(unsigned long long)per_iter_flops);
	LOG("  total:    %llu bytes moved, %llu FLOPs\n",
		(unsigned long long)total_bytes,
		(unsigned long long)total_flops);
	LOG("  ----------------------------------------\n");
	LOG("  GPU wall: %lld us   %lld MB/s   %lld MFLOPS\n",
		(long long)gpu_wall_us, gpu_mbps, gpu_mflops);
	LOG("  CPU wall: %lld us   %lld MB/s   %lld MFLOPS\n",
		(long long)cpu_wall_us, cpu_mbps, cpu_mflops);
	LOG("  ----------------------------------------\n");
	LOG("  GPU/CPU throughput ratio: %lld.%02lldx\n",
		ratio_bps_x100 / 100, ratio_bps_x100 % 100);
	LOG("==================================================\n");

	media_pipeline_uninit(&ctx);
	return B_OK;
}


// ---------------------------------------------------------------------------
// Phase 2.2 — first kernel using Media Block Read via sampler cache
// ---------------------------------------------------------------------------

// Upload the embedded sampler_read kernel.
static status_t
upload_sampler_kernel(media_pipeline_context* ctx)
{
	return media_pipeline_upload_kernel(ctx,
		(const uint32*)kSamplerReadKernel, sizeof(kSamplerReadKernel));
}


// Configure the surfaces and binding table for the sampler read test:
//   BTI 0 = input, SURFTYPE_2D, 32 bytes wide × 1 row, linear
//   BTI 1 = output, SURFTYPE_BUFFER (reuse OWord Block Write path)
// Pre-fills input with byte[i] = i & 0xff so the first 32 bytes are
// 0x00, 0x01, ..., 0x1f — readable CPU-side for verification.
static void
setup_sampler_surfaces(media_pipeline_context* ctx)
{
	// Pre-fill input buffer with the pattern.
	uint8* xp = (uint8*)ctx->input_x_bo.cpu_addr;
	for (uint32 i = 0; i < 256; i++)
		xp[i] = (uint8)(i & 0xff);

	// Clear output sentinel so unwritten bytes stand out.
	memset((void*)ctx->output_bo.cpu_addr, 0xcc, 64);

	gpu_bo_clear(&ctx->surface_state_bo);

	// BTI 0: SURFTYPE_2D 32×1, pitch 32 bytes, linear.
	write_2d_surface_state_at(ctx, 0, ctx->input_x_bo.gtt_offset,
		32, 1, 32);
	// BTI 1: SURFTYPE_BUFFER covering the whole output BO.
	write_linear_surface_state_at(ctx, 32, ctx->output_bo.gtt_offset,
		ctx->output_bo.size);

	gpu_bo_clear(&ctx->binding_table_bo);
	const uint32 ss_base = ctx->surface_state_bo.gtt_offset;
	gpu_bo_write32(&ctx->binding_table_bo, 0, ss_base +  0);
	gpu_bo_write32(&ctx->binding_table_bo, 4, ss_base + 32);
}


status_t
media_pipeline_run_sampler_test(void)
{
	_sPrintf("intel_extreme media: *** sampler test entry reached ***\n");

	if (gInfo == NULL || gInfo->shared_info == NULL
		|| gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT sampler test prerequisites "
			"not met\n");
		return B_NOT_INITIALIZED;
	}

	LOG("==================================================\n");
	LOG("  PHASE 2.2 — SAMPLER READ TEST\n");
	LOG("==================================================\n");

	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("sampler: init failed %s\n", strerror(status));
		return status;
	}

	status = upload_sampler_kernel(&ctx);
	if (status != B_OK) {
		LOG("sampler: kernel upload failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	setup_sampler_surfaces(&ctx);

	LOG("sampler: input[0..7] = %02x %02x %02x %02x %02x %02x %02x %02x\n",
		((uint8*)ctx.input_x_bo.cpu_addr)[0],
		((uint8*)ctx.input_x_bo.cpu_addr)[1],
		((uint8*)ctx.input_x_bo.cpu_addr)[2],
		((uint8*)ctx.input_x_bo.cpu_addr)[3],
		((uint8*)ctx.input_x_bo.cpu_addr)[4],
		((uint8*)ctx.input_x_bo.cpu_addr)[5],
		((uint8*)ctx.input_x_bo.cpu_addr)[6],
		((uint8*)ctx.input_x_bo.cpu_addr)[7]);
	LOG("sampler: gtt input=0x%x, output=0x%x, ss=0x%x, bt=0x%x\n",
		ctx.input_x_bo.gtt_offset, ctx.output_bo.gtt_offset,
		ctx.surface_state_bo.gtt_offset, ctx.binding_table_bo.gtt_offset);

	gpu_debug_hexdump_bo(&ctx.kernel_bo, 0, 44);
	gpu_debug_hexdump_bo(&ctx.surface_state_bo, 0, 12);	// 2 surfaces × 6 DW
	gpu_debug_hexdump_bo(&ctx.binding_table_bo, 0, 2);

	bigtime_t t_start = bench_now_us();
	status = submit_parallel_generic(&ctx, 1, 2);
	if (status != B_OK) {
		LOG("sampler: submit failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	const uint32 expected_tag =
		MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
	volatile uint32* last_slot = (volatile uint32*)(ctx.marker_bo.cpu_addr
		+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);
	bool completed = gpu_debug_wait_value(last_slot, expected_tag, 1000000);
	bigtime_t t_end = bench_now_us();
	bench_log("sampler: submit+complete", t_end - t_start);

	media_pipeline_dump_markers(&ctx);

	if (!completed) {
		LOG("sampler: TIMEOUT — pipeline did not drain\n");
		gpu_debug_dump_registers("sampler post-timeout");
		media_pipeline_uninit(&ctx);
		return B_ERROR;
	}
	gpu_debug_dump_registers("sampler post-complete");

	// Verify: output[0..31] should match input[0..31] = 0x00..0x1f.
	const uint8* op = (const uint8*)ctx.output_bo.cpu_addr;
	uint32 correct = 0;
	uint32 first_wrong = 32;
	for (uint32 i = 0; i < 32; i++) {
		if (op[i] == (uint8)i)
			correct++;
		else if (first_wrong == 32)
			first_wrong = i;
	}

	LOG("sampler: output[0..31] = ");
	for (uint32 i = 0; i < 32; i++) {
		LOG("%02x%s", op[i], ((i & 7) == 7) ? "\n             " : " ");
	}
	LOG("\n");

	LOG("==================================================\n");
	if (correct == 32) {
		LOG("  PHASE 2.2 RESULT: PASSED — all 32/32 bytes correct\n");
		LOG("  Gen5 EU read a block from a SURFTYPE_2D surface via\n"
			"  Media Block Read through the sampler cache and wrote\n"
			"  the data back via OWord Block Write.\n");
	} else {
		LOG("  PHASE 2.2 RESULT: FAILURE — %u/32 bytes correct, "
			"first_wrong=%u\n", correct, first_wrong);
	}
	LOG("==================================================\n");

	media_pipeline_uninit(&ctx);
	return (correct == 32) ? B_OK : B_ERROR;
}


// ---------------------------------------------------------------------------
// Phase 2.2b — multi-row sampler read with variable (X, Y) origin
// ---------------------------------------------------------------------------

#define SAMPLER_2B_INPUT_WIDTH		64
#define SAMPLER_2B_INPUT_HEIGHT		8
#define SAMPLER_2B_BLOCK_WIDTH		32
#define SAMPLER_2B_BLOCK_HEIGHT		4
#define SAMPLER_2B_BLOCK_BYTES		(SAMPLER_2B_BLOCK_WIDTH \
	* SAMPLER_2B_BLOCK_HEIGHT)


static void
setup_sampler_2b_surfaces(media_pipeline_context* ctx)
{
	// Pre-fill input buffer with byte[i] = i & 0xff across the whole
	// 64-wide × 8-tall surface (512 bytes total).
	const uint32 input_bytes =
		SAMPLER_2B_INPUT_WIDTH * SAMPLER_2B_INPUT_HEIGHT;
	uint8* ip = (uint8*)ctx->input_x_bo.cpu_addr;
	for (uint32 i = 0; i < input_bytes; i++)
		ip[i] = (uint8)(i & 0xff);

	gpu_bo_clear(&ctx->surface_state_bo);

	// BTI 0: SURFTYPE_2D 64×8, pitch 64 bytes, linear.
	write_2d_surface_state_at(ctx, 0, ctx->input_x_bo.gtt_offset,
		SAMPLER_2B_INPUT_WIDTH, SAMPLER_2B_INPUT_HEIGHT,
		SAMPLER_2B_INPUT_WIDTH);
	// BTI 1: SURFTYPE_BUFFER for output, covers the whole BO.
	write_linear_surface_state_at(ctx, 32, ctx->output_bo.gtt_offset,
		ctx->output_bo.size);

	gpu_bo_clear(&ctx->binding_table_bo);
	const uint32 ss_base = ctx->surface_state_bo.gtt_offset;
	gpu_bo_write32(&ctx->binding_table_bo, 0, ss_base +  0);
	gpu_bo_write32(&ctx->binding_table_bo, 4, ss_base + 32);
}


// Dedicated single-thread submit for the sampler 2b kernel. Unlike
// submit_parallel_generic this emits exactly one MEDIA_OBJECT with
// (x, y) in inline DW0/DW1 so the kernel can build its Media Block
// Read header dynamically.
static status_t
submit_sampler_2b(media_pipeline_context* ctx, uint32 x, uint32 y)
{
	if (ctx == NULL || !ctx->initialized)
		return B_NOT_INITIALIZED;

	write_vfe_state(ctx, 1);
	write_interface_descriptor_ex(ctx, 2,
		ctx->binding_table_bo.gtt_offset);

	gpu_bo_clear(&ctx->marker_bo);
	for (uint32 i = 0; i < MEDIA_MARKER_COUNT; i++)
		gpu_bo_write32(&ctx->marker_bo, i * 4, MEDIA_MARKER_SENTINEL);

	batch_writer w;
	bw_init(&w);

	bw_emit_marker(&w, ctx, MEDIA_MARKER_START);
	emit_mi_flush(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MI_FLUSH_1);
	emit_3dstate_depth_buffer_null(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_DEPTH_BUFFER);
	emit_pipeline_select_media(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_PIPELINE_SELECT);
	emit_urb_fence(&w, 1);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_URB_FENCE);
	emit_state_base_address_ironlake(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_STATE_BASE);
	emit_media_state_pointers(&w, ctx);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MEDIA_STATE_POINTERS);
	emit_cs_urb_state(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_CS_URB);
	emit_constant_buffer(&w, ctx);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_CONSTANT_BUFFER);

	emit_media_object_inline3(&w, x, y, 0);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MEDIA_OBJECT);
	emit_mi_flush(&w);
	bw_emit_marker(&w, ctx, MEDIA_MARKER_AFTER_MI_FLUSH_2);

	if ((w.count & 1) != 0)
		bw_emit(&w, MI_NOOP);

	if (w.overflow) {
		LOG("submit_sampler_2b: BATCH OVERFLOW\n");
		return B_NO_MEMORY;
	}

	gpu_bo_flush_cpu_writes();

	{
		ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
		QueueCommands queue(ring);
		queue.MakeSpace(w.count);
		for (uint32 i = 0; i < w.count; i++)
			queue.Write(w.dwords[i]);
	}

	return B_OK;
}


// Check the first SAMPLER_2B_BLOCK_BYTES of output against the
// expected byte pattern for a 32×4 read at (x, y). Returns the
// count of matching bytes and writes the first mismatch index
// (or SAMPLER_2B_BLOCK_BYTES if all match).
static uint32
verify_sampler_2b_block(media_pipeline_context* ctx,
	uint32 x, uint32 y, uint32* first_wrong)
{
	const uint8* op = (const uint8*)ctx->output_bo.cpu_addr;
	uint32 correct = 0;
	uint32 fw = SAMPLER_2B_BLOCK_BYTES;
	for (uint32 row = 0; row < SAMPLER_2B_BLOCK_HEIGHT; row++) {
		for (uint32 col = 0; col < SAMPLER_2B_BLOCK_WIDTH; col++) {
			uint32 idx = row * SAMPLER_2B_BLOCK_WIDTH + col;
			uint32 input_off =
				(y + row) * SAMPLER_2B_INPUT_WIDTH + (x + col);
			uint8 expected = (uint8)(input_off & 0xff);
			if (op[idx] == expected)
				correct++;
			else if (fw == SAMPLER_2B_BLOCK_BYTES)
				fw = idx;
		}
	}
	if (first_wrong != NULL)
		*first_wrong = fw;
	return correct;
}


status_t
media_pipeline_run_sampler_2b_test(void)
{
	_sPrintf("intel_extreme media: *** sampler 2b test entry reached ***\n");

	if (gInfo == NULL || gInfo->shared_info == NULL
		|| gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT sampler 2b test "
			"prerequisites not met\n");
		return B_NOT_INITIALIZED;
	}

	LOG("==================================================\n");
	LOG("  PHASE 2.2b — SAMPLER MULTI-ROW + XY OFFSET\n");
	LOG("==================================================\n");

	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("sampler2b: init failed %s\n", strerror(status));
		return status;
	}

	status = media_pipeline_upload_kernel(&ctx,
		(const uint32*)kSamplerRead4RowKernel,
		sizeof(kSamplerRead4RowKernel));
	if (status != B_OK) {
		LOG("sampler2b: kernel upload failed %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	setup_sampler_2b_surfaces(&ctx);

	LOG("sampler2b: input %ux%u pitch %u, block %ux%u\n",
		SAMPLER_2B_INPUT_WIDTH, SAMPLER_2B_INPUT_HEIGHT,
		SAMPLER_2B_INPUT_WIDTH,
		SAMPLER_2B_BLOCK_WIDTH, SAMPLER_2B_BLOCK_HEIGHT);
	LOG("sampler2b: gtt input=0x%x output=0x%x ss=0x%x bt=0x%x\n",
		ctx.input_x_bo.gtt_offset, ctx.output_bo.gtt_offset,
		ctx.surface_state_bo.gtt_offset,
		ctx.binding_table_bo.gtt_offset);
	gpu_debug_hexdump_bo(&ctx.kernel_bo, 0, 60);
	gpu_debug_hexdump_bo(&ctx.surface_state_bo, 0, 12);
	gpu_debug_hexdump_bo(&ctx.binding_table_bo, 0, 2);

	struct sub_test {
		uint32 x;
		uint32 y;
		const char* label;
	};
	static const sub_test sub_tests[] = {
		{  0, 0, "origin (0,0)"  },
		{ 16, 0, "X offset (16,0)" },
		{  0, 2, "Y offset (0,2)"  },
		{ 16, 2, "XY offset (16,2)"},
	};
	const uint32 num_sub_tests = sizeof(sub_tests) / sizeof(sub_tests[0]);

	uint32 passes = 0;
	for (uint32 t = 0; t < num_sub_tests; t++) {
		const sub_test& st = sub_tests[t];
		LOG("----------------------------------------\n");
		LOG("  sub-test %u/%u: %s\n", t + 1, num_sub_tests, st.label);
		LOG("----------------------------------------\n");

		memset((void*)ctx.output_bo.cpu_addr, 0xcc,
			SAMPLER_2B_BLOCK_BYTES + 16);

		bigtime_t t_start = bench_now_us();
		status = submit_sampler_2b(&ctx, st.x, st.y);
		if (status != B_OK) {
			LOG("sampler2b: submit failed %s\n", strerror(status));
			continue;
		}

		const uint32 expected_tag =
			MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
		volatile uint32* last_slot =
			(volatile uint32*)(ctx.marker_bo.cpu_addr
				+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);
		bool completed = gpu_debug_wait_value(last_slot, expected_tag,
			1000000);
		bigtime_t t_end = bench_now_us();
		bench_log("sampler2b: submit+complete", t_end - t_start);

		if (!completed) {
			LOG("sampler2b: TIMEOUT\n");
			media_pipeline_dump_markers(&ctx);
			gpu_debug_dump_registers("sampler2b post-timeout");
			continue;
		}

		uint32 first_wrong = 0;
		uint32 correct = verify_sampler_2b_block(&ctx, st.x, st.y,
			&first_wrong);

		const uint8* op = (const uint8*)ctx.output_bo.cpu_addr;
		LOG("sampler2b: first 8 bytes = "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			op[0], op[1], op[2], op[3], op[4], op[5], op[6], op[7]);

		if (correct == SAMPLER_2B_BLOCK_BYTES) {
			LOG("sampler2b: PASS (%u/%u bytes)\n",
				correct, (unsigned)SAMPLER_2B_BLOCK_BYTES);
			passes++;
		} else {
			LOG("sampler2b: FAIL (%u/%u, first_wrong=%u)\n",
				correct, (unsigned)SAMPLER_2B_BLOCK_BYTES, first_wrong);
			uint32 ws = (first_wrong >= 4) ? first_wrong - 4 : 0;
			for (uint32 i = ws; i < ws + 16
				&& i < SAMPLER_2B_BLOCK_BYTES; i++) {
				uint32 row = i / SAMPLER_2B_BLOCK_WIDTH;
				uint32 col = i % SAMPLER_2B_BLOCK_WIDTH;
				uint32 in_off = (st.y + row) * SAMPLER_2B_INPUT_WIDTH
					+ (st.x + col);
				uint8 exp = (uint8)(in_off & 0xff);
				LOG("  idx %3u (r%u c%u): got 0x%02x exp 0x%02x %s\n",
					i, row, col, op[i], exp,
					(op[i] == exp) ? "OK" : "MISMATCH");
			}
		}
	}

	LOG("==================================================\n");
	if (passes == num_sub_tests) {
		LOG("  PHASE 2.2b RESULT: PASSED — all %u/%u sub-tests\n",
			passes, num_sub_tests);
		LOG("  Gen5 Media Block Read handles multi-row responses and\n"
			"  non-zero (X, Y) origins. Sampler path is ready for\n"
			"  Phase 2.3 (libva kernel port).\n");
	} else {
		LOG("  PHASE 2.2b RESULT: FAILURE — %u/%u sub-tests passed\n",
			passes, num_sub_tests);
	}
	LOG("==================================================\n");

	media_pipeline_uninit(&ctx);
	return (passes == num_sub_tests) ? B_OK : B_ERROR;
}


status_t
media_pipeline_run_parallel_test(void)
{
	_sPrintf("intel_extreme media: *** parallel test entry reached ***\n");

	if (gInfo == NULL || gInfo->shared_info == NULL
		|| gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT parallel test prerequisites "
			"not met\n");
		return B_NOT_INITIALIZED;
	}

	LOG("==================================================\n");
	LOG("  PHASE 1.2 — PARALLEL DISPATCH TEST\n");
	LOG("==================================================\n");

	bigtime_t hello_us = 0;
	status_t hello_status = run_media_test_core("baseline (1 thread)",
		submit_hello_adapter, 0, &hello_us);
	bench_log("baseline: total 1-thread dispatch", hello_us);

	bigtime_t parallel_us = 0;
	status_t parallel_status = run_media_test_core("parallel (48 threads)",
		submit_parallel_adapter, MEDIA_MAX_PARALLEL_THREADS, &parallel_us);
	bench_log_per_unit("parallel: total 48-thread dispatch",
		parallel_us, MEDIA_MAX_PARALLEL_THREADS, "thread");

	LOG("==================================================\n");
	if (hello_status == B_OK && parallel_status == B_OK) {
		LOG("  PHASE 1.2 RESULT: BOTH PASSED\n");
		if (hello_us > 0 && parallel_us > 0) {
			if (parallel_us >= hello_us) {
				// Parallel slower than baseline (expected: there's
				// per-dispatch overhead). Show ratio > 1.
				bigtime_t ratio_x100 = (parallel_us * 100) / hello_us;
				LOG("  parallel/hello = %lld.%02lldx "
					"(48 threads cost %lldx more total wall time)\n",
					(long long)(ratio_x100 / 100),
					(long long)(ratio_x100 % 100),
					(long long)(ratio_x100 / 100));
			} else {
				// Parallel faster than baseline (ratio < 1). On a
				// polling-based wait this usually means the baseline
				// wait burned a scheduler quantum that the parallel
				// case skipped — not a real per-thread speedup but
				// a timing artifact. Show reciprocal.
				bigtime_t recip_x100 = (hello_us * 100) / parallel_us;
				LOG("  parallel/hello = 1/%lld.%02lldx "
					"(parallel finished in %lld us vs baseline %lld us;\n"
					"   likely a polling-window artifact, not a real "
					"speedup)\n",
					(long long)(recip_x100 / 100),
					(long long)(recip_x100 % 100),
					(long long)parallel_us, (long long)hello_us);
			}
		}
	} else {
		LOG("  PHASE 1.2 RESULT: FAILURE — hello=%s parallel=%s\n",
			strerror(hello_status), strerror(parallel_status));
	}
	LOG("==================================================\n");

	return (hello_status == B_OK && parallel_status == B_OK)
		? B_OK : B_ERROR;
}


status_t
media_pipeline_run_hello_test(void)
{
	// Earliest possible log so we can tell whether the test entry even
	// ran. If the previous line appears in syslog and the next banner
	// does not, the crash is inside one of the first few calls below.
	_sPrintf("intel_extreme media: *** hello test entry reached ***\n");

	// Sanity: gInfo and shared_info must be valid before anything else.
	if (gInfo == NULL) {
		_sPrintf("intel_extreme media: ABORT gInfo is NULL\n");
		return B_NOT_INITIALIZED;
	}
	if (gInfo->shared_info == NULL) {
		_sPrintf("intel_extreme media: ABORT shared_info is NULL\n");
		return B_NOT_INITIALIZED;
	}
	if (gInfo->shared_info->graphics_memory == NULL) {
		_sPrintf("intel_extreme media: ABORT graphics_memory is NULL\n");
		return B_NOT_INITIALIZED;
	}

	// Only fire once per accelerant library load. If app_server restarts
	// within the same library instance, we don't want to repeatedly kick
	// the EU array with a bring-up probe.
	static bool sAlreadyRun = false;
	if (sAlreadyRun) {
		LOG("hello test: already run this session, skipping\n");
		return B_OK;
	}
	sAlreadyRun = true;

	LOG("==================================================\n");
	LOG("  HELLO-WORLD MEDIA PIPELINE TEST — start\n");
	LOG("==================================================\n");

	gpu_debug_dump_registers("pre-init");

	media_pipeline_context ctx;
	status_t status = media_pipeline_init(&ctx);
	if (status != B_OK) {
		LOG("FAIL: media_pipeline_init returned %s\n", strerror(status));
		return status;
	}

	status = media_pipeline_upload_hello_kernel(&ctx);
	if (status != B_OK) {
		LOG("FAIL: kernel upload returned %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	// Pre-submit visibility: where each BO lives, what the kernel looks
	// like, what the GPU is doing right now.
	gpu_bo_dump_live();
	gpu_debug_hexdump_bo(&ctx.kernel_bo, 0, 4);
	gpu_debug_dump_registers("pre-submit");

	status = media_pipeline_submit_hello(&ctx);
	if (status != B_OK) {
		LOG("FAIL: submit returned %s\n", strerror(status));
		media_pipeline_uninit(&ctx);
		return status;
	}

	// The hello-world kernel is a pure EOT — it writes nothing. Completion
	// is signalled by the *last* marker slot (AFTER_MI_FLUSH_2) getting
	// its expected tag. Poll that, not the output BO.
	const uint32 expected_last_tag =
		MEDIA_MARKER_TAG(MEDIA_MARKER_AFTER_MI_FLUSH_2);
	volatile uint32* last_slot = (volatile uint32*)(ctx.marker_bo.cpu_addr
		+ (uint32)MEDIA_MARKER_AFTER_MI_FLUSH_2 * 4);

	bool completed = gpu_debug_wait_value(last_slot, expected_last_tag,
		500000);

	// Full post-mortem regardless of outcome.
	gpu_debug_dump_registers(completed ? "post-complete" : "post-timeout");
	media_pipeline_dump_markers(&ctx);
	gpu_debug_hexdump_bo(&ctx.batch_bo, 0, 80);
	gpu_debug_hexdump_bo(&ctx.vfe_state_bo, 0, 6);
	gpu_debug_hexdump_bo(&ctx.idrt_bo, 0, 4);

	if (completed) {
		LOG("==================================================\n");
		LOG("  HELLO-WORLD TEST: PASSED\n");
		LOG("  EU array executed a kernel authored by us.\n");
		LOG("==================================================\n");
	} else {
		LOG("==================================================\n");
		LOG("  HELLO-WORLD TEST: TIMEOUT (500 ms)\n");
		LOG("  See marker dump above for last reached command.\n");
		LOG("==================================================\n");
	}

	media_pipeline_uninit(&ctx);
	return completed ? B_OK : B_TIMED_OUT;
}


void
media_pipeline_dump_markers(const media_pipeline_context* ctx)
{
	if (ctx == NULL || !ctx->initialized) {
		LOG("dump_markers: context not initialized\n");
		return;
	}

	static const char* kNames[MEDIA_MARKER_COUNT] = {
		"START",
		"after MI_FLUSH #1",
		"after 3DSTATE_DEPTH_BUFFER",
		"after PIPELINE_SELECT(media)",
		"after URB_FENCE",
		"after STATE_BASE_ADDRESS",
		"after MEDIA_STATE_POINTERS",
		"after CS_URB_STATE",
		"after CONSTANT_BUFFER",
		"after MEDIA_OBJECT",
		"after MI_FLUSH #2",
	};

	LOG("marker dump:\n");
	volatile uint32* slots =
		(volatile uint32*)ctx->marker_bo.cpu_addr;
	for (uint32 i = 0; i < MEDIA_MARKER_COUNT; i++) {
		uint32 val = slots[i];
		uint32 expected = MEDIA_MARKER_TAG(i);
		const char* status;
		if (val == expected)
			status = "OK";
		else if (val == MEDIA_MARKER_SENTINEL)
			status = "NOT REACHED";
		else
			status = "WRONG TAG";
		LOG("  [%2u] %-32s  0x%08" B_PRIx32 "  %s\n",
			i, kNames[i], val, status);
	}
	LOG("output[0] = 0x%08" B_PRIx32 " (%s)\n",
		gpu_bo_read32(&((media_pipeline_context*)ctx)->output_bo, 0),
		gpu_bo_read32(&((media_pipeline_context*)ctx)->output_bo, 0)
			== MEDIA_MARKER_SENTINEL ? "UNWRITTEN" : "written");
}
