/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * GPU buffer-object abstraction for the Gen5 media-pipeline compute path.
 *
 * Wraps intel_allocate_memory() / intel_free_memory() into a named buffer
 * with the GTT offset computed once at allocation. Intended for use by
 * the media-pipeline bring-up code where many small state BOs (VFE state,
 * interface descriptor, CURBE, kernel, output) are allocated separately
 * and referenced by GPU-side address from MEDIA_OBJECT batches.
 *
 * Not used by the (now frozen) 3D render path in render.cpp.
 */


#ifndef GPU_BO_H
#define GPU_BO_H


#include <SupportDefs.h>


struct gpu_bo {
	addr_t		cpu_addr;		// CPU virtual address, writable after alloc
	uint32		gtt_offset;		// GPU-visible offset into the GTT aperture
	uint32		size;			// allocation size (>= requested size)
	uint32		alignment;		// alignment passed to the allocator
	const char*	name;			// debug label, pointer must outlive the BO
	bool		valid;			// false until a successful alloc
};


// Allocate a GPU buffer with the given size and alignment.
// name must be a stable string pointer (string literal or equivalent).
// Returns B_OK on success; bo is zeroed on failure.
status_t gpu_bo_alloc(gpu_bo* bo, const char* name, uint32 size,
	uint32 alignment);

// Release the allocation. Safe to call on an already-freed or
// never-allocated BO.
void gpu_bo_free(gpu_bo* bo);

// Zero the entire buffer contents via the CPU mapping.
void gpu_bo_clear(gpu_bo* bo);

// Write a 32-bit value at the given byte offset (must be 4-byte aligned).
void gpu_bo_write32(gpu_bo* bo, uint32 offset, uint32 value);

// Read a 32-bit value at the given byte offset.
uint32 gpu_bo_read32(gpu_bo* bo, uint32 offset);

// Copy a block of bytes into the BO at the given offset.
void gpu_bo_write(gpu_bo* bo, uint32 offset, const void* data, uint32 size);

// Memory barrier (mfence) to order CPU writes. May NOT be sufficient for
// WC-mapped GTT memory — use gpu_bo_clflush() for guaranteed GPU visibility.
void gpu_bo_flush_cpu_writes(void);

// Flush a BO's CPU cache lines via clflush + mfence. Required on Gen5 where
// GTT aperture is mapped write-combining: mfence alone does not guarantee
// WC combining buffers are drained to GGTT-visible memory.
void gpu_bo_clflush(gpu_bo* bo);

// Debug: number of currently live BOs allocated through this API.
uint32 gpu_bo_live_count(void);

// Debug: dump all live BOs to the syslog.
void gpu_bo_dump_live(void);


#endif // GPU_BO_H
