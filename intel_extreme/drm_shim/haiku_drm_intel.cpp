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
#include "lock.h"

/* MI commands for ring submission */
#define MI_BATCH_GTT              (2 << 6)
#define MI_BATCH_NON_SECURE_I965  (1 << 8)
#define MI_BATCH_BUFFER_START_CMD ((0x31 << 23) | MI_BATCH_GTT)
#define MI_NOOP_CMD               0x00000000
#define MI_FLUSH_DRM              (0x04 << 23)
#define MI_STORE_DATA_IMM_GGTT    ((0x20 << 23) | (1 << 22) | 2)

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
	/* Marker for batch completion tracking */
	uint32_t     marker_gtt;   /* GTT offset of marker DWORD */
	volatile uint32_t* marker_cpu; /* CPU pointer to marker DWORD */
	uint32_t     marker_seq;   /* monotonic sequence number */
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
gem_set_tiling(struct drm_i915_gem_set_tiling* args)
{
	/* Store tiling mode in the BO's flags for future use.
	 * For now, accept any tiling mode but don't program HW fences. */
	uint32_t h = args->handle;
	if (h == 0 || h >= MAX_BOS || !sShim.bos[h].used) {
		errno = ENOENT;
		return -1;
	}
	/* Accept the tiling but report no swizzle */
	args->swizzle_mode = 0;  /* I915_BIT_6_SWIZZLE_NONE */
	return 0;
}


static int
gem_get_tiling(struct drm_i915_gem_get_tiling* args)
{
	uint32_t h = args->handle;
	if (h == 0 || h >= MAX_BOS || !sShim.bos[h].used) {
		errno = ENOENT;
		return -1;
	}
	/* Report untiled for now */
	args->tiling_mode = I915_TILING_NONE;
	args->swizzle_mode = 0;
	args->phys_swizzle_mode = 0;
	return 0;
}


static int
gem_wait(struct drm_i915_gem_wait* args)
{
	/* Synchronous execution — batch is already done when EXECBUFFER2 returns */
	(void)args;
	return 0;
}


static int
gem_context_getparam(struct drm_i915_gem_context_param* args)
{
	switch (args->param) {
	case I915_CONTEXT_PARAM_GTT_SIZE:
		args->value = 256 * 1024 * 1024;  /* 256 MB */
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}


static int
gem_context_setparam(struct drm_i915_gem_context_param* args)
{
	/* Accept but ignore */
	(void)args;
	return 0;
}


static int
get_reset_stats(struct drm_i915_reset_stats* args)
{
	/* No resets ever */
	memset(args, 0, sizeof(*args));
	return 0;
}


static inline void
ring_write32(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t*)(sShim.registers + offset) = value;
}

static inline uint32_t
ring_read32(uint32_t offset)
{
	return *(volatile uint32_t*)(sShim.registers + offset);
}

/* Write TAIL register via kernel ioctl (userspace MMIO is read-only) */
static void
ring_kick_tail(uint32_t tail_value)
{
	intel_ring_tail data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	data.tail_value = tail_value;
	ioctl(sShim.fd, INTEL_RING_WRITE_TAIL, &data, sizeof(data));
}

/* Reset ring via kernel ioctl */
static void
ring_reset_ioctl(void)
{
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	ioctl(sShim.fd, INTEL_RING_RESET, &data, sizeof(data));
}


