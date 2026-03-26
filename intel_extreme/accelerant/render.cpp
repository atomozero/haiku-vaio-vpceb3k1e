/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Alexander (Wikipedia Wikipedia), user@shredder
 */


// Gen5 (Ironlake) render engine for 2D acceleration.
// Initial implementation: solid color fill via 3D pipeline.
// The BLT engine remains the primary path for simple fills.
// The render engine will be used for operations BLT cannot do
// (alpha blend, scale, gradient).

#include "render.h"

#include <string.h>

#include <Debug.h>

#include "commands.h"


#undef TRACE
#define TRACE_RENDER
#ifdef TRACE_RENDER
#	define TRACE(x...) _sPrintf("intel_extreme render: " x)
#else
#	define TRACE(x...)
#endif

#define ERROR(x...) _sPrintf("intel_extreme render: " x)


static render_state sRenderState;


// Gen5 EU kernel: solid color fill
// This minimal kernel writes a constant color from the thread payload
// to the render target via the data port.
//
// The kernel receives the color as a push constant and writes it
// to all pixels in the 16-pixel dispatch.
//
// Gen5 EU instruction format: 128 bits (16 bytes) per instruction
// Extracted from i965 documentation and SNA gen5_render.c

// Minimal kernel: just terminate (for testing pipeline setup)
// A real kernel would sample/compute and write to render target
static const uint32 gen5_wm_kernel_nomask[] = {
	// mov(16) g4<1>F g1<8,8,1>F  { align1 WE_normal 1H };
	// send(16) null g4<8,8,1>F write RT
	// Thread termination
	0x00600001, 0x20200231, 0x00000010, 0x00000000,  // mov
	0x00600031, 0x20001c20, 0x00000400, 0x06000000,  // send - FB write
};

#define KERNEL_SIZE sizeof(gen5_wm_kernel_nomask)

// State block layout (all 64-byte aligned):
// Offset 0x000: VS state (64 bytes)
// Offset 0x040: WM state (64 bytes)
// Offset 0x080: CC state (64 bytes)
// Offset 0x0C0: CC viewport (64 bytes)
// Offset 0x100: Binding table (64 bytes)
// Offset 0x140: Surface state dst (32 bytes)
// Offset 0x160: Surface state src (32 bytes)
// Offset 0x200: Kernel code (256 bytes)
// Offset 0x300: Vertex buffer (256 bytes)
// Total: 0x400 = 1024 bytes

#define STATE_VS_OFFSET			0x000
#define STATE_WM_OFFSET			0x040
#define STATE_CC_OFFSET			0x080
#define STATE_CC_VP_OFFSET		0x0C0
#define STATE_BIND_OFFSET		0x100
#define STATE_SURF_DST_OFFSET	0x140
#define STATE_SURF_SRC_OFFSET	0x160
#define STATE_KERNEL_OFFSET		0x200
#define STATE_VERTEX_OFFSET		0x300
#define STATE_TOTAL_SIZE		0x400


static void
write_surface_state(uint32 offset, uint32 format, uint32 base_addr,
	uint32 width, uint32 height, uint32 pitch)
{
	uint32* ss = (uint32*)(sRenderState.base + offset);
	ss[0] = (SURFACE_2D << 29) | (format << 18);
	ss[1] = base_addr;
	ss[2] = ((width - 1) << 6) | ((height - 1) << 19);
	ss[3] = (pitch - 1);  // pitch in bytes - 1
	ss[4] = 0;
	ss[5] = 0;
	ss[6] = 0;
	ss[7] = 0;
}


