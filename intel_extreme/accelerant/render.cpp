/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Alexander (Wikipedia Wikipedia), user@shredder
 */


// Gen5 (Ironlake) render engine for 2D acceleration.
// Solid color fill via the 3D pipeline using a WM kernel with
// immediate MOV instructions.  Color values are patched into the
// kernel binary before each draw.
//
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


// ---------------------------------------------------------------------------
// Gen5 EU kernels (pre-compiled instruction binaries)
// ---------------------------------------------------------------------------

// Gen5 EU instruction format: 128 bits (16 bytes) per instruction.
// Encoding derived from brw_instruction bitfield layout in brw_eu.h
// (xf86-video-intel / Mesa i965).
//
// DW0 bits: [6:0] opcode, [8] access_mode, [9] mask_control (NoMask),
//           [23:21] execution_size (3=SIMD8, 4=SIMD16)
// DW1 bits: [1:0] dest_reg_file (0=ARF,1=GRF,2=MRF), [4:2] dest_reg_type,
//           [6:5] src0_reg_file (1=GRF,3=IMM), [9:7] src0_reg_type,
//           [20:16] dest_subreg_nr, [28:21] dest_reg_nr, [30:29] dest_hstride
// DW2 bits: src0 fields (da1 format) or send_gen5 descriptor
// DW3 bits: src1 or message descriptor


// SF kernel: minimal thread termination.
// Sends URB_WRITE with EOT, complete, and used flags.  The fixed-function
// rasterizer handles scan conversion for RECTLIST; the SF kernel only
// needs to signal completion (no attribute interpolation for solid fill).
//
// send(8) null:UW r0:UW  URB_WRITE  EOT complete used
static const uint32 gen5_sf_kernel[] = {
	// DW0: SEND(0x31), SIMD8(3<<21)
	// DW1: dest=null(ARF=0, reg=0, type=UW=2, hstride=1),
	//       src0=GRF(1), type=UW(2)
	// DW2: SFID=URB(6) bits[31:28], EOT=1 bit[26],
	//       src0=r0 vec8(vstride=8,width=8,hstride=1)
	// DW3: urb msg: EOT=1 bit[31], msg_length=1 bits[28:25],
	//       header_present=1 bit[13], complete=1 bit[11],
	//       used=1 bit[10], opcode=0 bit[0]
	0x00600031, 0x20000128, 0x648D0000, 0x82002C00,
};

#define SF_KERNEL_SIZE sizeof(gen5_sf_kernel)


// WM kernel: solid color fill, SIMD8 dispatch.
// Loads RGBA color as immediate float values (patched per fill),
// copies the thread header to m1, and sends FB_WRITE with EOT.
//
// Thread payload: r0 = header, r1 = pixel X/Y (barycentric)
// Output: m1 (header copy) + m2-m5 (R,G,B,A as float) -> FB_WRITE
//
// The color immediates in instructions 1-4 (DW3 of each) are
// overwritten by render_patch_color() before every draw.
static const uint32 gen5_wm_kernel_solid[] = {
	// inst 0: mov(8) m1<1>:UD  r0<8,8,1>:UD  {NoMask}
	//   Copy thread header for FB_WRITE message.
	0x00600201, 0x20200022, 0x008D0000, 0x00000000,

	// inst 1: mov(8) m2<1>:F  <imm>:F  {NoMask}  -- Red
	//   DW1: dest_file=MRF(2), type=F(7), dest_reg=2, hstride=1,
	//         src0_file=IMM(3), src0_type=F(7)
	0x00600201, 0x204003FE, 0x00000000, 0x3F800000,

	// inst 2: mov(8) m3<1>:F  <imm>:F  {NoMask}  -- Green
	0x00600201, 0x206003FE, 0x00000000, 0x3F800000,

	// inst 3: mov(8) m4<1>:F  <imm>:F  {NoMask}  -- Blue
	0x00600201, 0x208003FE, 0x00000000, 0x3F800000,

	// inst 4: mov(8) m5<1>:F  <imm>:F  {NoMask}  -- Alpha
	0x00600201, 0x20A003FE, 0x00000000, 0x3F800000,

	// inst 5: send(8) null:UW  r0:UW  FB_WRITE  EOT
	//   DW2: SFID=DATAPORT_WRITE(5) bits[31:28], EOT=1 bit[26],
	//         src0=r0 vec8
	//   DW3: EOT=1 bit[31], msg_length=6 bits[28:25],
	//         header_present=1 bit[19], msg_type=RT_WRITE(4) bits[14:12],
	//         last_render_target=1 bit[11],
	//         msg_control=SIMD8_SS01(2) bits[10:8],
	//         binding_table_index=0 bits[7:0]
	0x00600031, 0x20000128, 0x548D0000, 0x8C084A00,
};

