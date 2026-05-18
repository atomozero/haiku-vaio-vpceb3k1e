/*
 * gen_ops.h — Generation-specific GPU operations interface.
 *
 * Abstracts the differences between Intel GPU generations (Gen4-Gen9+)
 * so that the ring submission layer, media pipeline, and BLT engine
 * can work with any generation by calling through this interface.
 *
 * Each generation provides an implementation (gen5_ops.cpp, gen6_ops.cpp,
 * etc.) that fills in the gen_ops struct with the correct command
 * encodings, register offsets, and hardware limits.
 *
 * The batch_writer is a simple DWORD accumulator used by the emitters.
 * It is generation-independent.
 */

#ifndef GEN_OPS_H
#define GEN_OPS_H

#include <SupportDefs.h>


// -------------------------------------------------------------------
// batch_writer — accumulates GPU command DWORDs for ring submission.
// Generation-independent: any emitter can push DWORDs into it.
// -------------------------------------------------------------------

#define BATCH_CAPACITY_DW 8192

struct batch_writer {
	uint32	dwords[BATCH_CAPACITY_DW];
	uint32	count;
	bool	overflow;
};

static inline void
bw_init(batch_writer* w)
{
	w->count = 0;
	w->overflow = false;
}

static inline void
bw_emit(batch_writer* w, uint32 dword)
{
	if (w->count >= BATCH_CAPACITY_DW) {
		w->overflow = true;
		return;
	}
	w->dwords[w->count++] = dword;
}


// -------------------------------------------------------------------
// gen_info — static hardware capabilities for a given generation.
// -------------------------------------------------------------------

struct gen_info {
	uint32		generation;			// 4, 5, 6, 7, 8, 9, ...
	uint32		max_media_threads;	// max concurrent EU threads
	uint32		cs_urb_size_units;	// CS URB region size
	uint32		ring_base;			// MMIO offset for primary ring
};


// -------------------------------------------------------------------
// gen_ops — function pointers for generation-specific command emission.
//
// Each function writes one or more DWORDs into the batch_writer.
// The media pipeline calls these in sequence to build a command stream
// that works on the target generation.
// -------------------------------------------------------------------

struct gen_ops {
	gen_info	info;

	// Pipeline control
	void (*emit_mi_flush)(batch_writer* w);
	void (*emit_pipeline_select_media)(batch_writer* w);
	void (*emit_depth_buffer_null)(batch_writer* w);

	// State setup
	void (*emit_state_base_address)(batch_writer* w);
	void (*emit_urb_fence)(batch_writer* w, uint32 vfe_entries);
	void (*emit_cs_urb_state)(batch_writer* w);
	void (*emit_constant_buffer)(batch_writer* w, uint32 gtt_offset,
		uint32 read_length);

	// State pointers (gen-specific layout)
	void (*emit_media_state_pointers)(batch_writer* w,
		uint32 vfe_state_gtt, uint32 idrt_gtt);

	// MI_STORE_DATA_IMM marker (for completion tracking)
	void (*emit_marker)(batch_writer* w, uint32 gtt_offset, uint32 tag);

	// BLT engine
	void (*emit_xy_src_copy_blt)(batch_writer* w,
		uint32 src_gtt, uint32 src_pitch,
		uint32 dst_gtt, uint32 dst_pitch,
		uint32 dst_x, uint32 dst_y, uint32 width, uint32 height);
};


// -------------------------------------------------------------------
// Generation registration.
// Call the appropriate gen*_init_ops() to fill in the vtable.
// -------------------------------------------------------------------

// Gen5 (Ironlake) — implemented in gen5_ops.cpp
void gen5_init_ops(gen_ops* ops);

// Gen6 (Sandy Bridge) — implemented in gen6_ops.cpp
// STATUS: UNTESTED — needs SNB hardware for validation
void gen6_init_ops(gen_ops* ops);

// Future generations:
// void gen7_init_ops(gen_ops* ops);  // Ivy Bridge / Haswell
// void gen8_init_ops(gen_ops* ops);  // Broadwell


#endif // GEN_OPS_H
