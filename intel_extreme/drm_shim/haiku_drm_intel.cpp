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
#include "accelerant.h"
#include "gpu_ring.h"

/* gInfo required by gpu_ring.o, gpu_bo.o, engine.o (accelerant .o files).
 * Allocated and configured in haiku_drm_open(). */
accelerant_info* gInfo = NULL;

/* MI commands for ring submission */
#define MI_BATCH_BUFFER_START_CMD ((0x31 << 23) | (2 << 6))  /* Gen4/5 GGTT (MI_BATCH_GTT) */
#define MI_NOOP_CMD               0x00000000
#define MI_FLUSH_DRM              (0x04 << 23)
#define MI_STORE_DATA_IMM_GGTT    ((0x20 << 23) | (1 << 22) | 2)
/* PIPE_CONTROL for post-batch flush (Gen5+).
 * MI_FLUSH causes IS stall after 3D pipeline commands on ILK.
 * SNA/i915 use PIPE_CONTROL with RT flush after 3DPRIMITIVE. */
#define PIPE_CONTROL_CMD          0x7A000002  /* 4 DW total, flags in DW0 on Gen5 */
#define PIPE_CONTROL_CS_STALL     (1 << 20)   /* Command Streamer stall */
#define PIPE_CONTROL_QW_WRITE     (1 << 14)   /* QWord write to address in DW1 */
#define PIPE_CONTROL_WC_FLUSH     (1 << 12)   /* Render Target Cache Flush */
#define PIPE_CONTROL_GLOBAL_GTT   (1 << 2)    /* DW1: use GGTT (not PPGTT) */
#define PIPE_CONTROL_TC_FLUSH     (1 << 10)
#define PIPE_CONTROL_NOWRITE      0
/* PIPELINE_SELECT — must be 3D before any 3DSTATE commands */
#define CMD_PIPELINE_SELECT_3D    0x69040000

/* Flush WC (write-combining) buffers to ensure GPU visibility.
 * On Gen5, GTT aperture is mapped WC. mfence alone does NOT guarantee
 * data reaches GGTT — clflush is required per cache line. */
static inline void
clflush_range(void* addr, size_t size)
{
	uintptr_t start = (uintptr_t)addr & ~63ULL;
	uintptr_t end = ((uintptr_t)addr + size + 63) & ~63ULL;
	for (uintptr_t p = start; p < end; p += 64)
		asm volatile("clflush (%0)" :: "r"(p) : "memory");
	asm volatile("mfence" ::: "memory");
}

#define MAX_BOS 4096

