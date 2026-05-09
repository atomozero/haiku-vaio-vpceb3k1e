/*
 * gpu_triangle — Rotating 3D cube, direct framebuffer write.
 *
 * CPU: 3D transform + projection + per-pixel rasterization (BGRA)
 * Display: direct memcpy to screen framebuffer (graphics_memory)
 * No ring, no BLT, no MMIO writes — proven path (gpu_plasma_screen).
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

extern accelerant_info* gInfo;

#define IMG_W 480
#define IMG_H 480


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
	float d = 4.0f;
	float scale = 70.0f;
	float persp = d / (d + v.z + 1.5f);
	return {IMG_W/2.0f + v.x * scale * persp,
	        IMG_H/2.0f + v.y * scale * persp};
}


// ---- Cube geometry ----

static const vec3 cube_verts[8] = {
	{-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
	{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};

static const int faces[6][4] = {
	{0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7},
};

static const uint8 face_colors[6][3] = {
	{200,  50,  50}, { 50, 200,  50}, { 50,  50, 200},
	{200, 200,  50}, {200,  50, 200}, { 50, 200, 200},
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

static inline float
face_area(vec2 a, vec2 b, vec2 c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}


int
main(int, char**)
{
	printf("=== GPU 3D Cube — Direct Framebuffer ===\n");
	printf("  %dx%d, 6 faces, per-pixel rasterizer\n\n", IMG_W, IMG_H);

	if (!init_gpu()) { printf("GPU init failed\n"); return 1; }

	intel_shared_info& si = *gInfo->shared_info;
	uint32 scr_w = si.current_mode.timing.h_display;
	uint32 scr_h = si.current_mode.timing.v_display;
	uint32 bpr = si.bytes_per_row;
	printf("Screen: %ux%u, bpr=%u, fb_offset=0x%x\n",
		scr_w, scr_h, bpr, (unsigned)si.frame_buffer_offset);
	printf("graphics_memory: %p\n", si.graphics_memory);

	// Direct framebuffer pointer
	uint8* fb_base = (uint8*)si.graphics_memory + si.frame_buffer_offset;
	printf("Framebuffer at: %p\n", fb_base);

	// Offscreen render buffer (CPU-local, avoid slow WC writes per-pixel)
	uint32* render_buf = (uint32*)malloc(IMG_W * IMG_H * 4);
	if (!render_buf) {
		printf("malloc failed\n");
		cleanup_gpu(); return 1;
	}

	uint32 dst_x = (scr_w - IMG_W) / 2;
	uint32 dst_y = (scr_h - IMG_H) / 2;

	// Quick test: red rectangle
	for (uint32 i = 0; i < IMG_W * IMG_H; i++)
		render_buf[i] = 0x00FF0000;  // BGRX: red
	for (uint32 y = 0; y < IMG_H; y++) {
		memcpy(fb_base + (dst_y + y) * bpr + dst_x * 4,
			render_buf + y * IMG_W, IMG_W * 4);
	}
	printf("Red test square at (%u,%u) — check screen!\n", dst_x, dst_y);
	snooze(1000000);

	uint32 frame = 0;
	uint32 fps_cnt = 0;
	bigtime_t fps_t0 = system_time();

	printf("Rendering... Press Ctrl+C to stop.\n\n");

	const bigtime_t frame_time = 16667;  // ~60 FPS target

	while (frame < 1200) {
		bigtime_t frame_start = system_time();
		float ax = (float)frame * 0.015f;
		float ay = (float)frame * 0.023f;

		// Transform vertices
		vec2 proj[8];
		vec3 rot[8];
		for (int i = 0; i < 8; i++) {
			rot[i] = rotate(cube_verts[i], ax, ay);
			proj[i] = project(rot[i]);
		}

		// Clear to dark background
		uint32 bg = 0x002E1A1A;  // BGRX dark
		for (uint32 i = 0; i < IMG_W * IMG_H; i++)
			render_buf[i] = bg;

		// Sort faces back-to-front
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

		// Rasterize each visible face
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

			// BGRX color for Intel framebuffer (B at byte 0, R at byte 2)
			uint32 r = (uint32)(face_colors[f][0] * light);
			uint32 g = (uint32)(face_colors[f][1] * light);
			uint32 bl = (uint32)(face_colors[f][2] * light);
			uint32 bgrx = (r << 16) | (g << 8) | bl;

			// Bounding box
			float minx = a.x, maxx = a.x, miny = a.y, maxy = a.y;
			float pts[] = {b.x,b.y, c.x,c.y, d.x,d.y};
			for (int p = 0; p < 6; p += 2) {
				if (pts[p] < minx) minx = pts[p];
				if (pts[p] > maxx) maxx = pts[p];
				if (pts[p+1] < miny) miny = pts[p+1];
				if (pts[p+1] > maxy) maxy = pts[p+1];
			}
			int x0 = (int)minx; if (x0 < 0) x0 = 0;
			int y0 = (int)miny; if (y0 < 0) y0 = 0;
			int x1 = (int)maxx + 1; if (x1 > IMG_W) x1 = IMG_W;
			int y1 = (int)maxy + 1; if (y1 > IMG_H) y1 = IMG_H;

			for (int py = y0; py < y1; py++) {
				uint32* row = render_buf + py * IMG_W;
				for (int px = x0; px < x1; px++) {
					float fpx = px + 0.5f;
					float fpy = py + 0.5f;
					if (point_in_tri(a,b,c,fpx,fpy)
						|| point_in_tri(a,c,d,fpx,fpy)) {
						row[px] = bgrx;
					}
				}
			}
		}

		// Copy render buffer to screen framebuffer (row by row for pitch)
		for (uint32 y = 0; y < IMG_H; y++) {
			memcpy(fb_base + (dst_y + y) * bpr + dst_x * 4,
				render_buf + y * IMG_W, IMG_W * 4);
		}

		frame++;
		fps_cnt++;
		bigtime_t now = system_time();
		if (now - fps_t0 >= 1000000) {
			float fps = (float)fps_cnt * 1e6f / (float)(now - fps_t0);
			printf("  frame %u: %.1f FPS\n", frame, fps);
			fps_cnt = 0;
			fps_t0 = now;
		}

		// Frame limiter: wait for vsync-ish timing (~60 FPS)
		bigtime_t elapsed = system_time() - frame_start;
		if (elapsed < frame_time)
			snooze(frame_time - elapsed);
	}

	printf("Done, %u frames.\n", frame);
	free(render_buf);
	cleanup_gpu();
	return 0;
}
