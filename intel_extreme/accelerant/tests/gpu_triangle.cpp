/*
 * gpu_triangle — GPU-rasterized triangle via media pipeline (compute).
 * Uses tile fill kernel: CPU computes triangle coverage per 8×8 tile,
 * GPU writes colored tiles via Media Block Write.
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
#include "gpu_bo.h"

extern accelerant_info* gInfo;

#define IMG_W 160
#define IMG_H 160
#define TILE 8
#define TILES_X (IMG_W / TILE)
#define TILES_Y (IMG_H / TILE)
#define NTILES (TILES_X * TILES_Y)  // 1600

static bool
init_gpu(void)
{
	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) { printf("Cannot open GPU\n"); return false; }
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(fd); return false;
	}
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;
	gInfo->shared_info_area = clone_area("tri shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(fd); return false;
	}
	gInfo->regs_area = clone_area("tri regs",
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


// ---- Triangle edge function ----

// Returns > 0 if point (px,py) is on the left side of edge v0→v1
static inline float
edge(float v0x, float v0y, float v1x, float v1y, float px, float py)
{
	return (v1x - v0x) * (py - v0y) - (v1y - v0y) * (px - v0x);
}

// Check if any pixel in an 8×8 tile overlaps the triangle.
// Returns coverage fraction 0.0-1.0 (for anti-aliasing / smooth edges)
static float
tile_coverage(float v0x, float v0y, float v1x, float v1y,
	float v2x, float v2y, uint32 tx, uint32 ty)
{
	uint32 inside = 0;
	for (uint32 dy = 0; dy < TILE; dy++) {
		for (uint32 dx = 0; dx < TILE; dx++) {
			float px = (float)(tx + dx) + 0.5f;
			float py = (float)(ty + dy) + 0.5f;
			float e0 = edge(v0x, v0y, v1x, v1y, px, py);
			float e1 = edge(v1x, v1y, v2x, v2y, px, py);
			float e2 = edge(v2x, v2y, v0x, v0y, px, py);
			if (e0 >= 0 && e1 >= 0 && e2 >= 0)
				inside++;
		}
	}
	return (float)inside / 64.0f;
}


// ---- Grayscale to RGB32 ----

static void
gray_to_rgb(const uint8* gray, uint32* rgb, uint32 w, uint32 h,
	uint8 r, uint8 g, uint8 b, uint8 bg_r, uint8 bg_g, uint8 bg_b)
{
	for (uint32 i = 0; i < w * h; i++) {
		uint8 v = gray[i];
		// Blend: color = bg + (fg - bg) * v / 255
		uint8 pr = bg_r + (((int)r - bg_r) * v) / 255;
		uint8 pg = bg_g + (((int)g - bg_g) * v) / 255;
		uint8 pb = bg_b + (((int)b - bg_b) * v) / 255;
		rgb[i] = (255u << 24) | (pr << 16) | (pg << 8) | pb;
	}
}


// ---- BWindow ----

class TriView : public BView {
public:
	TriView(BRect r) : BView(r, "tv", B_FOLLOW_ALL, B_WILL_DRAW) {
		fBmp = new BBitmap(BRect(0, 0, IMG_W-1, IMG_H-1), B_RGB32);
	}
	~TriView() { delete fBmp; }
	void Draw(BRect) override { DrawBitmap(fBmp, Bounds()); }
	BBitmap* Bmp() { return fBmp; }
private:
	BBitmap* fBmp;
};

class TriWin : public BWindow {
public:
	TriWin() : BWindow(BRect(50, 50, 50+IMG_W*3-1, 50+IMG_H*3-1),
		"GPU Triangle — Intel Gen5 Compute", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_QUIT_ON_WINDOW_CLOSE) {
		fView = new TriView(Bounds());
		AddChild(fView);
		fAlive = true;
	}
	bool QuitRequested() override {
		fAlive = false;
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}
	TriView* View() { return fView; }
	bool Alive() { return fAlive; }
private:
	TriView* fView;
	volatile bool fAlive;
};


int
main(int, char**)
{
	printf("GPU Triangle Demo — Compute Rasterizer\n");
	printf("  %dx%d px, %d tiles (%dx%d)\n\n", IMG_W, IMG_H,
		NTILES, TILES_X, TILES_Y);

	if (!init_gpu()) return 1;

	media_pipeline_context ctx;
	if (media_pipeline_init(&ctx) != B_OK) {
		printf("pipeline init failed\n");
		cleanup_gpu(); return 1;
	}
	if (media_pipeline_setup_tile_fill(&ctx, IMG_W, IMG_H) != B_OK) {
		printf("tile fill setup failed\n");
		media_pipeline_uninit(&ctx);
		cleanup_gpu(); return 1;
	}

	printf("GPU pipeline ready.\n");

	BApplication app("application/x-gpu-triangle");
	TriWin* win = new TriWin();
	win->Show();

	gpu_tile_entry* tiles = (gpu_tile_entry*)malloc(
		NTILES * sizeof(gpu_tile_entry));
	uint32* rgb = (uint32*)malloc(IMG_W * IMG_H * 4);

	float angle = 0;
	uint32 frame = 0;
	uint32 fps_cnt = 0;
	bigtime_t fps_t0 = system_time();

	while (win->Alive()) {
		float cx = IMG_W / 2.0f, cy = IMG_H / 2.0f, sz = 120.0f;
		float a0 = angle, a1 = angle + 2.094f, a2 = angle + 4.189f;
		float v0x = cx + sz * sinf(a0), v0y = cy - sz * cosf(a0);
		float v1x = cx + sz * sinf(a1), v1y = cy - sz * cosf(a1);
		float v2x = cx + sz * sinf(a2), v2y = cy - sz * cosf(a2);

		// CPU: compute per-tile triangle coverage → fill color
		uint32 tile_count = 0;
		for (uint32 ty = 0; ty < TILES_Y; ty++) {
			for (uint32 tx = 0; tx < TILES_X; tx++) {
				float cov = tile_coverage(v0x, v0y, v1x, v1y,
					v2x, v2y, tx * TILE, ty * TILE);
				uint8 c = (uint8)(cov * 255.0f);
				if (c > 0 || true) {  // always fill (background + triangle)
					tiles[tile_count].x = tx * TILE;
					tiles[tile_count].y = ty * TILE;
					tiles[tile_count].color = c;
					tile_count++;
				}
			}
		}

		// GPU: fill all tiles
		status_t st = submit_tile_fill_batch(&ctx, tiles, tile_count);
		if (st != B_OK) {
			printf("GPU dispatch failed at frame %u: %s\n",
				frame, strerror(st));
			break;
		}

		// Read GPU output and convert to RGB
		const uint8* gray = (const uint8*)ctx.output_bo.cpu_addr;
		gray_to_rgb(gray, rgb, IMG_W, IMG_H,
			255, 60, 60,    // triangle color (red)
			26, 26, 46);    // background (dark blue)

		if (win->Lock()) {
			memcpy(win->View()->Bmp()->Bits(), rgb, IMG_W * IMG_H * 4);
			win->View()->Invalidate();
			win->Unlock();
		}

		angle += 0.03f;
		frame++;
		fps_cnt++;

		bigtime_t now = system_time();
		if (now - fps_t0 >= 1000000) {
			float fps = fps_cnt * 1e6f / (now - fps_t0);
			BString t;
			t.SetToFormat("GPU Triangle — %.1f FPS — %u tiles — "
				"Intel Gen5 Compute", fps, tile_count);
			if (win->Lock()) {
				win->SetTitle(t.String());
				win->Unlock();
			}
			printf("  frame %u: %.1f FPS, %u tiles\n",
				frame, fps, tile_count);
			fps_cnt = 0;
			fps_t0 = now;
		}

		snooze(5000);
	}

	printf("Done, %u frames.\n", frame);
	free(tiles);
	free(rgb);
	media_pipeline_uninit(&ctx);
	cleanup_gpu();
	return 0;
}
