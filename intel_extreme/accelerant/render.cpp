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
extern const uint32 gen5_sf_kernel[];
const uint32 gen5_sf_kernel[] = {
	0x00400031, 0x20c01fbd, 0x1069002c, 0x02100001,
	0x00400001, 0x206003be, 0x00690060, 0x00000000,
	0x00400040, 0x20e077bd, 0x00690080, 0x006940a0,
	0x00400041, 0x202077be, 0x006900e0, 0x000000c0,
	0x00400040, 0x20e077bd, 0x006900a0, 0x00694060,
	0x00400041, 0x204077be, 0x006900e0, 0x000000c8,
	0x00600031, 0x20001fbc, 0x648d0000, 0x8808c800,
};

#define SF_KERNEL_SIZE sizeof(gen5_sf_kernel)


// WM kernel: solid color fill, SIMD16 dispatch (matching SNA gen5).
// Gen5 Ironlake requires SIMD16 dispatch for WM — SIMD8 threads are
// not dispatched for RECTLIST primitives.
//
// Loads RGBA color as immediate float values (patched per fill),
// copies the thread header to m1, sends FB_WRITE SIMD16 with EOT.
//
// SIMD16 FB_WRITE layout:
//   m1      = header (8 DW)
//   m2-m3   = Red   (16 floats = 2 GRF regs)
//   m4-m5   = Green (16 floats)
//   m6-m7   = Blue  (16 floats)
//   m8-m9   = Alpha (16 floats)
//   msg_length = 9 (1 header + 4×2 color)
//
// The color immediates in instructions 1-4 (DW3 of each) are
// overwritten by render_patch_color() before every draw.
extern const uint32 gen5_wm_kernel_solid[];
const uint32 gen5_wm_kernel_solid[] = {
	// inst 0: mov(8) m1<1>:UD  r0<8,8,1>:UD  {NoMask}
	//   Copy thread header (always 8-wide).
	0x00600201, 0x20200022, 0x008D0000, 0x00000000,

	// inst 1: mov(16) m2<1>:F  <imm>:F  {NoMask}  -- Red → m2,m3
	0x00800201, 0x204003FE, 0x00000000, 0x3F800000,

	// inst 2: mov(16) m4<1>:F  <imm>:F  {NoMask}  -- Green → m4,m5
	0x00800201, 0x208003FE, 0x00000000, 0x3F800000,

	// inst 3: mov(16) m6<1>:F  <imm>:F  {NoMask}  -- Blue → m6,m7
	0x00800201, 0x20C003FE, 0x00000000, 0x3F800000,

	// inst 4: mov(16) m8<1>:F  <imm>:F  {NoMask}  -- Alpha → m8,m9
	0x00800201, 0x210003FE, 0x00000000, 0x3F800000,

	// inst 5: send(16) null:UW  FB_WRITE SIMD16  EOT  msg_reg_nr=1
	//   DW0: SEND opcode, exec_size=SIMD16, msg_reg_nr=1
	//   DW3: EOT=1, msg_length=9, header_present=1,
	//         msg_type=RT_WRITE(4), last_rt=1,
	//         msg_control=0 (SIMD16 single source),
	//         binding_table_index=0
	0x01800031, 0x20000128, 0x548D0000, 0x92084800,
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
// Marker slot in the unused gap between WM kernel end (0x2C0) and
// vertex buffer start (0x300).  Used by ring-health probe and by
// render_fill_rect pre/post batch markers.  Must NOT overlap any
// live state — writing the markers to base+0 corrupts vs[0].
#define STATE_CLIP_OFFSET		0x280	// CLIP state for TRILIST (ACCEPT_ALL)
#define STATE_MARKER_OFFSET		0x2C0
#define STATE_VERTEX_OFFSET		0x300
#define STATE_TOTAL_SIZE		0x800	// includes batch area at 0x400+

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

// URB_FENCE: non-pipelined (SubType=0, Opcode=0, SubOpcode=0)
// DW0 bits[7:0]=length, bits[13:8]=realloc flags, bits[31:16]=opcode
// Verified against Mesa brw_structs.h, SNA gen5_render.h, intel-vaapi-driver
#define CMD_URB_FENCE			GEN5_3D(0, 0, 0)	// 0x60000000
#define UF0_VS_REALLOC			(1 << 8)
#define UF0_GS_REALLOC			(1 << 9)
#define UF0_CLIP_REALLOC		(1 << 10)
#define UF0_SF_REALLOC			(1 << 11)
#define UF0_VFE_REALLOC			(1 << 12)
#define UF0_CS_REALLOC			(1 << 13)

// CS_URB_STATE: non-pipelined (SubType=0, Opcode=0, SubOpcode=1)
// Mesa brw_defines.h: CMD_CS_URB_STATE = 0x6001
#define CMD_CS_URB_STATE		GEN5_3D(0, 0, 1)	// 0x60010000


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

	// ----- VS state (matching SNA gen5_create_vs_unit_state) -----
	uint32* vs = (uint32*)(base + STATE_VS_OFFSET);
	vs[0] = 0;  // no kernel (passthrough)
	vs[1] = 0;  // no binding table, no single program flow
	// thread4: nr_urb_entries and urb_entry_allocation_size
	vs[4] = ((URB_VS_ENTRIES >> 2) << 11)  // nr_urb_entries (SNA divides by 4)
		| ((URB_VS_ENTRY_SIZE - 1) << 19);  // urb_entry_allocation_size
	// vs6: vs_enable=0, vert_cache_disable=1 (matching SNA)
	vs[6] = (1 << 1);  // vert_cache_disable at bit 1

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
		| WM_16_DISPATCH;	// enable_16_pix (SNA uses SIMD16, not SIMD8)

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

	// Flush all CPU writes to physical memory so GPU can read them via GTT
	asm volatile("mfence" ::: "memory");

	sRenderState.initialized = true;

	TRACE("Render engine initialized: state at GTT 0x%" B_PRIx32
		", SF kernel at 0x%" B_PRIx32
		", WM kernel at 0x%" B_PRIx32 "\n",
		stateOff, stateOff + STATE_SF_KERNEL_OFFSET,
		stateOff + STATE_WM_KERNEL_OFFSET);

	// --- 3D Pipeline Probe ---
	// PIPE_CONTROL Write Immediate hangs/fails on ILK without a fully
	// initialized 3D context — don't use it for diagnostics.
	// Use MI_STORE_DATA_IMM markers instead (always works via command streamer).
	if (gInfo->shared_info->device_type.InGroup(INTEL_GROUP_ILK)) {
		ring_buffer &ring = gInfo->shared_info->primary_ring_buffer;
		volatile uint32* marker =
			(volatile uint32*)(base + STATE_MARKER_OFFSET);

		// Quick ring health check — writes to dedicated marker slot,
		// NOT to base+0 (which is vs[0] and would corrupt VS state).
		*marker = 0;
		{
			QueueCommands queue(ring);
			queue.MakeSpace(4);
			queue.Write((0x20 << 23) | (1 << 22) | 2);  // MI_STORE_DATA_IMM|GGTT
			queue.Write(0);
			queue.Write(stateOff + STATE_MARKER_OFFSET);
			queue.Write(0xAAAAAAAA);
		}
		snooze(2000);
		TRACE("Ring health: 0x%08" B_PRIx32 " (%s)\n",
			*marker, *marker == 0xAAAAAAAA ? "OK" : "BROKEN");
	}

	return B_OK;
}