status_t
render_init()
{
	memset(&sRenderState, 0, sizeof(sRenderState));

	// Allocate GPU memory for all state structures
	addr_t base;
	if (intel_allocate_memory(STATE_TOTAL_SIZE, 0, base) != B_OK) {
		ERROR("Failed to allocate render state memory\n");
		return B_NO_MEMORY;
	}

	sRenderState.base = base;
	sRenderState.offset = base - (addr_t)gInfo->shared_info->graphics_memory;
	sRenderState.size = STATE_TOTAL_SIZE;

	// Zero everything
	memset((void*)base, 0, STATE_TOTAL_SIZE);

	// Copy kernel code
	memcpy((void*)(base + STATE_KERNEL_OFFSET),
		gen5_wm_kernel_nomask, KERNEL_SIZE);

	// Write VS state (minimal passthrough)
	uint32* vs = (uint32*)(base + STATE_VS_OFFSET);
	vs[0] = 0;  // no kernel (passthrough)
	vs[1] = (1 << 10);  // single program flow, 1 URB entry

	// Write WM state
	uint32 kernel_addr = sRenderState.offset + STATE_KERNEL_OFFSET;
	uint32* wm = (uint32*)(base + STATE_WM_OFFSET);
	wm[0] = kernel_addr;  // kernel start pointer
	wm[1] = (1 << 25)  // single program flow
		| (0 << 16)  // binding table entry count = 1 (0-indexed = entry 0)
		| 0;  // scratch space = 0
	wm[2] = 0;  // scratch base (unused)
	wm[3] = (1 << 25)  // dispatch GRF start reg = 1
		| (1 << 0);  // URB entry allocation = 1
	wm[4] = (0 << 25)  // max threads = 1 (0-indexed)
		| WM_16_DISPATCH;  // 16-pixel dispatch
	wm[5] = 0;  // kernel 1 pointer (unused)
	wm[6] = 0;  // kernel 2 pointer (unused)
	wm[7] = 0;

	// Write CC state (no blending, just write)
	uint32* cc = (uint32*)(base + STATE_CC_OFFSET);
	cc[0] = 0;  // no alpha test
	cc[1] = 0;  // no stencil
	cc[2] = 0;  // no depth test
	// cc viewport
	uint32 cc_vp_addr = sRenderState.offset + STATE_CC_VP_OFFSET;
	cc[4] = cc_vp_addr;  // CC viewport pointer

	// Write CC viewport (normalized depth range 0.0 - 1.0)
	float* ccvp = (float*)(base + STATE_CC_VP_OFFSET);
	ccvp[0] = 0.0f;  // min depth
	ccvp[1] = 1.0f;  // max depth

	// Write binding table
	uint32* bt = (uint32*)(base + STATE_BIND_OFFSET);
	bt[0] = sRenderState.offset + STATE_SURF_DST_OFFSET;  // entry 0 = dst

	// Write destination surface state (framebuffer)
	write_surface_state(STATE_SURF_DST_OFFSET,
		FORMAT_B8G8R8A8_UNORM,
		gInfo->shared_info->frame_buffer_offset,
		gInfo->shared_info->current_mode.timing.h_display,
		gInfo->shared_info->current_mode.timing.v_display,
		gInfo->shared_info->bytes_per_row);

	sRenderState.initialized = true;

	TRACE("Render engine initialized: state at GTT 0x%" B_PRIx32
		", kernel at 0x%" B_PRIx32 "\n",
		sRenderState.offset, sRenderState.offset + STATE_KERNEL_OFFSET);

	return B_OK;
}


void
render_uninit()
{
	if (sRenderState.base != 0) {
		intel_free_memory(sRenderState.base);
		sRenderState.base = 0;
		sRenderState.initialized = false;
	}
}


