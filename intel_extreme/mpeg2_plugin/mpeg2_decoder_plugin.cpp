/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * MPEG-2 video decoder plugin for Haiku media_kit.
 * Decodes I, P and B frames with motion compensation + CPU IDCT.
 * Outputs B_YCbCr420 planar (Y + Cb + Cr planes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <MediaFormats.h>
#include <MediaTrack.h>
#include <private/media/DecoderPlugin.h>

#include "mpeg2_parser.h"
#include "idct_ref.h"
#include "gpu_idct.h"


#define LOG(fmt, ...) fprintf(stderr, "mpeg2_plugin: " fmt, ##__VA_ARGS__)

static int32 sGpuRefCount = 0;


// ---------------------------------------------------------------------------
// Helper functions (same as in mpeg2_viewer.cpp / test_mc_decode.cpp)
// ---------------------------------------------------------------------------

static inline uint8
clamp8(int32 v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8)v;
}

// Decode intra block: IDCT + clamp (dc_pred embeds +128 level shift)
static void
decode_intra_pixels(const int16 coeffs[64], uint8 pixels[64])
{
	int16 idct[64];
	compute_idct_reference(coeffs, idct);
	for (int i = 0; i < 64; i++)
		pixels[i] = clamp8(idct[i]);
}

// Decode inter block: IDCT residual + MC reference
static void
decode_inter_pixels(const int16 coeffs[64], const uint8 mc_ref[64],
	uint8 pixels[64])
{
	int16 idct[64];
	compute_idct_reference(coeffs, idct);
	for (int i = 0; i < 64; i++)
		pixels[i] = clamp8((int32)mc_ref[i] + (int32)idct[i]);
}

// Fetch pixel from reference frame with boundary clamping.
static inline uint8
ref_pixel(const uint8* plane, int x, int y, int w, int h)
{
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= w) x = w - 1;
	if (y >= h) y = h - 1;
	return plane[y * w + x];
}

// Motion compensate one block from reference frame.
// mv_h, mv_v are in half-pel units.
static void
mc_block(const uint8* ref, int ref_w, int ref_h,
	int block_x, int block_y, int mv_h, int mv_v,
	uint8 dst[64], int bw, int bh)
{
	int int_x = block_x + (mv_h >> 1);
	int int_y = block_y + (mv_v >> 1);
	int frac_x = mv_h & 1;
	int frac_y = mv_v & 1;

	for (int r = 0; r < bh; r++) {
		for (int c = 0; c < bw; c++) {
			int px = int_x + c;
			int py = int_y + r;

			int32 val;
			if (!frac_x && !frac_y) {
				val = ref_pixel(ref, px, py, ref_w, ref_h);
			} else if (frac_x && !frac_y) {
				val = ((int32)ref_pixel(ref, px, py, ref_w, ref_h)
					+ (int32)ref_pixel(ref, px + 1, py, ref_w, ref_h)
					+ 1) >> 1;
			} else if (!frac_x && frac_y) {
				val = ((int32)ref_pixel(ref, px, py, ref_w, ref_h)
					+ (int32)ref_pixel(ref, px, py + 1, ref_w, ref_h)
					+ 1) >> 1;
			} else {
				val = ((int32)ref_pixel(ref, px, py, ref_w, ref_h)
					+ (int32)ref_pixel(ref, px + 1, py, ref_w, ref_h)
					+ (int32)ref_pixel(ref, px, py + 1, ref_w, ref_h)
					+ (int32)ref_pixel(ref, px + 1, py + 1, ref_w, ref_h)
					+ 2) >> 2;
			}
			dst[r * bw + c] = clamp8(val);
		}
	}
}

