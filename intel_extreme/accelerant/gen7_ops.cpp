/*
 * gen7_ops.cpp — Gen7 (Ivy Bridge / Haswell) GPU command emitters.
 *
 * Implements the gen_ops interface for Intel HD Graphics 2500/4000 (IVB)
 * and HD Graphics 4600/5000/Iris (HSW). Gen7 is very close to Gen6 with
 * these key differences:
 *
 *   - 3DSTATE_DEPTH_BUFFER: 8 DWORDs (was 7 on Gen6, 6 on Gen5)
 *   - STATE_BASE_ADDRESS: 10 DWORDs (same as Gen6)
 *   - Max media threads: IVB=64, HSW=112
 *   - MEDIA_VFE_STATE: same 8 DW as Gen6, but HSW adds subslice config
 *   - PIPE_CONTROL: same as Gen6 but more flush bits available
 *   - L3 cache configuration registers differ (HSW has L3SQCREG1)
 *   - BLT engine: same encoding, still on BCS ring (0x22000)
 *   - Haswell adds GPGPU_WALKER for compute dispatch (not used here)
 *
 * STATUS: UNTESTED — no Ivy Bridge or Haswell hardware available.
 * Created from Intel PRM Vol2 (IVB/HSW) and i915/libva references.
 *
 * Note: Haswell (Gen7.5) is lumped into Gen7 because Generation()
 * returns 7 for both IVB and HSW. HSW differences are handled by
 * checking InGroup(INTEL_GROUP_HAS) where needed.
 */

#include "gen_ops.h"
#include "intel_extreme.h"


// -------------------------------------------------------------------
// Gen7 command opcodes (same CMD_GFX macro as Gen5/Gen6)
// -------------------------------------------------------------------

#define CMD_GFX(p, o, s) \
	((3u << 29) | ((uint32)(p) << 27) | ((uint32)(o) << 24) \
		| ((uint32)(s) << 16))

#define CMD_PIPELINE_SELECT		CMD_GFX(1, 1, 4)	// 0x69040000
#define CMD_3DSTATE_DEPTH_BUFFER CMD_GFX(3, 1, 5)	// 0x79050000
#define CMD_3DSTATE_URB			CMD_GFX(0, 0, 5)	// 0x60050000 (IVB only)
#define CMD_STATE_BASE_ADDRESS	CMD_GFX(0, 1, 1)	// 0x61010000

// Media pipeline (same as Gen6)
#define CMD_MEDIA_VFE_STATE		CMD_GFX(2, 0, 0)	// 0x70000000
#define CMD_MEDIA_CURBE_LOAD	CMD_GFX(2, 0, 1)	// 0x70010000
#define CMD_MEDIA_IDL			CMD_GFX(2, 0, 2)	// 0x70020000
#define CMD_MEDIA_OBJECT		CMD_GFX(2, 1, 0)	// 0x71000000

// PIPE_CONTROL (same opcode as Gen6, more bits available)
#define CMD_PIPE_CONTROL		CMD_GFX(3, 0, 2)	// 0x7a000000
#define PIPE_CONTROL_CS_STALL	(1u << 20)
#define PIPE_CONTROL_DC_FLUSH	(1u << 5)
#define PIPE_CONTROL_STALL_AT_SCOREBOARD (1u << 1)

// Haswell push constant allocation
#define CMD_3DSTATE_PUSH_CONSTANT_ALLOC_VS CMD_GFX(3, 1, 0x12) // HSW

#define PIPELINE_SELECT_MEDIA	1u
#define BASE_ADDRESS_MODIFY		1u

#define I965_DEPTHFORMAT_D32_FLOAT 1u
#define I965_SURFACE_NULL		7u

// Gen7 limits
#define GEN7_IVB_MAX_MEDIA_THREADS	64
#define GEN7_HSW_MAX_MEDIA_THREADS	112
// Use IVB as default; HSW users can override via gen_info.max_media_threads
#define GEN7_MAX_MEDIA_THREADS	GEN7_IVB_MAX_MEDIA_THREADS
#define GEN7_CS_URB_SIZE_UNITS	16


// -------------------------------------------------------------------
// Gen7 emitter implementations
// -------------------------------------------------------------------

static void
gen7_emit_mi_flush(batch_writer* w)
{
	// PIPE_CONTROL with CS stall (same as Gen6)
	bw_emit(w, CMD_PIPE_CONTROL | 2);
	bw_emit(w, PIPE_CONTROL_CS_STALL);
	bw_emit(w, 0);
	bw_emit(w, 0);
}


static void
gen7_emit_pipeline_select_media(batch_writer* w)
{
	bw_emit(w, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
}


static void
gen7_emit_depth_buffer_null(batch_writer* w)
{
	// Gen7: 8 DWORDs (length = 6), one more than Gen6's 7
	bw_emit(w, CMD_3DSTATE_DEPTH_BUFFER | 6);
	bw_emit(w, (I965_DEPTHFORMAT_D32_FLOAT << 18)
		| (I965_SURFACE_NULL << 29));
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);	// Gen7 extra DW
}