#define WM_KERNEL_SIZE sizeof(gen5_wm_kernel_solid)

// Byte offsets of the RGBA immediate values within gen5_wm_kernel_solid.
// Each instruction is 16 bytes; the immediate is DW3 (byte 12) of each.
#define WM_KERNEL_RED_OFFSET	(1 * 16 + 12)
#define WM_KERNEL_GREEN_OFFSET	(2 * 16 + 12)
#define WM_KERNEL_BLUE_OFFSET	(3 * 16 + 12)
#define WM_KERNEL_ALPHA_OFFSET	(4 * 16 + 12)


// ---------------------------------------------------------------------------
// State block layout (all offsets 64-byte aligned):
//
//   0x000  VS state (64 bytes)
//   0x040  WM state (64 bytes)
//   0x080  CC state (64 bytes)
//   0x0C0  CC viewport (64 bytes)
//   0x100  Binding table (64 bytes)
//   0x140  Surface state dst (32 bytes)
//   0x160  Surface state src (32 bytes)
//   0x180  SF state (64 bytes)
//   0x1C0  SF kernel (64 bytes, 16 bytes used)
//   0x200  WM kernel (256 bytes, 96 bytes used)
//   0x300  Vertex buffer (256 bytes)
//   Total  0x400 = 1024 bytes
// ---------------------------------------------------------------------------

#define STATE_VS_OFFSET			0x000
#define STATE_WM_OFFSET			0x040
#define STATE_CC_OFFSET			0x080
#define STATE_CC_VP_OFFSET		0x0C0
#define STATE_BIND_OFFSET		0x100
#define STATE_SURF_DST_OFFSET	0x140
#define STATE_SURF_SRC_OFFSET	0x160
#define STATE_SF_OFFSET			0x180
#define STATE_SF_KERNEL_OFFSET	0x1C0
#define STATE_WM_KERNEL_OFFSET	0x200
#define STATE_VERTEX_OFFSET		0x300
#define STATE_TOTAL_SIZE		0x400

// Ironlake SF/WM max threads (from PRM)
#define ILK_SF_MAX_THREADS		48
#define ILK_WM_MAX_THREADS		72


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