// Bidirectional motion compensate: blend forward + backward refs.
// mv_fwd_h/v and mv_bwd_h/v are in half-pel units.
static void
mc_block_bidir(const uint8* fwd_ref, const uint8* bwd_ref,
	int ref_w, int ref_h,
	int block_x, int block_y,
	int mv_fwd_h, int mv_fwd_v,
	int mv_bwd_h, int mv_bwd_v,
	uint8 dst[64], int bw, int bh)
{
	uint8 fwd[64], bwd[64];
	mc_block(fwd_ref, ref_w, ref_h, block_x, block_y,
		mv_fwd_h, mv_fwd_v, fwd, bw, bh);
	mc_block(bwd_ref, ref_w, ref_h, block_x, block_y,
		mv_bwd_h, mv_bwd_v, bwd, bw, bh);
	for (int i = 0; i < bw * bh; i++)
		dst[i] = (uint8)(((int32)fwd[i] + (int32)bwd[i] + 1) >> 1);
}

// Write 8x8 block of pixels to a plane at (bx, by).
static void
write_block(uint8* plane, uint32 stride, uint32 pw, uint32 ph,
	uint32 bx, uint32 by, const uint8 pixels[64], int bw, int bh)
{
	for (int r = 0; r < bh; r++)
		for (int c = 0; c < bw; c++)
			if (bx + c < pw && by + r < ph)
				plane[(by + r) * stride + bx + c] = pixels[r * bw + c];
}


// ---------------------------------------------------------------------------
// MPEG2Decoder — per-stream decoder instance
// ---------------------------------------------------------------------------

static const int ydx[4] = {0, 8, 0, 8};
static const int ydy[4] = {0, 0, 8, 8};

class MPEG2Decoder : public Decoder {
public:
						MPEG2Decoder();
	virtual				~MPEG2Decoder();

	virtual void		GetCodecInfo(media_codec_info* codecInfo);
	virtual status_t	Setup(media_format* ioEncodedFormat,
							const void* infoBuffer, size_t infoSize);
	virtual status_t	NegotiateOutputFormat(media_format* ioDecodedFormat);
	virtual status_t	SeekedTo(int64 frame, bigtime_t time);
	virtual status_t	Decode(void* buffer, int64* frameCount,
							media_header* mediaHeader,
							media_decode_info* info);

private:
	status_t			_DecodePicture(const uint8* data, size_t size,
							uint8* yPlane, uint8* cbPlane, uint8* crPlane);
	void				_FreeReference();
	void				_SaveReference(const uint8* y, const uint8* cb,
							const uint8* cr);

	mpeg2_decoder		fDec;
	bool				fHeadersParsed;
	uint32				fWidth;
	uint32				fHeight;
	uint32				fFrameNum;
	media_format		fOutputFormat;

	// GPU IDCT batch
	static const uint32 kMaxBatch = GPU_IDCT_MAX_BATCH;
	struct BatchEntry {
		uint8*	dstPlane;
		uint32	stride;
		uint32	bx, by;
		uint32	pw, ph;
		bool	isInter;
		uint8	mcRef[64];
	};
	int16				fBatchIn[GPU_IDCT_MAX_BATCH][64];
	int16				fBatchOut[GPU_IDCT_MAX_BATCH][64];
	BatchEntry			fBatchMeta[GPU_IDCT_MAX_BATCH];
	uint32				fBatchCount;

	void				_EnqueueBlock(const int16 coeffs[64],
							uint8* plane, uint32 stride,
							uint32 bx, uint32 by, uint32 pw, uint32 ph,
							bool isInter, const uint8* mcRef);
	void				_FlushBatch();

	// Forward reference frame (most recent I/P)
	uint8*				fFwdRefY;
	uint8*				fFwdRefCb;
	uint8*				fFwdRefCr;
	bool				fHasFwdRef;

	// Backward reference frame (previous I/P before forward)
	uint8*				fBwdRefY;
	uint8*				fBwdRefCb;
	uint8*				fBwdRefCr;
	bool				fHasBwdRef;
};


MPEG2Decoder::MPEG2Decoder()
	:
	fHeadersParsed(false),
	fWidth(0),
	fHeight(0),
	fFrameNum(0),
	fFwdRefY(NULL),
	fFwdRefCb(NULL),
	fFwdRefCr(NULL),
	fHasFwdRef(false),
	fBwdRefY(NULL),
	fBwdRefCb(NULL),
	fBwdRefCr(NULL),
	fHasBwdRef(false)
{
	memset(&fDec, 0, sizeof(fDec));
	memset(&fOutputFormat, 0, sizeof(fOutputFormat));
	fBatchCount = 0;

	if (atomic_add(&sGpuRefCount, 1) == 0) {
		if (gpu_idct_init() == B_OK)
			LOG("GPU IDCT enabled\n");
		else
			LOG("GPU IDCT not available, using CPU fallback\n");
	}
}