// Clone version: allocates state + kernels AND re-initializes the ring.
// The ring may be hung from a prior media pipeline test.
status_t
render_init_clone()
{
	memset(&sRenderState, 0, sizeof(sRenderState));

	if (gInfo->shared_info->device_type.InGroup(INTEL_GROUP_ILK)) {
		ring_buffer &ring = gInfo->shared_info->primary_ring_buffer;
		uint32 ringReg = ring.register_base;

		// --- Step 1: FORCEWAKE — force GT out of RC6 power saving ---
		// Without this, MMIO register writes may be silently dropped.
		write32(0xA18C, 1);  // FORCEWAKE
		read32(0xA18C);
		snooze(1000);
		write32(0x209c, 0);  // WaIssueDummyWriteToWakeupFromRC6:ilk
		read32(0x209c);
		snooze(1000);

		// --- Step 2: ILK_GDSR domain reset ---
		// The boot-time IDCT test leaves the media EU pipeline hung
		// (IS stall from MEDIA_OBJECT + MI_FLUSH). A ring soft-reset
		// does NOT clear this. Must reset via ILK_GDSR.
		#define ILK_GDSR_REG	0x12ca4
		// Reset media domain (clears IS stall)
		write32(ILK_GDSR_REG, (3 << 1) | 1);
		for (int i = 0; i < 500; i++) {
			if ((read32(ILK_GDSR_REG) & 1) == 0) break;
			snooze(1000);
		}
		// Reset render domain
		write32(ILK_GDSR_REG, (1 << 1) | 1);
		for (int i = 0; i < 500; i++) {
			if ((read32(ILK_GDSR_REG) & 1) == 0) break;
			snooze(1000);
		}
		TRACE("render_init_clone: ILK_GDSR domain reset done\n");

		// --- Step 3: Full ring re-initialization (i915 init_ring_common) ---
		// After domain reset, the ring must be re-initialized from scratch.
		write32(ringReg + RING_BUFFER_CONTROL, 0);
		read32(ringReg + RING_BUFFER_CONTROL);
		snooze(1000);
		write32(ringReg + RING_BUFFER_HEAD, 0);
		read32(ringReg + RING_BUFFER_HEAD);
		for (int i = 0; i < 100; i++) {
			if ((read32(ringReg + RING_BUFFER_HEAD)
				& INTEL_RING_BUFFER_HEAD_MASK) == 0)
				break;
			snooze(100);
		}
		memset((void*)ring.base, 0, ring.size);
		write32(ringReg + RING_BUFFER_TAIL, 0);
		write32(ringReg + RING_BUFFER_START, ring.offset);
		write32(ringReg + RING_BUFFER_CONTROL,
			((ring.size - B_PAGE_SIZE) & INTEL_RING_BUFFER_SIZE_MASK)
			| INTEL_RING_BUFFER_ENABLED);
		ring.position = 0;
		ring.space_left = ring.size;

		// Gen5 workarounds (must re-apply after domain reset)
		write32(0x209c, (1 << 22) | (1 << 6));  // MI_MODE
		write32(0x208c, (1 << 30) | (1 << 14));  // _3D_CHICKEN2
		write32(0x2120, (1 << 24) | (1 << 16) | (1 << 8));  // CACHE_MODE_0
		write32(0x2064, 0);  // clear EIR
		write32(0x2068, 0);  // clear EMR

		TRACE("render_init_clone: ring re-initialized after domain reset\n");
	}

	// Allocate GPU memory for state structures
	addr_t base;
	if (intel_allocate_memory(STATE_TOTAL_SIZE, 0, base) != B_OK) {
		ERROR("render_init_clone: alloc failed\n");
		return B_NO_MEMORY;
	}

	sRenderState.base = base;
	sRenderState.offset = base - (addr_t)gInfo->shared_info->graphics_memory;
	sRenderState.size = STATE_TOTAL_SIZE;

	memset((void*)base, 0, STATE_TOTAL_SIZE);

	// Copy SF + WM kernel code
	memcpy((void*)(base + STATE_SF_KERNEL_OFFSET),
		gen5_sf_kernel, SF_KERNEL_SIZE);
	memcpy((void*)(base + STATE_WM_KERNEL_OFFSET),
		gen5_wm_kernel_solid, WM_KERNEL_SIZE);

	uint32 stateOff = sRenderState.offset;

	// VS state (passthrough)
	uint32* vs = (uint32*)(base + STATE_VS_OFFSET);
	vs[0] = 0;
	vs[1] = 0;
	vs[4] = ((URB_VS_ENTRIES >> 2) << 11)
		| ((URB_VS_ENTRY_SIZE - 1) << 19);
	vs[6] = (1 << 1);

	// CLIP state (CLIPMODE_ACCEPT_ALL — required for TRILIST)
	// Gen5 CLIP unit state: 8 DWORDs
	// DW0-3: thread config (all 0 for ACCEPT_ALL — no clip kernel needed)
	// DW4 (thread4): nr_urb_entries, urb_entry_alloc_size, max_threads
	// DW5 (clip5): clip_mode at bits [2:0] — 4 = ACCEPT_ALL
	uint32* clip = (uint32*)(base + STATE_CLIP_OFFSET);
	clip[0] = 0;  // no kernel
	clip[1] = 0;
	clip[2] = 0;
	clip[3] = 0;
	// DW4: nr_urb_entries=6 (bits[12:6]), urb_entry_alloc_size=0 (bits[18:14]),
	//       max_threads=1 (bits[24:20] for ILK, 0-based)
	clip[4] = (6 << 6) | (0 << 14) | (0 << 20);
	// DW5: clip_mode = ACCEPT_ALL (4) at bits [2:0]
	clip[5] = 4;  // GEN5_CLIPMODE_ACCEPT_ALL
	clip[6] = 0;
	clip[7] = 0;

	// SF state
	uint32 sfKernelOff = stateOff + STATE_SF_KERNEL_OFFSET;
	uint32* sf = (uint32*)(base + STATE_SF_OFFSET);
	sf[0] = sfKernelOff;
	sf[3] = (3 << 0) | (1 << 4) | (1 << 11);
	sf[4] = ((ILK_SF_MAX_THREADS - 1) << 25)
		| (1 << 19) | (URB_SF_ENTRIES << 11);
	sf[6] = (1 << 29) | (0x8 << 9) | (0x8 << 13);
	sf[7] = (2 << 12);

	// WM state
	uint32 wmKernelOff = stateOff + STATE_WM_KERNEL_OFFSET;
	uint32* wm = (uint32*)(base + STATE_WM_OFFSET);
	wm[0] = wmKernelOff | (2 << 1);
	wm[1] = 0;
	wm[3] = (3 << 0) | (0 << 4) | (2 << 11);
	wm[5] = ((ILK_WM_MAX_THREADS - 1) << 25)
		| (1 << 19) | (1 << 18) | WM_16_DISPATCH;

	// CC state
	uint32* cc = (uint32*)(base + STATE_CC_OFFSET);
	uint32 ccVpOff = stateOff + STATE_CC_VP_OFFSET;
	cc[4] = ccVpOff;
	float* ccvp = (float*)(base + STATE_CC_VP_OFFSET);
	ccvp[0] = 0.0f;
	ccvp[1] = 1.0f;

	// Binding table
	uint32* bt = (uint32*)(base + STATE_BIND_OFFSET);
	bt[0] = stateOff + STATE_SURF_DST_OFFSET;

	asm volatile("mfence" ::: "memory");
	sRenderState.initialized = true;

	TRACE("render_init_clone: state at GTT 0x%x\n", stateOff);
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

	// Flush surface state + patched kernel to physical memory for GPU
	asm volatile("mfence" ::: "memory");

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

	// Memory fence: flush CPU write-combining buffers to physical memory.
	// Without this, GPU reads via GTT may see stale/zero data for all
	// state structures (VS, SF, WM, CC, binding table, surface state,
	// vertex buffer, kernel code) written by the CPU.
	// atomic_add is NOT sufficient for WC memory — need x86 MFENCE.
	asm volatile("mfence" ::: "memory");

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

	// Write all 3D commands into a batch buffer, then submit via
	// MI_BATCH_BUFFER_START.  i915 Linux NEVER puts 3D commands
	// directly in the ring buffer — they always go through batch
	// buffers.  On Gen5, 3D commands in the ring may be parsed by
	// the command streamer but NOT dispatched to the 3D pipeline.

	// Use the tail end of the state block as a mini batch buffer.
	// STATE_VERTEX_OFFSET+256 gives us room after the vertex data.
	#define BATCH_AREA_OFFSET  0x400  // after all state/vertex data
	uint32* batch = (uint32*)(sRenderState.base + BATCH_AREA_OFFSET);
	uint32 batchGTT = stateBase + BATCH_AREA_OFFSET;
	uint32 markerGTT = stateBase + STATE_MARKER_OFFSET;
	int bp = 0;  // batch position (in DWORDs)

	#define EMIT(val) do { batch[bp++] = (val); } while(0)

	// STAGE marker: emits an MI_STORE_DATA_IMM that writes `tag` to the
	// marker slot.  After the batch runs, the marker slot holds the tag
	// of the LAST stage the CS successfully executed.  Any stage at
	// which the CS hangs leaves its predecessor's tag in the slot — so
	// we can pinpoint the exact command that hung.  4 DW per stage.
	#define STAGE(tag) do { \
		EMIT((0x20 << 23) | (1 << 22) | 2); \
		EMIT(0); \
		EMIT(markerGTT); \
		EMIT(0x3D3D00 | (tag)); \
	} while(0)

	STAGE(0x10);  // entered batch, before any 3D command

	// MI_FLUSH at the start of the batch: drain BLT state from CS
	// before transitioning to 3D via PIPELINE_SELECT.  Required on
	// Gen5 when the previous ring activity was BLT.
	EMIT(MI_FLUSH_CMD);
	STAGE(0x20);  // MI_FLUSH done

	// PIPELINE_SELECT = 3D
	EMIT(CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D);
	STAGE(0x30);  // PIPELINE_SELECT done

	// STATE_BASE_ADDRESS
	EMIT(CMD_STATE_BASE_ADDRESS);
	EMIT(0 | 1);  // general state base = 0
	EMIT(0 | 1);  // surface state base = 0
	EMIT(0 | 1);  // indirect object base = 0
	EMIT(0 | 1);  // instruction base = 0
	EMIT(0);       // general state upper (don't modify)
	EMIT(0);       // indirect obj upper (don't modify)
	EMIT(0);       // instruction upper (don't modify)
	STAGE(0x40);  // STATE_BASE_ADDRESS done

	// URB_FENCE
	{
		uint32 vsFence = URB_VS_ENTRIES * URB_VS_ENTRY_SIZE;
		uint32 sfFence = vsFence + URB_SF_ENTRIES * URB_SF_ENTRY_SIZE;
		EMIT(CMD_URB_FENCE | UF0_VS_REALLOC | UF0_GS_REALLOC
			| UF0_CLIP_REALLOC | UF0_SF_REALLOC
			| UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
		EMIT((vsFence << 20) | (vsFence << 10) | vsFence);
		EMIT((sfFence << 20) | (sfFence << 10) | sfFence);
	}
	STAGE(0x50);  // URB_FENCE done

	// CS_URB_STATE
	EMIT(CMD_CS_URB_STATE);
	EMIT(((URB_CS_ENTRY_SIZE - 1) << 4) | URB_CS_ENTRIES);
	STAGE(0x60);  // CS_URB_STATE done

	// PIPELINED_POINTERS
	EMIT(CMD_PIPELINED_POINTERS);
	EMIT(stateBase + STATE_VS_OFFSET);
	EMIT(0);  // GS disabled
	EMIT(0);  // CLIP disabled
	EMIT(stateBase + STATE_SF_OFFSET);
	EMIT(stateBase + STATE_WM_OFFSET);
	EMIT(stateBase + STATE_CC_OFFSET);
	STAGE(0x70);  // PIPELINED_POINTERS done

	// DRAWING_RECTANGLE
	{
		intel_shared_info &info = *gInfo->shared_info;
		EMIT(CMD_DRAWING_RECTANGLE);
		EMIT(0);
		EMIT(((info.current_mode.timing.v_display - 1) << 16)
			| (info.current_mode.timing.h_display - 1));
		EMIT(0);
	}
	STAGE(0x80);  // DRAWING_RECTANGLE done

	// BINDING_TABLE_POINTERS
	EMIT(CMD_BINDING_TABLE_PTRS | 4);
	EMIT(0); EMIT(0); EMIT(0); EMIT(0);
	EMIT(stateBase + STATE_BIND_OFFSET);
	STAGE(0x90);  // BINDING_TABLE_POINTERS done

	// VERTEX_BUFFERS
	EMIT(CMD_VERTEX_BUFFERS | 3);
	EMIT((0 << 26) | (8 << 0));
	EMIT(stateBase + STATE_VERTEX_OFFSET);
	EMIT(stateBase + STATE_VERTEX_OFFSET + 3 * 8 - 1);
	EMIT(0);
	STAGE(0xA0);  // VERTEX_BUFFERS done

	// VERTEX_ELEMENTS
	EMIT(CMD_VERTEX_ELEMENTS | 1);
	EMIT((0 << 27) | (1 << 26) | (FORMAT_R32G32_FLOAT << 16) | 0);
	EMIT((VFCOMP_STORE_SRC << 28) | (VFCOMP_STORE_SRC << 24)
		| (VFCOMP_STORE_0 << 20) | (VFCOMP_STORE_1_FP << 16));
	STAGE(0xB0);  // VERTEX_ELEMENTS done

	// 3DPRIMITIVE (RECTLIST, 3 vertices)
	EMIT(CMD_3DPRIMITIVE | (PRIM_RECTLIST << 10) | (6 - 2));
	EMIT(3);   // vertex count
	EMIT(0);   // start vertex
	EMIT(1);   // instance count
	EMIT(0);   // start instance
	EMIT(0);   // base vertex
	STAGE(0xC0);  // 3DPRIMITIVE issued (draw may still be in flight)

	// MI_FLUSH — serializes 3D and forces render cache flush so the
	// framebuffer write from the WM FB_WRITE is visible to the CPU.
	EMIT(MI_FLUSH_CMD);
	STAGE(0xD0);  // MI_FLUSH post-draw done

	// MI_BATCH_BUFFER_END (must be QWord aligned)
	EMIT(MI_BATCH_BUFFER_END);
	if (bp & 1)
		EMIT(MI_NOOP);

	// Flush batch buffer writes to physical memory
	asm volatile("mfence" ::: "memory");

	TRACE("Batch: %d DWORDs at GTT 0x%x\n", bp, batchGTT);

	// Clear marker slot before submitting so post-batch readback is meaningful
	*(volatile uint32*)(sRenderState.base + STATE_MARKER_OFFSET) = 0;

	// Submit batch via ring buffer
	{
		QueueCommands queue(gInfo->shared_info->primary_ring_buffer);

		// MI marker BEFORE batch (proves ring submission)
		queue.MakeSpace(4);
		queue.Write((0x20 << 23) | (1 << 22) | 2);
		queue.Write(0);
		queue.Write(markerGTT);
		queue.Write(0x3D3D0001);

		// MI_BATCH_BUFFER_START
		queue.MakeSpace(2);
		queue.Write(MI_BATCH_BUFFER_START | MI_BATCH_GTT);
		queue.Write(batchGTT);

		// MI marker AFTER batch return (proves batch completed)
		queue.MakeSpace(4);
		queue.Write((0x20 << 23) | (1 << 22) | 2);
		queue.Write(0);
		queue.Write(markerGTT);
		queue.Write(0x3D3D0002);
	}
	// QueueCommands destructor has now written TAIL → commands submitted

	// Check MI_STORE_DATA_IMM markers (reliable, unlike PIPE_CONTROL on ILK)
	snooze(10000);
	uint32 marker = *(volatile uint32*)(sRenderState.base + STATE_MARKER_OFFSET);
	TRACE("render_fill_rect: %d,%d - %d,%d color 0x%08" B_PRIx32 "\n",
		left, top, right, bottom, color);
	TRACE("MI marker: 0x%08" B_PRIx32 " (%s)\n", marker,
		marker == 0x3D3D0002 ? "POST-DRAW OK" :
		marker == 0x3D3D0001 ? "PRE-DRAW ONLY (3DPRIMITIVE hung?)" :
		"NO MARKERS (ring stuck?)");

	// Dump GPU error state
	TRACE("GPU: INSTDONE=0x%08" B_PRIx32 " IPEHR=0x%08" B_PRIx32
		" EIR=0x%08" B_PRIx32 " HEAD=0x%08" B_PRIx32 "\n",
		read32(0x206C), read32(0x2068), read32(0x20B0),
		read32(gInfo->shared_info->primary_ring_buffer.register_base
			+ RING_BUFFER_HEAD));

	return B_OK;
}


