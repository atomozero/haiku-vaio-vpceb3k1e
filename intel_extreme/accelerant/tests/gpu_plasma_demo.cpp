/*
 * gpu_plasma_demo — Real-time animated plasma powered by GPU IDCT.
 *
 * 400 blocks (20x20 grid = 160x160), each frame recomputed on GPU via
 * 48 parallel EU threads running the IDCT dp4 kernel. Scaled 3x to 480x480.
 */

#include <Application.h>
#include <Bitmap.h>
#include <String.h>
#include <Window.h>
#include <View.h>
#include <OS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "media_pipeline.h"
#include "idct_ref.h"

extern accelerant_info* gInfo;

// Grid: 20x20 blocks of 8x8 = 160x160 pixels
#define GRID_W 20
#define GRID_H 20
#define NBLOCKS (GRID_W * GRID_H)
#define IMG_W (GRID_W * 8)
#define IMG_H (GRID_H * 8)
#define SCALE 3
#define WIN_W (IMG_W * SCALE)
#define WIN_H (IMG_H * SCALE)


static bool
init_gpu(void)
{
	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) { printf("Cannot open GPU device\n"); return false; }

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(fd); return false;
	}

	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;

	gInfo->shared_info_area = clone_area("demo shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(fd); return false;
	}
	gInfo->regs_area = clone_area("demo regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(fd); return false;
	}
	return true;
}

static void
cleanup_gpu(void)
{
	if (!gInfo) return;
	int fd = gInfo->device;
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo); gInfo = NULL;
	close(fd);
}


// ---- Plasma: generate time-varying DCT coefficients ----

static void
gen_plasma(gpu_block_entry* blk, uint32 frame)
{
	float t = (float)frame * 0.07f;

	for (uint32 by = 0; by < GRID_H; by++) {
		for (uint32 bx = 0; bx < GRID_W; bx++) {
			uint32 i = by * GRID_W + bx;
			float fx = (float)bx / GRID_W;
			float fy = (float)by / GRID_H;

			memset(blk[i].coeffs, 0, 128);
			blk[i].x = 0;
			blk[i].y = i * 8;

			float dc = 900.0f
				+ 300.0f * sinf(fx * 6.28f + t)
				+ 300.0f * cosf(fy * 6.28f + t * 0.7f)
				+ 200.0f * sinf((fx + fy) * 4.0f + t * 1.3f);
			blk[i].coeffs[0] = (int16)dc;
			blk[i].coeffs[1] = (int16)(250.0f * sinf(fy * 3.14f + t * 2.1f));
			blk[i].coeffs[8] = (int16)(250.0f * cosf(fx * 3.14f + t * 1.7f));
			blk[i].coeffs[9] = (int16)(150.0f * sinf((fx-fy) * 6.28f + t * 3.0f));
			blk[i].coeffs[2] = (int16)(100.0f * cosf(fx * 9.42f + t * 0.9f));
			blk[i].coeffs[16] = (int16)(100.0f * sinf(fy * 9.42f - t * 1.1f));
		}
	}
}


// ---- Assemble stacked 8-wide blocks into 160x160 image ----

static void
assemble(const uint8* gpu_out, uint8 img[IMG_H][IMG_W])
{
	for (uint32 by = 0; by < GRID_H; by++) {
		for (uint32 bx = 0; bx < GRID_W; bx++) {
			uint32 src_y = (by * GRID_W + bx) * 8;
			uint32 dst_y = by * 8;
			uint32 dst_x = bx * 8;
			for (uint32 r = 0; r < 8; r++)
				for (uint32 c = 0; c < 8; c++)
					img[dst_y + r][dst_x + c] =
						gpu_out[(src_y + r) * 8 + c];
		}
	}
}


// ---- Grayscale → RGB32 with animated plasma color palette (LUT) ----

static uint32 sColorLUT[256];  // pre-computed per frame

static void
colorize_build_lut(uint32 frame)
{
	float t = (float)frame * 0.04f;
	for (uint32 v = 0; v < 256; v++) {
		float fv = (float)v / 255.0f;
		uint8 r = (uint8)(255.0f * (0.5f + 0.5f * sinf(fv * 6.28f + t)));
		uint8 g = (uint8)(255.0f * (0.5f + 0.5f * sinf(fv * 6.28f + t + 2.09f)));
		uint8 b = (uint8)(255.0f * (0.5f + 0.5f * sinf(fv * 6.28f + t + 4.19f)));
		sColorLUT[v] = (255u << 24) | (r << 16) | (g << 8) | b;
	}
}

static void
colorize(const uint8 img[IMG_H][IMG_W], uint32* rgb,
	uint32 stride, uint32 frame)
{
	colorize_build_lut(frame);  // 256 sinf instead of 700K
	for (uint32 y = 0; y < WIN_H; y++) {
		for (uint32 x = 0; x < WIN_W; x++) {
			rgb[y * stride + x] = sColorLUT[img[y / SCALE][x / SCALE]];
		}
	}
}


