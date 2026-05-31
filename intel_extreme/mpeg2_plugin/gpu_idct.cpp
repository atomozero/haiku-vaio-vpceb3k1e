/*
 * GPU-accelerated IDCT for the MPEG-2 decoder plugin.
 *
 * Uses the proven gpu_ring layer for ring buffer submission (same
 * layer used by gpu_idct_bench, gpu_plasma_demo, media_pipeline).
 * Completion is tracked via MI_STORE_DATA_IMM markers in GTT memory,
 * not HEAD register polling.
 *
 * Design: see GPU_IDCT_DESIGN.md
 */

#include "gpu_idct.h"
#include "idct_ref.h"    // kIdctTableGpu cosine table
#include "gpu_ring.h"    // proven ring submission layer

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
// Hardware command definitions (not in intel_extreme.h)
// ---------------------------------------------------------------------------

#define CMD_PIPELINE_SELECT      0x69040000
#define PIPELINE_SELECT_MEDIA    0x00000001
#define CMD_STATE_BASE_ADDRESS   0x61010000
#define CMD_URB_FENCE            0x60000000
#define CMD_MEDIA_STATE_POINTERS 0x70000000
#define CMD_CS_URB_STATE         0x60010000
#define CMD_CONSTANT_BUFFER      0x60020000
#define CMD_MEDIA_OBJECT         0x71000000
#define CMD_3DSTATE_DEPTH_BUFFER 0x79050005

#define BASE_ADDRESS_MODIFY      0x00000001
#define UF0_VFE_REALLOC          (1 << 12)
#define UF0_CS_REALLOC           (1 << 13)

#define VFE_GENERIC_MODE         0

// MI_STORE_DATA_IMM: write a DWORD to GGTT address.
// DW0 = (0x20 << 23) | (1 << 22) | 2  = 0x10400002
// DW1 = 0 (reserved), DW2 = GGTT addr, DW3 = data
#define MI_STORE_DATA_IMM_GGTT   0x10400002

// Marker sentinel / tag
#define MARKER_SENTINEL          0xDEADBEEFu
#define MARKER_TAG               0xBEEF0042u


// ---------------------------------------------------------------------------
// IDCT kernel binary
// ---------------------------------------------------------------------------

static const uint32 kIdctKernel[][4] = {
#include "../accelerant/kernels/idct_single.g4b.gen5"
};


// ---------------------------------------------------------------------------
// GPU context
// ---------------------------------------------------------------------------

struct gpu_context {
	int             device_fd;
	area_id         shared_area;

	intel_shared_info* shared_info;
	uint8*          graphics_memory;

	gpu_ring        ring;        // proven ring submission layer

	// GTT-allocated buffers (CPU virtual addresses from allocator)
	addr_t          kernel_base;
	addr_t          curbe_base;
	addr_t          input_base;
	addr_t          output_base;
	addr_t          vfe_state_base;
	addr_t          idrt_base;
	addr_t          surface_state_base;
	addr_t          binding_table_base;
	addr_t          batch_base;
	addr_t          marker_base;

	bool            initialized;
	bool            ring_tested;   // phase 1 passed
	bool            marker_tested; // phase 2 passed
};

static gpu_context sCtx = {};


// ---------------------------------------------------------------------------
// GTT helpers
// ---------------------------------------------------------------------------

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
	if (base == 0)
		return;
	intel_free_graphics_memory free_mem;
	free_mem.magic = INTEL_PRIVATE_DATA_MAGIC;
	free_mem.buffer_base = base;
	ioctl(sCtx.device_fd, INTEL_FREE_GRAPHICS_MEMORY, &free_mem,
		sizeof(free_mem));
}

static inline uint8*
gtt_cpu(addr_t base)
{
	return sCtx.graphics_memory + (base - (addr_t)sCtx.graphics_memory);
}

static inline uint32
gtt_offset(addr_t base)
{
	return (uint32)(base - (addr_t)sCtx.graphics_memory);
}

static void
gtt_write(addr_t base, uint32 offset, const void* data, size_t size)
{
	memcpy(gtt_cpu(base) + offset, data, size);
}