status_t
render_draw_triangle(uint32 color,
	float x0, float y0, float x1, float y1, float x2, float y2)
{
	if (!sRenderState.initialized)
		return B_NOT_INITIALIZED;

	render_update_surface();
	render_patch_color(color);

	// Write vertex data: 3 vertices, (X, Y) floats each
	float* vb = (float*)(sRenderState.base + STATE_VERTEX_OFFSET);
	vb[0] = x0;  vb[1] = y0;
	vb[2] = x1;  vb[3] = y1;
	vb[4] = x2;  vb[5] = y2;

	asm volatile("mfence" ::: "memory");

	uint32 stateBase = sRenderState.offset;
	uint32 markerGTT = stateBase + STATE_MARKER_OFFSET;

	uint32* batch = (uint32*)(sRenderState.base + BATCH_AREA_OFFSET);
	uint32 batchGTT = stateBase + BATCH_AREA_OFFSET;
	int bp = 0;

	// Use STAGE markers (write to markerGTT) to track progress.
	#define EMIT_T(val) do { batch[bp++] = (val); } while(0)
	#define STAGE_T(tag) do { \
		EMIT_T((0x20 << 23) | (1 << 22) | 2); \
		EMIT_T(0); EMIT_T(markerGTT); EMIT_T(0xAA000000 | (tag)); \
	} while(0)

	// Clear marker
	*(volatile uint32*)(sRenderState.base + STATE_MARKER_OFFSET) = 0;

	STAGE_T(0x01);  // entered ring

	// MI_FLUSH with State Instruction Cache Invalidate — critical for
	// clone rendering where the GPU cache has stale state from app_server.
	EMIT_T(MI_FLUSH_CMD | (1 << 1));
	STAGE_T(0x02);  // MI_FLUSH + cache invalidate done

	EMIT_T(CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D);
	STAGE_T(0x03);  // PIPELINE_SELECT done

	EMIT_T(CMD_STATE_BASE_ADDRESS);
	EMIT_T(0 | 1); EMIT_T(0 | 1); EMIT_T(0 | 1); EMIT_T(0 | 1);
	EMIT_T(0); EMIT_T(0); EMIT_T(0);
	STAGE_T(0x04);  // STATE_BASE_ADDRESS done

	{
		uint32 vsFence = URB_VS_ENTRIES * URB_VS_ENTRY_SIZE;
		uint32 sfFence = vsFence + URB_SF_ENTRIES * URB_SF_ENTRY_SIZE;
		EMIT_T(CMD_URB_FENCE | UF0_VS_REALLOC | UF0_GS_REALLOC
			| UF0_CLIP_REALLOC | UF0_SF_REALLOC
			| UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
		EMIT_T((vsFence << 20) | (vsFence << 10) | vsFence);
		EMIT_T((sfFence << 20) | (sfFence << 10) | sfFence);
	}
	STAGE_T(0x05);  // URB_FENCE done

	EMIT_T(CMD_CS_URB_STATE);
	EMIT_T(((URB_CS_ENTRY_SIZE - 1) << 4) | URB_CS_ENTRIES);
	STAGE_T(0x06);  // CS_URB_STATE done

	EMIT_T(CMD_PIPELINED_POINTERS);
	EMIT_T(stateBase + STATE_VS_OFFSET);
	EMIT_T(0);  // GS disabled
	// CLIP: pointer to CLIP_STATE | bit 0 = CLIP_ENABLE
	EMIT_T((stateBase + STATE_CLIP_OFFSET) | 1);
	EMIT_T(stateBase + STATE_SF_OFFSET);
	EMIT_T(stateBase + STATE_WM_OFFSET);
	EMIT_T(stateBase + STATE_CC_OFFSET);
	STAGE_T(0x07);  // PIPELINED_POINTERS done

	{
		intel_shared_info &info = *gInfo->shared_info;
		EMIT_T(CMD_DRAWING_RECTANGLE);
		EMIT_T(0);
		EMIT_T(((info.current_mode.timing.v_display - 1) << 16)
			| (info.current_mode.timing.h_display - 1));
		EMIT_T(0);
	}
	STAGE_T(0x08);  // DRAWING_RECTANGLE done

	EMIT_T(CMD_BINDING_TABLE_PTRS | 4);
	EMIT_T(0); EMIT_T(0); EMIT_T(0); EMIT_T(0);
	EMIT_T(stateBase + STATE_BIND_OFFSET);
	STAGE_T(0x09);  // BINDING_TABLE done

	EMIT_T(CMD_VERTEX_BUFFERS | 3);
	EMIT_T((0 << 26) | (8 << 0));  // stride 8 bytes (X,Y floats)
	EMIT_T(stateBase + STATE_VERTEX_OFFSET);
	EMIT_T(stateBase + STATE_VERTEX_OFFSET + 3 * 8 - 1);
	EMIT_T(0);
	STAGE_T(0x0A);  // VERTEX_BUFFERS done

	// Vertex format: 2 elements matching SNA gen5_render.c VUE layout.
	// Element 0: VUE header pad [0,0,0,0]
	// Element 1: Position [X,Y,1.0,1.0]
	EMIT_T(CMD_VERTEX_ELEMENTS | 3);
	EMIT_T((0 << 27) | (1 << 26) | (FORMAT_R32G32_FLOAT << 16) | 0);
	EMIT_T((VFCOMP_STORE_0 << 28) | (VFCOMP_STORE_0 << 24)
		| (VFCOMP_STORE_0 << 20) | (VFCOMP_STORE_0 << 16));
	EMIT_T((0 << 27) | (1 << 26) | (FORMAT_R32G32_FLOAT << 16) | 0);
	EMIT_T((VFCOMP_STORE_SRC << 28) | (VFCOMP_STORE_SRC << 24)
		| (VFCOMP_STORE_1_FP << 20) | (VFCOMP_STORE_1_FP << 16));
	STAGE_T(0x0B);  // VERTEX_ELEMENTS done

	STAGE_T(0x0C);  // before 3DPRIMITIVE

	// Try RECTLIST first (known working from app_server), then TRILIST.
	// Toggle via a static flag to test both from the same binary.
	static int sUseTrilist = 0;
	int prim = (sUseTrilist++ & 1) ? PRIM_TRILIST : PRIM_RECTLIST;
	TRACE("render_draw_triangle: using %s\n",
		prim == PRIM_TRILIST ? "TRILIST" : "RECTLIST");

	EMIT_T(CMD_3DPRIMITIVE | (prim << 10) | (6 - 2));
	EMIT_T(3);
	EMIT_T(0);
	EMIT_T(1);
	EMIT_T(0);
	EMIT_T(0);

	STAGE_T(0x0D);  // after 3DPRIMITIVE

	EMIT_T(MI_FLUSH_CMD);
	STAGE_T(0x0E);  // after MI_FLUSH

	if (bp & 1)
		EMIT_T(MI_NOOP);

	asm volatile("mfence" ::: "memory");

	// MI_BATCH_BUFFER_END (required when using MI_BATCH_BUFFER_START)
	EMIT_T(MI_BATCH_BUFFER_END);
	if (bp & 1)
		EMIT_T(MI_NOOP);

	asm volatile("mfence" ::: "memory");

	TRACE("render_draw_triangle: %d DWORDs in batch at GTT 0x%x\n",
		bp, batchGTT);

	// Submit via MI_BATCH_BUFFER_START — 3D commands MUST go through
	// batch buffers on Gen5, NOT directly in the ring!
	{
		ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
		QueueCommands queue(ring);

		// Pre-batch marker
		queue.MakeSpace(4);
		queue.Write((0x20 << 23) | (1 << 22) | 2);
		queue.Write(0);
		queue.Write(markerGTT);
		queue.Write(0xAA000001);

		// MI_BATCH_BUFFER_START
		queue.MakeSpace(2);
		queue.Write(MI_BATCH_BUFFER_START | MI_BATCH_GTT);
		queue.Write(batchGTT);

		// Post-batch marker
		queue.MakeSpace(4);
		queue.Write((0x20 << 23) | (1 << 22) | 2);
		queue.Write(0);
		queue.Write(markerGTT);
		queue.Write(0xAA00000F);
	}

	snooze(50000);
	uint32 marker = *(volatile uint32*)(sRenderState.base
		+ STATE_MARKER_OFFSET);

	const char* result;
	if (marker == 0xAA00000F)
		result = "BATCH COMPLETED (post-marker OK)";
	else if (marker == 0xAA000001)
		result = "PRE-BATCH ONLY (batch hung inside)";
	else if ((marker & 0xFF000000) == 0xAA000000)
		result = "BATCH IN PROGRESS (stuck at stage)";
	else
		result = "NO MARKERS (ring not processing)";

	TRACE("render_draw_triangle: marker=0x%08x — %s\n", marker, result);
	printf("  marker=0x%08x — %s\n", marker, result);

	return B_OK;
}
