/*
 * test_2d_blt — Test and benchmark 2D BLT acceleration via accelerant API.
 *
 * Exercises the same code paths app_server uses:
 *   intel_fill_rectangle (XY_COLOR_BLT)
 *   intel_screen_to_screen_blit (XY_SRC_COPY_BLT)
 *   intel_wait_engine_idle
 *
 * These go through QueueCommands which now kicks TAIL via kernel ioctl.
 *
 * Build (from tests/ after 'make' in accelerant/):
 *   g++ -Wall -O2 -I.. [include flags] -o test_2d_blt test_2d_blt.cpp \
 *       ../*.o ../../libaccelerantscommon.a -lbe -lstdc++
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
#include "accelerant_protos.h"
#include "commands.h"

extern accelerant_info* gInfo;

static engine_token sToken = { 1, B_2D_ACCELERATION, NULL };


static bool
init_gpu(void)
{
	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) return false;
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(fd); return false;
	}
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;
	gInfo->shared_info_area = clone_area("blt shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(fd); return false;
	}
	gInfo->regs_area = clone_area("blt regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(fd); return false;
	}
	return true;
}

static void cleanup_gpu(void) {
	if (!gInfo) return;
	int fd = gInfo->device;
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo); gInfo = NULL;
	close(fd);
}


int
main(int, char**)
{
	printf("=== 2D BLT Acceleration Test & Benchmark ===\n\n");

	if (!init_gpu()) { printf("GPU init failed\n"); return 1; }

	intel_shared_info& si = *gInfo->shared_info;
	uint32 scr_w = si.current_mode.timing.h_display;
	uint32 scr_h = si.current_mode.timing.v_display;
	uint32 bpp = si.bits_per_pixel;
	printf("Screen: %ux%u %ubpp, bpr=%u, fb_offset=0x%x\n",
		scr_w, scr_h, bpp, si.bytes_per_row,
		(unsigned)si.frame_buffer_offset);

	ring_buffer& ring = si.primary_ring_buffer;
	uint32 head0 = read32(ring.register_base + RING_BUFFER_HEAD)
		& INTEL_RING_BUFFER_HEAD_MASK;
	printf("Ring: HEAD=0x%x TAIL=0x%x pos=%u\n\n",
		head0,
		read32(ring.register_base + RING_BUFFER_TAIL) & (ring.size - 1),
		ring.position);

	// ---- Test 1: XY_COLOR_BLT (fill rectangle) ----
	printf("--- Test 1: XY_COLOR_BLT (fill_rectangle) ---\n");
	{
		// Fill a red rectangle at top-left
		fill_rect_params rect;
		rect.left = 50;
		rect.top = 50;
		rect.right = 249;
		rect.bottom = 149;

		printf("Filling 200x100 red rect at (50,50)...\n");
		intel_fill_rectangle(&sToken, 0xFFFF0000, &rect, 1);

		// Wait for GPU
		intel_wait_engine_idle();

		uint32 head1 = read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		printf("HEAD: 0x%x → 0x%x (%s)\n\n",
			head0, head1,
			head1 != head0 ? "ADVANCED — GPU executed!" : "STUCK");
		head0 = head1;
	}

	// Fill more colors to make a pattern
	{
		fill_rect_params rects[3];
		// Green
		rects[0].left = 260; rects[0].top = 50;
		rects[0].right = 459; rects[0].bottom = 149;
		// Blue
		rects[1].left = 50; rects[1].top = 160;
		rects[1].right = 249; rects[1].bottom = 259;
		// Yellow
		rects[2].left = 260; rects[2].top = 160;
		rects[2].right = 459; rects[2].bottom = 259;

		intel_fill_rectangle(&sToken, 0xFF00FF00, &rects[0], 1);
		intel_fill_rectangle(&sToken, 0xFF0000FF, &rects[1], 1);
		intel_fill_rectangle(&sToken, 0xFFFFFF00, &rects[2], 1);
		intel_wait_engine_idle();
		printf("4 colored rects drawn. Check top-left of screen!\n");
		snooze(2000000);
	}

	// ---- Test 2: XY_SRC_COPY_BLT (screen-to-screen blit) ----
	printf("\n--- Test 2: XY_SRC_COPY_BLT (screen_to_screen_blit) ---\n");
	{
		// Copy the 4-rect pattern to another location
		blit_params bp;
		bp.src_left = 50;
		bp.src_top = 50;
		bp.dest_left = 500;
		bp.dest_top = 50;
		bp.width = 409;    // 460 - 50 - 1
		bp.height = 209;   // 260 - 50 - 1

		printf("Copying 410x210 from (50,50) to (500,50)...\n");
		intel_screen_to_screen_blit(&sToken, &bp, 1);
		intel_wait_engine_idle();

		uint32 head2 = read32(ring.register_base + RING_BUFFER_HEAD)
			& INTEL_RING_BUFFER_HEAD_MASK;
		printf("HEAD: 0x%x → 0x%x (%s)\n",
			head0, head2,
			head2 != head0 ? "ADVANCED — blit worked!" : "STUCK");
		head0 = head2;
		printf("Check screen: pattern should be duplicated!\n");
		snooze(2000000);
	}

	// ---- Benchmark: fill_rectangle throughput ----
	printf("\n--- Benchmark: XY_COLOR_BLT throughput ---\n");
	{
		const uint32 ITERS = 1000;
		fill_rect_params rect;
		rect.left = 50;
		rect.top = 300;
		rect.right = 249;   // 200x100
		rect.bottom = 399;

		// Warm up
		for (uint32 i = 0; i < 10; i++) {
			intel_fill_rectangle(&sToken, 0xFF000000 | (i * 25),
				&rect, 1);
		}
		intel_wait_engine_idle();

		// Timed run
		bigtime_t t0 = system_time();
		for (uint32 i = 0; i < ITERS; i++) {
			uint32 color = 0xFF000000 | ((i & 0xFF) << 16)
				| (((i >> 3) & 0xFF) << 8);
			intel_fill_rectangle(&sToken, color, &rect, 1);
		}
		intel_wait_engine_idle();
		bigtime_t elapsed = system_time() - t0;

		float fills_per_sec = (float)ITERS * 1e6f / (float)elapsed;
		float mpix_per_sec = fills_per_sec * 200 * 100 / 1e6f;
		printf("  %u fills of 200x100 in %ld us\n", ITERS, (long)elapsed);
		printf("  %.0f fills/sec, %.1f Mpix/sec\n",
			fills_per_sec, mpix_per_sec);
	}

	// ---- Benchmark: screen_to_screen_blit throughput ----
	printf("\n--- Benchmark: XY_SRC_COPY_BLT throughput ---\n");
	{
		const uint32 ITERS = 500;
		blit_params bp;
		bp.src_left = 50;
		bp.src_top = 50;
		bp.dest_left = 50;
		bp.dest_top = 400;
		bp.width = 199;   // 200x100
		bp.height = 99;

		bigtime_t t0 = system_time();
		for (uint32 i = 0; i < ITERS; i++) {
			bp.dest_top = 400 + (i & 1);  // alternate to avoid caching
			intel_screen_to_screen_blit(&sToken, &bp, 1);
		}
		intel_wait_engine_idle();
		bigtime_t elapsed = system_time() - t0;

		float blits_per_sec = (float)ITERS * 1e6f / (float)elapsed;
		float mpix_per_sec = blits_per_sec * 200 * 100 / 1e6f;
		printf("  %u blits of 200x100 in %ld us\n", ITERS, (long)elapsed);
		printf("  %.0f blits/sec, %.1f Mpix/sec\n",
			blits_per_sec, mpix_per_sec);
	}

	// ---- Benchmark: full-screen fill ----
	printf("\n--- Benchmark: Full-screen fill ---\n");
	{
		const uint32 ITERS = 100;
		fill_rect_params rect;
		rect.left = 0;
		rect.top = 0;
		rect.right = scr_w - 1;
		rect.bottom = scr_h - 1;

		bigtime_t t0 = system_time();
		for (uint32 i = 0; i < ITERS; i++) {
			intel_fill_rectangle(&sToken, 0xFF000000 | (i * 0x010203),
				&rect, 1);
		}
		intel_wait_engine_idle();
		bigtime_t elapsed = system_time() - t0;

		float fills_per_sec = (float)ITERS * 1e6f / (float)elapsed;
		float mpix_per_sec = fills_per_sec * scr_w * scr_h / 1e6f;
		printf("  %u fills of %ux%u in %ld us\n", ITERS,
			scr_w, scr_h, (long)elapsed);
		printf("  %.0f fills/sec, %.1f Mpix/sec\n",
			fills_per_sec, mpix_per_sec);
	}

	// Clean up: fill screen with desktop-like color
	{
		fill_rect_params rect;
		rect.left = 0; rect.top = 0;
		rect.right = scr_w - 1; rect.bottom = scr_h - 1;
		intel_fill_rectangle(&sToken, 0xFF336699, &rect, 1);
		intel_wait_engine_idle();
	}

	printf("\n=== All tests completed ===\n");
	cleanup_gpu();
	return 0;
}
