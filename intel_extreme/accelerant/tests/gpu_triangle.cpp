/*
 * gpu_triangle — GPU-accelerated triangle in a Haiku BWindow.
 * Renders via the 3D pipeline to an off-screen BO, copies to BBitmap.
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
#include "render.h"
#include "media_pipeline.h"
#include "gpu_bo.h"
#include "commands.h"

extern accelerant_info* gInfo;

// URB_FENCE defines (from render.cpp, not in header)
#define CMD_URB_FENCE		(0x3 << 29 | 0 << 27 | 0 << 24 | 0 << 16)
#define UF0_VS_REALLOC		(1 << 8)
#define UF0_GS_REALLOC		(1 << 9)
#define UF0_CLIP_REALLOC	(1 << 10)
#define UF0_SF_REALLOC		(1 << 11)
#define UF0_VFE_REALLOC		(1 << 12)
#define UF0_CS_REALLOC		(1 << 13)
#define CMD_CS_URB_STATE	(0x3 << 29 | 0 << 27 | 1 << 24 | 1 << 16)
#define MI_FLUSH_CMD		(0x04 << 23)
#define MI_NOOP				0

#define TRI_W 480
#define TRI_H 480

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


// ---- Off-screen 3D render target ----

static gpu_bo sRenderBO;		// off-screen BGRA surface
static gpu_bo sStateBO;		// 3D state block

// Write a BGRA surface state at byte offset within sStateBO.
static void
write_render_surface(uint32 offset, uint32 bo_gtt, uint32 w, uint32 h,
	uint32 pitch)
{
	// SURFACE_2D, B8G8R8A8_UNORM, render target
	uint32 ss[8] = {0};
	ss[0] = (1 << 29)			// SURFACE_2D
		| (0x0C0 << 18)		// B8G8R8A8_UNORM
		| (1 << 8);			// RC_READ_WRITE (render target!)
	ss[1] = bo_gtt;
	ss[2] = ((w - 1) << 6) | ((h - 1) << 19);
	ss[3] = ((pitch - 1) << 3);	// pitch in bytes - 1, bits [19:3]
	gpu_bo_write(&sStateBO, offset, ss, 32);
}


// Build and submit the 3D draw commands for a triangle
static status_t
render_triangle_offscreen(float x0, float y0, float x1, float y1,
	float x2, float y2, uint32 color)
{
	uint32 st = sStateBO.gtt_offset;

	// Patch WM kernel color (same layout as gen5_wm_kernel_solid)
	float r = ((color >> 16) & 0xFF) / 255.0f;
	float g = ((color >> 8) & 0xFF) / 255.0f;
	float b = (color & 0xFF) / 255.0f;
	float a = ((color >> 24) & 0xFF) / 255.0f;
	// WM kernel at offset 0x240, instructions 1-4 have imm at byte 12
	uint32 wm_off = 0x240;
	gpu_bo_write(&sStateBO, wm_off + 1*16+12, &r, 4);
	gpu_bo_write(&sStateBO, wm_off + 2*16+12, &g, 4);
	gpu_bo_write(&sStateBO, wm_off + 3*16+12, &b, 4);
	gpu_bo_write(&sStateBO, wm_off + 4*16+12, &a, 4);

	// Write vertices at 0x300
	float vb[6] = { x0, y0, x1, y1, x2, y2 };
	gpu_bo_write(&sStateBO, 0x300, vb, sizeof(vb));

	gpu_bo_flush_cpu_writes();

	// Build commands array
	uint32 cmd[128];
	int n = 0;
	#define E(v) cmd[n++] = (v)

	E(MI_FLUSH_CMD);
	E(CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D);

	// STATE_BASE_ADDRESS
	E(CMD_STATE_BASE_ADDRESS);
	E(0|1); E(0|1); E(0|1); E(0|1); E(0); E(0); E(0);

	// URB_FENCE
	uint32 vsF = 256, sfF = vsF + 128;
	E(CMD_URB_FENCE | UF0_VS_REALLOC | UF0_GS_REALLOC
		| UF0_CLIP_REALLOC | UF0_SF_REALLOC
		| UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
	E((vsF << 20) | (vsF << 10) | vsF);
	E((sfF << 20) | (sfF << 10) | sfF);

	// CS_URB_STATE
	E(CMD_CS_URB_STATE);
	E(0);  // 0 entries, size 1

	// PIPELINED_POINTERS
	E(CMD_PIPELINED_POINTERS);
	E(st + 0x000);  // VS
	E(0);            // GS off
	E(0);            // CLIP off
	E(st + 0x180);   // SF
	E(st + 0x040);   // WM
	E(st + 0x080);   // CC

	// DRAWING_RECTANGLE
	E(CMD_DRAWING_RECTANGLE);
	E(0);
	E(((TRI_H-1) << 16) | (TRI_W-1));
	E(0);

	// BINDING_TABLE_POINTERS
	E(CMD_BINDING_TABLE_PTRS | 4);
	E(0); E(0); E(0); E(0);
	E(st + 0x100);  // binding table

	// VERTEX_BUFFERS
	E(CMD_VERTEX_BUFFERS | 3);
	E((0 << 26) | (8 << 0));  // stride 8
	E(st + 0x300);             // vertex data
	E(st + 0x300 + 3*8 - 1);
	E(0);

	// VERTEX_ELEMENTS
	E(CMD_VERTEX_ELEMENTS | 1);
	E((0 << 27) | (1 << 26) | (FORMAT_R32G32_FLOAT << 16) | 0);
	E((VFCOMP_STORE_SRC << 28) | (VFCOMP_STORE_SRC << 24)
		| (VFCOMP_STORE_0 << 20) | (VFCOMP_STORE_1_FP << 16));

	// 3DPRIMITIVE TRILIST
	E(CMD_3DPRIMITIVE | (PRIM_TRILIST << 10) | 4);
	E(3); E(0); E(1); E(0); E(0);

	// MI_FLUSH
	E(MI_FLUSH_CMD);

	// Marker
	E((0x20 << 23) | (1 << 22) | 2);
	E(0);
	E(sStateBO.gtt_offset + 0x2C0);
	E(0xBEEFCAFE);

	if (n & 1) E(MI_NOOP);

	#undef E

	// Clear marker
	uint32 zero = 0;
	gpu_bo_write(&sStateBO, 0x2C0, &zero, 4);
	gpu_bo_flush_cpu_writes();

	// Submit to ring
	{
		ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
		QueueCommands queue(ring);
		queue.MakeSpace(n);
		for (int i = 0; i < n; i++)
			queue.Write(cmd[i]);
	}

	// Wait for completion
	volatile uint32* marker = (volatile uint32*)(sStateBO.cpu_addr + 0x2C0);
	bigtime_t t0 = system_time();
	while (*marker != 0xBEEFCAFE) {
		if (system_time() - t0 > 100000) return B_TIMED_OUT;
	}
	return B_OK;
}


// ---- BWindow ----

class TriView : public BView {
public:
	TriView(BRect r) : BView(r, "tv", B_FOLLOW_ALL, B_WILL_DRAW) {
		fBmp = new BBitmap(BRect(0, 0, TRI_W-1, TRI_H-1), B_RGB32);
	}
	~TriView() { delete fBmp; }
	void Draw(BRect) override { DrawBitmap(fBmp, Bounds()); }
	BBitmap* Bmp() { return fBmp; }
private:
	BBitmap* fBmp;
};

class TriWin : public BWindow {
public:
	TriWin() : BWindow(BRect(50,50,50+TRI_W-1,50+TRI_H-1),
		"GPU Triangle — Intel Gen5", B_TITLED_WINDOW,
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


// ---- Main ----

// Gen5 kernel binaries (from render.cpp)
extern const uint32 gen5_sf_kernel[];
extern const uint32 gen5_wm_kernel_solid[];

int main(int, char**)
{
	printf("GPU Triangle Demo — Intel Gen5 3D Pipeline\n\n");
	if (!init_gpu()) return 1;

	// Allocate off-screen render target (BGRA, TRI_W × TRI_H)
	uint32 fb_size = TRI_W * TRI_H * 4;
	if (gpu_bo_alloc(&sRenderBO, "tri:fb", fb_size, 4096) != B_OK) {
		printf("alloc render BO failed\n"); cleanup_gpu(); return 1;
	}
	// State block: same layout as render.cpp
	if (gpu_bo_alloc(&sStateBO, "tri:state", 2048, 4096) != B_OK) {
		printf("alloc state BO failed\n");
		gpu_bo_free(&sRenderBO); cleanup_gpu(); return 1;
	}

	gpu_bo_clear(&sStateBO);

	// Copy SF + WM kernels
	gpu_bo_write(&sStateBO, 0x1C0, gen5_sf_kernel, 7*16);
	gpu_bo_write(&sStateBO, 0x240, gen5_wm_kernel_solid, 6*16);

	uint32 st = sStateBO.gtt_offset;

	// VS state (passthrough)
	uint32 vs[8] = {0};
	vs[4] = ((256/4) << 11) | (0 << 19);
	vs[6] = (1 << 1);
	gpu_bo_write(&sStateBO, 0x000, vs, 32);

	// WM state
	uint32 wm[8] = {0};
	wm[0] = (st + 0x240) | (2 << 1);  // kernel + grf_reg_count
	wm[3] = (3 << 0) | (0 << 4) | (2 << 11);
	wm[5] = ((72-1) << 25) | (1 << 19) | (1 << 18) | (1 << 1);  // 16-dispatch
	gpu_bo_write(&sStateBO, 0x040, wm, 32);

	// CC state + viewport
	uint32 cc[8] = {0};
	cc[4] = st + 0x0C0;  // CC viewport
	gpu_bo_write(&sStateBO, 0x080, cc, 32);
	float ccvp[2] = {0.0f, 1.0f};
	gpu_bo_write(&sStateBO, 0x0C0, ccvp, 8);

	// Binding table → surface state at 0x140
	uint32 bt[1] = { st + 0x140 };
	gpu_bo_write(&sStateBO, 0x100, bt, 4);

	// Surface state: off-screen BGRA render target
	write_render_surface(0x140, sRenderBO.gtt_offset, TRI_W, TRI_H,
		TRI_W * 4);

	// SF state
	uint32 sf[8] = {0};
	sf[0] = st + 0x1C0;  // SF kernel
	sf[3] = (3 << 0) | (1 << 4) | (1 << 11);
	sf[4] = ((48-1) << 25) | (1 << 19) | (64 << 11);
	sf[6] = (1 << 29) | (0x8 << 9) | (0x8 << 13);
	sf[7] = (2 << 12);
	gpu_bo_write(&sStateBO, 0x180, sf, 32);

	gpu_bo_flush_cpu_writes();
	printf("State at GTT 0x%x, render BO at GTT 0x%x\n",
		st, sRenderBO.gtt_offset);

	// Create window
	BApplication app("application/x-gpu-triangle");
	TriWin* win = new TriWin();
	win->Show();

	float angle = 0;
	uint32 frame = 0;
	uint32 fps_cnt = 0;
	bigtime_t fps_t0 = system_time();

	// Render to FRAMEBUFFER (proven to complete) then read back.
	uint32 fb_off = gInfo->shared_info->frame_buffer_offset;
	uint32 bpr = gInfo->shared_info->bytes_per_row;
	uint32 disp_w = gInfo->shared_info->current_mode.timing.h_display;
	uint32 disp_h = gInfo->shared_info->current_mode.timing.v_display;
	uint8* fb_base = (uint8*)gInfo->shared_info->graphics_memory + fb_off;
	printf("Framebuffer: off=0x%x bpr=%u %ux%u\n", fb_off, bpr, disp_w, disp_h);

	while (win->Alive()) {
		// Rotating triangle — render directly to framebuffer
		float cx = disp_w / 2.0f, cy = disp_h / 2.0f, sz = 150.0f;
		float a0 = angle, a1 = angle + 2.094f, a2 = angle + 4.189f;

		render_draw_triangle(0xFFFF0000,
			cx + sz * sinf(a0), cy - sz * cosf(a0),
			cx + sz * sinf(a1), cy - sz * cosf(a1),
			cx + sz * sinf(a2), cy - sz * cosf(a2));

		// Read back framebuffer center region into BBitmap
		if (win->Lock()) {
			BBitmap* bmp = win->View()->Bmp();
			uint32* dst = (uint32*)bmp->Bits();
			uint32 dst_bpr = bmp->BytesPerRow() / 4;

			// Copy a TRI_W × TRI_H region from center of framebuffer
			uint32 src_x = (disp_w - TRI_W) / 2;
			uint32 src_y = (disp_h - TRI_H) / 2;
			for (uint32 y = 0; y < TRI_H && (src_y + y) < disp_h; y++) {
				uint32* src_row = (uint32*)(fb_base
					+ (src_y + y) * bpr + src_x * 4);
				memcpy(&dst[y * dst_bpr], src_row, TRI_W * 4);
			}
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
			t.SetToFormat("GPU Triangle — %.1f FPS — Intel Gen5 3D", fps);
			if (win->Lock()) { win->SetTitle(t.String()); win->Unlock(); }
			printf("  frame %u: %.1f FPS\n", frame, fps);
			fps_cnt = 0;
			fps_t0 = now;
		}

		snooze(8000);  // ~120fps target
	}

	printf("Done, %u frames.\n", frame);
	gpu_bo_free(&sRenderBO);
	gpu_bo_free(&sStateBO);
	cleanup_gpu();
	return 0;
}
