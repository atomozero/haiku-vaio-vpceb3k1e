/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Intel Gen5-Gen8 GPU kernel driver — device state and shared structures.
 */

#ifndef INTEL_GEN_GPU_DEVICE_H
#define INTEL_GEN_GPU_DEVICE_H

#include <SupportDefs.h>


/* Generation identifiers */
enum gpu_generation {
	GPU_GEN5 = 5,	/* Ironlake */
	GPU_GEN6 = 6,	/* Sandy Bridge */
	GPU_GEN7 = 7,	/* Ivy Bridge / Haswell */
	GPU_GEN8 = 8	/* Broadwell — last with classic ring */
};


/* Ring buffer types */
enum ring_type {
	RING_RENDER = 0,	/* Render Command Streamer (RCS) */
	RING_BLT    = 1,	/* Blitter (BCS, Gen6+) */
	RING_VIDEO  = 2,	/* Video Command Streamer (VCS) */
	RING_COUNT  = 3
};


/* Ring buffer state (kernel-internal) */
struct gpu_ring_state {
	uint32		register_base;	/* MMIO base for HEAD/TAIL/START/CTL */
	phys_addr_t	physical_base;	/* Physical address of ring memory */
	addr_t		virtual_base;	/* Kernel virtual address */
	uint32		size;			/* Ring size in bytes */
	uint32		head;			/* Last known HEAD position */
	uint32		tail;			/* Current TAIL position */
	bool		initialized;
	uint32		hangcheck_seq;	/* Sequence for hang detection */
};


/* GEM buffer object (kernel-tracked) */
struct gem_object {
	uint32		handle;			/* Userspace handle */
	addr_t		gtt_offset;		/* GTT offset */
	addr_t		cpu_addr;		/* CPU virtual address */
	size_t		size;			/* Allocation size */
	uint32		tiling_mode;	/* I915_TILING_NONE/X/Y */
	uint32		stride;			/* Tiling stride */
	int32		fence_reg;		/* HW fence register (-1 if none) */
	int32		ref_count;		/* Reference count */
	uint32		last_ring_seq;	/* Last ring sequence using this BO */
	bool		busy;			/* GPU is using this BO */
};


/* Per-open device state */
struct device_context {
	int32		ref_count;
	team_id		owner_team;

	/* GEM handle table */
	gem_object**	handles;
	uint32			handle_count;
	uint32			handle_capacity;
	uint32			next_handle;
};


/* Global device state (one per GPU) */
struct gpu_device {
	uint16		device_id;		/* PCI device ID */
	uint8		generation;		/* GPU_GEN5..GPU_GEN8 */
	uint8		revision;

	/* PCI BAR mappings */
	phys_addr_t	mmio_phys;		/* BAR0 physical address */
	addr_t		mmio_base;		/* Kernel virtual MMIO */
	size_t		mmio_size;
	area_id		mmio_area;

	phys_addr_t	gtt_phys;		/* GTT aperture physical */
	addr_t		gtt_base;		/* Kernel virtual GTT */
	size_t		gtt_size;		/* GTT aperture size */
	area_id		gtt_area;

	/* Ring buffers */
	gpu_ring_state	rings[RING_COUNT];

	/* GTT page table */
	uint32*		gtt_entries;	/* GTT PTE array */
	uint32		gtt_total_entries;
	uint32		gtt_stolen_entries;	/* Reserved by BIOS */

	/* Fence registers (for tiling) */
	uint32		fence_count;	/* 16 for Gen5, 32 for Gen6+ */
	uint32		fence_base;		/* Register base (0x3000 Gen5, 0x100000 Gen6) */
	int32		fence_owners[32]; /* handle owning each fence, -1 if free */

	/* Power management */
	bool		forcewake_held;	/* Gen6+: FORCEWAKE asserted */
	int32		forcewake_count; /* Nesting count */

	/* Generation-specific ops */
	struct gen_kernel_ops*	ops;

	/* Shared info for accelerant communication */
	area_id		shared_area;
};


/* Generation-specific kernel operations */
struct gen_kernel_ops {
	status_t	(*init_gtt)(gpu_device* dev);
	status_t	(*init_ring)(gpu_device* dev, ring_type type);
	status_t	(*gpu_reset)(gpu_device* dev, uint32 engine_mask);
	status_t	(*forcewake_get)(gpu_device* dev);
	void		(*forcewake_put)(gpu_device* dev);
	void		(*apply_workarounds)(gpu_device* dev);
	uint32		(*read_timestamp)(gpu_device* dev);
};


#endif /* INTEL_GEN_GPU_DEVICE_H */
