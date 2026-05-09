/*
 * gpu_triangle — Rotating 3D cube, BLT to screen via kernel ioctl.
 *
 * CPU: 3D transform + projection + per-pixel rasterization (BGRA)
 * GPU: XY_SRC_COPY_BLT from GTT buffer to screen framebuffer
 * Ring commands written to ring memory, kicked via INTEL_RING_WRITE_TAIL ioctl.
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
#include "gpu_bo.h"

extern accelerant_info* gInfo;

#define IMG_W 480
#define IMG_H 480
#define IMG_BYTES (IMG_W * IMG_H * 4)

static int sDeviceFd = -1;


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
	gInfo->shared_info_area = clone_area("cube shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(sDeviceFd); return false;
	}
	gInfo->regs_area = clone_area("cube regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(sDeviceFd); return false;
	}
	return true;
}

static void cleanup_gpu(void) {
	if (!gInfo) return;
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo); gInfo = NULL;
	close(sDeviceFd); sDeviceFd = -1;
}


// ---- Ring helpers via ioctl ----

static status_t
ring_reset(void)
{
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	return ioctl(sDeviceFd, INTEL_RING_RESET, &data, sizeof(data));
}

static status_t
ring_kick(uint32 tail)
{
	intel_ring_tail data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	data.tail_value = tail;
	return ioctl(sDeviceFd, INTEL_RING_WRITE_TAIL, &data, sizeof(data));
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


// Write XY_SRC_COPY_BLT to ring memory and kick.
// src_gtt: GTT offset of source RGBA buffer
// Returns new ring position after the command.
static uint32
blt_to_screen(uint32 ring_pos, uint32 src_gtt,
	uint32 dst_x, uint32 dst_y)
{
	intel_shared_info& si = *gInfo->shared_info;
	ring_buffer& ring = si.primary_ring_buffer;
	uint32* cmd = (uint32*)(ring.base + ring_pos);

	// XY_SRC_COPY_BLT: 8 DWORDs
	cmd[0] = XY_COMMAND_SOURCE_BLIT | COMMAND_BLIT_RGBA;
	cmd[1] = si.bytes_per_row | (0xCC << 16)
		| ((uint32)COMMAND_MODE_RGB32 << 24);
	cmd[2] = (dst_y << 16) | dst_x;
	cmd[3] = ((dst_y + IMG_H) << 16) | (dst_x + IMG_W);
	cmd[4] = si.frame_buffer_offset;
	cmd[5] = 0;				// src y/x = 0,0
	cmd[6] = IMG_W * 4;		// src pitch
	cmd[7] = src_gtt;			// src base GTT offset
	asm volatile("mfence" ::: "memory");

	uint32 new_pos = (ring_pos + 32) & (ring.size - 1);
	ring_kick(new_pos);
	return new_pos;
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
	printf("=== GPU 3D Cube — BLT via Kernel Ioctl ===\n");
	printf("  %dx%d, CPU raster + XY_SRC_COPY_BLT\n\n", IMG_W, IMG_H);

	if (!init_gpu()) { printf("GPU init failed\n"); return 1; }

	intel_shared_info& si = *gInfo->shared_info;
	uint32 scr_w = si.current_mode.timing.h_display;
	uint32 scr_h = si.current_mode.timing.v_display;
	printf("Screen: %ux%u, bpr=%u, fb_offset=0x%x\n",
		scr_w, scr_h, si.bytes_per_row, (unsigned)si.frame_buffer_offset);

	// Allocate GTT-mapped buffer for RGBA render output
	gpu_bo render_bo;
	if (gpu_bo_alloc(&render_bo, "cube:render", IMG_BYTES, 4096) != B_OK) {
		printf("render BO alloc failed\n");
		cleanup_gpu(); return 1;
	}
	printf("render_bo: GTT=0x%x, %u bytes\n", render_bo.gtt_offset, IMG_BYTES);

	// Reset ring via ioctl
	if (ring_reset() != 0) {
		printf("RING_RESET ioctl failed\n");
		gpu_bo_free(&render_bo);
		cleanup_gpu(); return 1;
	}
	printf("Ring reset OK\n");

	uint32 dst_x = (scr_w - IMG_W) / 2;
	uint32 dst_y = (scr_h - IMG_H) / 2;

	// BLT test: red rectangle
	{
		uint32* px = (uint32*)render_bo.cpu_addr;
		for (uint32 i = 0; i < IMG_W * IMG_H; i++)
			px[i] = 0x00FF0000;  // BGRX red
		asm volatile("mfence" ::: "memory");

		uint32 pos = blt_to_screen(0, render_bo.gtt_offset, dst_x, dst_y);
		bool ok = ring_wait_idle(100000);
		printf("BLT test: red square at (%u,%u), %s\n",
			dst_x, dst_y, ok ? "GPU completed!" : "TIMEOUT");
		if (!ok) {
			ring_buffer& ring = si.primary_ring_buffer;
			uint32 head = read32(ring.register_base + RING_BUFFER_HEAD)
				& INTEL_RING_BUFFER_HEAD_MASK;
			printf("  HEAD=0x%x (expected 0x%x)\n", head, pos);
		}
		printf("Check screen! Waiting 2 seconds...\n");
		snooze(2000000);
	}

	// Main render loop with BLT display
	uint32 ring_pos = 0;
	ring_reset();

	uint32 frame = 0;
	uint32 fps_cnt = 0;
	bigtime_t fps_t0 = system_time();
	const bigtime_t frame_time = 16667;  // ~60 FPS

	printf("Rendering at (%u,%u)... Press Ctrl+C to stop.\n\n", dst_x, dst_y);

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
		uint32* fb = (uint32*)render_bo.cpu_addr;
		uint32 bg = 0x002E1A1A;
		for (uint32 i = 0; i < IMG_W * IMG_H; i++)
			fb[i] = bg;

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

			uint32 r = (uint32)(face_colors[f][0] * light);
			uint32 g = (uint32)(face_colors[f][1] * light);
			uint32 bl = (uint32)(face_colors[f][2] * light);
			uint32 bgrx = (r << 16) | (g << 8) | bl;

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
				uint32* row = fb + py * IMG_W;
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

		// BLT render buffer to screen
		asm volatile("mfence" ::: "memory");
		ring_pos = blt_to_screen(ring_pos, render_bo.gtt_offset,
			dst_x, dst_y);
		ring_wait_idle(50000);

		// Reset ring when it gets near the end (leave 4KB margin)
		if (ring_pos > gInfo->shared_info->primary_ring_buffer.size - 4096) {
			ring_wait_idle(100000);
			ring_reset();
			ring_pos = 0;
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

		bigtime_t elapsed = system_time() - frame_start;
		if (elapsed < frame_time)
			snooze(frame_time - elapsed);
	}

	ring_wait_idle(100000);
	printf("Done, %u frames.\n", frame);
	gpu_bo_free(&render_bo);
	cleanup_gpu();
	return 0;
}