static void
gtt_write32(addr_t base, uint32 offset, uint32 value)
{
	*(uint32*)(gtt_cpu(base) + offset) = value;
}

static void
gtt_clear(addr_t base, size_t size)
{
	memset(gtt_cpu(base), 0, size);
}


// ---------------------------------------------------------------------------
// Wait for marker value in GTT memory (CPU polls)
// ---------------------------------------------------------------------------

static bool
wait_marker(uint32 timeout_us)
{
	volatile uint32* slot = (volatile uint32*)gtt_cpu(sCtx.marker_base);

	// Fast spin for 2ms
	bigtime_t t0 = system_time();
	while ((system_time() - t0) < 2000) {
		if (*slot == MARKER_TAG)
			return true;
	}

	// Slow poll with snooze
	bigtime_t deadline = t0 + timeout_us;
	while (system_time() < deadline) {
		if (*slot == MARKER_TAG)
			return true;
		snooze(100);
	}

	return false;
}


// ---------------------------------------------------------------------------
// GPU state setup
// ---------------------------------------------------------------------------

static void
setup_vfe_state(uint32 max_threads)
{
	gtt_clear(sCtx.vfe_state_base, 64);
	// DW0: no scratch space
	gtt_write32(sCtx.vfe_state_base, 0, 0);
	// DW1: mode=GENERIC, num_urb_entries, alloc_size=0, max_threads
	uint32 vfe1 = (VFE_GENERIC_MODE << 3)
		| ((max_threads & 0x7f) << 9)
		| (0u << 16)
		| (((max_threads - 1) & 0x7f) << 25);
	gtt_write32(sCtx.vfe_state_base, 4, vfe1);
	// DW2: IDRT pointer
	gtt_write32(sCtx.vfe_state_base, 8,
		gtt_offset(sCtx.idrt_base) & ~0xfu);
}

static void
setup_idrt(uint32 curbe_read_len)
{
	gtt_clear(sCtx.idrt_base, 64);
	// DW0: kernel pointer + grf_reg_blocks=15 (128 GRF file)
	uint32 desc0 = 15u | (gtt_offset(sCtx.kernel_base) & ~0x3fu);
	gtt_write32(sCtx.idrt_base, 0, desc0);
	// DW1: single_program_flow=1, const_urb_entry_read_len
	uint32 desc1 = (1u << 18) | ((curbe_read_len & 0x3fu) << 26);
	gtt_write32(sCtx.idrt_base, 4, desc1);
	// DW2: no sampler
	gtt_write32(sCtx.idrt_base, 8, 0);
	// DW3: binding table pointer + count
	uint32 desc3 = (2u & 0x1fu)
		| (gtt_offset(sCtx.binding_table_base) & ~0x1fu);
	gtt_write32(sCtx.idrt_base, 12, desc3);
}

static void
setup_curbe(void)
{
	// CURBE layout: g1-g4 = 128 bytes padding, g5-g20 = cosine table.
	// curbe_read_len=20 pushes these as g1..g20 into the kernel.
	gtt_clear(sCtx.curbe_base, 1024);
	gtt_write(sCtx.curbe_base, 128, kIdctTableGpu, sizeof(kIdctTableGpu));
}

static void
write_buffer_surface(addr_t ss_base, uint32 ss_offset,
	uint32 buffer_gtt, uint32 buffer_bytes)
{
	// Match the EXACT encoding from media_pipeline.cpp's
	// write_linear_surface_state_at(). The surface size is expressed
	// as num_entries of 16 bytes (OWord = 128 bits = 16 bytes).
	if (buffer_bytes < 16)
		buffer_bytes = 16;
	uint32 num_entries = buffer_bytes / 16;
	uint32 ne_m1 = num_entries - 1;

	// DW0: SURFTYPE_BUFFER + SURFACEFORMAT_RAW
	uint32 ss0 = (4u << 29) | (0x1FFu << 18);  // I965_SURFACEFORMAT_RAW = 0x1FF
	gtt_write32(ss_base, ss_offset + 0, ss0);

	// DW1: base address (GTT offset)
	gtt_write32(ss_base, ss_offset + 4, buffer_gtt);

	// DW2: Width [12:6] = ne_m1[6:0], Height [31:19] = ne_m1[19:7]
	uint32 width_bits  = ne_m1 & 0x7Fu;
	uint32 height_bits = (ne_m1 >> 7) & 0x1FFFu;
	uint32 ss2 = (width_bits << 6) | (height_bits << 19);
	gtt_write32(ss_base, ss_offset + 8, ss2);

	// DW3: Depth [27:21] = ne_m1[26:20], Surface Pitch [19:3] = 15
	uint32 depth_bits = (ne_m1 >> 20) & 0x7Fu;
	uint32 ss3 = (depth_bits << 21) | (15u << 3);
	gtt_write32(ss_base, ss_offset + 12, ss3);
}