/* Per-BO tracking */
struct gem_bo {
	bool     used;
	uint64_t size;
	uint32_t gtt_offset;
	void*    cpu_addr;     /* mapped CPU address */
	uint32_t tiling_mode;  /* I915_TILING_NONE/X/Y */
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
	/* Generic ring submission layer (via kernel ioctl) */
	gpu_ring     ring;
	bool         ring_ready;
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
		/* Return error for unknown params — Gen5 doesn't support topology
		 * masks, slices, etc. Returning 0 causes Mesa to crash processing
		 * empty masks. Returning EINVAL makes Mesa use hardcoded fallbacks. */
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
	sShim.bos[h].tiling_mode = args->tiling_mode;
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
	args->tiling_mode = sShim.bos[h].tiling_mode;
	args->swizzle_mode = 0;  /* I915_BIT_6_SWIZZLE_NONE */
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
	uint32_t total_relocs = 0;
	bool no_reloc = (args->flags & I915_EXEC_NO_RELOC) != 0;
	if (!no_reloc) {
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

				/* Bounds check: reloc offset must be within BO */
				if (relocs[r].offset + 4 > bo.size) {
					printf("[drm] EXECBUF2: reloc offset 0x%llx OOB "
						"(BO size 0x%llx)\n",
						(unsigned long long)relocs[r].offset,
						(unsigned long long)bo.size);
					errno = EINVAL;
					return -1;
				}
				/* Patch the address into the BO at reloc.offset */
				uint32_t* patch_addr = (uint32_t*)((uint8_t*)bo.cpu_addr
					+ relocs[r].offset);
				*patch_addr = patched;

				/* Update presumed_offset for NO_RELOC optimization */
				relocs[r].presumed_offset = target_gtt;
				total_relocs++;
			}
		}
	}

	/* Flush CPU writes to all BOs via clflush (WC requires this).
	 * mfence alone does NOT flush WC combining buffers on Gen5.
	 * clflush forces each cache line out to the GGTT aperture. */
	for (uint32_t i = 0; i < args->buffer_count; i++) {
		uint32_t h = objs[i].handle;
		gem_bo& bo = sShim.bos[h];
		clflush_range(bo.cpu_addr, bo.size);
	}

	/* --- Step 3: Inline batch into ring + MI_STORE_DATA_IMM marker --- */
	uint32_t seq = ++sShim.marker_seq;
	if (sShim.marker_cpu)
		*sShim.marker_cpu = 0;

	uint32_t batch_dw_count = args->batch_len / 4;
	uint32_t* batch_cmds = (uint32_t*)((uint8_t*)batch_bo.cpu_addr
		+ args->batch_start_offset);

	/* Strip trailing MI_BATCH_BUFFER_END + padding (not valid in ring) */
	while (batch_dw_count > 0
		&& (batch_cmds[batch_dw_count - 1] == (0x0A << 23)
			|| batch_cmds[batch_dw_count - 1] == 0))
		batch_dw_count--;

	/* 3D pipeline preamble: ensure pipeline is in 3D mode.
	 * NOTE: MI_FLUSH before PIPELINE_SELECT hangs the CS on ILK
	 * when the ring was previously idle. Only emit PIPELINE_SELECT. */
	uint32_t preamble_dw = 2;  /* PIPELINE_SELECT(3D) + MI_NOOP pad */
	uint32_t total_dw = preamble_dw + batch_dw_count
		+ (sShim.marker_cpu ? 4 : 0);
	gpu_ring_begin(&sShim.ring, total_dw);

	/* PIPELINE_SELECT: 3D pipeline (value 0).
	 * Opcode: CMD_GFX(1,1,4) = 0x69040000. Bit 0 = 0 for 3D. */
	gpu_ring_emit(&sShim.ring, 0x69040000);  /* PIPELINE_SELECT(3D) */
	gpu_ring_emit(&sShim.ring, 0x00000000);  /* MI_NOOP pad */

	for (uint32_t i = 0; i < batch_dw_count; i++)
		gpu_ring_emit(&sShim.ring, batch_cmds[i]);

	if (sShim.marker_cpu) {
		gpu_ring_emit(&sShim.ring, MI_STORE_DATA_IMM_GGTT);
		gpu_ring_emit(&sShim.ring, 0);
		gpu_ring_emit(&sShim.ring, sShim.marker_gtt);
		gpu_ring_emit(&sShim.ring, seq);
	}

	gpu_ring_advance(&sShim.ring);

	/* Debug */
	static uint32_t sExecCount = 0;
	sExecCount++;
	if (sExecCount <= 20) {
		uint32_t head = gpu_ring_read_head(&sShim.ring);
		printf("[drm] EXECBUF2 #%u: %u DW inlined, "
			"HEAD=0x%x pos=0x%x\n",
			sExecCount, batch_dw_count, head, sShim.ring.pos);
	}

	/* --- Step 4: Wait for completion --- */
	if (sShim.marker_cpu) {
		bigtime_t deadline = system_time() + 2000000; /* 2s */
		while (*sShim.marker_cpu != seq) {
			if (system_time() > deadline) {
				ring_buffer& ring = sShim.shared->primary_ring_buffer;
				uint32_t head = ring_read32(ring.register_base
					+ RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK;
				uint32_t tail = ring_read32(ring.register_base
					+ RING_BUFFER_TAIL) & (ring.size - 1);
				printf("[drm] EXECBUF2: TIMEOUT seq %u (got 0x%x) "
					"HEAD=0x%x TAIL=0x%x rpos=0x%x\n",
					seq, *sShim.marker_cpu, head, tail,
					sShim.ring.pos);
				/* Dump GPU debug registers */
				printf("[drm]   IPEHR=0x%08x IPEIR=0x%08x\n",
					ring_read32(0x2068), ring_read32(0x2064));
				printf("[drm]   INSTDONE=0x%08x ACTHD=0x%08x\n",
					ring_read32(0x206C), ring_read32(0x2074));
				printf("[drm]   EIR=0x%08x ESR=0x%08x\n",
					ring_read32(0x20B0), ring_read32(0x20B8));
				/* Dump ring content around HEAD */
				{
					uint32_t* rbd = (uint32_t*)(addr_t)sShim.ring.base;
					uint32_t hmask = (sShim.ring.size/4) - 1;
					uint32_t hp = (head / 4);
					printf("[drm]   Ring @HEAD=0x%x:", head);
					for (int d = -2; d < 10; d++)
						printf(" %08x", rbd[(hp+d) & hmask]);
					printf("\n");
				}
				/* Dump batch BO content */
				{
					uint32_t* bcmd = (uint32_t*)((uint8_t*)batch_bo.cpu_addr
						+ args->batch_start_offset);
					printf("[drm]   Batch BO (%u DW):\n", (args->batch_len / 4));
					for (uint32_t i = 0; i < (args->batch_len / 4) && i < 128; i += 8) {
						printf("[drm]   [%3u]", i);
						for (uint32_t j = i; j < i+8 && j < (args->batch_len / 4); j++)
							printf(" %08x", bcmd[j]);
						printf("\n");
					}
				}
				errno = ETIMEDOUT;
				return -1;
			}
		}
	} else {
		/* No marker — just wait a bit */
		snooze(5000);
	}

	{
		uint32_t marker_val = sShim.marker_cpu ? *sShim.marker_cpu : 0;
		if (sExecCount <= 20) {
			uint32_t head_post = ring_read32(
				sShim.shared->primary_ring_buffer.register_base
				+ RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK;
			printf("[drm] EXECBUF2 #%u: OK seq=%u HEAD→0x%x rpos=0x%x\n",
				sExecCount, seq, head_post, sShim.ring.pos);
		}
	}
	if (sExecCount <= 5) {
		/* Check render target BO content (first object that isn't batch) */
		for (uint32_t i = 0; i < args->buffer_count; i++) {
			uint32_t h = objs[i].handle;
			if (h != batch_handle && h > 0 && h < MAX_BOS
				&& sShim.bos[h].used && sShim.bos[h].size >= 16) {
				uint32_t* p = (uint32_t*)sShim.bos[h].cpu_addr;
				printf("[drm]   BO#%u gtt=0x%x: %08x %08x %08x %08x\n",
					h, sShim.bos[h].gtt_offset,
					p[0], p[1], p[2], p[3]);
				break;
			}
		}
	}
	return 0;
}


/* ---- Public API ---- */

extern "C" int
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

	/* Set up gInfo for accelerant .o files (gpu_ring, gpu_bo, engine) */
	gInfo = (accelerant_info*)calloc(1, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = sShim.fd;
	gInfo->shared_info = sShim.shared;
	gInfo->shared_info_area = sShim.shared_area;
	gInfo->registers = sShim.registers;
	gInfo->regs_area = sShim.regs_area;

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

	/* Apply Gen5 3D workarounds via kernel ioctl.
	 * Critical: WaDisableRenderCachePipelinedFlush (CACHE_MODE_0 bit 8)
	 * Without this, MI_FLUSH after 3DSTATE commands hangs the CS. */
	{
		intel_get_private_data init3d;
		init3d.magic = INTEL_PRIVATE_DATA_MAGIC;
		if (ioctl(sShim.fd, INTEL_RING_INIT_3D, &init3d, sizeof(init3d)) == 0)
			printf("[drm] 3D pipeline workarounds applied\n");
		else
			printf("[drm] WARNING: INTEL_RING_INIT_3D failed!\n");
	}

	/* Init generic ring layer (syncs with hardware TAIL, no reset) */
	if (gpu_ring_init(&sShim.ring, sShim.fd) == B_OK) {
		sShim.ring_ready = true;
		printf("[drm] Ring init: pos=%u size=%u\n",
			sShim.ring.pos, sShim.ring.size);
	} else {
		printf("[drm] Ring init FAILED\n");
	}

	/* Ring test: MI_STORE_DATA_IMM via gpu_ring */
	if (sShim.ring_ready && sShim.marker_cpu) {
		*sShim.marker_cpu = 0;
		asm volatile("mfence" ::: "memory");

		uint32_t test_cmds[6];
		test_cmds[0] = MI_STORE_DATA_IMM_GGTT;
		test_cmds[1] = 0;
		test_cmds[2] = sShim.marker_gtt;
		test_cmds[3] = 0xBEEF0001;
		test_cmds[4] = 0;  // padding for QWord align
		test_cmds[5] = 0;
		gpu_ring_submit(&sShim.ring, test_cmds, 4);

		snooze(10000);
		printf("[drm] Ring test: marker=0x%08x → %s\n",
			*sShim.marker_cpu,
			*sShim.marker_cpu == 0xBEEF0001 ? "GPU WORKS!" : "FAILED");
	}

	printf("[drm] Haiku DRM shim opened: chipset=0x%04x\n",
		sShim.shared->device_type.type);
	return sShim.fd;
}


extern "C" void
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


extern "C" int
haiku_drm_ioctl(int fd, unsigned long request, void* arg)
{
	(void)fd;

	/* Extract base ioctl number: Mesa uses BSD _IOWR encoding (0xC0xx6479)
	 * but our constants are simple numbers (0x79). Mask to get the low byte
	 * which contains (group << 8 | num), then just take the low 8 bits. */
	unsigned long req = request & 0xFF;

	/* Debug: log first few unique ioctl requests */
	static unsigned long seen[32];
	static int nseen = 0;
	bool is_new = true;
	for (int i = 0; i < nseen; i++)
		if (seen[i] == request) { is_new = false; break; }
	if (is_new && nseen < 32) {
		seen[nseen++] = request;
		printf("[drm] ioctl: request=0x%lx → masked=0x%lx\n", request, req);
	}

	switch (req) {
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
	case DRM_IOCTL_I915_QUERY:
		/* Not supported on Gen5 — Mesa falls back to getparam for ver < 10 */
		errno = EINVAL;
		return -1;
	case DRM_IOCTL_I915_GEM_SET_CACHING:
	case DRM_IOCTL_I915_GEM_MADVISE:
		return 0;  /* silently accept */
	case DRM_IOCTL_SYNCOBJ_CREATE:
	{
		struct { uint32_t handle; uint32_t flags; }* so =
			(decltype(so))arg;
		static uint32_t sNextSyncobj = 100;
		so->handle = ++sNextSyncobj;
		printf("[drm] SYNCOBJ_CREATE: handle=%u\n", so->handle);
		return 0;
	}
	case DRM_IOCTL_SYNCOBJ_WAIT:
	case DRM_IOCTL_SYNCOBJ_DESTROY:
		return 0;  /* stub — always signaled, no-op destroy */
	case DRM_IOCTL_I915_REG_READ:
	{
		/* struct drm_i915_reg_read { uint64_t offset; uint64_t val; } */
		struct { uint64_t offset; uint64_t val; }* rr =
			(decltype(rr))arg;
		uint32_t reg = (uint32_t)(rr->offset & 0xFFFFFF);
		rr->val = ring_read32(reg);
		return 0;
	}
	case DRM_IOCTL_I915_GEM_THROTTLE:
		return 0;  /* no throttling needed */
	case DRM_IOCTL_I915_GEM_USERPTR:
	case DRM_IOCTL_GEM_OPEN:
		errno = ENOTSUP;
		return -1;
	default:
		printf("[drm] Unknown ioctl 0x%lx\n", request);
		errno = EINVAL;
		return -1;
	}
}


/* Exported functions for Mesa (extern "C", not inline) */
extern "C" int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned long nr = (request >> 0) & 0xFF;  /* _IOC_NR */
	return haiku_drm_ioctl(fd, nr, arg);
}

extern "C" int
drmOpen(const char *name, const char *busid)
{
	(void)name; (void)busid;
	return haiku_drm_open();
}

extern "C" int
drmClose(int fd)
{
	haiku_drm_close(fd);
	return 0;
}

/* Stub for softpipe_create_screen — pulled in by ddebug_screen_create
 * via sw_helper.h. We never use the software fallback. */
extern "C" void*
softpipe_create_screen(void* winsys)
{
	(void)winsys;
	return NULL;
}