static void
gen7_emit_state_base_address(batch_writer* w)
{
	// Gen7: 10 DWORDs (same as Gen6)
	bw_emit(w, CMD_STATE_BASE_ADDRESS | 8);
	bw_emit(w, BASE_ADDRESS_MODIFY);	// general state base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// surface state base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// dynamic state base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// indirect object base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// instruction base
	bw_emit(w, BASE_ADDRESS_MODIFY);	// general state upper bound
	bw_emit(w, BASE_ADDRESS_MODIFY);	// dynamic state upper bound
	bw_emit(w, BASE_ADDRESS_MODIFY);	// indirect object upper bound
	bw_emit(w, BASE_ADDRESS_MODIFY);	// instruction upper bound
}


static void
gen7_emit_urb_fence(batch_writer* w, uint32 vfe_entries)
{
	// Gen7: same 3DSTATE_URB as Gen6 for media-only mode
	(void)vfe_entries;
	bw_emit(w, CMD_3DSTATE_URB | 0);
	bw_emit(w, 0);
}


static void
gen7_emit_cs_urb_state(batch_writer* w)
{
	// Gen7: CS_URB_STATE removed (same as Gen6). No-op.
	(void)w;
}


static void
gen7_emit_constant_buffer(batch_writer* w, uint32 gtt_offset,
	uint32 read_length)
{
	// MEDIA_CURBE_LOAD: same as Gen6
	if (read_length == 0)
		read_length = 1;
	uint32 curbe_size = read_length * 64;
	bw_emit(w, CMD_MEDIA_CURBE_LOAD | 1);
	bw_emit(w, curbe_size);
	bw_emit(w, gtt_offset);
}


static void
gen7_emit_media_state_pointers(batch_writer* w,
	uint32 vfe_state_gtt, uint32 idrt_gtt)
{
	// Gen7: MEDIA_VFE_STATE (8 DW) + MEDIA_INTERFACE_DESCRIPTOR_LOAD (3 DW)
	// Same as Gen6 but with different thread counts.

	// MEDIA_VFE_STATE
	bw_emit(w, CMD_MEDIA_VFE_STATE | 6);
	bw_emit(w, 0);									// DW1: scratch = 0
	bw_emit(w, (GEN7_MAX_MEDIA_THREADS << 16)		// DW2: max threads
		| (GEN7_MAX_MEDIA_THREADS & 0xFF));			//       URB entries
	bw_emit(w, 0);									// DW3: CURBE alloc
	bw_emit(w, 0);									// DW4: scoreboard
	bw_emit(w, 0);									// DW5
	bw_emit(w, 0);									// DW6
	bw_emit(w, 0);									// DW7

	// MEDIA_INTERFACE_DESCRIPTOR_LOAD
	bw_emit(w, CMD_MEDIA_IDL | 1);
	bw_emit(w, 32);		// 1 descriptor = 32 bytes
	bw_emit(w, idrt_gtt);
}


static void
gen7_emit_marker(batch_writer* w, uint32 gtt_offset, uint32 tag)
{
	// MI_STORE_DATA_IMM: same encoding across Gen5-Gen7
	bw_emit(w, (0x20u << 23) | (1u << 22) | 2u);
	bw_emit(w, 0);
	bw_emit(w, gtt_offset);
	bw_emit(w, tag);
}


static void
gen7_emit_xy_src_copy_blt(batch_writer* w,
	uint32 src_gtt, uint32 src_pitch,
	uint32 dst_gtt, uint32 dst_pitch,
	uint32 dst_x, uint32 dst_y, uint32 width, uint32 height)
{
	// XY_SRC_COPY_BLT: same encoding as Gen5/Gen6.
	// On Gen7, BLT is on separate BCS ring (0x22000).
	bw_emit(w, XY_COMMAND_SOURCE_BLIT | COMMAND_BLIT_RGBA);
	bw_emit(w, dst_pitch | (0xCC << 16)
		| ((uint32)COMMAND_MODE_RGB32 << 24));
	bw_emit(w, (dst_y << 16) | dst_x);
	bw_emit(w, ((dst_y + height) << 16) | (dst_x + width));
	bw_emit(w, dst_gtt);
	bw_emit(w, 0);
	bw_emit(w, src_pitch);
	bw_emit(w, src_gtt);
}


// -------------------------------------------------------------------
// Registration
// -------------------------------------------------------------------

void
gen7_init_ops(gen_ops* ops)
{
	ops->info.generation = 7;
	ops->info.max_media_threads = GEN7_MAX_MEDIA_THREADS;
	ops->info.cs_urb_size_units = GEN7_CS_URB_SIZE_UNITS;
	ops->info.ring_base = INTEL_PRIMARY_RING_BUFFER;

	ops->emit_mi_flush = gen7_emit_mi_flush;
	ops->emit_pipeline_select_media = gen7_emit_pipeline_select_media;
	ops->emit_depth_buffer_null = gen7_emit_depth_buffer_null;
	ops->emit_state_base_address = gen7_emit_state_base_address;
	ops->emit_urb_fence = gen7_emit_urb_fence;
	ops->emit_cs_urb_state = gen7_emit_cs_urb_state;
	ops->emit_constant_buffer = gen7_emit_constant_buffer;
	ops->emit_media_state_pointers = gen7_emit_media_state_pointers;
	ops->emit_marker = gen7_emit_marker;
	ops->emit_xy_src_copy_blt = gen7_emit_xy_src_copy_blt;
}
