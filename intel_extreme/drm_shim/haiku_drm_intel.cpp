/*
 * haiku_drm_intel.cpp — DRM shim implementation for Mesa crocus on Haiku.
 *
 * Translates i915 DRM GEM ioctls to Haiku intel_extreme driver calls.
 * GEM handles are indices into a local BO table.
 */

#include "haiku_drm_intel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <OS.h>

/* Haiku intel_extreme headers */
#include "intel_extreme.h"

#define MAX_BOS 4096

/* Per-BO tracking */
struct gem_bo {
	bool     used;
	uint64_t size;
	uint32_t gtt_offset;
	void*    cpu_addr;     /* mapped CPU address */
};

/* Global shim state */
static struct {
	int          fd;           /* Haiku device fd */
	intel_shared_info* shared;
	area_id      shared_area;
	area_id      regs_area;
	uint8*       registers;
	gem_bo       bos[MAX_BOS];
	uint32_t     next_handle;  /* starts at 1, 0 = invalid */
	uint32_t     next_ctx;
} sShim;


/* ---- Internal helpers ---- */

static uint32_t
alloc_handle(void)
{
	for (uint32_t i = 1; i < MAX_BOS; i++) {
		uint32_t h = ((sShim.next_handle + i) % (MAX_BOS - 1)) + 1;
		if (!sShim.bos[h].used) {
			sShim.next_handle = h;
			return h;
		}
	}
	return 0; /* table full */
}


static int
gem_create(struct drm_i915_gem_create* args)
{
	uint32_t h = alloc_handle();
	if (h == 0) { errno = ENOMEM; return -1; }

	/* Round up to page size */
	uint64_t size = (args->size + 4095) & ~4095ULL;

	/* Allocate via Haiku ioctl */
	intel_allocate_graphics_memory alloc;
	alloc.magic = INTEL_PRIVATE_DATA_MAGIC;
	alloc.size = (uint32_t)size;
	alloc.alignment = 4096;
	alloc.flags = 0;

	if (ioctl(sShim.fd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &alloc,
		sizeof(alloc)) != 0) {
		errno = ENOMEM;
		return -1;
	}

	gem_bo& bo = sShim.bos[h];
	bo.used = true;
	bo.size = size;
	bo.cpu_addr = (void*)(addr_t)alloc.buffer_base;
	bo.gtt_offset = (uint32_t)((addr_t)alloc.buffer_base
		- (addr_t)sShim.shared->graphics_memory);

	/* Zero the buffer (Linux GEM guarantees this) */
	memset(bo.cpu_addr, 0, size);

	args->handle = h;

	printf("[drm] GEM_CREATE: handle=%u size=%llu gtt=0x%x cpu=%p\n",
		h, (unsigned long long)size, bo.gtt_offset, bo.cpu_addr);
	return 0;
}


static int
gem_close(struct drm_gem_close* args)
{
	uint32_t h = args->handle;
	if (h == 0 || h >= MAX_BOS || !sShim.bos[h].used) {
		errno = ENOENT;
		return -1;
	}

	gem_bo& bo = sShim.bos[h];

	/* Free via Haiku ioctl */
	intel_free_graphics_memory freed;
	freed.magic = INTEL_PRIVATE_DATA_MAGIC;
	freed.buffer_base = (uint32_t)(addr_t)bo.cpu_addr;
	ioctl(sShim.fd, INTEL_FREE_GRAPHICS_MEMORY, &freed, sizeof(freed));

	bo.used = false;
	bo.cpu_addr = NULL;
	bo.size = 0;
	return 0;
}


static int
gem_mmap(struct drm_i915_gem_mmap* args)
{
	uint32_t h = args->handle;
	if (h == 0 || h >= MAX_BOS || !sShim.bos[h].used) {
		errno = ENOENT;
		return -1;
	}

	/* Already mapped by the allocator — just return the address */
	args->addr_ptr = (uint64_t)(addr_t)sShim.bos[h].cpu_addr + args->offset;
	return 0;
}


static int
gem_busy(struct drm_i915_gem_busy* args)
{
	/* For now, always report idle. TODO: track pending batches. */
	args->busy = 0;
	return 0;
}


static int
gem_set_domain(struct drm_i915_gem_set_domain* args)
{
	/* CPU↔GPU coherency: just mfence */
	asm volatile("mfence" ::: "memory");
	return 0;
}