// Convert uint32 BGRA color to float and patch the WM kernel immediates.
static void
render_patch_color(uint32 color)
{
	uint8* kernel = (uint8*)(sRenderState.base + STATE_WM_KERNEL_OFFSET);

	// Haiku uses BGRA8888: color = 0xAARRGGBB
	float red   = ((color >> 16) & 0xFF) / 255.0f;
	float green = ((color >>  8) & 0xFF) / 255.0f;
	float blue  = ((color >>  0) & 0xFF) / 255.0f;
	float alpha = ((color >> 24) & 0xFF) / 255.0f;

	// Patch DW3 of instructions 1-4 (the immediate float values)
	memcpy(kernel + WM_KERNEL_RED_OFFSET,   &red,   sizeof(float));
	memcpy(kernel + WM_KERNEL_GREEN_OFFSET, &green, sizeof(float));
	memcpy(kernel + WM_KERNEL_BLUE_OFFSET,  &blue,  sizeof(float));
	memcpy(kernel + WM_KERNEL_ALPHA_OFFSET, &alpha, sizeof(float));
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

	// ----- Copy kernel code -----
	memcpy((void*)(base + STATE_SF_KERNEL_OFFSET),
		gen5_sf_kernel, SF_KERNEL_SIZE);
	memcpy((void*)(base + STATE_WM_KERNEL_OFFSET),
		gen5_wm_kernel_solid, WM_KERNEL_SIZE);

	uint32 stateOff = sRenderState.offset;

	// ----- VS state (minimal passthrough) -----
	uint32* vs = (uint32*)(base + STATE_VS_OFFSET);
	vs[0] = 0;  // no kernel (passthrough)
	vs[1] = (1 << 10);  // single program flow, 1 URB entry

	// ----- SF state -----
	// Gen5 SF unit state (brw_sf_unit_state): 8 DWORDs
	//   DW0 thread0: bits[31:6] = kernel_start_pointer (addr >> 6),
	//                bits[3:1] = grf_reg_count
	//   DW1 sf1: (defaults)
	//   DW2: scratch space (0)
	//   DW3 sf3: bits[4:1] = dispatch_grf_start_reg,
	//            bits[10:5] = urb_entry_read_offset,
	//            bits[18:12] = urb_entry_read_length,
	//            bits[25:20] = const_urb_entry_read_offset,
	//            bits[31:26] = const_urb_entry_read_length
	//   DW4 sf4: bits[16:9] = nr_urb_entries,
	//            bits[22:18] = urb_entry_allocation_size,
	//            bits[30:25] = max_threads
	//   DW5 sf5: viewport transform etc.
	//   DW6 sf6: cull mode, dest org bias
	//   DW7 sf7: trifan pv
	uint32 sfKernelOff = stateOff + STATE_SF_KERNEL_OFFSET;
	uint32* sf = (uint32*)(base + STATE_SF_OFFSET);
	sf[0] = sfKernelOff;  // kernel pointer (64-byte aligned, grf_count=0)
	sf[1] = 0;
	sf[2] = 0;
	sf[3] = (3 << 1)	// dispatch_grf_start_reg = 3
		| (1 << 5)		// urb_entry_read_offset = 1 (skip VUE header)
		| (1 << 12);	// urb_entry_read_length = 1 (position only)
	sf[4] = ((ILK_SF_MAX_THREADS - 1) << 25)	// max threads
		| (1 << 18)								// urb_entry_allocation_size = 1
		| (64 << 9);							// nr_urb_entries = 64
	sf[5] = 0;
	sf[6] = (2 << 29)	// cull mode = NONE (Gen5: 2 = CULL_NONE)
		| (0x8 << 22)	// dest_org_vbias = 0x8 (SNA default)
		| (0x8 << 16);	// dest_org_hbias = 0x8
	sf[7] = (2 << 12);	// trifan_pv = 2

	// ----- WM state -----
	// Gen5 WM unit state: 8 DWORDs
	//   DW0 thread0: bits[31:6] = kernel_start_pointer (addr >> 6),
	//                bits[3:1] = grf_reg_count
	//   DW1 thread1: bit[25] = single_program_flow
	//   DW2: scratch space
	//   DW3 thread3: bits[3:0] = dispatch_grf_start_reg,
	//                bits[9:4] = urb_entry_read_offset,
	//                bits[17:11] = urb_entry_read_length,
	//                bits[25:20] = const_urb_entry_read_offset,
	//                bits[31:26] = const_urb_entry_read_length
	//   DW4 wm4: bits[0:0] = sampler_count (0 = no samplers),
	//            bits[30:25] = max_threads
	//   DW5 wm5: bit[0] = enable_8_pix,
	//            bit[29] = thread_dispatch_enable,
	//            bit[30] = early_depth_test
	uint32 wmKernelOff = stateOff + STATE_WM_KERNEL_OFFSET;
	uint32* wm = (uint32*)(base + STATE_WM_OFFSET);
	wm[0] = wmKernelOff;	// kernel start pointer (grf_count=0)
	wm[1] = (1 << 25);		// single program flow
	wm[2] = 0;				// no scratch space
	wm[3] = (1 << 0)		// dispatch_grf_start_reg = 1
		| (1 << 4)			// urb_entry_read_offset = 1
		| (1 << 11);		// urb_entry_read_length = 1
	wm[4] = ((ILK_WM_MAX_THREADS - 1) << 25);	// max threads, no samplers
	wm[5] = (1 << 29)		// thread_dispatch_enable
		| WM_8_DISPATCH;	// 8-pixel dispatch (matches SIMD8 kernel)

	// ----- CC state (no blending, just write) -----
	uint32* cc = (uint32*)(base + STATE_CC_OFFSET);
	cc[0] = 0;  // no alpha test
	cc[1] = 0;  // no stencil
	cc[2] = 0;  // no depth test
	cc[3] = 0;
	uint32 ccVpOff = stateOff + STATE_CC_VP_OFFSET;
	cc[4] = ccVpOff;  // CC viewport pointer

	// CC viewport (normalized depth range)
	float* ccvp = (float*)(base + STATE_CC_VP_OFFSET);
	ccvp[0] = 0.0f;  // min depth
	ccvp[1] = 1.0f;  // max depth

	// ----- Binding table -----
	uint32* bt = (uint32*)(base + STATE_BIND_OFFSET);
	bt[0] = stateOff + STATE_SURF_DST_OFFSET;  // entry 0 = dst

	// NOTE: surface state is NOT written here because render_init() runs
	// before any display mode is set, so frame_buffer_offset/bytes_per_row
	// are not yet valid.  The surface state is updated lazily in
	// render_update_surface() before each draw call.

	sRenderState.initialized = true;

	TRACE("Render engine initialized: state at GTT 0x%" B_PRIx32
		", SF kernel at 0x%" B_PRIx32
		", WM kernel at 0x%" B_PRIx32 "\n",
		stateOff, stateOff + STATE_SF_KERNEL_OFFSET,
		stateOff + STATE_WM_KERNEL_OFFSET);

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


static void
render_update_surface()
{
	// Update destination surface state to match current framebuffer.
	// Must be called before each draw because the framebuffer may have
	// changed since render_init() (mode switch, resolution change).
	intel_shared_info &info = *gInfo->shared_info;

	write_surface_state(STATE_SURF_DST_OFFSET,
		FORMAT_B8G8R8A8_UNORM,
		info.frame_buffer_offset,
		info.current_mode.timing.h_display,
		info.current_mode.timing.v_display,
		info.bytes_per_row);
}


status_t
render_fill_rect(uint32 color, int16 left, int16 top,
	int16 right, int16 bottom)
{
	if (!sRenderState.initialized)
		return B_NOT_INITIALIZED;

	render_update_surface();
	render_patch_color(color);

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

	// Emit 3D commands via ring buffer (scoped so destructor submits)
	{
		QueueCommands queue(gInfo->shared_info->primary_ring_buffer);

		// 1. MI_FLUSH to switch from BLT to 3D
		queue.MakeSpace(2);
		queue.Write(MI_FLUSH_CMD | MI_FLUSH_STATE_INST_CACHE);
		queue.Write(MI_NOOP);

		// 2. PIPELINE_SELECT = 3D
		queue.MakeSpace(2);
		queue.Write(CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D);
		queue.Write(MI_NOOP);

		// 3. STATE_BASE_ADDRESS - all bases = 0 so all pointers are
		//    absolute GTT offsets (same approach as SNA/xf86-video-intel)
		queue.MakeSpace(8);
		queue.Write(CMD_STATE_BASE_ADDRESS);
		queue.Write(0 | 1);				// general state base = 0 (valid)
		queue.Write(0 | 1);				// surface state base = 0 (valid)
		queue.Write(0);					// indirect object base
		queue.Write(0 | 1);				// instruction base = 0 (valid)
		queue.Write(0);					// general state upper bound
		queue.Write(0);					// indirect object upper bound
		queue.Write(0);					// instruction upper bound

		// 4. 3DSTATE_PIPELINED_POINTERS (7 DWORDs)
		queue.MakeSpace(8);
		queue.Write(CMD_PIPELINED_POINTERS);
		queue.Write(stateBase + STATE_VS_OFFSET);	// VS state
		queue.Write(0);								// GS disabled
		queue.Write(0);								// CLIP disabled
		queue.Write(stateBase + STATE_SF_OFFSET);	// SF state
		queue.Write(stateBase + STATE_WM_OFFSET);	// WM state
		queue.Write(stateBase + STATE_CC_OFFSET);	// CC state
		queue.Write(MI_NOOP);

		// 5. 3DSTATE_DRAWING_RECTANGLE - set clip rect to framebuffer
		{
			intel_shared_info &info = *gInfo->shared_info;
			queue.MakeSpace(4);
			queue.Write(CMD_DRAWING_RECTANGLE);
			queue.Write(0);		// top-left: (0, 0)
			queue.Write(((info.current_mode.timing.v_display - 1) << 16)
				| (info.current_mode.timing.h_display - 1));
			queue.Write(0);		// origin: (0, 0)
		}

		// 6. 3DSTATE_BINDING_TABLE_POINTERS
		queue.MakeSpace(2);
		queue.Write(CMD_BINDING_TABLE_PTRS);
		queue.Write(stateBase + STATE_BIND_OFFSET);

		// 7. 3DSTATE_VERTEX_BUFFERS (one buffer, 2 floats per vertex)
		queue.MakeSpace(6);
		queue.Write(CMD_VERTEX_BUFFERS | (3 - 2));
		queue.Write((0 << 26) | (8 << 0));
		queue.Write(stateBase + STATE_VERTEX_OFFSET);
		queue.Write(stateBase + STATE_VERTEX_OFFSET + 3 * 8 - 1);

		// 7. 3DSTATE_VERTEX_ELEMENTS (position only)
		queue.MakeSpace(4);
		queue.Write(CMD_VERTEX_ELEMENTS | (2 - 2));
		queue.Write((0 << 26) | (FORMAT_R32G32_FLOAT << 16) | (1 << 25));
		queue.Write((VFCOMP_STORE_SRC << 28) | (VFCOMP_STORE_SRC << 24)
			| (VFCOMP_STORE_0 << 20) | (VFCOMP_STORE_1_FP << 16));

		// 8. 3DPRIMITIVE (draw RECTLIST, 3 vertices)
		queue.MakeSpace(6);
		queue.Write(CMD_3DPRIMITIVE | (PRIM_RECTLIST << 10) | (6 - 2));
		queue.Write(3);		// vertex count
		queue.Write(0);		// start vertex
		queue.Write(1);		// instance count
		queue.Write(0);		// start instance
		queue.Write(0);		// base vertex location

		// MI_FLUSH to complete 3D and allow BLT again
		queue.MakeSpace(2);
		queue.Write(MI_FLUSH_CMD);
		queue.Write(MI_NOOP);
	}
	// QueueCommands destructor has now written TAIL → commands submitted

	TRACE("render_fill_rect: %d,%d - %d,%d color 0x%08" B_PRIx32 "\n",
		left, top, right, bottom, color);

	// Wait for GPU to process, then dump error registers
	snooze(10000);
	uint32 instdone = read32(0x206C);
	uint32 ipeir = read32(0x2064);
	uint32 ipehr = read32(0x2068);
	uint32 eir = read32(0x20B0);
	TRACE("GPU diag: INSTDONE=0x%08" B_PRIx32 " IPEIR=0x%08" B_PRIx32
		" IPEHR=0x%08" B_PRIx32 " EIR=0x%08" B_PRIx32 "\n",
		instdone, ipeir, ipehr, eir);

	return B_OK;
}
