/*
 * GPU Plasma on Screen — optimized with LUT palette.
 * GPU: 300 IDCT blocks in parallel (48 EU threads)
 * CPU: LUT color mapping + scaled blit to framebuffer
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <OS.h>
#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "media_pipeline.h"
#include "gpu_bo.h"

extern accelerant_info* gInfo;

static bool init_gpu(void) {
	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) return false;
	intel_get_private_data d;
	d.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &d, sizeof(d)) != 0) {
		close(fd); return false;
	}
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;
	gInfo->shared_info_area = clone_area("p s",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, d.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(fd); return false;
	}
	gInfo->regs_area = clone_area("p r",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(fd); return false;
	}
	return true;
}

#define GW 20
#define GH 15
#define NBLK (GW * GH)
#define IW (GW * 8)
#define IH (GH * 8)
#define SCALE 4
#define WIN_W (IW * SCALE)
#define WIN_H (IH * SCALE)

// Pre-computed color LUT: 256 entries, updated per-frame
static uint32 sLUT[256];

static void
update_lut(float t)
{
	for (int v = 0; v < 256; v++) {
		float fv = (float)v / 255.0f;
		uint8 r = (uint8)(255.0f * (0.5f + 0.5f * sinf(fv * 6.28f + t)));
		uint8 g = (uint8)(255.0f * (0.5f + 0.5f * sinf(fv * 6.28f + t + 2.09f)));
		uint8 b = (uint8)(255.0f * (0.5f + 0.5f * sinf(fv * 6.28f + t + 4.19f)));
		sLUT[v] = b | (g << 8) | (r << 16);
	}
}

int main(int, char**) {
	printf("=== GPU Plasma on Screen (LUT optimized) ===\n");
	printf("  %dx%d = %dx%d, %dx scale = %dx%d on screen\n\n",
		GW, GH, IW, IH, SCALE, WIN_W, WIN_H);

	if (!init_gpu()) { printf("No GPU\n"); return 1; }

	intel_shared_info& info = *gInfo->shared_info;
	uint32 scr_w = info.current_mode.timing.h_display;
	uint32 scr_h = info.current_mode.timing.v_display;
	uint32 bpr = info.bytes_per_row;
	uint8* fb = (uint8*)info.graphics_memory + info.frame_buffer_offset;

	media_pipeline_context ctx;
	if (media_pipeline_init(&ctx) != B_OK ||
		media_pipeline_setup_idct_to_u8(&ctx, NBLK) != B_OK) {
		printf("GPU init failed\n"); return 1;
	}

	gpu_block_entry* blk = (gpu_block_entry*)malloc(
		NBLK * sizeof(gpu_block_entry));

	// Staging line buffer for scaled blit
	uint32* line = (uint32*)malloc(WIN_W * 4);

	uint32 ox = (scr_w - WIN_W) / 2;
	uint32 oy = (scr_h - WIN_H) / 2;

	uint32 frame = 0, fps_cnt = 0;
	bigtime_t fps_t0 = system_time();
	bigtime_t gpu_total = 0, blit_total = 0;

	printf("Rendering at (%u,%u), Ctrl+C to stop\n\n", ox, oy);

	while (true) {
		float t = (float)frame * 0.06f;

		// 1. Generate coefficients (CPU, fast)
		for (uint32 by = 0; by < GH; by++) {
			for (uint32 bx = 0; bx < GW; bx++) {
				uint32 i = by * GW + bx;
				float fx = (float)bx / GW;
				float fy = (float)by / GH;
				memset(blk[i].coeffs, 0, 128);
				blk[i].x = 0;
				blk[i].y = i * 8;
				blk[i].coeffs[0] = (int16)(900.0f
					+ 300.0f * sinf(fx * 6.28f + t)
					+ 300.0f * cosf(fy * 6.28f + t * 0.7f)
					+ 200.0f * sinf((fx + fy) * 4.0f + t * 1.3f));
				blk[i].coeffs[1] = (int16)(250.0f * sinf(fy * 3.14f + t * 2.1f));
				blk[i].coeffs[8] = (int16)(250.0f * cosf(fx * 3.14f + t * 1.7f));
				blk[i].coeffs[9] = (int16)(150.0f * sinf((fx-fy)*6.28f + t*3.0f));
				blk[i].coeffs[2] = (int16)(100.0f * cosf(fx*9.42f + t*0.9f));
				blk[i].coeffs[16] = (int16)(100.0f * sinf(fy*9.42f - t*1.1f));
			}
		}

		// 2. GPU IDCT (300 blocks, 48 EU threads)
		bigtime_t t_gpu = system_time();
		status_t st = submit_blocks_batch_gpu(&ctx, blk, NBLK);
		gpu_total += system_time() - t_gpu;
		if (st != B_OK) { printf("GPU fail\n"); break; }

		// 3. Update LUT (256 sinf — much less than 57600)
		update_lut(t * 0.7f);

		// 4. Blit GPU output → framebuffer with LUT + scale
		bigtime_t t_blit = system_time();
		const uint8* gpu = (const uint8*)ctx.output_bo.cpu_addr;

		for (uint32 py = 0; py < IH; py++) {
			// Which block row and row within block?
			uint32 by = py / 8;
			uint32 r = py % 8;

			// Build one scaled line
			for (uint32 px = 0; px < IW; px++) {
				uint32 bx = px / 8;
				uint32 c = px % 8;
				uint32 bi = by * GW + bx;
				uint8 v = gpu[(bi * 8 + r) * 8 + c];
				uint32 color = sLUT[v];
				for (uint32 sx = 0; sx < SCALE; sx++)
					line[px * SCALE + sx] = color;
			}

			// Copy scaled line to framebuffer SCALE times
			for (uint32 sy = 0; sy < SCALE; sy++) {
				uint32 dy = oy + py * SCALE + sy;
				if (dy < scr_h)
					memcpy(fb + dy * bpr + ox * 4, line, WIN_W * 4);
			}
		}
		asm volatile("mfence" ::: "memory");
		blit_total += system_time() - t_blit;

		frame++;
		fps_cnt++;
		snooze(1000);  // ~1ms yield — prevents 100% CPU spin

		bigtime_t now = system_time();
		if (now - fps_t0 >= 2000000) {
			float fps = (float)fps_cnt * 1e6f / (float)(now - fps_t0);
			float gpu_avg = (float)gpu_total / fps_cnt / 1000.0f;
			float blit_avg = (float)blit_total / fps_cnt / 1000.0f;
			printf("  frame %u: %.1f FPS  (gpu=%.1fms blit=%.1fms)\n",
				frame, fps, gpu_avg, blit_avg);
			fps_cnt = 0;
			fps_t0 = now;
			gpu_total = 0;
			blit_total = 0;
		}
	}

	free(line);
	free(blk);
	media_pipeline_uninit(&ctx);
	return 0;
}
