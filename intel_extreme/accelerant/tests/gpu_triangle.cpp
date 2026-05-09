/*
 * gpu_triangle — GPU-accelerated rotating 3D cube in a BWindow.
 *
 * CPU: 3D transform + projection + edge test (which tiles inside each face)
 * GPU: parallel tile fill via EU compute kernel
 * Display: colorize + BWindow
 */

#include <Application.h>
#include <Bitmap.h>
#include <String.h>
#include <Window.h>
#include <View.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>

#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "media_pipeline.h"
#include "gpu_bo.h"

extern accelerant_info* gInfo;

#define IMG_W 480
#define IMG_H 480
#define TILE  8
#define TILES_X (IMG_W / TILE)
#define TILES_Y (IMG_H / TILE)
#define WIN_W 720
#define WIN_H 720

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
	gInfo->shared_info_area = clone_area("cube shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(fd); return false;
	}
	gInfo->regs_area = clone_area("cube regs",
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


// ---- 3D Math ----

struct vec3 { float x, y, z; };
struct vec2 { float x, y; };

static vec3 rotate(vec3 v, float ax, float ay)
{
	// Rotate around Y then X
	float cy = cosf(ay), sy = sinf(ay);
	float x1 = v.x * cy + v.z * sy;
	float z1 = -v.x * sy + v.z * cy;
	float cx = cosf(ax), sx = sinf(ax);
	float y1 = v.y * cx - z1 * sx;
	float z2 = v.y * sx + z1 * cx;
	return {x1, y1, z2};
}

static vec2 project(vec3 v)
{
	float d = 4.0f;  // camera distance
	float scale = 70.0f;
	float persp = d / (d + v.z + 1.5f);
	return {IMG_W/2.0f + v.x * scale * persp,
	        IMG_H/2.0f + v.y * scale * persp};
}


// ---- Cube geometry ----

static const vec3 cube_verts[8] = {
	{-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},  // back
	{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},  // front
};

// 6 faces, each 4 vertex indices (2 triangles per face)
static const int faces[6][4] = {
	{0,1,2,3},  // back
	{5,4,7,6},  // front
	{4,0,3,7},  // left
	{1,5,6,2},  // right
	{4,5,1,0},  // bottom
	{3,2,6,7},  // top
};

// Face colors (R, G, B)
static const uint8 face_colors[6][3] = {
	{200,  50,  50},  // back - red
	{ 50, 200,  50},  // front - green
	{ 50,  50, 200},  // left - blue
	{200, 200,  50},  // right - yellow
	{200,  50, 200},  // bottom - magenta
	{ 50, 200, 200},  // top - cyan
};


// ---- Edge function rasterizer ----

static inline float
edge(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static inline bool
point_in_tri(vec2 a, vec2 b, vec2 c, float px, float py)
{
	float e0 = edge(a.x, a.y, b.x, b.y, px, py);
	float e1 = edge(b.x, b.y, c.x, c.y, px, py);
	float e2 = edge(c.x, c.y, a.x, a.y, px, py);
	return (e0 >= 0 && e1 >= 0 && e2 >= 0)
		|| (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

// Back-face cull: positive cross product = front-facing
static inline float
face_area(vec2 a, vec2 b, vec2 c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}


// ---- BWindow with BBitmap (DrawBitmap uses HW BLT) ----

class CubeView : public BView {
public:
	CubeView(BRect r) : BView(r, "cv", B_FOLLOW_ALL, B_WILL_DRAW) {
		fBmp = new BBitmap(BRect(0, 0, IMG_W-1, IMG_H-1), B_RGB32);
	}
	~CubeView() { delete fBmp; }
	void Draw(BRect) override { DrawBitmap(fBmp, Bounds()); }
	BBitmap* Bmp() { return fBmp; }
private:
	BBitmap* fBmp;
};

class CubeWin : public BWindow {
public:
	CubeWin() : BWindow(BRect(50, 50, 50+WIN_W-1, 50+WIN_H-1),
		"GPU 3D Cube — Intel Gen5", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_QUIT_ON_WINDOW_CLOSE) {
		fView = new CubeView(Bounds());
		AddChild(fView);
		fAlive = true;
	}
	bool QuitRequested() override {
		fAlive = false;
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}
	CubeView* View() { return fView; }
	bool Alive() { return fAlive; }
private:
	CubeView* fView;
	volatile bool fAlive;
};


int
main(int, char**)
{
	printf("=== GPU 3D Cube — Compute Rasterizer ===\n");
	printf("  %dx%d raster (%dx%d window), 6 faces, tile fill via Gen5 EU\n\n",
		IMG_W, IMG_H, WIN_W, WIN_H);

	if (!init_gpu()) { printf("GPU init failed\n"); return 1; }
	printf("GPU OK, Gen %u\n",
		gInfo->shared_info->device_type.Generation());

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

	BApplication app("application/x-gpu-cube");
	CubeWin* win = new CubeWin();
	win->Show();

	printf("Window ready\n");

	gpu_tile_entry* tiles = (gpu_tile_entry*)malloc(
		TILES_X * TILES_Y * sizeof(gpu_tile_entry));

	uint32 frame = 0;
	uint32 fps_cnt = 0;
	bigtime_t fps_t0 = system_time();
	const bigtime_t frame_budget = 33333;  // 30 FPS target

	while (win->Alive()) {
		bigtime_t frame_start = system_time();
		float ax = (float)frame * 0.015f;
		float ay = (float)frame * 0.023f;

		// Transform all vertices
		vec2 proj[8];
		vec3 rot[8];
		for (int i = 0; i < 8; i++) {
			rot[i] = rotate(cube_verts[i], ax, ay);
			proj[i] = project(rot[i]);
		}

		// Clear framebuffer
		memset((void*)ctx.output_bo.cpu_addr, 0, IMG_W * IMG_H);

		// Sort faces by average Z (painter's algorithm — back to front)
		float face_z[6];
		int face_order[6];
		for (int f = 0; f < 6; f++) {
			face_z[f] = 0;
			for (int j = 0; j < 4; j++)
				face_z[f] += rot[faces[f][j]].z;
			face_z[f] /= 4.0f;
			face_order[f] = f;
		}
		for (int i = 0; i < 5; i++)
			for (int j = 0; j < 5-i; j++)
				if (face_z[face_order[j]] > face_z[face_order[j+1]]) {
					int tmp = face_order[j];
					face_order[j] = face_order[j+1];
					face_order[j+1] = tmp;
				}

		// Collect ALL tiles from all visible faces into one batch
		uint32 total_tiles = 0;
		for (int fi = 0; fi < 6; fi++) {
			int f = face_order[fi];
			vec2 a = proj[faces[f][0]];
			vec2 b = proj[faces[f][1]];
			vec2 c = proj[faces[f][2]];
			vec2 d = proj[faces[f][3]];

			if (face_area(a, b, c) <= 0)
				continue;

			// Lighting
			vec3 e1 = {rot[faces[f][1]].x - rot[faces[f][0]].x,
			           rot[faces[f][1]].y - rot[faces[f][0]].y,
			           rot[faces[f][1]].z - rot[faces[f][0]].z};
			vec3 e2 = {rot[faces[f][2]].x - rot[faces[f][0]].x,
			           rot[faces[f][2]].y - rot[faces[f][0]].y,
			           rot[faces[f][2]].z - rot[faces[f][0]].z};
			float nx = e1.y*e2.z - e1.z*e2.y;
			float ny = e1.z*e2.x - e1.x*e2.z;
			float nz = e1.x*e2.y - e1.y*e2.x;
			float len = sqrtf(nx*nx + ny*ny + nz*nz);
			if (len > 0) { nx /= len; ny /= len; nz /= len; }
			float light = nx*0.3f + ny*-0.5f + nz*0.8f;
			if (light < 0.15f) light = 0.15f;
			if (light > 1.0f) light = 1.0f;
			uint32 fill = ((f+1) << 4) | (uint32)(light * 15.0f);

			// Bounding box of the quad (a,b,c,d) in tile coords
			float minx = a.x, maxx = a.x, miny = a.y, maxy = a.y;
			float pts[] = {b.x,b.y, c.x,c.y, d.x,d.y};
			for (int p = 0; p < 6; p += 2) {
				if (pts[p] < minx) minx = pts[p];
				if (pts[p] > maxx) maxx = pts[p];
				if (pts[p+1] < miny) miny = pts[p+1];
				if (pts[p+1] > maxy) maxy = pts[p+1];
			}
			int tx0 = (int)(minx / TILE) - 1;
			int ty0 = (int)(miny / TILE) - 1;
			int tx1 = (int)(maxx / TILE) + 1;
			int ty1 = (int)(maxy / TILE) + 1;
			if (tx0 < 0) tx0 = 0;
			if (ty0 < 0) ty0 = 0;
			if (tx1 >= (int)TILES_X) tx1 = TILES_X - 1;
			if (ty1 >= (int)TILES_Y) ty1 = TILES_Y - 1;

			for (int ty = ty0; ty <= ty1; ty++) {
				for (int tx = tx0; tx <= tx1; tx++) {
					float px = tx * TILE + 4.0f;
					float py = ty * TILE + 4.0f;
					if (point_in_tri(a,b,c,px,py)
						|| point_in_tri(a,c,d,px,py)) {
						tiles[total_tiles].x = tx * TILE;
						tiles[total_tiles].y = ty * TILE;
						tiles[total_tiles].color = fill;
						total_tiles++;
					}
				}
			}
		}

		bigtime_t t_gpu = system_time();
		// Submit tiles in batches of 400 (BATCH_MAX_BLOCKS limit)
		for (uint32 off = 0; off < total_tiles; off += 400) {
			uint32 chunk = total_tiles - off;
			if (chunk > 400) chunk = 400;
			submit_tile_fill_batch(&ctx, tiles + off, chunk);
		}
		bigtime_t gpu_us = system_time() - t_gpu;

		// Colorize to BBitmap, DrawBitmap scales via HW BLT
		if (win->Lock()) {
			BBitmap* bmp = win->View()->Bmp();
			uint32 stride = bmp->BytesPerRow() / 4;
			uint32* out = (uint32*)bmp->Bits();
			const uint8* src = (const uint8*)ctx.output_bo.cpu_addr;

			static uint32 lut[256];
			static bool lut_init = false;
			if (!lut_init) {
				lut[0] = 0xFF1A1A2E;
				for (int f = 0; f < 6; f++) {
					for (int b = 0; b < 16; b++) {
						uint32 idx = ((f + 1) << 4) | b;
						uint32 bright = b * 17;
						uint32 r = (face_colors[f][0] * bright) >> 8;
						uint32 g = (face_colors[f][1] * bright) >> 8;
						uint32 bl = (face_colors[f][2] * bright) >> 8;
						lut[idx] = 0xFF000000 | (r << 16) | (g << 8) | bl;
					}
				}
				lut_init = true;
			}

			for (uint32 y = 0; y < IMG_H; y++) {
				const uint8* row = src + y * IMG_W;
				uint32* dst = out + y * stride;
				for (uint32 x = 0; x < IMG_W; x++)
					dst[x] = lut[row[x]];
			}
			win->View()->Invalidate();
			win->Unlock();
		}

		frame++;
		fps_cnt++;
		bigtime_t now = system_time();
		if (now - fps_t0 >= 1000000) {
			float fps = (float)fps_cnt * 1e6f / (float)(now - fps_t0);
			BString title;
			title.SetToFormat("GPU 3D Cube — %.0f FPS — %u tiles — "
				"%dx%d — Intel Gen5 EU",
				fps, total_tiles, IMG_W, IMG_H);
			if (win->Lock()) {
				win->SetTitle(title.String());
				win->Unlock();
			}
			printf("  frame %u: %.1f FPS, %u tiles, gpu=%lld us\n",
				frame, fps, total_tiles, gpu_us);
			fps_cnt = 0;
			fps_t0 = now;
		}

		// Frame limiter: sleep remaining time to hit 60 FPS
		bigtime_t elapsed = system_time() - frame_start;
		if (elapsed < frame_budget)
			snooze(frame_budget - elapsed);
	}

	printf("Done, %u frames.\n", frame);
	free(tiles);
	media_pipeline_uninit(&ctx);
	cleanup_gpu();
	return 0;
}
