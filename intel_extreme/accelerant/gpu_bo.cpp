/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * GPU buffer-object abstraction — implementation.
 */


#include "gpu_bo.h"

#include <string.h>

#include "accelerant.h"
#include "intel_extreme.h"


#undef TRACE
//#define TRACE_GPU_BO
#ifdef TRACE_GPU_BO
#	define TRACE(x...) _sPrintf("intel_extreme gpu_bo: " x)
#else
#	define TRACE(x...)
#endif

#define ERROR(x...) _sPrintf("intel_extreme gpu_bo: " x)


// Debug live-BO tracking. A tiny fixed table is enough for the media
// pipeline — we never have more than ~20 BOs live at once.
#define MAX_TRACKED_BOS 32

static gpu_bo* sLiveBos[MAX_TRACKED_BOS];
static uint32 sLiveCount;


static void
track_alloc(gpu_bo* bo)
{
	for (uint32 i = 0; i < MAX_TRACKED_BOS; i++) {
		if (sLiveBos[i] == NULL) {
			sLiveBos[i] = bo;
			sLiveCount++;
			return;
		}
	}
	ERROR("BO tracking table full (%u entries), leak detection disabled for %s\n",
		MAX_TRACKED_BOS, bo->name);
}


static void
track_free(gpu_bo* bo)
{
	for (uint32 i = 0; i < MAX_TRACKED_BOS; i++) {
		if (sLiveBos[i] == bo) {
			sLiveBos[i] = NULL;
			sLiveCount--;
			return;
		}
	}
}


status_t
gpu_bo_alloc(gpu_bo* bo, const char* name, uint32 size, uint32 alignment)
{
	if (bo == NULL)
		return B_BAD_VALUE;

	memset(bo, 0, sizeof(*bo));

	if (size == 0) {
		ERROR("%s: zero-size allocation rejected\n", name ? name : "(unnamed)");
		return B_BAD_VALUE;
	}

	// Every other caller in the accelerant passes alignment=0 to the
	// kernel ioctl (see engine.cpp:225, render.cpp:319, mode.cpp:375,
	// overlay.cpp:392). The kernel guarantees page alignment on the
	// returned address, which is 4096 bytes — far exceeding any Gen5
	// state-object alignment requirement (64-byte kernel, 32-byte VFE
	// state, 16-byte interface descriptor, 32-byte surface state). We
	// therefore ignore the requested alignment for the ioctl call and
	// rely on the kernel's page granularity. The 'alignment' field is
	// kept in the gpu_bo struct purely for debug logging.
	addr_t base = 0;
	status_t status = intel_allocate_memory(size, 0, 0, base);
	if (status != B_OK) {
		ERROR("%s: intel_allocate_memory(%u) failed: %s\n",
			name ? name : "(unnamed)", size, strerror(status));
		return status;
	}

	bo->cpu_addr = base;
	bo->gtt_offset = (uint32)(base - (addr_t)gInfo->shared_info->graphics_memory);
	bo->size = size;
	bo->alignment = alignment;
	bo->name = name;
	bo->valid = true;

	track_alloc(bo);

	TRACE("%s: alloc %u bytes @ cpu=%p gtt=0x%x\n",
		name ? name : "(unnamed)", size, (void*)base, bo->gtt_offset);

	return B_OK;
}


void
gpu_bo_free(gpu_bo* bo)
{
	if (bo == NULL || !bo->valid)
		return;

	TRACE("%s: free @ cpu=%p gtt=0x%x\n",
		bo->name ? bo->name : "(unnamed)", (void*)bo->cpu_addr,
		bo->gtt_offset);

	track_free(bo);
	intel_free_memory(bo->cpu_addr);
	memset(bo, 0, sizeof(*bo));
}


void
gpu_bo_clear(gpu_bo* bo)
{
	if (bo == NULL || !bo->valid)
		return;
	memset((void*)bo->cpu_addr, 0, bo->size);
}


void
gpu_bo_write32(gpu_bo* bo, uint32 offset, uint32 value)
{
	if (bo == NULL || !bo->valid)
		return;
	if (offset + 4 > bo->size) {
		ERROR("%s: write32 out of bounds (offset=%u size=%u)\n",
			bo->name ? bo->name : "(unnamed)", offset, bo->size);
		return;
	}
	*(volatile uint32*)(bo->cpu_addr + offset) = value;
}


uint32
gpu_bo_read32(gpu_bo* bo, uint32 offset)
{
	if (bo == NULL || !bo->valid)
		return 0;
	if (offset + 4 > bo->size) {
		ERROR("%s: read32 out of bounds (offset=%u size=%u)\n",
			bo->name ? bo->name : "(unnamed)", offset, bo->size);
		return 0;
	}
	return *(volatile uint32*)(bo->cpu_addr + offset);
}


void
gpu_bo_write(gpu_bo* bo, uint32 offset, const void* data, uint32 size)
{
	if (bo == NULL || !bo->valid || data == NULL)
		return;
	if (offset + size > bo->size) {
		ERROR("%s: write out of bounds (offset=%u len=%u size=%u)\n",
			bo->name ? bo->name : "(unnamed)", offset, size, bo->size);
		return;
	}
	memcpy((void*)(bo->cpu_addr + offset), data, size);
}


static inline void
_clflush_range(void* addr, size_t size)
{
	uintptr_t start = (uintptr_t)addr & ~63ULL;
	uintptr_t end = ((uintptr_t)addr + size + 63) & ~63ULL;
	for (uintptr_t p = start; p < end; p += 64)
		asm volatile("clflush (%0)" :: "r"(p) : "memory");
	asm volatile("mfence" ::: "memory");
}


void
gpu_bo_flush_cpu_writes(void)
{
	asm volatile("mfence" ::: "memory");
}


void
gpu_bo_clflush(gpu_bo* bo)
{
	if (bo == NULL || !bo->valid)
		return;
	_clflush_range((void*)bo->cpu_addr, bo->size);
}


uint32
gpu_bo_live_count(void)
{
	return sLiveCount;
}


void
gpu_bo_dump_live(void)
{
	_sPrintf("intel_extreme gpu_bo: %u live BOs\n", sLiveCount);
	for (uint32 i = 0; i < MAX_TRACKED_BOS; i++) {
		gpu_bo* bo = sLiveBos[i];
		if (bo == NULL)
			continue;
		_sPrintf("  [%u] %s: %u bytes @ gtt=0x%x cpu=%p\n",
			i, bo->name ? bo->name : "(unnamed)",
			bo->size, bo->gtt_offset, (void*)bo->cpu_addr);
	}
}