MPEG2Decoder::~MPEG2Decoder()
{
	_FreeReference();
	if (atomic_add(&sGpuRefCount, -1) == 1)
		gpu_idct_uninit();
}


void
MPEG2Decoder::_FreeReference()
{
	free(fFwdRefY);
	free(fFwdRefCb);
	free(fFwdRefCr);
	fFwdRefY = fFwdRefCb = fFwdRefCr = NULL;
	fHasFwdRef = false;

	free(fBwdRefY);
	free(fBwdRefCb);
	free(fBwdRefCr);
	fBwdRefY = fBwdRefCb = fBwdRefCr = NULL;
	fHasBwdRef = false;
}


void
MPEG2Decoder::_SaveReference(const uint8* y, const uint8* cb, const uint8* cr)
{
	uint32 ySize = fWidth * fHeight;
	uint32 cSize = (fWidth / 2) * (fHeight / 2);

	// Shift current forward ref → backward ref
	if (fHasFwdRef) {
		if (!fBwdRefY) {
			fBwdRefY = (uint8*)malloc(ySize);
			fBwdRefCb = (uint8*)malloc(cSize);
			fBwdRefCr = (uint8*)malloc(cSize);
		}
		memcpy(fBwdRefY, fFwdRefY, ySize);
		memcpy(fBwdRefCb, fFwdRefCb, cSize);
		memcpy(fBwdRefCr, fFwdRefCr, cSize);
		fHasBwdRef = true;
	}

	// Save new frame as forward ref
	if (!fFwdRefY) {
		fFwdRefY = (uint8*)malloc(ySize);
		fFwdRefCb = (uint8*)malloc(cSize);
		fFwdRefCr = (uint8*)malloc(cSize);
	}
	memcpy(fFwdRefY, y, ySize);
	memcpy(fFwdRefCb, cb, cSize);
	memcpy(fFwdRefCr, cr, cSize);
	fHasFwdRef = true;
}


void
MPEG2Decoder::GetCodecInfo(media_codec_info* info)
{
	strcpy(info->pretty_name, "MPEG-2 Video (Haiku/Gen5 HW-accel)");
	strcpy(info->short_name, "mpeg2_haiku");
	info->id = 0;
	info->sub_id = 0;
}


status_t
MPEG2Decoder::Setup(media_format* ioEncodedFormat,
	const void* infoBuffer, size_t infoSize)
{
	if (ioEncodedFormat->type != B_MEDIA_ENCODED_VIDEO)
		return B_MEDIA_BAD_FORMAT;

	fHeadersParsed = false;
	fFrameNum = 0;
	_FreeReference();
	return B_OK;
}


status_t
MPEG2Decoder::NegotiateOutputFormat(media_format* ioDecodedFormat)
{
	ioDecodedFormat->type = B_MEDIA_RAW_VIDEO;

	media_raw_video_format& raw = ioDecodedFormat->u.raw_video;
	raw.interlace = 1;
	raw.first_active = 0;
	raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	raw.pixel_width_aspect = 1;
	raw.pixel_height_aspect = 1;

	if (fWidth > 0) {
		raw.display.line_width = fWidth;
		raw.display.line_count = fHeight;
		raw.display.bytes_per_row = fWidth;
	} else {
		raw.display.line_width = 720;
		raw.display.line_count = 480;
		raw.display.bytes_per_row = 720;
	}

	if (raw.display.format == 0)
		raw.display.format = B_YCbCr420;

	fOutputFormat = *ioDecodedFormat;
	return B_OK;
}


status_t
MPEG2Decoder::SeekedTo(int64 frame, bigtime_t time)
{
	memset(&fDec, 0, sizeof(fDec));
	fHeadersParsed = false;
	_FreeReference();
	return B_OK;
}


