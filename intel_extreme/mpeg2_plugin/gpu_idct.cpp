/*
 * GPU-accelerated IDCT implementation for Intel Gen5 (Ironlake).
 *
 * Uses the same mechanism as the accelerant's media_pipeline.cpp:
 * - Open device, get shared_info via ioctl
 * - Clone register and graphics memory areas
 * - Allocate GTT buffers for kernel, CURBE, input, output
 * - Submit MEDIA_OBJECT batches via the primary ring buffer
 *
 * The ring buffer lock (benaphore in shared_info) ensures safe
 * concurrent access with app_server's accelerant.
 */

#include "gpu_idct.h"
#include "idct_ref.h"  // for kIdctTableGpu

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <OS.h>
#include <intel_extreme.h>


#define LOG(fmt, ...) fprintf(stderr, "gpu_idct: " fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Ring buffer and hardware definitions (from accelerant headers)
// ---------------------------------------------------------------------------

#define RING_BUFFER_TAIL		0x00
#define RING_BUFFER_HEAD		0x04
#define RING_BUFFER_START		0x08
#define RING_BUFFER_CONTROL		0x0C

#define MI_NOOP					0x00000000
#define MI_FLUSH				0x04000000
#define MI_STORE_DATA_IMM		0x20200000

#define CMD_PIPELINE_SELECT		0x69040000
#define PIPELINE_SELECT_MEDIA	0x00000001
#define CMD_STATE_BASE_ADDRESS	0x61010000
#define CMD_URB_FENCE			0x60000000
#define CMD_MEDIA_STATE_POINTERS 0x70000000
#define CMD_CS_URB_STATE		0x60010000
#define CMD_CONSTANT_BUFFER		0x60020000
#define CMD_MEDIA_OBJECT		0x71000000
#define CMD_3DSTATE_DEPTH_BUFFER 0x79050005

#define BASE_ADDRESS_MODIFY		0x00000001
#define UF0_VFE_REALLOC			(1 << 12)
#define UF0_CS_REALLOC			(1 << 13)

// VFE state
#define VFE_GENERIC_MODE		0


// ---------------------------------------------------------------------------
// IDCT kernel binary (from idct_single.g4b.gen5, assembled)
// ---------------------------------------------------------------------------

static const uint32 kIdctKernel[][4] = {
#include "../accelerant/kernels/idct_single.g4b.gen5"
};


// ---------------------------------------------------------------------------
// GPU context
// ---------------------------------------------------------------------------

struct gpu_context {
	int			device_fd;
	area_id		shared_area;
	area_id		regs_area;

	intel_shared_info*	shared_info;
	uint8*		registers;
	uint8*		graphics_memory;

	// GTT-allocated buffers
	addr_t		kernel_base;
	addr_t		curbe_base;
	addr_t		input_base;
	addr_t		output_base;
	addr_t		vfe_state_base;
	addr_t		idrt_base;
	addr_t		surface_state_base;
	addr_t		binding_table_base;

	bool		initialized;
};

static gpu_context sCtx = {};


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline uint32
read32(uint32 offset)
{
	return *(volatile uint32*)(sCtx.registers + offset);
}

static inline void
write32(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sCtx.registers + offset) = value;
}

static status_t
gpu_alloc(size_t size, size_t alignment, addr_t& base)
{
	intel_allocate_graphics_memory alloc;
	alloc.magic = INTEL_PRIVATE_DATA_MAGIC;
	alloc.size = size;
	alloc.alignment = alignment;
	alloc.flags = 0;

	if (ioctl(sCtx.device_fd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &alloc,
			sizeof(alloc)) < 0)
		return errno;

	base = alloc.buffer_base;
	return B_OK;
}

static void
gpu_free(addr_t base)
{
	intel_free_graphics_memory free_mem;
	free_mem.magic = INTEL_PRIVATE_DATA_MAGIC;
	free_mem.buffer_base = base;
	ioctl(sCtx.device_fd, INTEL_FREE_GRAPHICS_MEMORY, &free_mem,
		sizeof(free_mem));
}

static inline uint8*
gtt_ptr(addr_t base)
{
	return sCtx.graphics_memory + (base - (addr_t)sCtx.graphics_memory);
}

