/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Intel Gen GPU kernel driver — ioctl interface v2.
 *
 * Shared between kernel driver and userspace (accelerant, libdrm_haiku).
 * Backward compatible with v1 ioctls from intel_extreme.
 */

#ifndef INTEL_GEN_GPU_IOCTL_H
#define INTEL_GEN_GPU_IOCTL_H

#include <Drivers.h>
#include <SupportDefs.h>


#define INTEL_GEN_GPU_MAGIC		'iGPU'
#define INTEL_GEN_IOCTL_VERSION	2


/* -------------------------------------------------------------------------
 * v1 ioctls (preserved from intel_extreme for backward compatibility)
 * ---------------------------------------------------------------------- */

enum {
	/* Existing v1 ioctls — same opcodes as intel_extreme */
	IGEN_GET_PRIVATE_DATA = B_DEVICE_OP_CODES_END + 1,
	IGEN_GET_DEVICE_NAME,
	IGEN_ALLOC_GFX_MEMORY,
	IGEN_FREE_GFX_MEMORY,
	IGEN_RING_WRITE_TAIL,
	IGEN_RING_INIT_3D,

	/* v2 ioctls — new functionality */
	IGEN_GET_VERSION,			/* Query ioctl interface version */
	IGEN_GET_PARAM,				/* Chipset info queries */
	IGEN_GET_APERTURE,			/* GTT aperture size */

	IGEN_GEM_CREATE,			/* Allocate BO with handle */
	IGEN_GEM_CLOSE,				/* Free BO by handle */
	IGEN_GEM_MMAP,				/* Map BO to userspace */
	IGEN_GEM_SET_TILING,		/* Set tiling mode + program fence */
	IGEN_GEM_GET_TILING,		/* Query tiling mode */
	IGEN_GEM_EXECBUFFER2,		/* Batch submit with relocations */
	IGEN_GEM_WAIT,				/* Wait for BO idle */
	IGEN_GEM_BUSY,				/* Check if BO is busy */
	IGEN_GEM_SET_DOMAIN,		/* CPU/GPU cache domain */

	IGEN_CONTEXT_CREATE,		/* Create HW context */
	IGEN_CONTEXT_DESTROY,		/* Destroy HW context */

	IGEN_GPU_RESET,				/* Controlled GPU reset */
	IGEN_RING_INIT,				/* Per-ring init with workarounds */
};


/* -------------------------------------------------------------------------
 * Ioctl data structures
 * ---------------------------------------------------------------------- */

struct igen_version {
	uint32		magic;			/* INTEL_GEN_GPU_MAGIC */
	uint32		version;		/* out: INTEL_GEN_IOCTL_VERSION */
	uint32		generation;		/* out: GPU_GEN5..GPU_GEN8 */
	uint16		device_id;		/* out: PCI device ID */
};

struct igen_get_param {
	uint32		magic;
	int32		param;			/* in: parameter ID (see I915_PARAM_*) */
	int32		value;			/* out: parameter value */
};

struct igen_get_aperture {
	uint32		magic;
	uint64		aperture_size;		/* out: total GTT size */
	uint64		aperture_available;	/* out: available GTT space */
};

struct igen_gem_create {
	uint32		magic;
	uint64		size;			/* in: requested size */
	uint32		handle;			/* out: GEM handle */
};

struct igen_gem_close {
	uint32		magic;
	uint32		handle;			/* in: GEM handle to free */
};

struct igen_gem_mmap {
	uint32		magic;
	uint32		handle;			/* in: GEM handle */
	uint64		offset;			/* in: offset within BO */
	uint64		size;			/* in: map size */
	addr_t		addr;			/* out: mapped address */
};

struct igen_gem_tiling {
	uint32		magic;
	uint32		handle;			/* in: GEM handle */
	uint32		tiling_mode;	/* in/out: I915_TILING_NONE/X/Y */
	uint32		stride;			/* in/out: tiling stride */
	uint32		swizzle_mode;	/* out: swizzle mode */
};

struct igen_gem_wait {
	uint32		magic;
	uint32		handle;			/* in: GEM handle */
	int64		timeout_ns;		/* in: timeout in nanoseconds (-1 = forever) */
};

struct igen_gem_busy {
	uint32		magic;
	uint32		handle;			/* in: GEM handle */
	uint32		busy;			/* out: bitmask of busy engines */
};

struct igen_gem_domain {
	uint32		magic;
	uint32		handle;
	uint32		read_domains;
	uint32		write_domain;
};


/* Exec object — one per BO referenced by the batch */
struct igen_exec_object {
	uint32		handle;
	uint32		relocation_count;
	addr_t		relocs_ptr;		/* pointer to igen_relocation array */
	uint64		alignment;
	uint64		offset;			/* GTT offset (in/out) */
	uint64		flags;
};

/* Relocation entry */
struct igen_relocation {
	uint32		target_handle;	/* BO to point at */
	uint32		delta;			/* Offset within target */
	uint32		offset;			/* Offset in batch BO to patch */
	uint32		presumed_offset; /* Cached GTT offset */
	uint32		read_domains;
	uint32		write_domain;
};

/* Exec buffer — the main batch submission ioctl */
struct igen_execbuffer2 {
	uint32		magic;
	addr_t		buffers_ptr;	/* pointer to igen_exec_object array */
	uint32		buffer_count;
	uint32		batch_start_offset;
	uint32		batch_len;
	uint32		flags;			/* IGEN_EXEC_* flags */
	int32		fence_fd;		/* out: sync fence (-1 if none) */
};

/* Exec flags */
#define IGEN_EXEC_RENDER		(0 << 0)
#define IGEN_EXEC_BLT			(1 << 0)
#define IGEN_EXEC_VIDEO			(2 << 0)
#define IGEN_EXEC_RING_MASK		0x03
#define IGEN_EXEC_HANDLE_LUT	(1 << 12)
#define IGEN_EXEC_BATCH_FIRST	(1 << 15)


struct igen_context_create {
	uint32		magic;
	uint32		ctx_id;			/* out: context ID */
};

struct igen_context_destroy {
	uint32		magic;
	uint32		ctx_id;			/* in: context ID */
};

struct igen_ring_tail {
	uint32		magic;
	uint32		tail_value;		/* in: TAIL register value to write */
};

struct igen_ring_init {
	uint32		magic;
	uint32		ring_type;		/* in: RING_RENDER, RING_BLT, etc. */
	uint32		flags;			/* in: init flags */
};

struct igen_gpu_reset {
	uint32		magic;
	uint32		engine_mask;	/* in: bitmask of engines to reset */
};


/* -------------------------------------------------------------------------
 * GETPARAM parameter IDs (compatible with i915)
 * ---------------------------------------------------------------------- */

#define IGEN_PARAM_CHIPSET_ID		1
#define IGEN_PARAM_HAS_GEM			2
#define IGEN_PARAM_NUM_FENCES		3
#define IGEN_PARAM_HAS_EXECBUF2		9
#define IGEN_PARAM_HAS_RELAXED_FENCING	12
#define IGEN_PARAM_HAS_WAIT_TIMEOUT	16
#define IGEN_PARAM_HAS_ALIASING_PPGTT	18
#define IGEN_PARAM_HAS_VEBOX		22
#define IGEN_PARAM_SUBSLICE_TOTAL	33
#define IGEN_PARAM_EU_TOTAL			34
#define IGEN_PARAM_REVISION			40
#define IGEN_PARAM_MMAP_GTT_VERSION	41


#endif /* INTEL_GEN_GPU_IOCTL_H */
