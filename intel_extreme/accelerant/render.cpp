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


// SF kernel: attribute interpolation setup for Gen5.
// From intel-vaapi-driver exa_sf.g4b.gen5 / SNA brw_sf_kernel__nomask.
// Computes attribute deltas (dA/dx, dA/dy) and writes setup data
// to the URB for the WM rasterizer.  7 instructions:
//   0: math_inv  r6 = 1/det (from r1.11)
//   1: mov       m3 = v0 attributes (vertex 0 base values)
//   2: add       r7 = v1 - v2
//   3: mul       m1 = (v1-v2) * inv (dA/dx)
//   4: add       r7 = v2 - v0
//   5: mul       m2 = (v2-v0) * inv (dA/dy)
//   6: send      URB_WRITE EOT (msg_length=4: m0-m3)
static const uint32 gen5_sf_kernel[] = {
	0x00400031, 0x20c01fbd, 0x1069002c, 0x02100001,
	0x00400001, 0x206003be, 0x00690060, 0x00000000,
	0x00400040, 0x20e077bd, 0x00690080, 0x006940a0,
	0x00400041, 0x202077be, 0x006900e0, 0x000000c0,
	0x00400040, 0x20e077bd, 0x006900a0, 0x00694060,
	0x00400041, 0x204077be, 0x006900e0, 0x000000c8,
	0x00600031, 0x20001fbc, 0x648d0000, 0x8808c800,
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

	// inst 5: send(8) null:UW  FB_WRITE  EOT  msg_reg_nr=1
	//   DW0: SEND, SIMD8, msg_reg_nr=1 (bits[27:24]=1)
	//         payload starts at MRF m1 (header,R,G,B,A = m1-m5)
	//   DW2: SFID=DATAPORT_WRITE(5) bits[31:28], EOT=1 bit[26],
	//         src0=r0 vec8
	//   DW3: EOT=1 bit[31], msg_length=6 bits[28:25],
	//         header_present=1 bit[19], msg_type=RT_WRITE(4) bits[14:12],
	//         last_render_target=1 bit[11],
	//         msg_control=SIMD8_SS01(2) bits[10:8],
	//         binding_table_index=0 bits[7:0]
	0x01600031, 0x20000128, 0x548D0000, 0x8C084A00,
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
//   0x1C0  SF kernel (128 bytes, 112 bytes used = 7 instructions)
//   0x240  WM kernel (128 bytes, 96 bytes used = 6 instructions)
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
#define STATE_WM_KERNEL_OFFSET	0x240
#define STATE_VERTEX_OFFSET		0x300
#define STATE_TOTAL_SIZE		0x400

// Ironlake SF/WM max threads (from PRM)
#define ILK_SF_MAX_THREADS		48
#define ILK_WM_MAX_THREADS		72

// URB partitioning (matching SNA gen5_emit_urb)
#define URB_VS_ENTRIES			256
#define URB_VS_ENTRY_SIZE		1
#define URB_SF_ENTRIES			64
#define URB_SF_ENTRY_SIZE		2
#define URB_CS_ENTRIES			0
#define URB_CS_ENTRY_SIZE		1

// URB_FENCE: Type 3 non-pipelined (opcode 0, subopcode 5)
#define CMD_URB_FENCE			GEN5_3D(0, 0, 5)	// 0x60050000
#define UF0_VS_REALLOC			(1 << 9)
#define UF0_GS_REALLOC			(1 << 10)
#define UF0_CLIP_REALLOC		(1 << 11)
#define UF0_SF_REALLOC			(1 << 12)
#define UF0_CS_REALLOC			(1 << 13)

// CS_URB_STATE: Type 3 non-pipelined (opcode 1, subopcode 0)
#define CMD_CS_URB_STATE		GEN5_3D(0, 1, 0)	// 0x61000000


static void
write_surface_state(uint32 offset, uint32 format, uint32 base_addr,
	uint32 width, uint32 height, uint32 pitch, bool is_dst)
{
	uint32* ss = (uint32*)(sRenderState.base + offset);
	ss[0] = (SURFACE_2D << 29) | (format << 18)
		| (is_dst ? (1 << 8) : 0);  // RC_READ_WRITE for render targets
	ss[1] = base_addr;
	ss[2] = ((width - 1) << 6) | ((height - 1) << 19);
	ss[3] = ((pitch - 1) << 3);  // pitch in bytes - 1, bits [19:3]
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

	// Gen5 (Ironlake): re-initialize the render ring and apply
	// workarounds required for 3D command processing.
	// The Haiku kernel driver sets up the ring for BLT only, without
	// the full disable→reset→re-enable cycle that i915 does in
	// init_ring_common().  Without this, Type 3 (3D) commands are
	// silently ignored by the command parser.
	if (gInfo->shared_info->device_type.InGroup(INTEL_GROUP_ILK)) {
		ring_buffer &ring = gInfo->shared_info->primary_ring_buffer;
		uint32 ringReg = ring.register_base;

		// WaIssueDummyWriteToWakeupFromRC6:ilk
		write32(0x209c, 0);

		// Full ring re-initialization (matching i915 init_ring_common)
		// 1. Disable ring
		write32(ringReg + RING_BUFFER_CONTROL, 0);
		read32(ringReg + RING_BUFFER_CONTROL);	// posting read
		snooze(1000);

		// 2. Reset HEAD to 0
		write32(ringReg + RING_BUFFER_HEAD, 0);
		read32(ringReg + RING_BUFFER_HEAD);		// posting read

		// 3. Wait for HEAD to clear
		for (int i = 0; i < 100; i++) {
			if ((read32(ringReg + RING_BUFFER_HEAD)
				& INTEL_RING_BUFFER_HEAD_MASK) == 0)
				break;
			snooze(100);
		}

		// 4. Zero ring buffer memory (stale commands persist across reboots!)
		memset((void*)ring.base, 0, ring.size);

		// 5. Set TAIL = 0
		write32(ringReg + RING_BUFFER_TAIL, 0);

		// 6. Set ring start address (preserve existing)
		write32(ringReg + RING_BUFFER_START, ring.offset);

		// 6. Re-enable ring
		write32(ringReg + RING_BUFFER_CONTROL,
			((ring.size - B_PAGE_SIZE) & INTEL_RING_BUFFER_SIZE_MASK)
			| INTEL_RING_BUFFER_ENABLED);

		// 7. Reset software tracking
		ring.position = 0;
		ring.space_left = ring.size;

		TRACE("Ring re-initialized: CTL=0x%08" B_PRIx32
			" HEAD=0x%08" B_PRIx32 "\n",
			read32(ringReg + RING_BUFFER_CONTROL),
			read32(ringReg + RING_BUFFER_HEAD));

		// Apply Gen5 workarounds (masked register writes)
		// WaTimedSingleVertexDispatch:ilk - MI_MODE bit[6]
		write32(0x209c, (1 << 22) | (1 << 6));

		// Required on all Ironlake steppings (B-Spec)
		// _3D_CHICKEN2 bit[14] = WM_READ_PIPELINED
		write32(0x208c, (1 << 30) | (1 << 14));

		// WaDisable_RenderCache_OperationalFlush:ilk
		// WaDisableRenderCachePipelinedFlush:ilk
		write32(0x2120, (1 << 24) | (1 << 16) | (1 << 8));

		// Clear stale error flags
		write32(0x2064, 0);
		write32(0x2068, 0);

		TRACE("Gen5 workarounds applied: MI_MODE=0x%08" B_PRIx32
			" _3D_CHICKEN2=0x%08" B_PRIx32
			" CACHE_MODE_0=0x%08" B_PRIx32 "\n",
			read32(0x209c), read32(0x208c), read32(0x2120));
	}

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

	// ----- SF state (matching SNA gen5_create_sf_state) -----
	uint32 sfKernelOff = stateOff + STATE_SF_KERNEL_OFFSET;
	uint32* sf = (uint32*)(base + STATE_SF_OFFSET);
	sf[0] = sfKernelOff;	// kernel pointer (grf_count=0)
	sf[1] = 0;
	sf[2] = 0;
	// SF thread3 (brw_structs.h brw_sf_unit_state.thread3):
	//   dispatch_grf_start_reg: bits[3:0], urb_entry_read_offset: bits[9:4],
	//   urb_entry_read_length: bits[16:11]
	sf[3] = (3 << 0)		// dispatch_grf_start_reg = 3
		| (1 << 4)			// urb_entry_read_offset = 1
		| (1 << 11);		// urb_entry_read_length = 1
	// SF thread4: nr_urb_entries: bits[17:11], urb_entry_alloc_size: bits[23:19],
	//   max_threads: bits[30:25]
	sf[4] = ((ILK_SF_MAX_THREADS - 1) << 25)
		| (1 << 19)			// urb_entry_allocation_size = 2-1 = 1
		| (URB_SF_ENTRIES << 11);
	sf[5] = 0;				// viewport_transform = false
	// SF sf6: dest_org_vbias: bits[12:9], dest_org_hbias: bits[16:13],
	//   cull_mode: bits[30:29]
	sf[6] = (1 << 29)		// cull_mode = NONE (GEN5_CULLMODE_NONE = 1)
		| (0x8 << 9)		// dest_org_vbias
		| (0x8 << 13);		// dest_org_hbias
	sf[7] = (2 << 12);		// trifan_pv = 2

	// ----- WM state (matching SNA gen5_init_wm_state) -----
	uint32 wmKernelOff = stateOff + STATE_WM_KERNEL_OFFSET;
	uint32* wm = (uint32*)(base + STATE_WM_OFFSET);
	wm[0] = wmKernelOff	// kernel start pointer
		| (2 << 1);		// grf_reg_count = 2 (3 blocks of 16 regs)
	wm[1] = 0;				// binding_table_entry_count MUST be 0 on Ironlake!
	wm[2] = 0;				// no scratch space
	wm[3] = (3 << 0)		// dispatch_grf_start_reg = 3 (must match SF)
		| (0 << 4)			// urb_entry_read_offset = 0
		| (2 << 11);		// urb_entry_read_length = 2 (matching SNA)
	wm[4] = 0;				// no samplers
	wm[5] = ((ILK_WM_MAX_THREADS - 1) << 25)
		| (1 << 19)			// thread_dispatch_enable
		| (1 << 18)			// early_depth_test
		| WM_8_DISPATCH;	// enable_8_pix

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
		info.bytes_per_row, true);
}