// ---------------------------------------------------------------------------
// Picture decode — handles both I and P frames
// ---------------------------------------------------------------------------

void
MPEG2Decoder::_EnqueueBlock(const int16 coeffs[64],
	uint8* plane, uint32 stride, uint32 bx, uint32 by,
	uint32 pw, uint32 ph, bool isInter, const uint8* mcRef)
{
	uint32 idx = fBatchCount;
	memcpy(fBatchIn[idx], coeffs, 128);
	BatchEntry& e = fBatchMeta[idx];
	e.dstPlane = plane;
	e.stride = stride;
	e.bx = bx;
	e.by = by;
	e.pw = pw;
	e.ph = ph;
	e.isInter = isInter;
	if (isInter && mcRef)
		memcpy(e.mcRef, mcRef, 64);
	fBatchCount++;

	if (fBatchCount >= kMaxBatch)
		_FlushBatch();
}


void
MPEG2Decoder::_FlushBatch()
{
	if (fBatchCount == 0)
		return;

	uint32 count = fBatchCount;

	// GPU path: batch IDCT
	if (gpu_idct_available()
		&& gpu_idct_process(fBatchIn, fBatchOut, count) == B_OK) {
		// GPU succeeded
	} else {
		// CPU fallback
		for (uint32 i = 0; i < count; i++)
			compute_idct_reference(fBatchIn[i], fBatchOut[i]);
	}

	// Apply results to output planes
	for (uint32 i = 0; i < count; i++) {
		BatchEntry& e = fBatchMeta[i];
		uint8 pix[64];
		if (e.isInter) {
			for (int j = 0; j < 64; j++)
				pix[j] = clamp8((int32)e.mcRef[j] + (int32)fBatchOut[i][j]);
		} else {
			for (int j = 0; j < 64; j++)
				pix[j] = clamp8(fBatchOut[i][j]);
		}
		write_block(e.dstPlane, e.stride, e.pw, e.ph,
			e.bx, e.by, pix, 8, 8);
	}

	fBatchCount = 0;
}


