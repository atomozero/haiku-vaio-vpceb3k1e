/*
 * gen6_ops.cpp — Gen6 (Sandy Bridge) GPU command emitters.
 *
 * Implements the gen_ops interface for Intel HD Graphics 2000/3000 (Gen6).
 * Derived from gen5_ops.cpp with the following key changes:
 *
 *   - STATE_BASE_ADDRESS: 10 DWORDs (was 8 on Gen5/ILK)
 *   - URB_FENCE removed: replaced by 3DSTATE_URB (CMD_GFX(0,0,5))
 *   - CS_URB_STATE removed: URB config folded into MEDIA_VFE_STATE
 *   - MEDIA_VFE_STATE is now a ring command (CMD_GFX(2,0,0), 8 DW)
 *     instead of a state object pointed to by MEDIA_STATE_POINTERS
 *   - MEDIA_INTERFACE_DESCRIPTOR_LOAD replaces state-pointer IDRT
 *   - MI_FLUSH deprecated: use PIPE_CONTROL (CMD_GFX(3,0,2))
 *   - Max media threads: 60 (was 48 on Gen5)
 *   - BLT engine on separate ring (BCS at 0x22000), but same encoding
 *
 * STATUS: UNTESTED — no Sandy Bridge hardware available.
 * Created from Intel PRM Vol2 Part1 (SNB) and i915/libva references.
 * Needs hardware validation before use.
 *
 * To test: someone with SNB hardware builds the accelerant, sets up
 * the kernel ioctl driver (INTEL_RING_WRITE_TAIL), and runs gpu_idct_bench.
 */

#include "gen_ops.h"
#include "intel_extreme.h"


// -------------------------------------------------------------------
// Gen6 command opcodes
// -------------------------------------------------------------------

#define CMD_GFX(p, o, s) \
	((3u << 29) | ((uint32)(p) << 27) | ((uint32)(o) << 24) \
		| ((uint32)(s) << 16))

// Pipeline control (same opcodes, different semantics)
#define CMD_PIPELINE_SELECT		CMD_GFX(1, 1, 4)	// 0x69040000
#define CMD_3DSTATE_DEPTH_BUFFER CMD_GFX(3, 1, 5)	// 0x79050000

// Gen6 replaces URB_FENCE with 3DSTATE_URB
#define CMD_3DSTATE_URB			CMD_GFX(0, 0, 5)	// 0x60050000

// State base address (same opcode, more DWORDs)
#define CMD_STATE_BASE_ADDRESS	CMD_GFX(0, 1, 1)	// 0x61010000

// Media pipeline (same opcode, but now a ring command with inline state)
#define CMD_MEDIA_VFE_STATE		CMD_GFX(2, 0, 0)	// 0x70000000
#define CMD_MEDIA_CURBE_LOAD	CMD_GFX(2, 0, 1)	// 0x70010000
#define CMD_MEDIA_IDL			CMD_GFX(2, 0, 2)	// 0x70020000 (Interface Descriptor Load)
#define CMD_MEDIA_OBJECT		CMD_GFX(2, 1, 0)	// 0x71000000

// PIPE_CONTROL replaces MI_FLUSH on Gen6+
#define CMD_PIPE_CONTROL		CMD_GFX(3, 0, 2)	// 0x7a000000
#define PIPE_CONTROL_CS_STALL	(1u << 20)
#define PIPE_CONTROL_DC_FLUSH	(1u << 5)

#define PIPELINE_SELECT_MEDIA	1u
#define BASE_ADDRESS_MODIFY		1u

#define I965_DEPTHFORMAT_D32_FLOAT 1u
#define I965_SURFACE_NULL		7u

// Gen6 limits
#define GEN6_MAX_MEDIA_THREADS	60
#define GEN6_CS_URB_SIZE_UNITS	16	// same as Gen5 for media


// -------------------------------------------------------------------
// Gen6 emitter implementations
// -------------------------------------------------------------------

static void
gen6_emit_mi_flush(batch_writer* w)
{
	// Gen6 uses PIPE_CONTROL instead of MI_FLUSH.
	// CS stall ensures all previous commands complete.
	bw_emit(w, CMD_PIPE_CONTROL | 2);	// length - 2 = 2 → 4 DWORDs
	bw_emit(w, PIPE_CONTROL_CS_STALL);
	bw_emit(w, 0);	// address (unused without write)
	bw_emit(w, 0);	// data (unused)
}


static void
gen6_emit_pipeline_select_media(batch_writer* w)
{
	// Same as Gen5
	bw_emit(w, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
}


static void
gen6_emit_depth_buffer_null(batch_writer* w)
{
	// Gen6: 7 DWORDs (length = 5), one more than Gen5's 6
	bw_emit(w, CMD_3DSTATE_DEPTH_BUFFER | 5);
	bw_emit(w, (I965_DEPTHFORMAT_D32_FLOAT << 18)
		| (I965_SURFACE_NULL << 29));
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);
	bw_emit(w, 0);	// Gen6 extra DW
}


