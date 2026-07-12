/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Andrea Bernardi
 */
#ifndef INTEL_GEM_H
#define INTEL_GEM_H


#include "intel_extreme.h"

#include <OS.h>


// GEM buffer object uAPI for the intel_extreme driver, RFC section 6.
//
// The ioctl semantics follow the Linux i915 GEM uAPI (pre-execlists era)
// so that libdrm's intel backend and Mesa's crocus driver can be ported
// as thin shims. The mechanisms are Haiku-native: buffer objects are
// backed by areas (CPU mapping via clone_area(), cross-team sharing via
// the area_id itself), fences are semaphores.
//
// Versioning convention: every request struct starts with
//		uint32 magic;	// INTEL_PRIVATE_DATA_MAGIC
//		uint32 size;	// sizeof(struct) as compiled into the caller
// The kernel rejects a request whose magic mismatches, accepts a size
// equal to or larger than the version it knows (extra tail ignored),
// and rejects anything smaller. New fields are only ever appended.

// GEM opcodes get their own fixed base instead of literally continuing
// the legacy enum in intel_extreme.h: the legacy enum still grows (the
// ring-control ioctls were appended recently), and uAPI opcodes must
// never shift once a userland binary uses them.
#define INTEL_GEM_IOCTL_BASE			(B_DEVICE_OP_CODES_END + 0x80)

enum {
	INTEL_GEM_CREATE = INTEL_GEM_IOCTL_BASE,	// create BO (area-backed)
	INTEL_GEM_CLOSE,							// drop handle reference
	INTEL_GEM_OPEN_AREA,						// wrap foreign area as BO
	INTEL_GEM_SET_DOMAIN,						// cache domain transition
	INTEL_GEM_SET_TILING,						// X/Y tiling + fence regs
	INTEL_GEM_EXECBUFFER,						// submit batch buffer
	INTEL_GEM_WAIT,								// wait on BO rendering
	INTEL_GEM_BUSY,								// non-blocking busy check
	INTEL_GET_PARAM,							// chipset id, HW caps
	INTEL_GEM_READ_BO							// coherent kernel-side BO read
};

// cache/usage domains, value-compatible with I915_GEM_DOMAIN_*
#define INTEL_GEM_DOMAIN_CPU			0x00000001
#define INTEL_GEM_DOMAIN_RENDER			0x00000002
#define INTEL_GEM_DOMAIN_SAMPLER		0x00000004
#define INTEL_GEM_DOMAIN_COMMAND		0x00000008
#define INTEL_GEM_DOMAIN_INSTRUCTION	0x00000010
#define INTEL_GEM_DOMAIN_VERTEX			0x00000020
#define INTEL_GEM_DOMAIN_GTT			0x00000040

// tiling modes, value-compatible with I915_TILING_*
#define INTEL_GEM_TILING_NONE			0
#define INTEL_GEM_TILING_X				1
#define INTEL_GEM_TILING_Y				2

// bit 6 swizzling results, value-compatible with I915_BIT_6_SWIZZLE_*
#define INTEL_GEM_BIT_6_SWIZZLE_NONE	0
#define INTEL_GEM_BIT_6_SWIZZLE_9		1
#define INTEL_GEM_BIT_6_SWIZZLE_9_10	2
#define INTEL_GEM_BIT_6_SWIZZLE_9_11	3
#define INTEL_GEM_BIT_6_SWIZZLE_9_10_11	4
#define INTEL_GEM_BIT_6_SWIZZLE_UNKNOWN	5

// INTEL_GET_PARAM parameter ids
#define INTEL_GEM_PARAM_CHIPSET_ID		1	// PCI device id
#define INTEL_GEM_PARAM_HAS_GEM			2	// GEM enabled in settings
#define INTEL_GEM_PARAM_HAS_EXECBUFFER	3	// execbuffer path available
#define INTEL_GEM_PARAM_NUM_FENCES		4	// usable fence registers
#define INTEL_GEM_PARAM_APERTURE_SIZE	5	// mappable GTT bytes
#define INTEL_GEM_PARAM_HAS_RELAXED_DELTA 6	// reloc deltas unvalidated


// INTEL_GEM_CREATE
struct intel_gem_create {
	uint32		magic;
	uint32		size;			// sizeof(intel_gem_create)

	uint64		bo_size;		// in: bytes, rounded up to B_PAGE_SIZE
	uint32		flags;			// in: reserved, must be 0
	uint32		handle;			// out: per-fd BO handle
	area_id		area;			// out: backing area, clone_area() this
	int32		_reserved;
};