status_t
MPEG2Decoder::_DecodePicture(const uint8* data, size_t size,
	uint8* yPlane, uint8* cbPlane, uint8* crPlane)
{
	mpeg2_bits bs;
	mpeg2_bits_init(&bs, data, size);

	uint32 w = fWidth, h = fHeight;
	uint32 cw = w / 2, ch = h / 2;
	int picType = fDec.pic.picture_coding_type;
	bool is_p = (picType == 2);
	bool is_b = (picType == 3);
	uint32 mbDecoded = 0;

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;

		if (code < MPEG2_SLICE_START_MIN || code > MPEG2_SLICE_START_MAX) {
			// Non-slice start code — rewind
			if (bs.byte_pos >= 4) {
				bs.byte_pos -= 4;
				bs.bit_pos = 0;
			}
			break;
		}

		mpeg2_slice_header sl;
		mpeg2_parse_slice_header(&bs, code, &sl);
		mpeg2_reset_slice_state(fDec.pic_ext.intra_dc_precision);

		// Reset MV prediction at slice boundary
		fDec.mv_pred_h = 0;
		fDec.mv_pred_v = 0;
		fDec.mv_backward_pred_h = 0;
		fDec.mv_backward_pred_v = 0;

		uint32 mby = sl.slice_vertical_position - 1;
		uint32 mbx = 0;

		while (mbx < fDec.mb_width && mpeg2_bits_available(&bs, 8)) {
			if (mpeg2_bits_peek(&bs, 23) == 0) break;

			mpeg2_macroblock mb;
			mb.quantiser_scale_code = sl.quantiser_scale_code;
			status_t st;

			if (is_b)
				st = mpeg2_decode_b_macroblock(&bs, &fDec, &mb);
			else if (is_p)
				st = mpeg2_decode_p_macroblock(&bs, &fDec, &mb);
			else
				st = mpeg2_decode_intra_macroblock(&bs, &fDec, &mb);

			if (st != B_OK) {
				mbx++;
				continue;
			}

			// Handle skipped MBs (address_increment > 1)
			if (mb.address_increment > 1) {
				uint32 skip_count = mb.address_increment - 1;
				if (is_p && fHasFwdRef) {
					for (uint32 s = 0; s < skip_count
						&& mbx < fDec.mb_width; s++) {
						// Copy from reference (zero MV, no residual)
						for (int b = 0; b < 4; b++) {
							uint32 bx = mbx * 16 + ydx[b];
							uint32 by = mby * 16 + ydy[b];
							for (int r = 0; r < 8; r++)
								for (int c = 0; c < 8; c++)
									if (bx + c < w && by + r < h)
										yPlane[(by + r) * w + bx + c] =
											fFwdRefY[(by + r) * w + bx + c];
						}
						uint32 cx = mbx * 8, cy = mby * 8;
						for (int r = 0; r < 8; r++)
							for (int c = 0; c < 8; c++)
								if (cx + c < cw && cy + r < ch) {
									cbPlane[(cy + r) * cw + cx + c] =
										fFwdRefCb[(cy + r) * cw + cx + c];
									crPlane[(cy + r) * cw + cx + c] =
										fFwdRefCr[(cy + r) * cw + cx + c];
								}
						mbx++;
						mbDecoded++;
					}
				} else if (is_b && fHasFwdRef && fHasBwdRef) {
					// B-frame skipped MBs: MC with previous MV (zero for
					// first MB in slice), using forward prediction only
					for (uint32 s = 0; s < skip_count
						&& mbx < fDec.mb_width; s++) {
						for (int b = 0; b < 4; b++) {
							uint32 bx = mbx * 16 + ydx[b];
							uint32 by = mby * 16 + ydy[b];
							uint8 mc_ref_block[64];
							mc_block_bidir(fFwdRefY, fBwdRefY, w, h,
								bx, by, 0, 0, 0, 0,
								mc_ref_block, 8, 8);
							write_block(yPlane, w, w, h, bx, by,
								mc_ref_block, 8, 8);
						}
						uint32 cx = mbx * 8, cy = mby * 8;
						for (int cc = 0; cc < 2; cc++) {
							uint8* plane = (cc == 0) ? cbPlane : crPlane;
							const uint8* fwdP = (cc == 0) ? fFwdRefCb : fFwdRefCr;
							const uint8* bwdP = (cc == 0) ? fBwdRefCb : fBwdRefCr;
							uint8 mc_ref_block[64];
							mc_block_bidir(fwdP, bwdP, cw, ch,
								cx, cy, 0, 0, 0, 0,
								mc_ref_block, 8, 8);
							write_block(plane, cw, cw, ch, cx, cy,
								mc_ref_block, 8, 8);
						}
						mbx++;
						mbDecoded++;
					}
				} else {
					mbx += skip_count;
				}
			}

			// Update quantiser scale
			fDec.quantiser_scale = mpeg2_compute_quantiser_scale(
				mb.quantiser_scale_code, fDec.pic_ext.q_scale_type);

			if (mb.mb_type & MB_INTRA) {
				// Intra MB: no MC, just IDCT → enqueue for GPU batch
				if (is_p || is_b) {
					fDec.mv_pred_h = 0;
					fDec.mv_pred_v = 0;
					fDec.mv_backward_pred_h = 0;
					fDec.mv_backward_pred_v = 0;
				}
				for (int b = 0; b < 4; b++)
					_EnqueueBlock(mb.blocks[b], yPlane, w,
						mbx * 16 + ydx[b], mby * 16 + ydy[b],
						w, h, false, NULL);
				for (int cc = 0; cc < 2; cc++)
					_EnqueueBlock(mb.blocks[4 + cc],
						cc == 0 ? cbPlane : crPlane, cw,
						mbx * 8, mby * 8, cw, ch, false, NULL);
			} else if (is_p && fHasFwdRef) {
				// Inter MB with forward motion compensation
				int16 mv_h = mb.motion_h;
				int16 mv_v = mb.motion_v;

				// Y blocks
				for (int b = 0; b < 4; b++) {
					uint32 bx = mbx * 16 + ydx[b];
					uint32 by = mby * 16 + ydy[b];
					uint8 mc_ref_block[64];
					mc_block(fFwdRefY, w, h, bx, by, mv_h, mv_v,
						mc_ref_block, 8, 8);

					if (mb.coded_block_pattern & (1 << (5 - b))) {
						_EnqueueBlock(mb.blocks[b], yPlane, w,
							bx, by, w, h, true, mc_ref_block);
					} else {
						write_block(yPlane, w, w, h, bx, by,
							mc_ref_block, 8, 8);
					}
				}

				// Chroma with scaled MVs
				int16 cmv_h = mv_h / 2;
				int16 cmv_v = mv_v / 2;
				for (int cc = 0; cc < 2; cc++) {
					uint8* plane = (cc == 0) ? cbPlane : crPlane;
					const uint8* refPlane = (cc == 0) ? fFwdRefCb : fFwdRefCr;
					uint32 cx = mbx * 8, cy = mby * 8;
					uint8 mc_ref_block[64];
					mc_block(refPlane, cw, ch, cx, cy, cmv_h, cmv_v,
						mc_ref_block, 8, 8);

					if (mb.coded_block_pattern & (1 << (1 - cc))) {
						_EnqueueBlock(mb.blocks[4 + cc], plane, cw,
							cx, cy, cw, ch, true, mc_ref_block);
					} else {
						write_block(plane, cw, cw, ch, cx, cy,
							mc_ref_block, 8, 8);
					}
				}
			} else if (is_b) {
				// B-frame inter MB: forward, backward, or bidirectional
				bool hasFwd = (mb.mb_type & MB_MOTION_FORWARD) && fHasFwdRef;
				bool hasBwd = (mb.mb_type & MB_MOTION_BACKWARD) && fHasBwdRef;

				int16 fwd_h = mb.motion_h;
				int16 fwd_v = mb.motion_v;
				int16 bwd_h = mb.motion_backward_h;
				int16 bwd_v = mb.motion_backward_v;

				// Y blocks
				for (int b = 0; b < 4; b++) {
					uint32 bx = mbx * 16 + ydx[b];
					uint32 by = mby * 16 + ydy[b];
					uint8 mc_ref_block[64];

					if (hasFwd && hasBwd) {
						mc_block_bidir(fFwdRefY, fBwdRefY, w, h,
							bx, by, fwd_h, fwd_v, bwd_h, bwd_v,
							mc_ref_block, 8, 8);
					} else if (hasBwd) {
						mc_block(fBwdRefY, w, h, bx, by,
							bwd_h, bwd_v, mc_ref_block, 8, 8);
					} else if (hasFwd) {
						mc_block(fFwdRefY, w, h, bx, by,
							fwd_h, fwd_v, mc_ref_block, 8, 8);
					} else {
						memset(mc_ref_block, 128, 64);
					}

					if (mb.coded_block_pattern & (1 << (5 - b))) {
						_EnqueueBlock(mb.blocks[b], yPlane, w,
							bx, by, w, h, true, mc_ref_block);
					} else {
						write_block(yPlane, w, w, h, bx, by,
							mc_ref_block, 8, 8);
					}
				}

				// Chroma with scaled MVs
				int16 cfwd_h = fwd_h / 2;
				int16 cfwd_v = fwd_v / 2;
				int16 cbwd_h = bwd_h / 2;
				int16 cbwd_v = bwd_v / 2;
				for (int cc = 0; cc < 2; cc++) {
					uint8* plane = (cc == 0) ? cbPlane : crPlane;
					const uint8* fwdP = (cc == 0) ? fFwdRefCb : fFwdRefCr;
					const uint8* bwdP = (cc == 0) ? fBwdRefCb : fBwdRefCr;
					uint32 cx = mbx * 8, cy = mby * 8;
					uint8 mc_ref_block[64];

					if (hasFwd && hasBwd) {
						mc_block_bidir(fwdP, bwdP, cw, ch,
							cx, cy, cfwd_h, cfwd_v, cbwd_h, cbwd_v,
							mc_ref_block, 8, 8);
					} else if (hasBwd) {
						mc_block(bwdP, cw, ch, cx, cy,
							cbwd_h, cbwd_v, mc_ref_block, 8, 8);
					} else if (hasFwd) {
						mc_block(fwdP, cw, ch, cx, cy,
							cfwd_h, cfwd_v, mc_ref_block, 8, 8);
					} else {
						memset(mc_ref_block, 128, 64);
					}

					if (mb.coded_block_pattern & (1 << (1 - cc))) {
						_EnqueueBlock(mb.blocks[4 + cc], plane, cw,
							cx, cy, cw, ch, true, mc_ref_block);
					} else {
						write_block(plane, cw, cw, ch, cx, cy,
							mc_ref_block, 8, 8);
					}
				}
			}

			mbDecoded++;
			mbx++;
		}
	}

	// Flush remaining batched blocks
	_FlushBatch();

	return (mbDecoded > 0) ? B_OK : B_ERROR;
}