static inline uint32
gtt_offset(addr_t base)
{
	return (uint32)(base - (addr_t)sCtx.graphics_memory);
}

static void
gpu_write_mem(addr_t base, uint32 offset, const void* data, size_t size)
{
	memcpy(gtt_ptr(base) + offset, data, size);
}

static void
gpu_write32_mem(addr_t base, uint32 offset, uint32 value)
{
	*(uint32*)(gtt_ptr(base) + offset) = value;
}

static void
gpu_clear_mem(addr_t base, size_t size)
{
	memset(gtt_ptr(base), 0, size);
}


// ---------------------------------------------------------------------------
// Ring buffer command submission
// ---------------------------------------------------------------------------

struct ring_writer {
	ring_buffer*	ring;
	uint32			count;
};

static uint32 sIdctRingPos = 0;
static bool sIdctRingInited = false;

static void
rw_begin(ring_writer* rw)
{
	rw->ring = &sCtx.shared_info->primary_ring_buffer;
	rw->count = 0;

	if (!sIdctRingInited) {
		// First use: sync with hardware TAIL
		uint32 hwTail = read32(rw->ring->register_base + RING_BUFFER_TAIL)
			& (rw->ring->size - 1);
		sIdctRingPos = hwTail;
		sIdctRingInited = true;
	}

	rw->ring->position = sIdctRingPos;
	acquire_lock(&rw->ring->lock);
}

static void
rw_write(ring_writer* rw, uint32 dword)
{
	uint32 pos = rw->ring->position;
	uint32* ringBase = (uint32*)(sCtx.graphics_memory
		+ rw->ring->offset);
	ringBase[pos / 4] = dword;
	rw->ring->position = (pos + 4) & (rw->ring->size - 1);
	rw->count++;
}

static void
rw_end(ring_writer* rw)
{
	// Pad to 8-byte alignment
	if (rw->ring->position & 0x07)
		rw_write(rw, MI_NOOP);

	// Flush CPU writes
	asm volatile("mfence" ::: "memory");

	// Kick GPU via kernel ioctl (userspace MMIO writes don't work)
	intel_ring_tail tail_data;
	tail_data.magic = INTEL_PRIVATE_DATA_MAGIC;
	tail_data.tail_value = rw->ring->position;
	ioctl(sCtx.device_fd, INTEL_RING_WRITE_TAIL, &tail_data,
		sizeof(tail_data));

	sIdctRingPos = rw->ring->position;
	release_lock(&rw->ring->lock);
}

// Wait for ring to drain (HEAD catches up to TAIL).
// After completion, sync sIdctRingPos with HEAD for ring reuse.
static bool
ring_wait(uint32 timeout_us)
{
	ring_buffer& ring = sCtx.shared_info->primary_ring_buffer;
	bigtime_t deadline = system_time() + timeout_us;
	uint32 tail = ring.position;
	while (system_time() < deadline) {
		uint32 head = read32(ring.register_base + RING_BUFFER_HEAD)
			& 0x001FFFFC;
		if (head == tail) {
			sIdctRingPos = head;
			return true;
		}
		snooze(10);
	}
	return false;
}


// ---------------------------------------------------------------------------
// GPU IDCT setup (VFE state, IDRT, CURBE, surface states)
// ---------------------------------------------------------------------------

static void
setup_vfe_state(uint32 max_threads)
{
	gpu_clear_mem(sCtx.vfe_state_base, 64);
	// DW0: no scratch
	gpu_write32_mem(sCtx.vfe_state_base, 0, 0);
	// DW1: mode=GENERIC, num_urb_entries=max_threads, alloc_size=0, max=count-1
	uint32 vfe1 = (VFE_GENERIC_MODE << 3)
		| ((max_threads & 0x7f) << 9)
		| (0u << 16)
		| (((max_threads - 1) & 0x7f) << 25);
	gpu_write32_mem(sCtx.vfe_state_base, 4, vfe1);
	// DW2: idrt pointer
	gpu_write32_mem(sCtx.vfe_state_base, 8,
		gtt_offset(sCtx.idrt_base) & ~0xfu);
}

