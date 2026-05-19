/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Per-generation command encoding vtable.
 *
 * Each generation (Gen5/6/7/8) provides an implementation of this
 * interface. The batch_writer accumulates DWORDs that are then
 * submitted via gpu_ring or EXECBUFFER2.
 *
 * Usage:
 *   gen_ops ops;
 *   init_gen_ops(&ops, GPU_GEN5);
 *   batch_writer w;
 *   bw_init(&w);
 *   ops.emit_mi_flush(&w);
 *   ops.emit_pipeline_select_media(&w);
 *   // ... submit w.dwords via ring or execbuf ...
 */

#ifndef INTEL_GEN_OPS_H
#define INTEL_GEN_OPS_H

#include <SupportDefs.h>


/* -------------------------------------------------------------------------
 * Batch writer — DWORD accumulator
 * ---------------------------------------------------------------------- */

#define BATCH_CAPACITY_DW	16384

struct batch_writer {
	uint32		dwords[BATCH_CAPACITY_DW];
	uint32		count;
	bool		overflow;
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


/* -------------------------------------------------------------------------
 * BLT parameters
 * ---------------------------------------------------------------------- */

struct blt_copy_params {
	uint32		src_gtt;
	uint32		src_stride;
	uint32		src_x, src_y;
	uint32		dst_gtt;
	uint32		dst_stride;
	uint32		dst_x, dst_y;
	uint32		width, height;
	uint32		bpp;			/* 8, 16, or 32 */
	uint32		tiling;			/* 0=none, 1=X, 2=Y */
};


/* -------------------------------------------------------------------------
 * Generation operations vtable
 * ---------------------------------------------------------------------- */

struct gen_ops {
	/* Identity */
	uint32		generation;
	uint32		max_eu_threads;

	/* Pipeline control */
	void		(*emit_mi_flush)(batch_writer* w);
	void		(*emit_mi_noop)(batch_writer* w);
	void		(*emit_pipeline_select_media)(batch_writer* w);
	void		(*emit_pipeline_select_3d)(batch_writer* w);
	void		(*emit_batch_buffer_end)(batch_writer* w);

	/* State setup */
	void		(*emit_state_base_address)(batch_writer* w);
	void		(*emit_depth_buffer_null)(batch_writer* w);
	void		(*emit_urb_fence)(batch_writer* w, uint32 thread_count);
	void		(*emit_media_state_pointers)(batch_writer* w,
					uint32 vfe_gtt, uint32 idrt_gtt);
	void		(*emit_cs_urb_state)(batch_writer* w);
	void		(*emit_constant_buffer)(batch_writer* w,
					uint32 curbe_gtt, uint32 read_length);

	/* Pipe control (Gen6+ replaces MI_FLUSH) */
	void		(*emit_pipe_control)(batch_writer* w, uint32 flags,
					uint32 addr, uint32 data);

	/* Media dispatch */
	void		(*emit_media_object)(batch_writer* w,
					uint32 interface_idx,
					const uint32* inline_data,
					uint32 inline_dwords);

	/* Marker (MI_STORE_DATA_IMM to GTT address) */
	void		(*emit_marker)(batch_writer* w,
					uint32 gtt_addr, uint32 tag);

	/* BLT */
	void		(*emit_blt_copy)(batch_writer* w,
					const blt_copy_params* params);
};


/* Initialize gen_ops for the given generation. */
void	init_gen_ops(gen_ops* ops, uint32 generation);

/* Per-generation initializers (called by init_gen_ops). */
void	init_gen5_ops(gen_ops* ops);
void	init_gen6_ops(gen_ops* ops);
void	init_gen7_ops(gen_ops* ops);
void	init_gen8_ops(gen_ops* ops);


#endif /* INTEL_GEN_OPS_H */