static int
gem_execbuffer2(struct drm_i915_gem_execbuffer2* args)
{
	if (args->buffer_count == 0) { errno = EINVAL; return -1; }

	struct drm_i915_gem_exec_object2* objs =
		(struct drm_i915_gem_exec_object2*)(uintptr_t)args->buffers_ptr;

	/* --- Step 1: Identify the batch BO --- */
	uint32_t batch_idx = 0;
	if (args->flags & I915_EXEC_BATCH_FIRST)
		batch_idx = 0;
	else
		batch_idx = args->buffer_count - 1;

	uint32_t batch_handle = objs[batch_idx].handle;
	if (batch_handle == 0 || batch_handle >= MAX_BOS
		|| !sShim.bos[batch_handle].used) {
		printf("[drm] EXECBUF2: invalid batch handle %u\n", batch_handle);
		errno = ENOENT;
		return -1;
	}

	gem_bo& batch_bo = sShim.bos[batch_handle];

	/* Fill in GTT offsets for all BOs in the validation list */
	for (uint32_t i = 0; i < args->buffer_count; i++) {
		uint32_t h = objs[i].handle;
		if (h == 0 || h >= MAX_BOS || !sShim.bos[h].used) {
			printf("[drm] EXECBUF2: invalid handle %u at index %u\n", h, i);
			errno = ENOENT;
			return -1;
		}
		objs[i].offset = sShim.bos[h].gtt_offset;
	}

	/* --- Step 2: Apply relocations --- */
	if (!(args->flags & I915_EXEC_NO_RELOC)) {
		for (uint32_t i = 0; i < args->buffer_count; i++) {
			if (objs[i].relocation_count == 0)
				continue;

			uint32_t h = objs[i].handle;
			gem_bo& bo = sShim.bos[h];
			struct drm_i915_gem_relocation_entry* relocs =
				(struct drm_i915_gem_relocation_entry*)(uintptr_t)
				objs[i].relocs_ptr;

			for (uint32_t r = 0; r < objs[i].relocation_count; r++) {
				/* Target BO (with HANDLE_LUT, target_handle is index) */
				uint32_t target_idx = (uint32_t)relocs[r].target_handle;
				if (target_idx >= args->buffer_count) {
					printf("[drm] EXECBUF2: reloc target %u out of range\n",
						target_idx);
					errno = EINVAL;
					return -1;
				}

				uint32_t target_h = objs[target_idx].handle;
				uint32_t target_gtt = sShim.bos[target_h].gtt_offset;
				uint32_t patched = target_gtt + (uint32_t)relocs[r].delta;

				/* Patch the address into the BO at reloc.offset */
				uint32_t* patch_addr = (uint32_t*)((uint8_t*)bo.cpu_addr
					+ relocs[r].offset);
				*patch_addr = patched;

				/* Update presumed_offset for NO_RELOC optimization */
				relocs[r].presumed_offset = target_gtt;
			}
		}
	}

	/* Flush CPU writes to all BOs */
	asm volatile("mfence" ::: "memory");

	/* --- Step 3: Submit batch via kernel INTEL_EXEC_BATCH ioctl --- */
	/* The kernel emits MI_BATCH_BUFFER_START + MI_FLUSH to the ring
	 * and writes the TAIL register. Same pattern as i915 execbuffer. */
	uint32_t batch_gtt = batch_bo.gtt_offset + args->batch_start_offset;

	/* Prepare completion marker */
	uint32_t seq = ++sShim.marker_seq;
	if (sShim.marker_cpu)
		*sShim.marker_cpu = 0;

	/* Flush CPU cache for batch BO (WC aperture writes are not coherent
	 * with GPU instruction fetch — discovered on this ILK hardware).
	 * Same purpose as i915_gem_clflush_object() in Linux. */
	{
		uint8_t* p = (uint8_t*)batch_bo.cpu_addr;
		for (uint32_t i = 0; i < batch_bo.size; i += 64)
			asm volatile("clflush (%0)" :: "r"(p + i) : "memory");
		asm volatile("mfence" ::: "memory");
	}

	/* Execute batch via kernel ioctl */
	intel_exec_batch execData;
	execData.magic = INTEL_PRIVATE_DATA_MAGIC;
	execData.batch_gtt = batch_gtt;
	execData.batch_len = args->batch_len;
	if (ioctl(sShim.fd, INTEL_EXEC_BATCH, &execData, sizeof(execData)) != 0) {
		printf("[drm] EXECBUF2: INTEL_EXEC_BATCH ioctl failed\n");
		errno = EIO;
		return -1;
	}

	/* Write completion marker to ring via TAIL kick */
	ring_buffer& ring = sShim.shared->primary_ring_buffer;
	if (sShim.marker_cpu) {
		uint32_t* rb = (uint32_t*)ring.base;
		uint32_t pos = ring.position / 4;
		uint32_t mask = (ring.size / 4) - 1;

		rb[pos & mask] = MI_STORE_DATA_IMM_GGTT; pos++;
		rb[pos & mask] = 0; pos++;
		rb[pos & mask] = sShim.marker_gtt; pos++;
		rb[pos & mask] = seq; pos++;
		if (pos & 1) { rb[pos & mask] = 0; pos++; }

		ring.position = (pos * 4) & (ring.size - 1);
		asm volatile("mfence" ::: "memory");
		ring_kick_tail(ring.position);
	}

	/* --- Step 4: Wait for completion (synchronous for now) --- */
	if (sShim.marker_cpu) {
		bigtime_t deadline = system_time() + 2000000; /* 2 second timeout */
		while (*sShim.marker_cpu != seq) {
			if (system_time() > deadline) {
				uint32_t head = ring_read32(ring.register_base
					+ RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK;
				uint32_t tail = ring_read32(ring.register_base
					+ RING_BUFFER_TAIL) & (ring.size - 1);
				printf("[drm] EXECBUF2: TIMEOUT seq %u (got 0x%x) "
					"HEAD=0x%x TAIL=0x%x ringpos=0x%x batch_gtt=0x%x\n",
					seq, *sShim.marker_cpu, head, tail,
					ring.position, batch_gtt);
				errno = ETIMEDOUT;
				return -1;
			}
		}
	} else {
		/* No marker — just wait a bit */
		snooze(5000);
	}

	return 0;
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

	/* Allocate a small marker BO for batch completion tracking */
	intel_allocate_graphics_memory marker_alloc;
	marker_alloc.magic = INTEL_PRIVATE_DATA_MAGIC;
	marker_alloc.size = 4096;
	marker_alloc.alignment = 4096;
	marker_alloc.flags = 0;
	if (ioctl(sShim.fd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &marker_alloc,
		sizeof(marker_alloc)) == 0) {
		sShim.marker_cpu = (volatile uint32_t*)(addr_t)marker_alloc.buffer_base;
		sShim.marker_gtt = (uint32_t)((addr_t)marker_alloc.buffer_base
			- (addr_t)sShim.shared->graphics_memory);
		*sShim.marker_cpu = 0;
		printf("[drm] Marker BO: gtt=0x%x cpu=%p\n",
			sShim.marker_gtt, (void*)sShim.marker_cpu);
	}

	/* Sync ring position with hardware TAIL (don't reset — kills CS).
	 * Same approach as render_init_clone: read HW TAIL, set sw pos. */
	{
		ring_buffer& ring = sShim.shared->primary_ring_buffer;
		uint32_t hwTail = ring_read32(ring.register_base + RING_BUFFER_TAIL)
			& (ring.size - 1);
		uint32_t hwHead = ring_read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		ring.position = hwTail;
		ring.space_left = ring.size - 64;
		printf("[drm] Ring sync: HEAD=0x%x TAIL=0x%x pos=%u size=%u\n",
			hwHead, hwTail, ring.position, ring.size);
	}

	/* Ring test: MI_STORE_DATA_IMM via ioctl TAIL kick */
	if (sShim.marker_cpu) {
		ring_buffer& ring = sShim.shared->primary_ring_buffer;
		*sShim.marker_cpu = 0;
		asm volatile("mfence" ::: "memory");

		uint32_t* rb = (uint32_t*)ring.base;
		uint32_t pos = ring.position / 4;
		uint32_t mask = (ring.size / 4) - 1;

		rb[pos & mask] = MI_STORE_DATA_IMM_GGTT; pos++;
		rb[pos & mask] = 0; pos++;
		rb[pos & mask] = sShim.marker_gtt; pos++;
		rb[pos & mask] = 0xBEEF0001; pos++;
		if (pos & 1) { rb[pos & mask] = 0; pos++; }

		ring.position = (pos * 4) & (ring.size - 1);
		asm volatile("mfence" ::: "memory");
		ring_kick_tail(ring.position);

		snooze(10000);
		uint32_t head_post = ring_read32(ring.register_base + 4) & 0x1FFFFC;

		printf("[drm] Ring test: marker=0x%08x HEAD=0x%x → %s\n",
			*sShim.marker_cpu, head_post,
			*sShim.marker_cpu == 0xBEEF0001 ? "GPU WORKS!" : "FAILED");
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
	case DRM_IOCTL_I915_GEM_SET_TILING:
		return gem_set_tiling((struct drm_i915_gem_set_tiling*)arg);
	case DRM_IOCTL_I915_GEM_GET_TILING:
		return gem_get_tiling((struct drm_i915_gem_get_tiling*)arg);
	case DRM_IOCTL_I915_GEM_WAIT:
		return gem_wait((struct drm_i915_gem_wait*)arg);
	case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM:
		return gem_context_getparam((struct drm_i915_gem_context_param*)arg);
	case DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM:
		return gem_context_setparam((struct drm_i915_gem_context_param*)arg);
	case DRM_IOCTL_I915_GET_RESET_STATS:
		return get_reset_stats((struct drm_i915_reset_stats*)arg);
	default:
		printf("[drm] Unknown ioctl 0x%lx\n", request);
		errno = EINVAL;
		return -1;
	}
}