static void
setup_surfaces(void)
{
	gtt_clear(sCtx.surface_state_base, 256);

	uint32 ss_off = gtt_offset(sCtx.surface_state_base);
	uint32 buf_size = GPU_IDCT_MAX_BATCH * 128;

	// Surface 0 (BTI 0): input buffer (OWord Block Read)
	write_buffer_surface(sCtx.surface_state_base, 0,
		gtt_offset(sCtx.input_base), buf_size);

	// Surface 1 (BTI 1): output buffer (OWord Block Write)
	write_buffer_surface(sCtx.surface_state_base, 32,
		gtt_offset(sCtx.output_base), buf_size);

	// Binding table: BTI 0 → surface 0, BTI 1 → surface 1
	gtt_clear(sCtx.binding_table_base, 64);
	gtt_write32(sCtx.binding_table_base, 0, ss_off + 0);
	gtt_write32(sCtx.binding_table_base, 4, ss_off + 32);
}


// ---------------------------------------------------------------------------
// Build media batch in batch_base GTT buffer
// ---------------------------------------------------------------------------

static uint32
build_batch(uint32 block_count)
{
	uint32* batch = (uint32*)gtt_cpu(sCtx.batch_base);
	uint32 bp = 0;

	// 10-command media preamble (matches working media_pipeline.cpp)
	batch[bp++] = MI_FLUSH;

	// 3DSTATE_DEPTH_BUFFER null (6 DW)
	batch[bp++] = CMD_3DSTATE_DEPTH_BUFFER;
	batch[bp++] = (7 << 29) | (1 << 18);
	batch[bp++] = 0;
	batch[bp++] = 0;
	batch[bp++] = 0;
	batch[bp++] = 0;

	// PIPELINE_SELECT media
	batch[bp++] = CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA;

	// STATE_BASE_ADDRESS Ironlake (8 DW)
	batch[bp++] = CMD_STATE_BASE_ADDRESS | 6;
	for (int j = 0; j < 7; j++)
		batch[bp++] = BASE_ADDRESS_MODIFY;

	// URB_FENCE
	uint32 vfe_entries = (block_count > 48) ? 48 : block_count;
	batch[bp++] = CMD_URB_FENCE | UF0_VFE_REALLOC | UF0_CS_REALLOC | 1;
	batch[bp++] = 0;
	batch[bp++] = (vfe_entries << 10) | ((vfe_entries + 16) << 20);

	// MEDIA_STATE_POINTERS
	batch[bp++] = CMD_MEDIA_STATE_POINTERS | 1;
	batch[bp++] = 0;
	batch[bp++] = gtt_offset(sCtx.vfe_state_base);

	// CS_URB_STATE
	batch[bp++] = CMD_CS_URB_STATE | 0;
	batch[bp++] = ((16 - 1) << 4) | 1;

	// CONSTANT_BUFFER: 10 units of 64 bytes = 640 bytes = 20 GRFs
	batch[bp++] = CMD_CONSTANT_BUFFER | (1u << 8) | 0;
	batch[bp++] = gtt_offset(sCtx.curbe_base) | ((10 - 1) & 0x1f);

	// N x MEDIA_OBJECT (20 DW each: 4 header + 16 inline)
	for (uint32 i = 0; i < block_count; i++) {
		uint32 byte_offset = i * 128;
		batch[bp++] = CMD_MEDIA_OBJECT | 18;
		batch[bp++] = 0;  // interface descriptor index
		batch[bp++] = 0;  // indirect data length
		batch[bp++] = 0;  // indirect data pointer
		batch[bp++] = byte_offset;  // inline DW0: input offset
		batch[bp++] = byte_offset;  // inline DW1: output offset
		for (int p = 2; p < 16; p++)
			batch[bp++] = 0;
	}

	// Completion marker: MI_STORE_DATA_IMM writes tag to marker_bo
	batch[bp++] = MI_STORE_DATA_IMM_GGTT;
	batch[bp++] = 0;
	batch[bp++] = gtt_offset(sCtx.marker_base);
	batch[bp++] = MARKER_TAG;

	// Final flush + end
	batch[bp++] = MI_FLUSH;
	batch[bp++] = (0x0A << 23);  // MI_BATCH_BUFFER_END
	if (bp & 1)
		batch[bp++] = MI_NOOP;

	asm volatile("mfence" ::: "memory");

	return bp;
}


