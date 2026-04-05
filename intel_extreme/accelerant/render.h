/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Alexander (Wikipedia Wikipedia), user@shredder
 */
#ifndef RENDER_H
#define RENDER_H


// Gen5 (Ironlake) 3D render engine for 2D acceleration.
// Uses the GPU's shader pipeline for operations the BLT engine
// cannot do: alpha compositing, scaling, gradients.

#include "accelerant.h"

// 3D Pipeline Commands (MI client = 0, 3D client = 3)

// Gen5 3D command encoding macro (matching xf86-video-intel SNA):
//   bits[31:29] = Type (3 = 3D instruction)
//   bits[28:27] = Pipeline/SubType
//   bits[26:24] = Opcode
//   bits[23:16] = SubOpcode
//   bits[15:0]  = Length (DWord count - 2)
#define GEN5_3D(pipeline, opcode, subopcode) \
	((0x3 << 29) | ((pipeline) << 27) | ((opcode) << 24) | ((subopcode) << 16))

// Pipeline select: choose between 3D and media pipeline
#define CMD_PIPELINE_SELECT			GEN5_3D(1, 1, 4)	// 0x69040000
#define PIPELINE_SELECT_3D			0

// MI_FLUSH - flush caches between BLT and 3D
#define MI_FLUSH_CMD				(0x04 << 23)
#define MI_FLUSH_STATE_INST_CACHE	(1 << 1)
#define MI_FLUSH_RENDER_CACHE		(1 << 2)

// STATE_BASE_ADDRESS (non-pipelined, opcode 1, subopcode 1)
#define CMD_STATE_BASE_ADDRESS		(GEN5_3D(0, 1, 1) | 0x06)	// 0x61010006

// 3DSTATE_DRAWING_RECTANGLE (pipelined, opcode 1, subopcode 0)
#define CMD_DRAWING_RECTANGLE		(GEN5_3D(1, 1, 0) | 0x02)	// 0x69000002

// 3DSTATE_PIPELINED_POINTERS (pipelined, opcode 0, subopcode 0)
#define CMD_PIPELINED_POINTERS		(GEN5_3D(1, 0, 0) | 0x05)	// 0x68000005

// 3DSTATE_BINDING_TABLE_POINTERS (pipelined, opcode 1, subopcode 1)
#define CMD_BINDING_TABLE_PTRS		GEN5_3D(1, 1, 1)			// 0x69010000

// 3DSTATE_VERTEX_BUFFERS (pipelined, opcode 0, subopcode 8)
#define CMD_VERTEX_BUFFERS			GEN5_3D(1, 0, 8)			// 0x68080000

// 3DSTATE_VERTEX_ELEMENTS (pipelined, opcode 0, subopcode 9)
#define CMD_VERTEX_ELEMENTS			GEN5_3D(1, 0, 9)			// 0x68090000

// 3DPRIMITIVE (3D, opcode 3, subopcode 0)
#define CMD_3DPRIMITIVE				GEN5_3D(3, 3, 0)			// 0x7B000000
#define PRIM_RECTLIST				0x0F

// PIPE_CONTROL (3D, opcode 2, subopcode 0)
#define CMD_PIPE_CONTROL			(GEN5_3D(3, 2, 0) | 0x02)	// 0x7A000002
#define PIPE_CONTROL_CS_STALL		(1 << 20)
#define PIPE_CONTROL_FLUSH			(1 << 12)

// Surface state format
#define SURFACE_1D					0
#define SURFACE_2D					1
#define SURFACE_3D					2
#define SURFACE_BUFFER				4

// Surface formats
#define FORMAT_B8G8R8A8_UNORM		0x0C0
#define FORMAT_B8G8R8X8_UNORM		0x0C8
#define FORMAT_R32G32B32A32_FLOAT	0x000
#define FORMAT_R32G32_FLOAT			0x004

// Vertex element source formats
#define VFCOMP_STORE_0				0
#define VFCOMP_STORE_1_FP			1
#define VFCOMP_STORE_SRC			2
#define VFCOMP_NOSTORE				3

// WM state dispatch mode
#define WM_DISPATCH_ENABLE			(1 << 29)
#define WM_8_DISPATCH				(1 << 0)
#define WM_16_DISPATCH				(1 << 1)
#define WM_32_DISPATCH				(1 << 2)

// Render state structures (must be 64-byte aligned in GPU memory)

struct gen5_surface_state {
	uint32 ss0;		// type, format
	uint32 ss1;		// base address (GTT offset)
	uint32 ss2;		// width, height
	uint32 ss3;		// pitch, tiling
	uint32 ss4;		// multisample
	uint32 ss5;		// reserved
	uint32 pad[2];	// pad to 32 bytes
};

struct gen5_sampler_state {
	uint32 ss0;		// filtering mode
	uint32 ss1;		// LOD
	uint32 ss2;		// border color
	uint32 ss3;		// chroma key
};

// Render engine state block - all state structures packed into one
// GPU memory allocation for simplicity
struct render_state {
	addr_t		base;			// virtual address
	uint32		offset;			// GTT offset
	uint32		size;			// total size

	// Offsets within the state block (relative to base)
	uint32		vs_offset;		// vertex shader state
	uint32		wm_offset;		// fragment shader state
	uint32		cc_offset;		// color calculator state
	uint32		binding_offset;	// binding table
	uint32		surface_offset;	// surface states (src + dst)
	uint32		kernel_offset;	// shader kernel code
	uint32		vertex_offset;	// vertex buffer

	bool		initialized;
};

// Render engine functions
status_t render_init();
void render_uninit();
status_t render_fill_rect(uint32 color, int16 left, int16 top,
	int16 right, int16 bottom);


#endif	// RENDER_H