static void
gen6_emit_state_base_address(batch_writer* w)
{
	// Gen6: 10 DWORDs (length = 8), was 8 on Gen5
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
gen6_emit_urb_fence(batch_writer* w, uint32 vfe_entries)
{
	// Gen6 replaces URB_FENCE with 3DSTATE_URB.
	// For media-only: VS entries = 0, GS entries = 0.
	// 2 DWORDs total (length = 0).
	(void)vfe_entries;
	bw_emit(w, CMD_3DSTATE_URB | 0);
	bw_emit(w, 0);	// VS size=0, VS entries=0, GS entries=0, GS size=0
}


static void
gen6_emit_cs_urb_state(batch_writer* w)
{
	// Gen6: CS_URB_STATE is gone. URB allocation for CS is now part of
	// MEDIA_VFE_STATE. This is a no-op on Gen6 — the caller should
	// configure URB via MEDIA_VFE_STATE instead.
	// Emit nothing.
	(void)w;
}


static void
gen6_emit_constant_buffer(batch_writer* w, uint32 gtt_offset,
	uint32 read_length)
{
	// Gen6 uses MEDIA_CURBE_LOAD instead of CMD_CONSTANT_BUFFER.
	// 3 DWORDs (length = 1).
	if (read_length == 0)
		read_length = 1;
	uint32 curbe_size = read_length * 64;	// in bytes (64 per GRF)
	bw_emit(w, CMD_MEDIA_CURBE_LOAD | 1);
	bw_emit(w, curbe_size);		// CURBE total data length
	bw_emit(w, gtt_offset);	// CURBE data start address
}


static void
gen6_emit_media_state_pointers(batch_writer* w,
	uint32 vfe_state_gtt, uint32 idrt_gtt)
{
	// Gen6: MEDIA_STATE_POINTERS is replaced by two separate commands:
	//
	// 1. MEDIA_VFE_STATE: 8 DWORDs (length = 6)
	//    Contains scratch, max threads, URB config inline.
	//
	// 2. MEDIA_INTERFACE_DESCRIPTOR_LOAD: 3 DWORDs (length = 1)
	//    Points to the IDRT in memory.
	//
	// For now, emit a minimal MEDIA_VFE_STATE + IDL.

	// MEDIA_VFE_STATE: 8 DWORDs
	bw_emit(w, CMD_MEDIA_VFE_STATE | 6);
	bw_emit(w, 0);									// DW1: scratch space = 0
	bw_emit(w, (GEN6_MAX_MEDIA_THREADS << 16)		// DW2: max threads
		| (GEN6_MAX_MEDIA_THREADS & 0xFF));			//       URB entries
	bw_emit(w, 0);									// DW3: CURBE alloc = 0
	bw_emit(w, 0);									// DW4: scoreboard (disabled)
	bw_emit(w, 0);									// DW5: scoreboard
	bw_emit(w, 0);									// DW6: scoreboard
	bw_emit(w, 0);									// DW7: scoreboard

	// MEDIA_INTERFACE_DESCRIPTOR_LOAD: 3 DWORDs
	// TODO: the IDRT format differs on Gen6 (different DW layout).
	// The gtt offset points to the IDRT table in GPU memory.
	bw_emit(w, CMD_MEDIA_IDL | 1);
	bw_emit(w, 32);		// IDRT data length (1 descriptor = 32 bytes)
	bw_emit(w, idrt_gtt);	// IDRT start address
}


static void
gen6_emit_marker(batch_writer* w, uint32 gtt_offset, uint32 tag)
{
	// MI_STORE_DATA_IMM: same encoding as Gen5 (4 DWORDs, GGTT bit 22)
	bw_emit(w, (0x20u << 23) | (1u << 22) | 2u);
	bw_emit(w, 0);
	bw_emit(w, gtt_offset);
	bw_emit(w, tag);
}


static void
gen6_emit_xy_src_copy_blt(batch_writer* w,
	uint32 src_gtt, uint32 src_pitch,
	uint32 dst_gtt, uint32 dst_pitch,
	uint32 dst_x, uint32 dst_y, uint32 width, uint32 height)
{
	// XY_SRC_COPY_BLT: same encoding as Gen5.
	// NOTE: On Gen6, the BLT engine has its own ring (BCS at 0x22000).
	// If submitting to the RCS (render ring), this command may not work.
	// For BCS submission, the gpu_ring would need to target the BLT ring
	// instead of the render ring.
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
gen6_init_ops(gen_ops* ops)
{
	ops->info.generation = 6;
	ops->info.max_media_threads = GEN6_MAX_MEDIA_THREADS;
	ops->info.cs_urb_size_units = GEN6_CS_URB_SIZE_UNITS;
	ops->info.ring_base = INTEL_PRIMARY_RING_BUFFER;  // 0x2030, same as Gen5

	ops->emit_mi_flush = gen6_emit_mi_flush;
	ops->emit_pipeline_select_media = gen6_emit_pipeline_select_media;
	ops->emit_depth_buffer_null = gen6_emit_depth_buffer_null;
	ops->emit_state_base_address = gen6_emit_state_base_address;
	ops->emit_urb_fence = gen6_emit_urb_fence;
	ops->emit_cs_urb_state = gen6_emit_cs_urb_state;
	ops->emit_constant_buffer = gen6_emit_constant_buffer;
	ops->emit_media_state_pointers = gen6_emit_media_state_pointers;
	ops->emit_marker = gen6_emit_marker;
	ops->emit_xy_src_copy_blt = gen6_emit_xy_src_copy_blt;
}