static int
getparam(struct drm_i915_getparam* args)
{
	if (args->value == NULL) { errno = EINVAL; return -1; }

	switch (args->param) {
	case I915_PARAM_CHIPSET_ID:
		*args->value = 0x0046;  /* Ironlake Mobile */
		break;
	case I915_PARAM_HAS_EXECBUF2:
		*args->value = 1;
		break;
	case I915_PARAM_NUM_FENCES_AVAIL:
		*args->value = 16;
		break;
	case I915_PARAM_HAS_RELAXED_FENCING:
		*args->value = 1;
		break;
	case I915_PARAM_REVISION:
		*args->value = 0x12;  /* ILK stepping */
		break;
	case I915_PARAM_MMAP_GTT_VERSION:
		*args->value = 0;  /* no GTT mmap, use regular mmap */
		break;
	case I915_PARAM_CS_TIMESTAMP_FREQUENCY:
		*args->value = 12500000;  /* 12.5 MHz for ILK */
		break;
	default:
		printf("[drm] GETPARAM: unknown param %d\n", args->param);
		errno = EINVAL;
		return -1;
	}
	return 0;
}


static int
get_aperture(struct drm_i915_gem_get_aperture* args)
{
	/* Stolen memory size from shared_info */
	args->aper_size = 256 * 1024 * 1024;  /* 256 MB typical for ILK */
	args->aper_available_size = 128 * 1024 * 1024;
	return 0;
}


static int
gem_context_create(struct drm_i915_gem_context_create* args)
{
	args->ctx_id = ++sShim.next_ctx;
	return 0;
}


static int
gem_context_destroy(struct drm_i915_gem_context_destroy* args)
{
	(void)args;
	return 0;
}


static int
gem_execbuffer2(struct drm_i915_gem_execbuffer2* args)
{
	/* TODO: This is the critical path for Mesa.
	 * 1. Resolve relocations in the batch BO
	 * 2. Submit via MI_BATCH_BUFFER_START to the ring
	 * 3. Wait for completion (or return async)
	 */
	printf("[drm] EXECBUFFER2: %u buffers, batch_len=%u — NOT YET IMPLEMENTED\n",
		args->buffer_count, args->batch_len);
	errno = ENOSYS;
	return -1;
}


/* ---- Public API ---- */

int
haiku_drm_open(void)
{
	memset(&sShim, 0, sizeof(sShim));
	sShim.next_handle = 1;

	sShim.fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (sShim.fd < 0) return -1;

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sShim.fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(sShim.fd);
		return -1;
	}

	sShim.shared_area = clone_area("drm shared",
		(void**)&sShim.shared, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sShim.shared_area < B_OK) {
		close(sShim.fd);
		return -1;
	}

	sShim.regs_area = clone_area("drm regs",
		(void**)&sShim.registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		sShim.shared->registers_area);
	if (sShim.regs_area < B_OK) {
		delete_area(sShim.shared_area);
		close(sShim.fd);
		return -1;
	}

	printf("[drm] Haiku DRM shim opened: chipset=0x%04x\n",
		sShim.shared->device_type.type);
	return sShim.fd;
}


void
haiku_drm_close(int fd)
{
	/* Free any remaining BOs */
	for (uint32_t i = 1; i < MAX_BOS; i++) {
		if (sShim.bos[i].used) {
			struct drm_gem_close c = { i, 0 };
			gem_close(&c);
		}
	}

	delete_area(sShim.regs_area);
	delete_area(sShim.shared_area);
	close(sShim.fd);
	memset(&sShim, 0, sizeof(sShim));
}


int
haiku_drm_ioctl(int fd, unsigned long request, void* arg)
{
	(void)fd;

	switch (request) {
	case DRM_IOCTL_I915_GEM_CREATE:
		return gem_create((struct drm_i915_gem_create*)arg);
	case DRM_IOCTL_GEM_CLOSE:
		return gem_close((struct drm_gem_close*)arg);
	case DRM_IOCTL_I915_GEM_MMAP:
	case DRM_IOCTL_I915_GEM_MMAP_GTT:
		return gem_mmap((struct drm_i915_gem_mmap*)arg);
	case DRM_IOCTL_I915_GEM_BUSY:
		return gem_busy((struct drm_i915_gem_busy*)arg);
	case DRM_IOCTL_I915_GEM_SET_DOMAIN:
		return gem_set_domain((struct drm_i915_gem_set_domain*)arg);
	case DRM_IOCTL_I915_GETPARAM:
		return getparam((struct drm_i915_getparam*)arg);
	case DRM_IOCTL_I915_GEM_GET_APERTURE:
		return get_aperture((struct drm_i915_gem_get_aperture*)arg);
	case DRM_IOCTL_I915_GEM_EXECBUFFER2:
		return gem_execbuffer2((struct drm_i915_gem_execbuffer2*)arg);
	case DRM_IOCTL_I915_GEM_CONTEXT_CREATE:
		return gem_context_create((struct drm_i915_gem_context_create*)arg);
	case DRM_IOCTL_I915_GEM_CONTEXT_DESTROY:
		return gem_context_destroy((struct drm_i915_gem_context_destroy*)arg);
	default:
		printf("[drm] Unknown ioctl 0x%lx\n", request);
		errno = EINVAL;
		return -1;
	}
}