// ---------------------------------------------------------------------------
// Phase 1: Ring test (MI_NOOP)
// ---------------------------------------------------------------------------

static bool
test_ring(void)
{
	uint32 head_before = gpu_ring_read_head(&sCtx.ring);

	status_t st = gpu_ring_begin(&sCtx.ring, 4);
	if (st != B_OK) {
		LOG("ring test: begin failed\n");
		return false;
	}
	gpu_ring_emit(&sCtx.ring, MI_NOOP);
	gpu_ring_emit(&sCtx.ring, MI_NOOP);
	st = gpu_ring_advance(&sCtx.ring);
	if (st != B_OK) {
		LOG("ring test: advance failed\n");
		return false;
	}

	bool idle = gpu_ring_wait_idle(&sCtx.ring, 50000);
	uint32 head_after = gpu_ring_read_head(&sCtx.ring);

	LOG("ring test: HEAD 0x%x → 0x%x %s\n",
		head_before, head_after,
		idle ? "RING OK" : "RING DEAD");

	return idle;
}


// ---------------------------------------------------------------------------
// Phase 2: Marker test (MI_STORE_DATA_IMM via ring)
// ---------------------------------------------------------------------------

static bool
test_marker(void)
{
	// Reset marker
	*(volatile uint32*)gtt_cpu(sCtx.marker_base) = MARKER_SENTINEL;
	asm volatile("mfence" ::: "memory");

	// Write MI_STORE_DATA_IMM directly in ring (4 DW)
	status_t st = gpu_ring_begin(&sCtx.ring, 6);
	if (st != B_OK) {
		LOG("marker test: begin failed\n");
		return false;
	}
	gpu_ring_emit(&sCtx.ring, MI_STORE_DATA_IMM_GGTT);
	gpu_ring_emit(&sCtx.ring, 0);
	gpu_ring_emit(&sCtx.ring, gtt_offset(sCtx.marker_base));
	gpu_ring_emit(&sCtx.ring, MARKER_TAG);
	st = gpu_ring_advance(&sCtx.ring);
	if (st != B_OK) {
		LOG("marker test: advance failed\n");
		return false;
	}

	bool ok = wait_marker(50000);
	uint32 val = *(volatile uint32*)gtt_cpu(sCtx.marker_base);

	LOG("marker test: expected=0x%x got=0x%x %s\n",
		MARKER_TAG, val, ok ? "MARKER OK" : "MARKER FAILED");

	return ok;
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
	sCtx.device_fd = open("/dev/graphics/intel_extreme_000200", O_RDWR);
	if (sCtx.device_fd < 0)
		sCtx.device_fd = open("/dev/graphics/intel_extreme/0", O_RDWR);
	if (sCtx.device_fd < 0) {
		LOG("cannot open GPU device\n");
		return B_ERROR;
	}

	// Clone shared_info (for graphics_memory pointer and GTT allocation)
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sCtx.device_fd, INTEL_GET_PRIVATE_DATA, &data,
			sizeof(data)) != 0) {
		LOG("INTEL_GET_PRIVATE_DATA failed\n");
		close(sCtx.device_fd);
		return B_ERROR;
	}

	sCtx.shared_area = clone_area("gpu_idct shared",
		(void**)&sCtx.shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sCtx.shared_area < 0) {
		LOG("clone shared_info failed\n");
		close(sCtx.device_fd);
		return B_ERROR;
	}

	sCtx.graphics_memory = (uint8*)sCtx.shared_info->graphics_memory;

	// Apply Gen5 workarounds (MI_MODE, CACHE_MODE_0, _3D_CHICKEN2).
	// CRITICAL: Without WaDisableRenderCachePipelinedFlush, MI_FLUSH
	// after media dispatch hangs the Command Streamer permanently.
	{
		intel_get_private_data init3d;
		init3d.magic = INTEL_PRIVATE_DATA_MAGIC;
		if (ioctl(sCtx.device_fd, INTEL_RING_INIT_3D, &init3d,
				sizeof(init3d)) == 0)
			LOG("3D workarounds applied (MI_MODE, CACHE_MODE_0)\n");
		else
			LOG("WARNING: INTEL_RING_INIT_3D failed\n");
	}

	// Phase 1: Init gpu_ring (syncs with HW TAIL, no reset)
	status_t st = gpu_ring_init(&sCtx.ring, sCtx.device_fd);
	if (st != B_OK) {
		LOG("gpu_ring_init failed\n");
		delete_area(sCtx.shared_area);
		close(sCtx.device_fd);
		return B_ERROR;
	}
	LOG("gpu_ring: pos=0x%x size=%u\n", sCtx.ring.pos, sCtx.ring.size);

	// Wait for ring to be idle before starting — previous users
	// (test tools, app_server) may have left pending commands.
	if (!gpu_ring_wait_idle(&sCtx.ring, 100000)) {
		uint32 head = gpu_ring_read_head(&sCtx.ring);
		LOG("ring not idle at init: HEAD=0x%x pos=0x%x (waiting...)\n",
			head, sCtx.ring.pos);
		// Try harder
		if (!gpu_ring_wait_idle(&sCtx.ring, 500000)) {
			LOG("ring stuck, syncing pos to HEAD\n");
			// Sync our pos to where HEAD actually is
			sCtx.ring.pos = gpu_ring_read_head(&sCtx.ring);
		}
	}

	// Allocate GTT buffers
	#define ALLOC_OR_FAIL(name, sz, align) \
		if (gpu_alloc(sz, align, sCtx.name) != B_OK) { \
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
	ALLOC_OR_FAIL(batch_base, 65536, 64);
	ALLOC_OR_FAIL(marker_base, 64, 64);  // QWord-aligned for MI_STORE_DATA_IMM
	#undef ALLOC_OR_FAIL

	// Upload kernel + setup state BEFORE ring test (for diagnostics)
	gtt_clear(sCtx.kernel_base, 4096);
	gtt_write(sCtx.kernel_base, 0, kIdctKernel, sizeof(kIdctKernel));
	setup_curbe();
	setup_surfaces();
	setup_vfe_state(1);
	setup_idrt(20);

	// Dump GTT diagnostics always (even if ring is dead)
	LOG("GTT offsets: kernel=0x%x curbe=0x%x input=0x%x output=0x%x\n",
		gtt_offset(sCtx.kernel_base), gtt_offset(sCtx.curbe_base),
		gtt_offset(sCtx.input_base), gtt_offset(sCtx.output_base));
	LOG("GTT offsets: vfe=0x%x idrt=0x%x ss=0x%x bt=0x%x marker=0x%x\n",
		gtt_offset(sCtx.vfe_state_base), gtt_offset(sCtx.idrt_base),
		gtt_offset(sCtx.surface_state_base), gtt_offset(sCtx.binding_table_base),
		gtt_offset(sCtx.marker_base));
	// Dump VFE state contents
	uint32* vfe = (uint32*)gtt_cpu(sCtx.vfe_state_base);
	LOG("VFE state: DW0=0x%x DW1=0x%x DW2=0x%x\n", vfe[0], vfe[1], vfe[2]);
	// Dump IDRT contents
	uint32* idrt = (uint32*)gtt_cpu(sCtx.idrt_base);
	LOG("IDRT: DW0=0x%x DW1=0x%x DW2=0x%x DW3=0x%x\n",
		idrt[0], idrt[1], idrt[2], idrt[3]);
	// Dump binding table
	uint32* bt = (uint32*)gtt_cpu(sCtx.binding_table_base);
	LOG("BT: [0]=0x%x [1]=0x%x\n", bt[0], bt[1]);
	// Dump surface state 0 (input)
	uint32* ss0 = (uint32*)gtt_cpu(sCtx.surface_state_base);
	LOG("SS0 (input):  DW0=0x%x DW1=0x%x DW2=0x%x DW3=0x%x\n",
		ss0[0], ss0[1], ss0[2], ss0[3]);
	uint32* ss1 = (uint32*)(gtt_cpu(sCtx.surface_state_base) + 32);
	LOG("SS1 (output): DW0=0x%x DW1=0x%x DW2=0x%x DW3=0x%x\n",
		ss1[0], ss1[1], ss1[2], ss1[3]);

	// Phase 1: Ring test
	if (!test_ring()) {
		LOG("PHASE 1 FAILED: ring is dead, GPU IDCT disabled\n");
		gpu_idct_uninit();
		return B_ERROR;
	}

	// Phase 2: Marker test
	if (!test_marker()) {
		LOG("PHASE 2 FAILED: marker write failed, GPU IDCT disabled\n");
		gpu_idct_uninit();
		return B_ERROR;
	}

	sCtx.initialized = true;
	LOG("initialized: kernel=%u bytes, max_batch=%d, ring OK, markers OK\n",
		(unsigned)sizeof(kIdctKernel), GPU_IDCT_MAX_BATCH);
	return B_OK;
}