// INTEL_GEM_CLOSE
struct intel_gem_close {
	uint32		magic;
	uint32		size;

	uint32		handle;			// in
	uint32		_reserved;
};

// INTEL_GEM_OPEN_AREA — import a foreign area as a BO (the DMA-BUF /
// flink use case: the area_id is the system-global buffer name)
struct intel_gem_open_area {
	uint32		magic;
	uint32		size;

	area_id		area;			// in: area to wrap
	uint32		handle;			// out: per-fd BO handle
	uint64		bo_size;		// out: size of the area in bytes
};

// INTEL_GEM_SET_DOMAIN
struct intel_gem_set_domain {
	uint32		magic;
	uint32		size;

	uint32		handle;			// in
	uint32		read_domains;	// in: INTEL_GEM_DOMAIN_*
	uint32		write_domain;	// in: single domain or 0
	uint32		_reserved;
};

// INTEL_GEM_READ_BO — coherent kernel-side read of a BO's contents.
// The kernel clflushes the BO's (fully-locked) pages and copies them to
// the caller. Works around userspace mmap-clone cache coherency on this
// non-LLC hardware (the CPU clone can read stale data even after clflush,
// while the kernel mapping is coherent).
struct intel_gem_read_bo {
	uint32		magic;
	uint32		size;

	uint32		handle;			// in
	uint32		_reserved;
	uint64		offset;			// in: byte offset into the BO
	uint64		read_size;		// in: bytes to copy out
	uint64		dest_ptr;		// in: user destination buffer
};

// INTEL_GEM_SET_TILING
struct intel_gem_set_tiling {
	uint32		magic;
	uint32		size;

	uint32		handle;			// in
	uint32		tiling_mode;	// in/out: INTEL_GEM_TILING_*
	uint32		stride;			// in/out: bytes per row when tiled
	uint32		swizzle_mode;	// out: INTEL_GEM_BIT_6_SWIZZLE_*
};

// relocation entry, layout-compatible with drm_i915_gem_relocation_entry
struct intel_gem_relocation_entry {
	uint32		target_handle;	// handle of the BO being pointed at
	uint32		delta;			// value added to target's GTT offset
	uint64		offset;			// byte offset of the DWORD to patch
	uint64		presumed_offset; // GTT offset userland assumed; if it
								// matches, the kernel may skip the write
	uint32		read_domains;
	uint32		write_domain;
};

// per-BO entry in the execbuffer handle list
struct intel_gem_exec_object {
	uint32		handle;
	uint32		relocation_count;
	uint64		relocs_ptr;		// userland intel_gem_relocation_entry[]
	uint64		offset;			// out: GTT offset the BO was bound at
	uint64		flags;			// reserved, must be 0
};

// INTEL_GEM_EXECBUFFER, RFC section 7.2
struct intel_gem_execbuffer {
	uint32		magic;
	uint32		size;

	uint64		buffers_ptr;	// in: userland intel_gem_exec_object[];
								// the LAST entry is the batch BO
	uint32		buffer_count;	// in
	uint32		batch_start_offset;	// in: byte offset into batch BO
	uint32		batch_len;		// in: bytes of commands
	uint32		flags;			// in: reserved, must be 0
	uint32		fence_seqno;	// out: seqno of this submission
	uint32		_reserved;
	sem_id		fence_sem;		// out: semaphore signaled at retirement.
								// Only meaningful while the submission
								// is outstanding: fences are pooled and
								// recycled after retirement, so use
								// INTEL_GEM_WAIT for late waits.
	int32		_reserved2;
};

// INTEL_GEM_WAIT
struct intel_gem_wait {
	uint32		magic;
	uint32		size;

	uint32		handle;			// in
	uint32		_reserved;
	bigtime_t	timeout;		// in: relative, B_INFINITE_TIMEOUT for
								// blocking; out: time left
};

// INTEL_GEM_BUSY
struct intel_gem_busy {
	uint32		magic;
	uint32		size;

	uint32		handle;			// in
	uint32		busy;			// out: 0 = idle, nonzero = on the GPU
};

// INTEL_GET_PARAM
struct intel_gem_get_param {
	uint32		magic;
	uint32		size;

	uint32		param;			// in: INTEL_GEM_PARAM_*
	uint32		_reserved;
	uint64		value;			// out
};


#endif	/* INTEL_GEM_H */