// ---- BWindow ----

class PlasmaView : public BView {
public:
	PlasmaView(BRect r) : BView(r, "pv", B_FOLLOW_ALL, B_WILL_DRAW) {
		fBmp = new BBitmap(BRect(0, 0, WIN_W-1, WIN_H-1), B_RGB32);
	}
	~PlasmaView() { delete fBmp; }
	void Draw(BRect) override { DrawBitmap(fBmp, Bounds()); }
	BBitmap* Bmp() { return fBmp; }
private:
	BBitmap* fBmp;
};

class PlasmaWin : public BWindow {
public:
	PlasmaWin() : BWindow(BRect(60,60, 60+WIN_W-1, 60+WIN_H-1),
		"GPU Plasma — Intel Gen5 IDCT", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_QUIT_ON_WINDOW_CLOSE) {
		fView = new PlasmaView(Bounds());
		AddChild(fView);
		fAlive = true;
	}
	bool QuitRequested() override {
		fAlive = false;
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}
	PlasmaView* View() { return fView; }
	bool Alive() { return fAlive; }
private:
	PlasmaView* fView;
	volatile bool fAlive;
};


int
main(int, char**)
{
	printf("GPU Plasma Demo — Intel Ironlake Gen5\n");
	printf("  %d blocks (%dx%d px), %dx scale = %dx%d window\n\n",
		NBLOCKS, IMG_W, IMG_H, SCALE, WIN_W, WIN_H);

	if (!init_gpu()) return 1;

	media_pipeline_context ctx;
	if (media_pipeline_init(&ctx) != B_OK) {
		printf("pipeline init failed\n");
		cleanup_gpu(); return 1;
	}
	if (media_pipeline_setup_idct_to_u8(&ctx, NBLOCKS) != B_OK) {
		printf("IDCT setup failed\n");
		media_pipeline_uninit(&ctx);
		cleanup_gpu(); return 1;
	}

	printf("GPU ready. Opening window...\n");

	BApplication app("application/x-gpu-plasma");
	PlasmaWin* win = new PlasmaWin();
	win->Show();

	// Pre-compute ALL frames' coefficients — zero CPU work in loop.
	const uint32 TOTAL_FRAMES = 600;  // ~10 seconds of animation
	printf("Pre-computing %u frames of coefficients...\n", TOTAL_FRAMES);
	gpu_block_entry* all_frames = (gpu_block_entry*)malloc(
		TOTAL_FRAMES * NBLOCKS * sizeof(gpu_block_entry));
	if (!all_frames) { printf("malloc failed\n"); return 1; }
	for (uint32 f = 0; f < TOTAL_FRAMES; f++)
		gen_plasma(&all_frames[f * NBLOCKS], f);
	printf("Done. Starting GPU-only render loop.\n\n");

	uint8 img[IMG_H][IMG_W];

	uint32 frame = 0;
	uint32 fps_cnt = 0;
	bigtime_t fps_t0 = system_time();
	bigtime_t idct_total = 0;

	while (win->Alive() && frame < TOTAL_FRAMES) {
		gpu_block_entry* blk = &all_frames[frame * NBLOCKS];

		bigtime_t t_idct_start = system_time();

		// GPU only: dispatch pre-computed coefficients.
		status_t st = submit_blocks_batch_gpu(&ctx, blk, NBLOCKS);
		if (st != B_OK) {
			printf("GPU dispatch failed at frame %u\n", frame);
			break;
		}
		assemble((const uint8*)ctx.output_bo.cpu_addr, img);

		bigtime_t t_idct_end = system_time();
		idct_total += (t_idct_end - t_idct_start);

		// LUT colorize + blit to window (minimal CPU work)
		if (win->Lock()) {
			BBitmap* bmp = win->View()->Bmp();
			colorize(img, (uint32*)bmp->Bits(),
				bmp->BytesPerRow() / 4, frame);
			win->View()->Invalidate();
			win->Unlock();
		}

		frame++;
		fps_cnt++;
		snooze(5000);  // yield CPU — target ~60fps

		bigtime_t now = system_time();
		if (now - fps_t0 >= 1000000) {
			float fps = (float)fps_cnt * 1e6f / (float)(now - fps_t0);
			float idct_avg_us = (float)idct_total / (float)fps_cnt;
			BString t;
			t.SetToFormat("GPU Plasma — %.0f FPS — GPU IDCT: %.0f us — "
				"%d blocks", fps, idct_avg_us, NBLOCKS);
			if (win->Lock()) {
				win->SetTitle(t.String());
				win->Unlock();
			}
			printf("  frame %u: %.0f FPS, GPU IDCT %.0f us\n",
				frame, fps, idct_avg_us);
			fps_cnt = 0;
			fps_t0 = now;
			idct_total = 0;
		}
	}

	printf("Done, %u frames.\n", frame);
	free(all_frames);
	media_pipeline_uninit(&ctx);
	cleanup_gpu();
	return 0;
}
