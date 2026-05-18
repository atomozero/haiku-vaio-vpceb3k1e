/*
 * test_3d_pipeline — Systematic Gen5 (Ironlake) 3D pipeline test suite.
 *
 * Tests each 3D pipeline component via BOTH:
 *   A) Direct ring emission + marker
 *   B) Batch buffer (MI_BATCH_BUFFER_START) + ring marker
 *
 * Key finding: "3D commands MUST go through batch buffers, not directly
 * in the ring" (render.cpp:657). This test verifies that hypothesis.
 *
 * Ring commands submitted via INTEL_RING_WRITE_TAIL kernel ioctl.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>

#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "gpu_bo.h"

extern accelerant_info* gInfo;

static int sDeviceFd = -1;
static uint32 sRingPos = 0;
static volatile uint32* sMarker = NULL;
static uint32 sMarkerGtt = 0;
static uint32 sSeq = 0;
static gpu_bo sStateBo;   // 16KB for pipeline state structures
static gpu_bo sBatchBo;   // 4KB for batch buffer
static int sPassCount = 0;
static int sFailCount = 0;


// ---- Init ----

static bool
init_gpu(void)
{
	sDeviceFd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (sDeviceFd < 0) return false;
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sDeviceFd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(sDeviceFd); return false;
	}
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = sDeviceFd;
	gInfo->shared_info_area = clone_area("test shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(sDeviceFd); return false;
	}
	gInfo->regs_area = clone_area("test regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(sDeviceFd); return false;
	}

	// Sync ring
	ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
	uint32 hwTail = read32(ring.register_base + RING_BUFFER_TAIL)
		& (ring.size - 1);
	ring.position = hwTail;
	sRingPos = hwTail;

	// Allocate BOs
	gpu_bo marker_bo;
	if (gpu_bo_alloc(&marker_bo, "t:marker", 4096, 4096) != B_OK) return false;
	sMarker = (volatile uint32*)marker_bo.cpu_addr;
	sMarkerGtt = marker_bo.gtt_offset;
	*sMarker = 0;

	if (gpu_bo_alloc(&sStateBo, "t:state", 16384, 4096) != B_OK) return false;
	memset((void*)sStateBo.cpu_addr, 0, 16384);

	if (gpu_bo_alloc(&sBatchBo, "t:batch", 4096, 4096) != B_OK) return false;
	memset((void*)sBatchBo.cpu_addr, 0, 4096);

	return true;
}


// ---- Ring writer ----

static uint32* sRingBase;
static uint32 sRingMask;
static uint32 sWP;  // write position in DWORDs

static void ring_begin(void) {
	ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
	sRingBase = (uint32*)ring.base;
	sRingMask = (ring.size / 4) - 1;
	sWP = sRingPos / 4;
}

static void ring_emit(uint32 dw) {
	sRingBase[sWP & sRingMask] = dw;
	sWP++;
}

static bool ring_kick_and_wait(uint32 timeout_us) {
	uint32 seq = ++sSeq;
	*sMarker = 0;

	// Completion marker
	ring_emit((0x20 << 23) | (1 << 22) | 2);  // MI_STORE_DATA_IMM GGTT
	ring_emit(0);
	ring_emit(sMarkerGtt);
	ring_emit(seq);
	if (sWP & 1) ring_emit(0);

	asm volatile("mfence" ::: "memory");
	sRingPos = (sWP * 4) & (gInfo->shared_info->primary_ring_buffer.size - 1);

	intel_ring_tail td;
	td.magic = INTEL_PRIVATE_DATA_MAGIC;
	td.tail_value = sRingPos;
	ioctl(sDeviceFd, INTEL_RING_WRITE_TAIL, &td, sizeof(td));

	bigtime_t deadline = system_time() + timeout_us;
	while (*sMarker != seq) {
		if (system_time() > deadline) {
			ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
			uint32 head = read32(ring.register_base + RING_BUFFER_HEAD)
				& INTEL_RING_BUFFER_HEAD_MASK;
			printf("    TIMEOUT HEAD=0x%x TAIL=0x%x IPEHR=0x%08x\n",
				head, sRingPos, read32(0x2068));
			return false;
		}
	}
	return true;
}


// ---- Batch buffer writer ----

static uint32* sBatchPtr;
static uint32 sBatchPos;  // in DWORDs

static void batch_begin(void) {
	sBatchPtr = (uint32*)sBatchBo.cpu_addr;
	sBatchPos = 0;
}

static void batch_emit(uint32 dw) {
	sBatchPtr[sBatchPos++] = dw;
}

static void batch_end(void) {
	batch_emit(0x0A000000);  // MI_BATCH_BUFFER_END
	if (sBatchPos & 1) batch_emit(0);
	asm volatile("mfence" ::: "memory");
}

// Submit batch via ring: MI_BATCH_BUFFER_START + completion marker
static bool batch_submit(uint32 timeout_us) {
	ring_begin();
	ring_emit(0x18800000);  // MI_BATCH_BUFFER_START
	ring_emit(sBatchBo.gtt_offset);
	return ring_kick_and_wait(timeout_us);
}


// ---- Test runner ----

static void
run_ring_test(const char* name, void (*emit)(void), uint32 timeout_us)
{
	ring_begin();
	if (emit) emit();
	bool ok = ring_kick_and_wait(timeout_us);
	printf("  [RING]  %-45s %s\n", name, ok ? "PASS" : "*** FAIL ***");
	if (ok) sPassCount++; else sFailCount++;
}

static void
run_batch_test(const char* name, void (*emit)(void), uint32 timeout_us)
{
	batch_begin();
	if (emit) emit();
	batch_end();
	bool ok = batch_submit(timeout_us);
	printf("  [BATCH] %-45s %s\n", name, ok ? "PASS" : "*** FAIL ***");
	if (ok) sPassCount++; else sFailCount++;
}


// ==== Test definitions ====

// MI commands
#define MI_NOOP       0x00000000
#define MI_FLUSH_CMD  (0x04 << 23)

static void emit_noop(void) {
	ring_emit(MI_NOOP); ring_emit(MI_NOOP);
}
static void emit_mi_flush(void) {
	ring_emit(MI_FLUSH_CMD); ring_emit(MI_NOOP);
}

static void batch_noop(void) {
	batch_emit(MI_NOOP); batch_emit(MI_NOOP);
}
static void batch_mi_flush(void) {
	batch_emit(MI_FLUSH_CMD); batch_emit(MI_NOOP);
}

// PIPE_CONTROL
#define PIPE_CONTROL_CMD  0x7A000002
#define PC_CS_STALL       (1 << 20)
#define PC_DEPTH_STALL    (1 << 13)
#define PC_WRITE_IMM      (1 << 14)

static void emit_pipe_control_stall(void) {
	ring_emit(PIPE_CONTROL_CMD);
	ring_emit(PC_CS_STALL);
	ring_emit(0); ring_emit(0);
}
static void batch_pipe_control_stall(void) {
	batch_emit(PIPE_CONTROL_CMD);
	batch_emit(PC_CS_STALL);
	batch_emit(0); batch_emit(0);
}
static void batch_pipe_control_depth_stall(void) {
	batch_emit(PIPE_CONTROL_CMD);
	batch_emit(PC_DEPTH_STALL);
	batch_emit(0); batch_emit(0);
}

// PIPELINE_SELECT
#define CMD_PIPELINE_SELECT  0x69040000

static void emit_pipeline_select_3d(void) {
	ring_emit(CMD_PIPELINE_SELECT);  // select 3D (bit 0 = 0)
	ring_emit(MI_NOOP);
}
static void batch_pipeline_select_3d(void) {
	batch_emit(CMD_PIPELINE_SELECT);
	batch_emit(MI_NOOP);
}

// STATE_BASE_ADDRESS (8 DW)
static void emit_state_base_address(void) {
	ring_emit(0x61010006);
	ring_emit(0x00000001);
	ring_emit(sStateBo.gtt_offset | 1);
	ring_emit(0x00000001);
	ring_emit(0x00000001);
	ring_emit(0xfffff001);
	ring_emit(0x00000001);
	ring_emit(0x00000001);
}
static void batch_state_base_address(void) {
	batch_emit(0x61010006);
	batch_emit(0x00000001);
	batch_emit(sStateBo.gtt_offset | 1);
	batch_emit(0x00000001);
	batch_emit(0x00000001);
	batch_emit(0xfffff001);
	batch_emit(0x00000001);
	batch_emit(0x00000001);
}

// DRAWING_RECTANGLE (4 DW)
static void emit_drawing_rect(void) {
	ring_emit(0x79000002);
	ring_emit(0x00000000);
	ring_emit(0x00c7012b);  // 300x200
	ring_emit(0x00000000);
}
static void batch_drawing_rect(void) {
	batch_emit(0x79000002);
	batch_emit(0x00000000);
	batch_emit(0x00c7012b);
	batch_emit(0x00000000);
}

// 3DSTATE_DEPTH_BUFFER null (6 DW)
static void emit_depth_null(void) {
	ring_emit(0x79050004);
	ring_emit(0xe0040000);
	ring_emit(0); ring_emit(0); ring_emit(0); ring_emit(0);
}
static void batch_depth_null(void) {
	batch_emit(0x79050004);
	batch_emit(0xe0040000);
	batch_emit(0); batch_emit(0); batch_emit(0); batch_emit(0);
}

// URB_FENCE (3 DW) — from Mesa crocus batch
static void emit_urb_fence(void) {
	ring_emit(0x60003f01);
	ring_emit(0x09222080);
	ring_emit(0x40000152);
}
static void batch_urb_fence(void) {
	batch_emit(0x60003f01);
	batch_emit(0x09222080);
	batch_emit(0x40000152);
}

// CS_URB_STATE + CONSTANT_BUFFER (4 DW total)
static void emit_cs_urb(void) {
	ring_emit(0x60010000); ring_emit(0);  // CS_URB_STATE
	ring_emit(0x60020000); ring_emit(0);  // CONSTANT_BUFFER
}
static void batch_cs_urb(void) {
	batch_emit(0x60010000); batch_emit(0);
	batch_emit(0x60020000); batch_emit(0);
}

// VF_STATISTICS (1 DW)
static void emit_vf_stats(void) {
	ring_emit(0x680b0000);
	ring_emit(MI_NOOP);
}
static void batch_vf_stats(void) {
	batch_emit(0x680b0000);
}

// PIPELINED_POINTERS (7 DW) — null states in sStateBo
static void emit_pipelined_ptrs(void) {
	ring_emit(0x78000005);
	ring_emit(sStateBo.gtt_offset + 0x000);  // VS
	ring_emit(0);                              // GS disabled
	ring_emit(0);                              // CLIP disabled
	ring_emit(sStateBo.gtt_offset + 0x100);  // SF
	ring_emit(sStateBo.gtt_offset + 0x200);  // WM
	ring_emit(sStateBo.gtt_offset + 0x300);  // CC
}
static void batch_pipelined_ptrs(void) {
	batch_emit(0x78000005);
	batch_emit(sStateBo.gtt_offset + 0x000);
	batch_emit(0); batch_emit(0);
	batch_emit(sStateBo.gtt_offset + 0x100);
	batch_emit(sStateBo.gtt_offset + 0x200);
	batch_emit(sStateBo.gtt_offset + 0x300);
}

// BINDING_TABLE_POINTERS (6 DW)
static void emit_binding_ptrs(void) {
	ring_emit(0x78010004);
	ring_emit(0); ring_emit(0); ring_emit(0); ring_emit(0); ring_emit(0);
}
static void batch_binding_ptrs(void) {
	batch_emit(0x78010004);
	batch_emit(0); batch_emit(0); batch_emit(0); batch_emit(0); batch_emit(0);
}

// ---- Combo tests ----

// SBA + MI_FLUSH (ring)
static void emit_sba_flush(void) {
	emit_state_base_address();
	ring_emit(MI_FLUSH_CMD);
	ring_emit(MI_NOOP);
}

// SBA + MI_FLUSH (batch)
static void batch_sba_flush(void) {
	batch_state_base_address();
	batch_emit(MI_FLUSH_CMD);
}

// Full Mesa-like state setup (batch) — NO primitive
static void batch_full_state_no_prim(void) {
	batch_drawing_rect();
	batch_state_base_address();
	batch_urb_fence();
	batch_cs_urb();
	batch_vf_stats();
	batch_pipelined_ptrs();
	batch_binding_ptrs();
	batch_depth_null();
}

// Full state + MI_FLUSH at the end (batch)
static void batch_full_state_mi_flush(void) {
	batch_full_state_no_prim();
	batch_emit(MI_FLUSH_CMD);
}

// Full state + PIPE_CONTROL instead of MI_FLUSH
static void batch_full_state_pipe_control(void) {
	batch_full_state_no_prim();
	batch_emit(PIPE_CONTROL_CMD);
	batch_emit(PC_CS_STALL | PC_DEPTH_STALL);
	batch_emit(0); batch_emit(0);
}

// Replicate the exact Mesa crocus batch #2 (65 DW)
static void batch_mesa_exact(void) {
	// Exact commands from gl_test EXECBUF2 #2
	batch_emit(0x79000002); batch_emit(0x00000000);
	batch_emit(0x00c7012b); batch_emit(0x00000000);
	batch_emit(0x61010006); batch_emit(0x00000001);
	batch_emit(sStateBo.gtt_offset | 1); batch_emit(0x00000001);
	batch_emit(0x00000001); batch_emit(0xfffff001);
	batch_emit(0x00000001); batch_emit(0x00000001);
	batch_emit(0x78080007); batch_emit(0x0000000c);
	batch_emit(sStateBo.gtt_offset + 0x040);
	batch_emit(sStateBo.gtt_offset + 0x063);
	batch_emit(0x00000000); batch_emit(0x0c000000);
	batch_emit(sStateBo.gtt_offset + 0x080);
	batch_emit(sStateBo.gtt_offset + 0x0af);
	batch_emit(0x00000000);
	batch_emit(0x78090009); batch_emit(0x0c000000);
	batch_emit(0x16220000); batch_emit(0x04400000);
	batch_emit(0x11130004); batch_emit(0x04400000);
	batch_emit(0x11130008); batch_emit(0x0c000010);
	batch_emit(0x1111000c); batch_emit(0x0c000020);
	batch_emit(0x11110010);
	batch_emit(0x680b0000);
	batch_emit(0x78000005);
	batch_emit(sStateBo.gtt_offset + 0x0c0);
	batch_emit(0x00000000); batch_emit(0x00000000);
	batch_emit(sStateBo.gtt_offset + 0x100);
	batch_emit(sStateBo.gtt_offset + 0x140);
	batch_emit(sStateBo.gtt_offset + 0x1c0);
	batch_emit(0x60003f01); batch_emit(0x09222080);
	batch_emit(0x40000152);
	batch_emit(0x60010000); batch_emit(0x00000000);
	batch_emit(0x60020000); batch_emit(0x00000000);
	batch_emit(0x78010004);
	batch_emit(0x00000000); batch_emit(0x00000000);
	batch_emit(0x00000000); batch_emit(0x00000000);
	batch_emit(0x000001e0);
	batch_emit(0x79050004); batch_emit(0xe0040000);
	batch_emit(0x00000000); batch_emit(0x00000000);
	batch_emit(0x00000000); batch_emit(0x00000000);
	// 3DPRIMITIVE: RECTLIST, 3 verts
	batch_emit(0x7b003c04);
	batch_emit(0x00000003); batch_emit(0x00000000);
	batch_emit(0x00000001); batch_emit(0x00000000);
	batch_emit(0x00000000);
}


int
main(int, char**)
{
	printf("=== Gen5 3D Pipeline Test Suite ===\n\n");

	if (!init_gpu()) { printf("GPU init failed\n"); return 1; }

	ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
	printf("Ring: HEAD=0x%x TAIL=0x%x pos=%u\n",
		read32(ring.register_base + RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK,
		read32(ring.register_base + RING_BUFFER_TAIL) & (ring.size - 1),
		sRingPos);
	printf("State BO: GTT=0x%x  Batch BO: GTT=0x%x  Marker: GTT=0x%x\n\n",
		sStateBo.gtt_offset, sBatchBo.gtt_offset, sMarkerGtt);

	// ============================================================
	printf("--- Section 1: MI Commands (Ring vs Batch) ---\n");
	run_ring_test("MI_NOOP", emit_noop, 100000);
	run_batch_test("MI_NOOP", batch_noop, 100000);
	run_ring_test("MI_FLUSH", emit_mi_flush, 200000);
	run_batch_test("MI_FLUSH", batch_mi_flush, 200000);

	// ============================================================
	printf("\n--- Section 2: PIPE_CONTROL (Ring vs Batch) ---\n");
	run_ring_test("PIPE_CONTROL (CS stall)", emit_pipe_control_stall, 200000);
	run_batch_test("PIPE_CONTROL (CS stall)", batch_pipe_control_stall, 200000);
	run_batch_test("PIPE_CONTROL (depth stall)", batch_pipe_control_depth_stall, 200000);

	// ============================================================
	printf("\n--- Section 3: Pipeline Select ---\n");
	run_ring_test("PIPELINE_SELECT(3D)", emit_pipeline_select_3d, 200000);
	run_batch_test("PIPELINE_SELECT(3D)", batch_pipeline_select_3d, 200000);

	// ============================================================
	printf("\n--- Section 4: State Commands (Ring direct) ---\n");
	run_ring_test("STATE_BASE_ADDRESS", NULL, 100000);  // just marker
	run_ring_test("SBA + MI_FLUSH", emit_sba_flush, 200000);
	run_ring_test("DRAWING_RECTANGLE", emit_drawing_rect, 100000);
	run_ring_test("DEPTH_BUFFER(null)", emit_depth_null, 100000);
	run_ring_test("URB_FENCE", emit_urb_fence, 100000);
	run_ring_test("CS_URB + CONST_BUF", emit_cs_urb, 100000);
	run_ring_test("VF_STATISTICS", emit_vf_stats, 100000);
	run_ring_test("PIPELINED_POINTERS", emit_pipelined_ptrs, 200000);
	run_ring_test("BINDING_TABLE_PTRS", emit_binding_ptrs, 100000);

	// ============================================================
	printf("\n--- Section 5: State Commands (Batch buffer) ---\n");
	run_batch_test("SBA only", batch_state_base_address, 200000);
	run_batch_test("SBA + MI_FLUSH", batch_sba_flush, 200000);
	run_batch_test("DRAWING_RECT", batch_drawing_rect, 200000);
	run_batch_test("DEPTH_BUFFER(null)", batch_depth_null, 200000);
	run_batch_test("URB_FENCE", batch_urb_fence, 200000);
	run_batch_test("CS_URB + CONST_BUF", batch_cs_urb, 200000);
	run_batch_test("PIPELINED_POINTERS", batch_pipelined_ptrs, 200000);
	run_batch_test("BINDING_TABLE_PTRS", batch_binding_ptrs, 200000);

	// ============================================================
	printf("\n--- Section 6: Combined State (Batch) ---\n");
	run_batch_test("Full state, NO flush/prim", batch_full_state_no_prim, 200000);
	run_batch_test("Full state + MI_FLUSH", batch_full_state_mi_flush, 500000);
	run_batch_test("Full state + PIPE_CONTROL", batch_full_state_pipe_control, 500000);

	// ============================================================
	printf("\n--- Section 7: Exact Mesa Crocus Batch ---\n");
	run_batch_test("Mesa batch #2 exact (65 DW)", batch_mesa_exact, 500000);

	// ============================================================
	printf("\n=== Results: %d PASS, %d FAIL ===\n", sPassCount, sFailCount);

	gpu_bo_free(&sStateBo);
	gpu_bo_free(&sBatchBo);
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo);
	close(sDeviceFd);
	return 0;
}
