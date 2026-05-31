/*
 * Copyright 2006-2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "accelerant_protos.h"
#include "accelerant.h"

#include <string.h>


// ARGB cursor buffer: 64x64x4 = 16384 bytes, allocated on first use.
// The stock kernel cursor buffer is only 4KB (B_PAGE_SIZE), too small
// for ARGB mode which always requires a 64x64 image.
static uint8* sArgbCursorBuffer = NULL;
static uint32 sArgbCursorOffset = 0;		// GTT offset
static phys_addr_t sArgbCursorPhysical = 0;	// physical address

#define ARGB_CURSOR_SIZE	(64 * 64 * 4)	// 16384 bytes

// Gen5+ (Ironlake) cursor mode values — bits [5:0] of CURACNTR.
// The stock Haiku constants use bits [28:24] which is Pre-Gen5.
// Linux i915 reference: MCURSOR_MODE_* in i915_reg.h.
#define ILK_CURSOR_MODE_DISABLE		0x00
#define ILK_CURSOR_MODE_64_2COLOR	0x06
#define ILK_CURSOR_MODE_64_ARGB		0x27


status_t
intel_set_cursor_shape(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	uint8* andMask, uint8* xorMask)
{
	if (width > 64 || height > 64)
		return B_BAD_VALUE;

	write32(INTEL_CURSOR_CONTROL, 0);
		// disable cursor

	// In two-color mode, the data is ordered as follows (always 64 bit per
	// line):
	//	plane 1: line 0 (AND mask)
	//	plane 0: line 0 (XOR mask)
	//	plane 1: line 1 (AND mask)
	//	...
	//
	// If the planes add to the value 0x2, the corresponding pixel is
	// transparent, for 0x3 it inverts the background, so only the first
	// two palette entries will be used (since we're using the 2 color mode).

	uint8* data = gInfo->shared_info->cursor_memory;
	uint8 byteWidth = (width + 7) / 8;

	for (int32 y = 0; y < height; y++) {
		for (int32 x = 0; x < byteWidth; x++) {
			data[16 * y + x] = andMask[byteWidth * y + x];
			data[16 * y + x + 8] = xorMask[byteWidth * y + x];
		}
	}

	// set palette entries to white/black
	write32(INTEL_CURSOR_PALETTE + 0, 0x00ffffff);
	write32(INTEL_CURSOR_PALETTE + 4, 0);

	if (gInfo->shared_info->device_type.Generation() >= 5) {
		gInfo->shared_info->cursor_format = ILK_CURSOR_MODE_64_2COLOR;
		write32(INTEL_CURSOR_CONTROL, ILK_CURSOR_MODE_64_2COLOR);
	} else {
		gInfo->shared_info->cursor_format = CURSOR_FORMAT_2_COLORS;
		write32(INTEL_CURSOR_CONTROL,
			CURSOR_ENABLED | gInfo->shared_info->cursor_format);
	}
	write32(INTEL_CURSOR_SIZE, height << 12 | width);

	write32(INTEL_CURSOR_BASE,
		(uint32)gInfo->shared_info->physical_graphics_memory
		+ gInfo->shared_info->cursor_buffer_offset);

	// changing the hot point changes the cursor position, too

	if (hotX != gInfo->shared_info->cursor_hot_x
		|| hotY != gInfo->shared_info->cursor_hot_y) {
		int32 x = read32(INTEL_CURSOR_POSITION);
		int32 y = x >> 16;
		x &= 0xffff;
		
		if (x & CURSOR_POSITION_NEGATIVE)
			x = -(x & CURSOR_POSITION_MASK);
		if (y & CURSOR_POSITION_NEGATIVE)
			y = -(y & CURSOR_POSITION_MASK);

		x += gInfo->shared_info->cursor_hot_x;
		y += gInfo->shared_info->cursor_hot_y;

		gInfo->shared_info->cursor_hot_x = hotX;
		gInfo->shared_info->cursor_hot_y = hotY;

		intel_move_cursor(x, y);
	}

	return B_OK;
}


status_t
intel_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX,
	uint16 hotY, color_space colorSpace, uint16 bytesPerRow,
	const void* bitmapData)
{
	_sPrintf("intel_extreme cursor: set_bitmap %ux%u hot(%u,%u) cs=0x%x bpr=%u\n",
		width, height, hotX, hotY, (uint32)colorSpace, bytesPerRow);

	if (width > 64 || height > 64)
		return B_BAD_VALUE;
	if (bitmapData == NULL)
		return B_BAD_VALUE;
	if (colorSpace != B_RGBA32 && colorSpace != B_RGB32)
		return B_BAD_VALUE;

	// Allocate a 16KB ARGB cursor buffer on first use.
	// The stock kernel cursor buffer is only 4KB — too small for
	// 64x64 ARGB which needs 16384 bytes.
	if (sArgbCursorBuffer == NULL) {
		intel_allocate_graphics_memory alloc;
		alloc.magic = INTEL_PRIVATE_DATA_MAGIC;
		alloc.size = ARGB_CURSOR_SIZE;
		alloc.alignment = ARGB_CURSOR_SIZE;	// naturally aligned
		alloc.flags = 0;
		if (ioctl(gInfo->device, INTEL_ALLOCATE_GRAPHICS_MEMORY,
				&alloc, sizeof(alloc)) < 0)
			return B_NO_MEMORY;

		sArgbCursorBuffer = (uint8*)alloc.buffer_base;
		sArgbCursorOffset = (uint32)(alloc.buffer_base
			- (addr_t)gInfo->shared_info->graphics_memory);
		sArgbCursorPhysical
			= (phys_addr_t)gInfo->shared_info->physical_graphics_memory
			+ sArgbCursorOffset;
	}

	write32(INTEL_CURSOR_CONTROL, 0);

	// Clear to transparent, then copy source bitmap.
	// Hardware expects 64x64 ARGB (premultiplied alpha).
	memset(sArgbCursorBuffer, 0, ARGB_CURSOR_SIZE);

	for (uint16 y = 0; y < height && y < 64; y++) {
		const uint32* src = (const uint32*)((const uint8*)bitmapData
			+ y * bytesPerRow);
		uint32* dst = (uint32*)sArgbCursorBuffer + y * 64;
		for (uint16 x = 0; x < width && x < 64; x++)
			dst[x] = src[x];
	}

	if (gInfo->shared_info->device_type.Generation() >= 5) {
		gInfo->shared_info->cursor_format = ILK_CURSOR_MODE_64_ARGB;
		write32(INTEL_CURSOR_CONTROL, ILK_CURSOR_MODE_64_ARGB);
	} else {
		gInfo->shared_info->cursor_format = CURSOR_FORMAT_ARGB;
		write32(INTEL_CURSOR_CONTROL,
			CURSOR_ENABLED | gInfo->shared_info->cursor_format);
	}
	write32(INTEL_CURSOR_SIZE, (64 << 12) | 64);
	write32(INTEL_CURSOR_BASE, (uint32)sArgbCursorPhysical);

	_sPrintf("intel_extreme cursor: ARGB enabled, base=0x%x ctl=0x%x\n",
		(uint32)sArgbCursorPhysical, read32(INTEL_CURSOR_CONTROL));

	// Update hot spot
	if (hotX != gInfo->shared_info->cursor_hot_x
		|| hotY != gInfo->shared_info->cursor_hot_y) {
		int32 x = read32(INTEL_CURSOR_POSITION);
		int32 y = x >> 16;
		x &= 0xffff;

		if (x & CURSOR_POSITION_NEGATIVE)
			x = -(x & CURSOR_POSITION_MASK);
		if (y & CURSOR_POSITION_NEGATIVE)
			y = -(y & CURSOR_POSITION_MASK);

		x += gInfo->shared_info->cursor_hot_x;
		y += gInfo->shared_info->cursor_hot_y;

		gInfo->shared_info->cursor_hot_x = hotX;
		gInfo->shared_info->cursor_hot_y = hotY;

		intel_move_cursor(x, y);
	} else {
		gInfo->shared_info->cursor_hot_x = hotX;
		gInfo->shared_info->cursor_hot_y = hotY;
	}

	return B_OK;
}


void
intel_move_cursor(uint16 _x, uint16 _y)
{
	int32 x = (int32)_x - gInfo->shared_info->cursor_hot_x;
	int32 y = (int32)_y - gInfo->shared_info->cursor_hot_y;

	if (x < 0)
		x = -x | CURSOR_POSITION_NEGATIVE;
	if (y < 0)
		y = -y | CURSOR_POSITION_NEGATIVE;

	write32(INTEL_CURSOR_POSITION, (y << 16) | x);
}


void
intel_show_cursor(bool isVisible)
{
	if (gInfo->shared_info->cursor_visible == isVisible)
		return;

	if (gInfo->shared_info->device_type.Generation() >= 5) {
		// Gen5+: cursor mode in bits [5:0], 0 = disabled
		write32(INTEL_CURSOR_CONTROL,
			isVisible ? gInfo->shared_info->cursor_format
				: ILK_CURSOR_MODE_DISABLE);
	} else {
		write32(INTEL_CURSOR_CONTROL, (isVisible ? CURSOR_ENABLED : 0)
			| gInfo->shared_info->cursor_format);
	}

	// Set cursor base — use ARGB buffer if in ARGB mode, else kernel buffer
	if (sArgbCursorBuffer != NULL
		&& gInfo->shared_info->cursor_format == ILK_CURSOR_MODE_64_ARGB) {
		write32(INTEL_CURSOR_BASE, (uint32)sArgbCursorPhysical);
	} else {
		write32(INTEL_CURSOR_BASE,
			(uint32)gInfo->shared_info->physical_graphics_memory
			+ gInfo->shared_info->cursor_buffer_offset);
	}

	gInfo->shared_info->cursor_visible = isVisible;
}