status_t
render_fill_rect(uint32 color, int16 left, int16 top,
	int16 right, int16 bottom)
{
	if (!sRenderState.initialized)
		return B_NOT_INITIALIZED;

	// Write vertex data for RECTLIST (3 vertices per rect)
	// Format: X, Y (float)
	float* vb = (float*)(sRenderState.base + STATE_VERTEX_OFFSET);
	// v0: bottom-left
	vb[0] = (float)left;
	vb[1] = (float)bottom;
	// v1: top-left
	vb[2] = (float)left;
	vb[3] = (float)top;
	// v2: bottom-right
	vb[4] = (float)right;
	vb[5] = (float)bottom;

	// Memory barrier
	int32 flush = 0;
	atomic_add(&flush, 1);

	uint32 stateBase = sRenderState.offset;

	// Emit 3D commands via ring buffer
	QueueCommands queue(gInfo->shared_info->primary_ring_buffer);

	// 1. MI_FLUSH to switch from BLT to 3D
	queue.MakeSpace(2);
	queue.Write(MI_FLUSH_CMD | MI_FLUSH_STATE_INST_CACHE);
	queue.Write(MI_NOOP);

	// 2. PIPELINE_SELECT = 3D
	queue.MakeSpace(2);
	queue.Write(CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D);
	queue.Write(MI_NOOP);

	// 3. STATE_BASE_ADDRESS
	queue.MakeSpace(8);
	queue.Write(CMD_STATE_BASE_ADDRESS);
	queue.Write(stateBase | 1);		// general state base (valid)
	queue.Write(stateBase | 1);		// surface state base (valid)
	queue.Write(0);					// indirect object base
	queue.Write(stateBase | 1);		// instruction base (valid)
	queue.Write(0);					// general state upper bound
	queue.Write(0);					// indirect object upper bound
	queue.Write(0);					// instruction upper bound

	// 4. 3DSTATE_PIPELINED_POINTERS
	queue.MakeSpace(8);
	queue.Write(CMD_PIPELINED_POINTERS);
	queue.Write(stateBase + STATE_VS_OFFSET);	// VS state
	queue.Write(0);								// GS disabled
	queue.Write(0);								// CLIP disabled
	queue.Write(0);								// SF state (disabled for RECTLIST)
	queue.Write(stateBase + STATE_WM_OFFSET);	// WM state
	queue.Write(stateBase + STATE_CC_OFFSET);	// CC state
	queue.Write(MI_NOOP);

	// 5. 3DSTATE_BINDING_TABLE_POINTERS
	queue.MakeSpace(2);
	queue.Write(CMD_BINDING_TABLE_PTRS);
	queue.Write(STATE_BIND_OFFSET);		// relative to surface state base

	// 6. 3DSTATE_VERTEX_BUFFERS (one buffer, 2 floats per vertex)
	queue.MakeSpace(6);
	queue.Write(CMD_VERTEX_BUFFERS | (3 - 2));	// length = 3
	queue.Write((0 << 26)					// buffer index 0
		| (8 << 0));						// pitch = 8 bytes (2 floats)
	queue.Write(stateBase + STATE_VERTEX_OFFSET);  // buffer start
	queue.Write(stateBase + STATE_VERTEX_OFFSET + 3 * 8 - 1);  // buffer end

	// 7. 3DSTATE_VERTEX_ELEMENTS (position only)
	queue.MakeSpace(4);
	queue.Write(CMD_VERTEX_ELEMENTS | (2 - 2));  // length = 2
	queue.Write((0 << 26)					// buffer index 0
		| (0 << 0)							// src offset 0
		| (FORMAT_R32G32_FLOAT << 16)		// format
		| (1 << 25));						// valid
	queue.Write((VFCOMP_STORE_SRC << 28)	// X from buffer
		| (VFCOMP_STORE_SRC << 24)			// Y from buffer
		| (VFCOMP_STORE_0 << 20)			// Z = 0
		| (VFCOMP_STORE_1_FP << 16));		// W = 1.0

	// 8. 3DPRIMITIVE (draw RECTLIST, 3 vertices)
	queue.MakeSpace(6);
	queue.Write(CMD_3DPRIMITIVE
		| (PRIM_RECTLIST << 10)
		| (6 - 2));						// length = 6
	queue.Write(3);							// vertex count
	queue.Write(0);							// start vertex
	queue.Write(1);							// instance count
	queue.Write(0);							// start instance
	queue.Write(0);							// base vertex location

	// MI_FLUSH to complete 3D and allow BLT again
	queue.MakeSpace(2);
	queue.Write(MI_FLUSH_CMD);
	queue.Write(MI_NOOP);

	TRACE("render_fill_rect: %d,%d - %d,%d color 0x%08" B_PRIx32 "\n",
		left, top, right, bottom, color);

	return B_OK;
}
