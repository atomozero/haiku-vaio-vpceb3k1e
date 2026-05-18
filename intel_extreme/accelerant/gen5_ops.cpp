/*
 * gen5_ops.cpp — Gen5 (Ironlake) specific GPU command emitters.
 *
 * Implements the gen_ops interface for Intel HD Graphics Gen5.
 * All command encodings, register offsets, and hardware limits
 * are Ironlake-specific. Other generations implement their own
 * gen*_ops.cpp with the same interface.
 */

#include "gen_ops.h"
#include "intel_extreme.h"


// -------------------------------------------------------------------
// Gen5 command opcodes (CMD(pipeline, op, sub_op) encoding)
// -------------------------------------------------------------------

#define CMD_GFX(p, o, s) \
	((3u << 29) | ((uint32)(p) << 27) | ((uint32)(o) << 24) \
		| ((uint32)(s) << 16))

#define CMD_URB_FENCE			CMD_GFX(0, 0, 0)
#define CMD_CS_URB_STATE		CMD_GFX(0, 0, 1)
#define CMD_CONSTANT_BUFFER		CMD_GFX(0, 0, 2)
#define CMD_STATE_BASE_ADDRESS	CMD_GFX(0, 1, 1)
#define CMD_PIPELINE_SELECT		CMD_GFX(1, 1, 4)
#define CMD_MEDIA_STATE_POINTERS CMD_GFX(2, 0, 0)
#define CMD_3DSTATE_DEPTH_BUFFER CMD_GFX(3, 1, 5)

#define PIPELINE_SELECT_MEDIA	1u

#define UF0_VFE_REALLOC			(1u << 12)
#define UF0_CS_REALLOC			(1u << 13)
#define UF2_VFE_FENCE_SHIFT		10
#define UF2_CS_FENCE_SHIFT		20

#define BASE_ADDRESS_MODIFY		1u

#define I965_DEPTHFORMAT_D32_FLOAT 1u
#define I965_SURFACE_NULL		7u

#define GEN5_CS_URB_SIZE_UNITS	16
#define GEN5_MAX_MEDIA_THREADS	48


// -------------------------------------------------------------------
// Gen5 emitter implementations
// -------------------------------------------------------------------

static void
gen5_emit_mi_flush(batch_writer* w)
{
	bw_emit(w, MI_FLUSH);
}


static void
gen5_emit_pipeline_select_media(batch_writer* w)
{
	bw_emit(w, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
}


static void
gen5_emit_depth_buffer_null(batch_writer* w)
{
	bw_emit(w, CMD_3DSTATE_DEPTH_BUFFER | 4);
	bw_emit(w, (I965_DEPTHFORMAT_D32_FLOAT << 18)
		| (I965_SURFACE_NULL << 29));
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
}


static void
gen5_emit_state_base_address(batch_writer* w)
{
	// Ironlake: 8 DWORDs (Gen4 was 6)
	bw_emit(w, CMD_STATE_BASE_ADDRESS | 6);
	bw_emit(w, BASE_ADDRESS_MODIFY);	// general state
	bw_emit(w, BASE_ADDRESS_MODIFY);	// surface state
	bw_emit(w, BASE_ADDRESS_MODIFY);	// indirect object
	bw_emit(w, BASE_ADDRESS_MODIFY);	// instruction (ILK extra)
	bw_emit(w, BASE_ADDRESS_MODIFY);	// general upper bound
	bw_emit(w, BASE_ADDRESS_MODIFY);	// dynamic upper bound (ILK)
	bw_emit(w, BASE_ADDRESS_MODIFY);	// indirect upper bound
}


static void
gen5_emit_urb_fence(batch_writer* w, uint32 vfe_entries)
{
	if (vfe_entries == 0)
		vfe_entries = 1;
	const uint32 vfe_fence = vfe_entries;
	const uint32 cs_fence = vfe_fence + GEN5_CS_URB_SIZE_UNITS;
	bw_emit(w, CMD_URB_FENCE | UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
	bw_emit(w, 0);
	bw_emit(w, (vfe_fence << UF2_VFE_FENCE_SHIFT)
		| (cs_fence << UF2_CS_FENCE_SHIFT));
}


static void
gen5_emit_cs_urb_state(batch_writer* w)
{
	bw_emit(w, CMD_CS_URB_STATE | 0);
	bw_emit(w, ((GEN5_CS_URB_SIZE_UNITS - 1u) << 4) | 1u);
}


static void
gen5_emit_constant_buffer(batch_writer* w, uint32 gtt_offset,
	uint32 read_length)
{
	if (read_length == 0)
		read_length = 1;
	bw_emit(w, CMD_CONSTANT_BUFFER | (1u << 8) | 0);
	bw_emit(w, gtt_offset | ((read_length - 1u) & 0x1fu));
}


static void
gen5_emit_media_state_pointers(batch_writer* w,
	uint32 vfe_state_gtt, uint32 idrt_gtt)
{
	bw_emit(w, CMD_MEDIA_STATE_POINTERS | 1);
	bw_emit(w, 0);					// extended state: none
	bw_emit(w, vfe_state_gtt);		// VFE state pointer
}


static void
gen5_emit_marker(batch_writer* w, uint32 gtt_offset, uint32 tag)
{
	// MI_STORE_DATA_IMM (GGTT): 4 DWORDs
	bw_emit(w, (0x20u << 23) | (1u << 22) | 2u);
	bw_emit(w, 0);
	bw_emit(w, gtt_offset);
	bw_emit(w, tag);
}


static void
gen5_emit_xy_src_copy_blt(batch_writer* w,
	uint32 src_gtt, uint32 src_pitch,
	uint32 dst_gtt, uint32 dst_pitch,
	uint32 dst_x, uint32 dst_y, uint32 width, uint32 height)
{
	// XY_SRC_COPY_BLT: 8 DWORDs, 32bpp RGBA
	bw_emit(w, XY_COMMAND_SOURCE_BLIT | COMMAND_BLIT_RGBA);
	bw_emit(w, dst_pitch | (0xCC << 16)
		| ((uint32)COMMAND_MODE_RGB32 << 24));
	bw_emit(w, (dst_y << 16) | dst_x);
	bw_emit(w, ((dst_y + height) << 16) | (dst_x + width));
	bw_emit(w, dst_gtt);
	bw_emit(w, 0);			// src y/x = 0
	bw_emit(w, src_pitch);
	bw_emit(w, src_gtt);
}


// -------------------------------------------------------------------
// Registration
// -------------------------------------------------------------------

void
gen5_init_ops(gen_ops* ops)
{
	ops->info.generation = 5;
	ops->info.max_media_threads = GEN5_MAX_MEDIA_THREADS;
	ops->info.cs_urb_size_units = GEN5_CS_URB_SIZE_UNITS;
	ops->info.ring_base = INTEL_PRIMARY_RING_BUFFER;

	ops->emit_mi_flush = gen5_emit_mi_flush;
	ops->emit_pipeline_select_media = gen5_emit_pipeline_select_media;
	ops->emit_depth_buffer_null = gen5_emit_depth_buffer_null;
	ops->emit_state_base_address = gen5_emit_state_base_address;
	ops->emit_urb_fence = gen5_emit_urb_fence;
	ops->emit_cs_urb_state = gen5_emit_cs_urb_state;
	ops->emit_constant_buffer = gen5_emit_constant_buffer;
	ops->emit_media_state_pointers = gen5_emit_media_state_pointers;
	ops->emit_marker = gen5_emit_marker;
	ops->emit_xy_src_copy_blt = gen5_emit_xy_src_copy_blt;
}
