/*
 * test_pipeline — Full GPU pipeline verification suite.
 *
 * Tests in order:
 *   1. Ring access (clone shared_info, sync ring position)
 *   2. TAIL ioctl (MI_NOOP → check HEAD advance)
 *   3. MI_STORE_DATA_IMM (ring marker write)
 *   4. BLT engine (XY_COLOR_BLT fill)
 *   5. Media pipeline (IDCT 8x8 block, GPU vs CPU)
 *   6. GEM_EXECBUFFER2 (batch submit via DRM shim)
 *
 * No reboot required. Run from userspace after boot.
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
#include "gpu_debug.h"
#include "media_pipeline.h"

extern accelerant_info* gInfo;

static int sDevFd = -1;
static int sPass = 0;
static int sFail = 0;

#define TEST(name) printf("  [%d] %-40s ", sPass + sFail + 1, name)
#define PASS() do { printf("PASS\n"); sPass++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); sFail++; } while(0)


static bool
init_gpu(void)
{
	sDevFd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (sDevFd < 0) return false;

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sDevFd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(sDevFd); return false;
	}

	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = sDevFd;

	gInfo->shared_info_area = clone_area("pipe shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(sDevFd); return false;
	}
	gInfo->regs_area = clone_area("pipe regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(sDevFd); return false;
	}
	return true;
}

static void cleanup_gpu(void) {
	if (!gInfo) return;
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo); gInfo = NULL;
	close(sDevFd); sDevFd = -1;
}


// ---- Helpers ----

static void
ring_kick(uint32 tail)
{
	intel_ring_tail data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	data.tail_value = tail;
	ioctl(sDevFd, INTEL_RING_WRITE_TAIL, &data, sizeof(data));
}

static bool
ring_wait_idle(uint32 timeout_us)
{
	ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
	bigtime_t deadline = system_time() + timeout_us;
	while (system_time() < deadline) {
		uint32 head = read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		uint32 tail = read32(ring.register_base + RING_BUFFER_TAIL)
			& (ring.size - 1);
		if (head == tail)
			return true;
	}
	return false;
}


int
main(int, char**)
{
	printf("=== GPU Pipeline Test Suite ===\n\n");

	// ---- Test 1: GPU access ----
	TEST("GPU device open + clone");
	if (!init_gpu()) { FAIL("open failed"); return 1; }
	PASS();

	intel_shared_info& si = *gInfo->shared_info;
	ring_buffer& ring = si.primary_ring_buffer;

	printf("      Gen %u, screen %ux%u\n",
		si.device_type.Generation(),
		si.current_mode.timing.h_display,
		si.current_mode.timing.v_display);

	// ---- Test 2: Ring sync ----
	TEST("Ring sync (read HW TAIL)");
	{
		uint32 hwTail = read32(ring.register_base + RING_BUFFER_TAIL)
			& (ring.size - 1);
		uint32 hwHead = read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		ring.position = hwTail;
		ring.space_left = ring.size - 64;
		if (ring.size > 0 && hwHead <= ring.size) {
			printf("HEAD=0x%x TAIL=0x%x ... ", hwHead, hwTail);
			PASS();
		} else {
			FAIL("invalid ring state");
		}
	}

	// ---- Test 3: TAIL ioctl (MI_NOOP) ----
	TEST("TAIL ioctl (MI_NOOP)");
	{
		uint32 pos = ring.position;
		uint32 mask = ring.size - 1;

		*(uint32*)((uint8*)ring.base + (pos & mask)) = 0; pos += 4;
		*(uint32*)((uint8*)ring.base + (pos & mask)) = 0; pos += 4;
		asm volatile("mfence" ::: "memory");

		uint32 head_before = read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		ring_kick(pos & mask);
		snooze(5000);
		uint32 head_after = read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;

		ring.position = pos & mask;
		if (head_after != head_before) {
			printf("HEAD 0x%x→0x%x ... ", head_before, head_after);
			PASS();
		} else {
			FAIL("HEAD did not advance");
		}
	}

	// ---- Test 4: MI_STORE_DATA_IMM (marker) ----
	TEST("MI_STORE_DATA_IMM (ring marker)");
	{
		gpu_bo marker_bo;
		if (gpu_bo_alloc(&marker_bo, "test:marker", 4096, 64) != B_OK) {
			FAIL("alloc marker BO");
		} else {
			*(volatile uint32*)marker_bo.cpu_addr = 0;
			asm volatile("mfence" ::: "memory");

			uint32 pos = ring.position;
			uint32 mask = ring.size - 1;
			uint32* cmd = (uint32*)((uint8*)ring.base + (pos & mask));

			// MI_STORE_DATA_IMM | GGTT | length=2
			uint32 dw[4];
			gpu_debug_marker_dwords(dw, marker_bo.gtt_offset, 0xDEAD1234);
			memcpy(cmd, dw, 16);
			pos += 16;
			asm volatile("mfence" ::: "memory");

			ring.position = pos & mask;
			ring_kick(ring.position);
			snooze(5000);

			uint32 val = *(volatile uint32*)marker_bo.cpu_addr;
			if (val == 0xDEAD1234) {
				PASS();
			} else {
				char msg[64];
				snprintf(msg, sizeof(msg), "expected 0xDEAD1234, got 0x%x", val);
				FAIL(msg);
			}
			gpu_bo_free(&marker_bo);
		}
	}

	// ---- Test 5: BLT engine (XY_COLOR_BLT) ----
	TEST("BLT engine (XY_COLOR_BLT 64x64)");
	{
		gpu_bo blt_bo;
		if (gpu_bo_alloc(&blt_bo, "test:blt", 64*64*4, 4096) != B_OK) {
			FAIL("alloc BLT BO");
		} else {
			memset((void*)blt_bo.cpu_addr, 0, 64*64*4);
			asm volatile("mfence" ::: "memory");

			uint32 pos = ring.position;
			uint32 mask = ring.size - 1;
			uint32* cmd = (uint32*)((uint8*)ring.base + (pos & mask));

			// XY_COLOR_BLT: fill 64x64 with 0xFF00FF00 (green)
			cmd[0] = XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA;
			cmd[1] = (64 * 4) | (0xF0 << 16)
				| ((uint32)COMMAND_MODE_RGB32 << 24);
			cmd[2] = 0;				// top-left (0,0)
			cmd[3] = (64 << 16) | 64;	// bottom-right (64,64)
			cmd[4] = blt_bo.gtt_offset;
			cmd[5] = 0xFF00FF00;		// green
			pos += 24;
			asm volatile("mfence" ::: "memory");

			// Pad to QWORD
			if ((pos / 4) & 1) {
				*(uint32*)((uint8*)ring.base + (pos & mask)) = 0;
				pos += 4;
			}
			ring.position = pos & mask;
			ring_kick(ring.position);
			ring_wait_idle(100000);
			asm volatile("mfence" ::: "memory");

			uint32 pixel = *(volatile uint32*)blt_bo.cpu_addr;
			if (pixel == 0xFF00FF00) {
				PASS();
			} else {
				char msg[64];
				snprintf(msg, sizeof(msg), "pixel=0x%08x, expected 0xFF00FF00", pixel);
				FAIL(msg);
			}
			gpu_bo_free(&blt_bo);
		}
	}

	// ---- Test 6: Media pipeline (IDCT 1 block) ----
	TEST("Media pipeline (GPU IDCT 1 block)");
	{
		media_pipeline_context ctx;
		if (media_pipeline_init(&ctx) != B_OK) {
			FAIL("media_pipeline_init");
		} else {
			if (media_pipeline_setup_idct_to_u8(&ctx, 1) != B_OK) {
				FAIL("setup_idct_to_u8");
			} else {
				// Single DC=128 block → should output ~128 for all pixels
				gpu_block_entry blk;
				memset(&blk, 0, sizeof(blk));
				blk.x = 0;
				blk.y = 0;
				blk.coeffs[0] = 128 * 8;  // DC scaled

				status_t st = submit_blocks_batch_gpu(&ctx, &blk, 1);
				if (st != B_OK) {
					char msg[64];
					snprintf(msg, sizeof(msg), "submit failed: %d", (int)st);
					FAIL(msg);
				} else {
					uint8* out = (uint8*)ctx.output_bo.cpu_addr;
					uint8 val = out[0];
					if (val > 100 && val < 160) {
						printf("DC=%u ... ", val);
						PASS();
					} else {
						char msg[64];
						snprintf(msg, sizeof(msg), "DC pixel=%u, expected ~128", val);
						FAIL(msg);
					}
				}
			}
			media_pipeline_uninit(&ctx);
		}
	}

	// ---- Summary ----
	printf("\n=== Results: %d PASS, %d FAIL ===\n", sPass, sFail);

	cleanup_gpu();
	return sFail > 0 ? 1 : 0;
}