status_t
render_fill_rect(uint32 color, int16 left, int16 top,
	int16 right, int16 bottom)
{
	if (!sRenderState.initialized)
		return B_NOT_INITIALIZED;

	render_update_surface();
	render_patch_color(color);

	// Verify patched kernel color values
	uint32* kw = (uint32*)(sRenderState.base + STATE_WM_KERNEL_OFFSET);
	TRACE("Patched kernel: R=0x%08x G=0x%08x B=0x%08x A=0x%08x\n",
		kw[WM_KERNEL_RED_OFFSET / 4], kw[WM_KERNEL_GREEN_OFFSET / 4],
		kw[WM_KERNEL_BLUE_OFFSET / 4], kw[WM_KERNEL_ALPHA_OFFSET / 4]);

	// Write vertex data for RECTLIST (3 vertices per rect)
	// Format: X, Y (float).  Order matches SNA gen5_emit_composite_primitive:
	// v0=(x2,y2), v1=(x1,y2), v2=(x1,y1) → hardware infers v3=(x2,y1)
	float* vb = (float*)(sRenderState.base + STATE_VERTEX_OFFSET);
	// v0: bottom-right
	vb[0] = (float)right;
	vb[1] = (float)bottom;
	// v1: bottom-left
	vb[2] = (float)left;
	vb[3] = (float)bottom;
	// v2: top-left
	vb[4] = (float)left;
	vb[5] = (float)top;

	// Memory barrier
	int32 flush = 0;
	atomic_add(&flush, 1);

	// Diagnostic: dump all state block contents
	uint32 stOff = sRenderState.offset;
	uint32* ss = (uint32*)(sRenderState.base + STATE_SURF_DST_OFFSET);
	uint32* bt = (uint32*)(sRenderState.base + STATE_BIND_OFFSET);
	uint32* sf = (uint32*)(sRenderState.base + STATE_SF_OFFSET);
	uint32* wm = (uint32*)(sRenderState.base + STATE_WM_OFFSET);
	uint32* cc = (uint32*)(sRenderState.base + STATE_CC_OFFSET);
	TRACE("stateOff=0x%x fb_off=0x%x bpr=%u\n",
		stOff, gInfo->shared_info->frame_buffer_offset,
		gInfo->shared_info->bytes_per_row);
	TRACE("SurfState: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		ss[0], ss[1], ss[2], ss[3]);
	TRACE("BindTbl[0]=0x%08x (expect 0x%08x)\n",
		bt[0], stOff + STATE_SURF_DST_OFFSET);
	TRACE("SF: DW0=0x%08x DW3=0x%08x DW4=0x%08x DW6=0x%08x\n",
		sf[0], sf[3], sf[4], sf[6]);
	TRACE("WM: DW0=0x%08x DW1=0x%08x DW3=0x%08x DW4=0x%08x DW5=0x%08x\n",
		wm[0], wm[1], wm[3], wm[4], wm[5]);
	TRACE("CC: DW0=0x%08x DW4=0x%08x\n", cc[0], cc[4]);
	TRACE("Vertices: (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)\n",
		vb[0], vb[1], vb[2], vb[3], vb[4], vb[5]);

	uint32 stateBase = sRenderState.offset;

	// Emit 3D commands via ring buffer (scoped so destructor submits)
	{
		QueueCommands queue(gInfo->shared_info->primary_ring_buffer);

		// 1. MI_FLUSH to serialize before switching to 3D
		queue.MakeSpace(2);
		queue.Write(MI_FLUSH_CMD | MI_FLUSH_STATE_INST_CACHE);
		queue.Write(MI_NOOP);

		// 2. Apply Gen5 workarounds via MI_LOAD_REGISTER_IMM
		//    (ring-based writes may be required for 3D context)
		//    MI_LRI: opcode=0x22, length=1 per register pair
		#define MI_LOAD_REGISTER_IMM	((0x22 << 23) | 1)

		// MI_MODE: enable VS_TIMER_DISPATCH (bit 6, masked)
		queue.MakeSpace(4);
		queue.Write(MI_LOAD_REGISTER_IMM);
		queue.Write(0x209c);
		queue.Write((1 << 22) | (1 << 6));
		queue.Write(MI_NOOP);

		// _3D_CHICKEN2: enable WM_READ_PIPELINED (bit 14, masked)
		queue.MakeSpace(4);
		queue.Write(MI_LOAD_REGISTER_IMM);
		queue.Write(0x208c);
		queue.Write((1 << 30) | (1 << 14));
		queue.Write(MI_NOOP);

		// CACHE_MODE_0: disable RC_OP_FLUSH (bit 0), enable
		// CM0_PIPELINED_RENDER_FLUSH_DISABLE (bit 8)
		queue.MakeSpace(4);
		queue.Write(MI_LOAD_REGISTER_IMM);
		queue.Write(0x2120);
		queue.Write((1 << 24) | (1 << 16) | (1 << 8));
		queue.Write(MI_NOOP);

		// 3. PIPELINE_SELECT = 3D
		queue.MakeSpace(2);
		queue.Write(CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D);
		queue.Write(MI_NOOP);

		// 3. STATE_BASE_ADDRESS - all bases = 0 so all pointers are
		//    absolute GTT offsets (same approach as SNA/xf86-video-intel)
		//    Gen5: 8 DWORDs (length=6), all Modify Enable bits set
		queue.MakeSpace(8);
		queue.Write(CMD_STATE_BASE_ADDRESS);
		queue.Write(0 | 1);		// general state base = 0 (modify)
		queue.Write(0 | 1);		// surface state base = 0 (modify)
		queue.Write(0 | 1);		// indirect object base = 0 (modify)
		queue.Write(0 | 1);		// instruction base = 0 (modify)
		queue.Write(0 | 1);		// general state upper = 0 = no limit (modify)
		queue.Write(0 | 1);		// indirect obj upper = 0 = no limit (modify)
		queue.Write(0 | 1);		// instruction upper = 0 = no limit (modify)

		// 4. URB_FENCE - partition URB (SNA gen5_emit_urb)
		//    Must be set BEFORE pipelined pointers (SF/WM need URB entries)
		//    VS: entries 0-255 (256 entries x 1 row)
		//    SF: entries 256-383 (64 entries x 2 rows)
		{
			uint32 vsFence = URB_VS_ENTRIES * URB_VS_ENTRY_SIZE;	// 256
			uint32 sfFence = vsFence + URB_SF_ENTRIES * URB_SF_ENTRY_SIZE; // 384

			queue.MakeSpace(6);
			queue.Write(CMD_URB_FENCE
				| UF0_CS_REALLOC | UF0_SF_REALLOC
				| UF0_CLIP_REALLOC | UF0_GS_REALLOC
				| UF0_VS_REALLOC | 1);
			queue.Write((vsFence << 20)		// CLIP fence = VS fence
				| (vsFence << 10)			// GS fence = VS fence
				| (vsFence << 0));			// VS fence
			queue.Write((sfFence << 10)		// CS fence = SF fence
				| (sfFence << 0));			// SF fence

			// CS_URB_STATE
			queue.Write(CMD_CS_URB_STATE);
			queue.Write(((URB_CS_ENTRY_SIZE - 1) << 4)
				| (URB_CS_ENTRIES << 0));
			queue.Write(MI_NOOP);
		}

		// 5. 3DSTATE_PIPELINED_POINTERS (7 DWORDs)
		queue.MakeSpace(8);
		queue.Write(CMD_PIPELINED_POINTERS);
		queue.Write(stateBase + STATE_VS_OFFSET);	// VS state
		queue.Write(0);								// GS disabled
		queue.Write(0);								// CLIP disabled
		queue.Write(stateBase + STATE_SF_OFFSET);	// SF state
		queue.Write(stateBase + STATE_WM_OFFSET);	// WM state
		queue.Write(stateBase + STATE_CC_OFFSET);	// CC state
		queue.Write(MI_NOOP);

		// 6. 3DSTATE_DRAWING_RECTANGLE - set clip rect to framebuffer
		{
			intel_shared_info &info = *gInfo->shared_info;
			queue.MakeSpace(4);
			queue.Write(CMD_DRAWING_RECTANGLE);
			queue.Write(0);		// top-left: (0, 0)
			queue.Write(((info.current_mode.timing.v_display - 1) << 16)
				| (info.current_mode.timing.h_display - 1));
			queue.Write(0);		// origin: (0, 0)
		}

		// 7. 3DSTATE_BINDING_TABLE_POINTERS (6 DWORDs)
		queue.MakeSpace(6);
		queue.Write(CMD_BINDING_TABLE_PTRS | 4);	// length = 4
		queue.Write(0);								// VS binding table
		queue.Write(0);								// GS binding table
		queue.Write(0);								// CLIP binding table
		queue.Write(0);								// SF binding table
		queue.Write(stateBase + STATE_BIND_OFFSET);	// WM binding table

		// 8. 3DSTATE_VERTEX_BUFFERS (one buffer, 2 floats per vertex)
		//    Gen5: 4 DWORDs per VB entry, length = 4*1 - 1 = 3
		queue.MakeSpace(6);
		queue.Write(CMD_VERTEX_BUFFERS | 3);
		queue.Write((0 << 26) | (8 << 0));					// VB0: pitch=8
		queue.Write(stateBase + STATE_VERTEX_OFFSET);		// start address
		queue.Write(stateBase + STATE_VERTEX_OFFSET + 3 * 8 - 1);	// end addr
		queue.Write(0);										// instance step rate

		// 9. 3DSTATE_VERTEX_ELEMENTS (1 element: X,Y position)
		//    Gen5: 2 DWORDs per element, length = 2*1 - 1 = 1
		//    DW0: VB_index[31:27], Valid[26], Format[25:16], Offset[11:0]
		queue.MakeSpace(4);
		queue.Write(CMD_VERTEX_ELEMENTS | 1);
		queue.Write((0 << 27)					// VB index = 0
			| (1 << 26)						// Valid = 1
			| (FORMAT_R32G32_FLOAT << 16)		// source format
			| (0 << 0));						// source offset = 0
		queue.Write((VFCOMP_STORE_SRC << 28)	// comp0 = X
			| (VFCOMP_STORE_SRC << 24)			// comp1 = Y
			| (VFCOMP_STORE_0 << 20)			// comp2 = 0.0
			| (VFCOMP_STORE_1_FP << 16));		// comp3 = 1.0

		// 10. 3DPRIMITIVE (draw RECTLIST, 3 vertices)
		queue.MakeSpace(6);
		queue.Write(CMD_3DPRIMITIVE | (PRIM_RECTLIST << 10) | (6 - 2));
		queue.Write(3);		// vertex count
		queue.Write(0);		// start vertex
		queue.Write(1);		// instance count
		queue.Write(0);		// start instance
		queue.Write(0);		// base vertex location

		// PIPE_CONTROL: write marker to verify 3D pipeline is active
		// Gen5 needs WC_FLUSH (bit 12) for Post-Sync data to reach memory
		queue.MakeSpace(4);
		queue.Write(CMD_PIPE_CONTROL);
		queue.Write((1 << 20) | (1 << 14) | (1 << 12));	// CS_STALL + Write Immediate + WC_FLUSH
		queue.Write((stateBase & ~0x7) | (1 << 2));	// QWord aligned + GGTT
		queue.Write(0xDEADBEEF);			// marker value

		// MI_FLUSH to complete 3D and allow BLT again
		queue.MakeSpace(2);
		queue.Write(MI_FLUSH_CMD);
		queue.Write(MI_NOOP);

		// MI_STORE_DATA_IMM: write a white pixel directly to framebuffer
		// This bypasses the entire 3D/BLT pipeline - pure command streamer
		{
			intel_shared_info &info = *gInfo->shared_info;
			uint32 fbOff = info.frame_buffer_offset;
			uint32 bpr = info.bytes_per_row;
			// Write 20x20 white block at (380,50) as ring-write test
			for (int y = 50; y < 70; y++) {
				for (int x = 380; x < 400; x++) {
					uint32 addr = fbOff + y * bpr + x * 4;
					queue.MakeSpace(4);
					queue.Write((0x20 << 23) | (1 << 22) | 2);  // MI_STORE_DATA_IMM|GGTT
					queue.Write(0);            // reserved
					queue.Write(addr);         // GTT address
					queue.Write(0xFFFFFFFF);   // white
				}
			}
		}
	}
	// QueueCommands destructor has now written TAIL → commands submitted

	// Check if PIPE_CONTROL marker was written (proves 3D pipeline is active)
	snooze(10000);
	uint32 marker = *(uint32*)(sRenderState.base);
	TRACE("render_fill_rect: %d,%d - %d,%d color 0x%08" B_PRIx32 "\n",
		left, top, right, bottom, color);
	TRACE("PIPE_CONTROL marker: 0x%08" B_PRIx32
		" (%s)\n", marker,
		marker == 0xDEADBEEF ? "3D ACTIVE" : "3D NOT ACTIVE");

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