// ---------------------------------------------------------------------------
// Decode entry point
// ---------------------------------------------------------------------------

status_t
MPEG2Decoder::Decode(void* buffer, int64* frameCount,
	media_header* mediaHeader, media_decode_info* info)
{
	const void* chunkData;
	size_t chunkSize;
	media_header chunkHeader;
	status_t st = GetNextChunk(&chunkData, &chunkSize, &chunkHeader);
	if (st != B_OK)
		return st;

	const uint8* data = (const uint8*)chunkData;

	// Parse MPEG-2 headers if not yet done
	if (!fHeadersParsed) {
		mpeg2_bits bs;
		mpeg2_bits_init(&bs, data, chunkSize);

		while (mpeg2_bits_available(&bs, 32)) {
			uint32 code = mpeg2_find_start_code(&bs);
			if (code == 0) break;

			if (code == MPEG2_SEQUENCE_HEADER_CODE)
				mpeg2_parse_sequence_header(&bs, &fDec.seq);
			else if (code == MPEG2_PICTURE_START_CODE)
				mpeg2_parse_picture_header(&bs, &fDec.pic);
			else if (code == MPEG2_EXTENSION_START_CODE) {
				if (mpeg2_bits_peek(&bs, 4) == 8) {
					mpeg2_bits_skip(&bs, 4);
					mpeg2_parse_picture_coding_extension(&bs,
						&fDec.pic_ext);
					mpeg2_decoder_init(&fDec);
				}
			} else if (code >= MPEG2_SLICE_START_MIN
				&& code <= MPEG2_SLICE_START_MAX) {
				break;
			}
		}

		if (fDec.initialized) {
			fWidth = fDec.seq.width;
			fHeight = fDec.seq.height;
			fHeadersParsed = true;

			fOutputFormat.u.raw_video.display.line_width = fWidth;
			fOutputFormat.u.raw_video.display.line_count = fHeight;
			fOutputFormat.u.raw_video.display.bytes_per_row = fWidth;

			LOG("initialized: %ux%u, %ux%u MBs\n",
				fWidth, fHeight, fDec.mb_width, fDec.mb_height);
		}
	}

	if (!fHeadersParsed || fWidth == 0 || fHeight == 0)
		return B_ERROR;

	// Re-parse picture header for each frame to get picture_coding_type
	{
		mpeg2_bits bs;
		mpeg2_bits_init(&bs, data, chunkSize);
		while (mpeg2_bits_available(&bs, 32)) {
			uint32 code = mpeg2_find_start_code(&bs);
			if (code == 0) break;
			if (code == MPEG2_PICTURE_START_CODE)
				mpeg2_parse_picture_header(&bs, &fDec.pic);
			else if (code == MPEG2_EXTENSION_START_CODE) {
				if (mpeg2_bits_peek(&bs, 4) == 8) {
					mpeg2_bits_skip(&bs, 4);
					mpeg2_parse_picture_coding_extension(&bs,
						&fDec.pic_ext);
					mpeg2_decoder_init(&fDec);
				}
			} else if (code >= MPEG2_SLICE_START_MIN
				&& code <= MPEG2_SLICE_START_MAX) {
				break;
			}
		}
	}

	int picType = fDec.pic.picture_coding_type;
	uint32 ySize = fWidth * fHeight;
	uint32 cSize = (fWidth / 2) * (fHeight / 2);
	uint8* yPlane = (uint8*)buffer;
	uint8* cbPlane = yPlane + ySize;
	uint8* crPlane = cbPlane + cSize;

	// P-frame without reference: skip (output black)
	if (picType == 2 && !fHasFwdRef) {
		LOG("P-frame without reference, outputting black\n");
		memset(yPlane, 16, ySize);
		memset(cbPlane, 128, cSize);
		memset(crPlane, 128, cSize);
		*frameCount = 1;
		mediaHeader->type = B_MEDIA_RAW_VIDEO;
		mediaHeader->size_used = ySize + 2 * cSize;
		mediaHeader->start_time = chunkHeader.start_time;
		mediaHeader->u.raw_video.field_sequence = fFrameNum++;
		return B_OK;
	}

	// B-frame without both references: output black
	if (picType == 3 && (!fHasFwdRef || !fHasBwdRef)) {
		LOG("B-frame without both references, outputting black\n");
		memset(yPlane, 16, ySize);
		memset(cbPlane, 128, cSize);
		memset(crPlane, 128, cSize);
		*frameCount = 1;
		mediaHeader->type = B_MEDIA_RAW_VIDEO;
		mediaHeader->size_used = ySize + 2 * cSize;
		mediaHeader->start_time = chunkHeader.start_time;
		mediaHeader->u.raw_video.field_sequence = fFrameNum++;
		return B_OK;
	}

	// Initialize planes
	if (picType == 2 && fHasFwdRef) {
		// P-frame: start from reference (skipped MBs copy from ref)
		memcpy(yPlane, fFwdRefY, ySize);
		memcpy(cbPlane, fFwdRefCb, cSize);
		memcpy(crPlane, fFwdRefCr, cSize);
	} else {
		memset(yPlane, 128, ySize);
		memset(cbPlane, 128, cSize);
		memset(crPlane, 128, cSize);
	}

	// Decode the picture (I, P, or B)
	st = _DecodePicture(data, chunkSize, yPlane, cbPlane, crPlane);

	// Save as reference for future frames (I and P only, NOT B)
	if (picType == 1 || picType == 2)
		_SaveReference(yPlane, cbPlane, crPlane);

	// Fill media header
	mediaHeader->type = B_MEDIA_RAW_VIDEO;
	mediaHeader->size_used = ySize + 2 * cSize;
	mediaHeader->start_time = chunkHeader.start_time;
	mediaHeader->u.raw_video.display_line_width = fWidth;
	mediaHeader->u.raw_video.display_line_count = fHeight;
	mediaHeader->u.raw_video.bytes_per_row = fWidth;
	mediaHeader->u.raw_video.field_sequence = fFrameNum++;

	*frameCount = 1;
	return B_OK;
}


// ---------------------------------------------------------------------------
// MPEG2DecoderPlugin — factory
// ---------------------------------------------------------------------------

class MPEG2DecoderPlugin : public DecoderPlugin {
public:
	MPEG2DecoderPlugin() {}
	virtual ~MPEG2DecoderPlugin() {}

	virtual Decoder* NewDecoder(uint index)
	{
		return new MPEG2Decoder();
	}

	virtual status_t GetSupportedFormats(media_format** formats,
		size_t* count)
	{
		media_format_description desc;
		memset(&desc, 0, sizeof(desc));
		desc.family = B_MPEG_FORMAT_FAMILY;
		desc.u.mpeg.id = B_MPEG_2_VIDEO;

		media_format fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.type = B_MEDIA_ENCODED_VIDEO;

		BMediaFormats mediaFormats;
		status_t st = mediaFormats.MakeFormatFor(&desc, 1, &fmt);
		if (st != B_OK) {
			LOG("MakeFormatFor failed: %s\n", strerror(st));
			return st;
		}

		*formats = new media_format[1];
		(*formats)[0] = fmt;
		*count = 1;
		return B_OK;
	}
};


// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

extern "C" MediaPlugin*
instantiate_plugin()
{
	return new MPEG2DecoderPlugin();
}