void
gpu_idct_uninit(void)
{
	gpu_free(sCtx.kernel_base);
	gpu_free(sCtx.curbe_base);
	gpu_free(sCtx.input_base);
	gpu_free(sCtx.output_base);
	gpu_free(sCtx.vfe_state_base);
	gpu_free(sCtx.idrt_base);
	gpu_free(sCtx.surface_state_base);
	gpu_free(sCtx.binding_table_base);
	gpu_free(sCtx.batch_base);
	gpu_free(sCtx.marker_base);

	if (sCtx.shared_area >= 0)
		delete_area(sCtx.shared_area);
	if (sCtx.device_fd >= 0)
		close(sCtx.device_fd);

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

	// Upload input coefficients to GPU memory
	gtt_write(sCtx.input_base, 0, blocks, count * 128);

	// Clear output
	gtt_clear(sCtx.output_base, count * 128);

	// Reset marker
	*(volatile uint32*)gtt_cpu(sCtx.marker_base) = MARKER_SENTINEL;
	asm volatile("mfence" ::: "memory");

	// Reconfigure VFE state + IDRT to match actual block count.
	// MUST be done every submission — VFE num_urb_entries must match
	// URB_FENCE allocation, otherwise EU threads stall.
	uint32 max_threads = (count > 48) ? 48 : count;
	setup_vfe_state(max_threads);
	setup_idrt(20);
	asm volatile("mfence" ::: "memory");

	// Calculate ring space needed:
	// Preamble: ~30 DW, per-block: 20 DW, postamble: ~10 DW
	uint32 ring_dwords = 40 + count * 20 + 10;
	status_t st = gpu_ring_begin(&sCtx.ring, ring_dwords);
	if (st != B_OK)
		return st;

	// --- Media preamble (directly in ring, NOT in batch buffer) ---
	// This matches the working media_pipeline.cpp:submit_blocks_batch_gpu()

	gpu_ring_emit(&sCtx.ring, MI_FLUSH);

	// 3DSTATE_DEPTH_BUFFER null (6 DW)
	gpu_ring_emit(&sCtx.ring, CMD_3DSTATE_DEPTH_BUFFER);
	gpu_ring_emit(&sCtx.ring, (7 << 29) | (1 << 18));
	gpu_ring_emit(&sCtx.ring, 0);
	gpu_ring_emit(&sCtx.ring, 0);
	gpu_ring_emit(&sCtx.ring, 0);
	gpu_ring_emit(&sCtx.ring, 0);

	// PIPELINE_SELECT media
	gpu_ring_emit(&sCtx.ring, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);

	// STATE_BASE_ADDRESS Ironlake (8 DW)
	gpu_ring_emit(&sCtx.ring, CMD_STATE_BASE_ADDRESS | 6);
	for (int j = 0; j < 7; j++)
		gpu_ring_emit(&sCtx.ring, BASE_ADDRESS_MODIFY);

	// URB_FENCE (must match VFE num_urb_entries = max_threads)
	gpu_ring_emit(&sCtx.ring, CMD_URB_FENCE | UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
	gpu_ring_emit(&sCtx.ring, 0);
	gpu_ring_emit(&sCtx.ring, (max_threads << 10) | ((max_threads + 16) << 20));

	// MEDIA_STATE_POINTERS
	gpu_ring_emit(&sCtx.ring, CMD_MEDIA_STATE_POINTERS | 1);
	gpu_ring_emit(&sCtx.ring, 0);
	gpu_ring_emit(&sCtx.ring, gtt_offset(sCtx.vfe_state_base));

	// CS_URB_STATE
	gpu_ring_emit(&sCtx.ring, CMD_CS_URB_STATE | 0);
	gpu_ring_emit(&sCtx.ring, ((16 - 1) << 4) | 1);

	// CONSTANT_BUFFER: 10 units of 64 bytes = 640 bytes = 20 GRFs
	gpu_ring_emit(&sCtx.ring, CMD_CONSTANT_BUFFER | (1u << 8) | 0);
	gpu_ring_emit(&sCtx.ring, gtt_offset(sCtx.curbe_base) | ((10 - 1) & 0x1f));

	// --- N x MEDIA_OBJECT (20 DW each) ---
	for (uint32 i = 0; i < count; i++) {
		uint32 byte_offset = i * 128;
		gpu_ring_emit(&sCtx.ring, CMD_MEDIA_OBJECT | 18);
		gpu_ring_emit(&sCtx.ring, 0);  // interface descriptor index
		gpu_ring_emit(&sCtx.ring, 0);  // indirect data length
		gpu_ring_emit(&sCtx.ring, 0);  // indirect data pointer
		gpu_ring_emit(&sCtx.ring, byte_offset);  // inline DW0: input offset
		gpu_ring_emit(&sCtx.ring, byte_offset);  // inline DW1: output offset
		for (int p = 2; p < 16; p++)
			gpu_ring_emit(&sCtx.ring, 0);
	}

	// --- Completion marker ---
	gpu_ring_emit(&sCtx.ring, MI_STORE_DATA_IMM_GGTT);
	gpu_ring_emit(&sCtx.ring, 0);
	gpu_ring_emit(&sCtx.ring, gtt_offset(sCtx.marker_base));
	gpu_ring_emit(&sCtx.ring, MARKER_TAG);

	// Final flush
	gpu_ring_emit(&sCtx.ring, MI_FLUSH);

	// Kick TAIL — single submission, all commands at once
	st = gpu_ring_advance(&sCtx.ring);
	if (st != B_OK)
		return st;

	// Wait for marker (GPU writes tag when batch completes)
	uint32 timeout = 500000 + count * 5000;
	if (!wait_marker(timeout)) {
		uint32 marker_val = *(volatile uint32*)gtt_cpu(sCtx.marker_base);
		uint32 head = gpu_ring_read_head(&sCtx.ring);
		LOG("GPU timeout: %u blocks, marker=0x%x (want 0x%x), HEAD=0x%x\n",
			count, marker_val, MARKER_TAG, head);
		return B_TIMED_OUT;
	}

	// Read back IDCT results
	memcpy(output, gtt_cpu(sCtx.output_base), count * 128);

	return B_OK;
}