static void
setup_idrt(uint32 curbe_read_len)
{
	gpu_clear_mem(sCtx.idrt_base, 64);
	// DW0: grf_reg_blocks=15 (full 128 GRF file), kernel pointer
	uint32 desc0 = 15u | (gtt_offset(sCtx.kernel_base) & ~0x3fu);
	gpu_write32_mem(sCtx.idrt_base, 0, desc0);
	// DW1: single_program_flow=1, const_urb_entry_read_len
	uint32 desc1 = (1u << 18) | ((curbe_read_len & 0x3fu) << 26);
	gpu_write32_mem(sCtx.idrt_base, 4, desc1);
	// DW2: no sampler
	gpu_write32_mem(sCtx.idrt_base, 8, 0);
	// DW3: binding table
	uint32 desc3 = (2u & 0x1fu)
		| (gtt_offset(sCtx.binding_table_base) & ~0x1fu);
	gpu_write32_mem(sCtx.idrt_base, 12, desc3);
}

static void
setup_curbe(void)
{
	// IDCT kernel uses curbe_read_len=20: g1-g4 padding + g5-g20 cosine table.
	// Write table at byte offset 128 (= g5 start).
	gpu_clear_mem(sCtx.curbe_base, 1024);
	gpu_write_mem(sCtx.curbe_base, 128, kIdctTableGpu, sizeof(kIdctTableGpu));
}

