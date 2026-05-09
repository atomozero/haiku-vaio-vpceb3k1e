/*
 * gpu_triangle — GPU-accelerated rotating 3D cube, BLT direct to screen.
 *
 * CPU: 3D transform + projection + edge test
 * GPU: RGBA tile fill via EU compute kernel
 * Display: XY_SRC_COPY_BLT from GPU output_bo to screen framebuffer
 * No BWindow, no app_server overhead — pure GPU → screen pipeline.
 */

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
#include "commands.h"

extern accelerant_info* gInfo;

#define IMG_W 480
#define IMG_H 480
#define TILE  8
#define TILES_X (IMG_W / TILE)
#define TILES_Y (IMG_H / TILE)
// No window needed — BLT directly to screen!


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


int
main(int, char**)
{
	printf("=== GPU 3D Cube — RGBA + BLT to Screen ===\n");
	printf("  %dx%d, 6 faces, RGBA tile fill + XY_SRC_COPY_BLT\n\n",
		IMG_W, IMG_H);

	if (!init_gpu()) { printf("GPU init failed\n"); return 1; }
	printf("GPU OK, Gen %u\n",
		gInfo->shared_info->device_type.Generation());
	printf("Screen: %ux%u, fb_offset=0x%x, bpr=%u\n",
		gInfo->shared_info->current_mode.timing.h_display,
		gInfo->shared_info->current_mode.timing.v_display,
		(unsigned)gInfo->shared_info->frame_buffer_offset,
		(unsigned)gInfo->shared_info->bytes_per_row);

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
	printf("RGBA tile fill ready, output_bo GTT=0x%x\n",
		ctx.output_bo.gtt_offset);

	gpu_tile_entry* tiles = (gpu_tile_entry*)malloc(
		TILES_X * TILES_Y * sizeof(gpu_tile_entry));

	// Center cube on screen
	uint32 scr_w = gInfo->shared_info->current_mode.timing.h_display;
	uint32 scr_h = gInfo->shared_info->current_mode.timing.v_display;
	uint32 dst_x = (scr_w - IMG_W) / 2;
	uint32 dst_y = (scr_h - IMG_H) / 2;

	uint32 frame = 0;
	uint32 fps_cnt = 0;
	bigtime_t fps_t0 = system_time();
	bool running = true;

	printf("Rendering to screen at (%u,%u)...\n", dst_x, dst_y);
	printf("Press Ctrl+C to stop.\n\n");

	while (running) {
		float ax = (float)frame * 0.015f;
		float ay = (float)frame * 0.023f;

		// Transform vertices
		vec2 proj[8];
		vec3 rot[8];
		for (int i = 0; i < 8; i++) {
			rot[i] = rotate(cube_verts[i], ax, ay);
			proj[i] = project(rot[i]);
		}

		// Clear output to dark background (ARGB)
		{
			uint32* px = (uint32*)ctx.output_bo.cpu_addr;
			uint32 bg = 0xFF1A1A2E;
			for (uint32 i = 0; i < IMG_W * IMG_H; i++)
				px[i] = bg;
		}

		// Sort faces (painter's algorithm)
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

		// Collect tiles with ARGB colors
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

			// Compute ARGB color directly
			uint32 r = (uint32)(face_colors[f][0] * light);
			uint32 g = (uint32)(face_colors[f][1] * light);
			uint32 bl = (uint32)(face_colors[f][2] * light);
			uint32 argb = 0xFF000000 | (r << 16) | (g << 8) | bl;

			// Bounding box scan
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
						tiles[total_tiles].color = argb;
						total_tiles++;
					}
				}
			}
		}

		// GPU: U8 tile fill (proven working)
		bigtime_t t_gpu = system_time();
		for (uint32 off = 0; off < total_tiles; off += 400) {
			uint32 chunk = total_tiles - off;
			if (chunk > 400) chunk = 400;
			submit_tile_fill_batch(&ctx, tiles + off, chunk);
		}
		bigtime_t gpu_us = system_time() - t_gpu;

		// CPU: colorize U8 → RGBA in output_bo (reuse same BO)
		// Write RGBA at offset IMG_W*IMG_H (after U8 data)
		const uint8* u8src = (const uint8*)ctx.output_bo.cpu_addr;
		uint32* rgba_dst = (uint32*)(ctx.output_bo.cpu_addr + IMG_W * IMG_H);
		static uint32 lut[256];
		static bool lut_init = false;
		if (!lut_init) {
			lut[0] = 0xFF1A1A2E;
			for (int fc = 0; fc < 6; fc++) {
				for (int b = 0; b < 16; b++) {
					uint32 idx = ((fc + 1) << 4) | b;
					uint32 bright = b * 17;
					uint32 r = (face_colors[fc][0] * bright) >> 8;
					uint32 g = (face_colors[fc][1] * bright) >> 8;
					uint32 bl2 = (face_colors[fc][2] * bright) >> 8;
					lut[idx] = 0xFF000000 | (bl2 << 16) | (g << 8) | r;  // BGRA
				}
			}
			lut_init = true;
		}
		for (uint32 y = 0; y < IMG_H; y++) {
			const uint8* row = u8src + y * IMG_W;
			uint32* dst = rgba_dst + y * IMG_W;
			for (uint32 x = 0; x < IMG_W; x++)
				dst[x] = lut[row[x]];
		}
		asm volatile("mfence" ::: "memory");

		// GPU: BLT RGBA region of output_bo → screen framebuffer
		{
			intel_shared_info& info = *gInfo->shared_info;
			uint32 src_base = ctx.output_bo.gtt_offset + IMG_W * IMG_H;
			uint32 src_pitch = IMG_W * 4;
			uint32 dst_base = info.frame_buffer_offset;
			uint32 dst_pitch = info.bytes_per_row;

			ring_buffer& ring = info.primary_ring_buffer;
			QueueCommands queue(ring);
			queue.MakeSpace(8);
			queue.Write((0x53 << 22) | (1 << 21) | (1 << 20) | 6);
			queue.Write(dst_pitch | (0xCC << 16) | (3 << 24));
			queue.Write((dst_y << 16) | dst_x);
			queue.Write(((dst_y + IMG_H) << 16) | (dst_x + IMG_W));
			queue.Write(dst_base);
			queue.Write(0);
			queue.Write(src_pitch);
			queue.Write(src_base);
		}

		frame++;
		fps_cnt++;
		bigtime_t now = system_time();
		if (now - fps_t0 >= 1000000) {
			float fps = (float)fps_cnt * 1e6f / (float)(now - fps_t0);
			printf("  frame %u: %.1f FPS, %u tiles, gpu=%ld us\n",
				frame, fps, total_tiles, (long)gpu_us);
			fps_cnt = 0;
			fps_t0 = now;
		}

		// Stop after 600 frames (~20 seconds)
		if (frame >= 600)
			running = false;
	}

	printf("Done, %u frames.\n", frame);
	free(tiles);
	media_pipeline_uninit(&ctx);
	cleanup_gpu();
	return 0;
}