static void
setup_surfaces(void)
{
	// Two SURFTYPE_BUFFER surface states: input (BTI 0), output (BTI 1).
	// Surface state = 8 DWORDs (32 bytes) each.
	gpu_clear_mem(sCtx.surface_state_base, 256);

	uint32 ss_offset = gtt_offset(sCtx.surface_state_base);

	// Surface 0: input buffer (read)
	uint32 input_gtt = gtt_offset(sCtx.input_base);
	gpu_write32_mem(sCtx.surface_state_base, 0, (4 << 29));  // SURFTYPE_BUFFER
	gpu_write32_mem(sCtx.surface_state_base, 4, input_gtt);  // base address
	gpu_write32_mem(sCtx.surface_state_base, 8,
		((GPU_IDCT_MAX_BATCH * 128 - 1) & 0x7f) | (((GPU_IDCT_MAX_BATCH * 128 - 1) >> 7) << 10));
	gpu_write32_mem(sCtx.surface_state_base, 12, (GPU_IDCT_MAX_BATCH * 128 - 1) >> 21);

	// Surface 1: output buffer (write) at offset 32
	uint32 output_gtt = gtt_offset(sCtx.output_base);
	gpu_write32_mem(sCtx.surface_state_base, 32, (4 << 29));
	gpu_write32_mem(sCtx.surface_state_base, 36, output_gtt);
	gpu_write32_mem(sCtx.surface_state_base, 40,
		((GPU_IDCT_MAX_BATCH * 128 - 1) & 0x7f) | (((GPU_IDCT_MAX_BATCH * 128 - 1) >> 7) << 10));
	gpu_write32_mem(sCtx.surface_state_base, 44, (GPU_IDCT_MAX_BATCH * 128 - 1) >> 21);

	// Binding table: BTI 0 → surface 0, BTI 1 → surface 1
	gpu_clear_mem(sCtx.binding_table_base, 64);
	gpu_write32_mem(sCtx.binding_table_base, 0, ss_offset + 0);
	gpu_write32_mem(sCtx.binding_table_base, 4, ss_offset + 32);
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

status_t
gpu_idct_init(void)
{
	if (sCtx.initialized)
		return B_OK;

	// Open device
	// Try both device path formats
	sCtx.device_fd = open("/dev/graphics/intel_extreme_000200", O_RDWR);
	if (sCtx.device_fd < 0)
		sCtx.device_fd = open("/dev/graphics/intel_extreme/0", O_RDWR);
	if (sCtx.device_fd < 0) {
		LOG("cannot open GPU device\n");
		return B_ERROR;
	}

	// Get shared_info area
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sCtx.device_fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		LOG("INTEL_GET_PRIVATE_DATA failed\n");
		close(sCtx.device_fd);
		return B_ERROR;
	}

	// Clone shared info
	sCtx.shared_area = clone_area("gpu_idct shared",
		(void**)&sCtx.shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sCtx.shared_area < 0) {
		LOG("clone shared_info failed\n");
		close(sCtx.device_fd);
		return B_ERROR;
	}

	// Clone registers
	sCtx.regs_area = clone_area("gpu_idct regs",
		(void**)&sCtx.registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, sCtx.shared_info->registers_area);
	if (sCtx.regs_area < 0) {
		LOG("clone registers failed\n");
		delete_area(sCtx.shared_area);
		close(sCtx.device_fd);
		return B_ERROR;
	}

	sCtx.graphics_memory = (uint8*)sCtx.shared_info->graphics_memory;

	// Sync ring position with hardware TAIL (don't reset — kills CS)
	{
		ring_buffer& ring = sCtx.shared_info->primary_ring_buffer;
		uint32 hwTail = read32(ring.register_base + RING_BUFFER_TAIL)
			& (ring.size - 1);
		ring.position = hwTail;
		ring.space_left = ring.size - 64;
	}

	// Allocate GTT buffers
	#define ALLOC_OR_FAIL(name, size, align) \
		if (gpu_alloc(size, align, sCtx.name) != B_OK) { \
			LOG("alloc " #name " failed\n"); \
			gpu_idct_uninit(); return B_ERROR; \
		}

	ALLOC_OR_FAIL(kernel_base, 4096, 64);
	ALLOC_OR_FAIL(curbe_base, 1024, 64);
	ALLOC_OR_FAIL(input_base, GPU_IDCT_MAX_BATCH * 128, 64);
	ALLOC_OR_FAIL(output_base, GPU_IDCT_MAX_BATCH * 128, 64);
	ALLOC_OR_FAIL(vfe_state_base, 64, 64);
	ALLOC_OR_FAIL(idrt_base, 64, 64);
	ALLOC_OR_FAIL(surface_state_base, 256, 64);
	ALLOC_OR_FAIL(binding_table_base, 64, 64);
	#undef ALLOC_OR_FAIL

	// Upload IDCT kernel
	gpu_clear_mem(sCtx.kernel_base, 4096);
	gpu_write_mem(sCtx.kernel_base, 0, kIdctKernel, sizeof(kIdctKernel));

	// Setup static state (CURBE, surfaces, VFE, IDRT)
	setup_curbe();
	setup_surfaces();
	setup_vfe_state(GPU_IDCT_MAX_BATCH > 48 ? 48 : GPU_IDCT_MAX_BATCH);
	setup_idrt(20);  // curbe_read_len=20 for g5-g20 IDCT table

	sCtx.initialized = true;
	LOG("initialized: kernel=%u bytes, max_batch=%d\n",
		(unsigned)sizeof(kIdctKernel), GPU_IDCT_MAX_BATCH);
	return B_OK;
}


void
gpu_idct_uninit(void)
{
	if (sCtx.kernel_base) gpu_free(sCtx.kernel_base);
	if (sCtx.curbe_base) gpu_free(sCtx.curbe_base);
	if (sCtx.input_base) gpu_free(sCtx.input_base);
	if (sCtx.output_base) gpu_free(sCtx.output_base);
	if (sCtx.vfe_state_base) gpu_free(sCtx.vfe_state_base);
	if (sCtx.idrt_base) gpu_free(sCtx.idrt_base);
	if (sCtx.surface_state_base) gpu_free(sCtx.surface_state_base);
	if (sCtx.binding_table_base) gpu_free(sCtx.binding_table_base);

	if (sCtx.regs_area >= 0) delete_area(sCtx.regs_area);
	if (sCtx.shared_area >= 0) delete_area(sCtx.shared_area);
	if (sCtx.device_fd >= 0) close(sCtx.device_fd);

	memset(&sCtx, 0, sizeof(sCtx));
}


bool
gpu_idct_available(void)
{
	return sCtx.initialized;
}


status_t
gpu_idct_process(const int16 blocks[][64], int16 output[][64], uint32 count)
{
	if (!sCtx.initialized || count == 0)
		return B_NOT_INITIALIZED;
	if (count > GPU_IDCT_MAX_BATCH)
		count = GPU_IDCT_MAX_BATCH;

	// Upload input blocks to GPU memory
	gpu_write_mem(sCtx.input_base, 0, blocks, count * 128);

	// Clear output
	gpu_clear_mem(sCtx.output_base, count * 128);

	// Memory fence before GPU reads
	asm volatile("mfence" ::: "memory");

	ring_buffer& ring = sCtx.shared_info->primary_ring_buffer;

	// Build batch in a GTT buffer (not the ring) to avoid ring space issues.
	// The ring only gets MI_BATCH_BUFFER_START (2 DW per submission).
	uint32* batch = (uint32*)gtt_ptr(sCtx.input_base
		+ GPU_IDCT_MAX_BATCH * 128);  // use space after input data
	uint32 bp = 0;

	// Preamble
	batch[bp++] = MI_FLUSH;
	batch[bp++] = CMD_3DSTATE_DEPTH_BUFFER;
	batch[bp++] = (7 << 29) | (1 << 18);
	batch[bp++] = 0; batch[bp++] = 0; batch[bp++] = 0; batch[bp++] = 0;
	batch[bp++] = CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA;
	batch[bp++] = CMD_STATE_BASE_ADDRESS | 6;
	for (int j = 0; j < 7; j++) batch[bp++] = BASE_ADDRESS_MODIFY;

	uint32 vfe_entries = (count > 48) ? 48 : count;
	batch[bp++] = CMD_URB_FENCE | UF0_VFE_REALLOC | UF0_CS_REALLOC | 1;
	batch[bp++] = 0;
	batch[bp++] = (vfe_entries << 10) | ((vfe_entries + 16) << 20);
	batch[bp++] = CMD_MEDIA_STATE_POINTERS | 1;
	batch[bp++] = 0;
	batch[bp++] = gtt_offset(sCtx.vfe_state_base);
	batch[bp++] = CMD_CS_URB_STATE | 0;
	batch[bp++] = ((16 - 1) << 4) | 1;
	batch[bp++] = CMD_CONSTANT_BUFFER | (1u << 8) | 0;
	batch[bp++] = gtt_offset(sCtx.curbe_base) | ((10 - 1) & 0x1f);

	// MEDIA_OBJECTs
	for (uint32 i = 0; i < count; i++) {
		uint32 byte_offset = i * 128;
		batch[bp++] = CMD_MEDIA_OBJECT | 18;
		batch[bp++] = 0; batch[bp++] = 0; batch[bp++] = 0;
		batch[bp++] = byte_offset;
		batch[bp++] = byte_offset;
		for (int p = 2; p < 16; p++) batch[bp++] = 0;
	}

	batch[bp++] = MI_FLUSH;
	batch[bp++] = (0x0A << 23);  // MI_BATCH_BUFFER_END
	if (bp & 1) batch[bp++] = MI_NOOP;

	asm volatile("mfence" ::: "memory");

	// Submit from ring: MI_BATCH_BUFFER_START → batch GTT addr
	uint32 batch_gtt = gtt_offset(sCtx.input_base) + GPU_IDCT_MAX_BATCH * 128;

	// Ensure ring has space for 4 DW (BBS + padding)
	if (sIdctRingPos + 16 > ring.size) {
		ring_wait(500000);
		sIdctRingPos = 0;
	}

	ring_writer rw;
	rw_begin(&rw);
	rw_write(&rw, (0x31 << 23));  // MI_BATCH_BUFFER_START
	rw_write(&rw, batch_gtt);
	rw_end(&rw);

	// Wait for completion
	if (!ring_wait(500000)) {
		uint32 head = read32(ring.register_base + RING_BUFFER_HEAD)
			& 0x001FFFFC;
		uint32 tail = read32(ring.register_base + RING_BUFFER_TAIL)
			& (ring.size - 1);
		LOG("GPU timeout after %u blocks: HEAD=0x%x TAIL=0x%x "
			"ringPos=0x%x sIdctRingPos=0x%x ringSize=%u\n",
			count, head, tail, ring.position, sIdctRingPos, ring.size);
		return B_TIMED_OUT;
	}

	// Read back results
	memcpy(output, gtt_ptr(sCtx.output_base), count * 128);

	return B_OK;
}
