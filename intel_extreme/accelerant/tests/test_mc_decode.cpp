/*
 * MPEG-2 I+P+B frame decoder with Motion Compensation (CPU-only).
 *
 * Decodes I-frames as anchors, P-frames with forward MC, and B-frames
 * with forward/backward/bidirectional MC. Outputs each frame as PPM.
 *
 * Build:
 *   g++ -Wall -O2 -I.. -I/boot/system/develop/headers/os/support \
 *       -o test_mc_decode test_mc_decode.cpp ../mpeg2_parser.cpp
 *   ./test_mc_decode input.m2v
 */

#include "mpeg2_parser.h"
#include "idct_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int _sPrintf(const char* format, ...) {
	va_list args; va_start(args, format);
	int r = vprintf(format, args); va_end(args); return r;
}

static inline uint8 clamp8(int32 v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8)v;
}


// ---------------------------------------------------------------------------
// Motion Compensation: half-pel bilinear interpolation
// ---------------------------------------------------------------------------

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

// Motion compensate one block (8x8) from reference frame.
// mv_h, mv_v are in half-pel units. Output goes to dst[].
static void
mc_block(const uint8* ref, int ref_w, int ref_h,
	int block_x, int block_y, int mv_h, int mv_v,
	uint8 dst[64], int bw, int bh)
{
	// Integer and fractional parts of motion vector
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
				// No interpolation
				val = ref_pixel(ref, px, py, ref_w, ref_h);
			} else if (frac_x && !frac_y) {
				// Horizontal half-pel: average two horizontal neighbors
				val = ((int32)ref_pixel(ref, px, py, ref_w, ref_h)
					+ (int32)ref_pixel(ref, px + 1, py, ref_w, ref_h)
					+ 1) >> 1;
			} else if (!frac_x && frac_y) {
				// Vertical half-pel: average two vertical neighbors
				val = ((int32)ref_pixel(ref, px, py, ref_w, ref_h)
					+ (int32)ref_pixel(ref, px, py + 1, ref_w, ref_h)
					+ 1) >> 1;
			} else {
				// Diagonal half-pel: bilinear 4-point average
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


// Motion compensate bidirectionally: blend forward and backward predictions.
// fwd/bwd refs, block position, MVs in half-pel. Output: dst[].
static void
mc_block_bidir(const uint8* fwd_ref, const uint8* bwd_ref,
	int ref_w, int ref_h,
	int block_x, int block_y,
	int fwd_mv_h, int fwd_mv_v,
	int bwd_mv_h, int bwd_mv_v,
	uint8 dst[64], int bw, int bh)
{
	uint8 fwd_pred[64];
	uint8 bwd_pred[64];
	mc_block(fwd_ref, ref_w, ref_h, block_x, block_y,
		fwd_mv_h, fwd_mv_v, fwd_pred, bw, bh);
	mc_block(bwd_ref, ref_w, ref_h, block_x, block_y,
		bwd_mv_h, bwd_mv_v, bwd_pred, bw, bh);
	for (int i = 0; i < bw * bh; i++)
		dst[i] = (uint8)(((int32)fwd_pred[i] + (int32)bwd_pred[i] + 1) >> 1);
}


// ---------------------------------------------------------------------------
// Block decode helpers
// ---------------------------------------------------------------------------

// Decode intra block: IDCT + clamp (no MC, dc_pred embeds +128)
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


// ---------------------------------------------------------------------------
// Frame decode
// ---------------------------------------------------------------------------

struct frame_planes {
	uint8*	y;
	uint8*	cb;
	uint8*	cr;
	uint32	w, h;
};

static void
frame_alloc(frame_planes* f, uint32 w, uint32 h)
{
	f->w = w;
	f->h = h;
	f->y = (uint8*)calloc(w * h, 1);
	f->cb = (uint8*)malloc(w/2 * h/2);
	f->cr = (uint8*)malloc(w/2 * h/2);
	memset(f->cb, 128, w/2 * h/2);
	memset(f->cr, 128, w/2 * h/2);
}

static void
frame_free(frame_planes* f)
{
	free(f->y); free(f->cb); free(f->cr);
	f->y = f->cb = f->cr = NULL;
}

static void
frame_copy(frame_planes* dst, const frame_planes* src)
{
	memcpy(dst->y, src->y, src->w * src->h);
	memcpy(dst->cb, src->cb, src->w/2 * src->h/2);
	memcpy(dst->cr, src->cr, src->w/2 * src->h/2);
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

// Write PPM from YCbCr planes (BT.601).
static void
write_ppm(const char* path, const frame_planes* f)
{
	FILE* fp = fopen(path, "wb");
	if (!fp) { printf("Cannot write %s\n", path); return; }
	uint32 w = f->w, h = f->h, cw = w/2;
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	for (uint32 y = 0; y < h; y++) {
		for (uint32 x = 0; x < w; x++) {
			int32 yv = (int32)f->y[y*w + x] - 16;
			int32 cbv = (int32)f->cb[(y/2)*cw + (x/2)] - 128;
			int32 crv = (int32)f->cr[(y/2)*cw + (x/2)] - 128;
			int32 r = (1192 * yv + 1634 * crv) >> 10;
			int32 g = (1192 * yv - 401 * cbv - 832 * crv) >> 10;
			int32 b = (1192 * yv + 2066 * cbv) >> 10;
			uint8 rgb[3] = { clamp8(r), clamp8(g), clamp8(b) };
			fwrite(rgb, 1, 3, fp);
		}
	}
	fclose(fp);
	printf("Wrote %s (%ux%u)\n", path, w, h);
}

static const int ydx[4] = {0, 8, 0, 8};
static const int ydy[4] = {0, 0, 8, 8};

// Decode one picture (I, P, or B) into dst frame.
// For P-frames, fwd_ref must point to the forward reference frame.
// For B-frames, both fwd_ref and bwd_ref must be valid.
static uint32
decode_picture(mpeg2_bits* bs, mpeg2_decoder* dec,
	frame_planes* dst, const frame_planes* fwd_ref,
	const frame_planes* bwd_ref)
{
	uint32 w = dst->w, h = dst->h;
	uint32 cw = w/2, ch = h/2;
	int pic_type = dec->pic.picture_coding_type;
	bool is_p = (pic_type == 2);
	bool is_b = (pic_type == 3);
	uint32 mb_decoded = 0;

	while (mpeg2_bits_available(bs, 32)) {
		uint32 saved_byte = bs->byte_pos;
		uint32 saved_bit = bs->bit_pos;
		uint32 code = mpeg2_find_start_code(bs);
		if (code == 0) break;
		if (code < MPEG2_SLICE_START_MIN || code > MPEG2_SLICE_START_MAX) {
			// Non-slice start code — push back so outer loop can parse it
			bs->byte_pos = bs->byte_pos - 4;  // rewind past 00 00 01 XX
			bs->bit_pos = 0;
			break;
		}

		mpeg2_slice_header sl;
		mpeg2_parse_slice_header(bs, code, &sl);
		mpeg2_reset_slice_state(dec->pic_ext.intra_dc_precision);

		// Reset motion vector prediction at slice start
		dec->mv_pred_h = 0;
		dec->mv_pred_v = 0;
		dec->mv_backward_pred_h = 0;
		dec->mv_backward_pred_v = 0;

		uint32 mby = sl.slice_vertical_position - 1;
		uint32 mbx = 0;

		while (mbx < dec->mb_width && mpeg2_bits_available(bs, 8)) {
			if (mpeg2_bits_peek(bs, 23) == 0) break;

			mpeg2_macroblock mb;
			mb.quantiser_scale_code = sl.quantiser_scale_code;
			status_t st;

			if (is_b)
				st = mpeg2_decode_b_macroblock(bs, dec, &mb);
			else if (is_p)
				st = mpeg2_decode_p_macroblock(bs, dec, &mb);
			else
				st = mpeg2_decode_intra_macroblock(bs, dec, &mb);

			if (st != B_OK) {
				mbx++;
				continue;
			}

			// Handle skipped MBs (addr_inc > 1)
			if (mb.address_increment > 1) {
				uint32 skip_count = mb.address_increment - 1;
				if ((is_p || is_b) && fwd_ref) {
					// Skipped MBs in P/B-frame: copy from forward reference
					// (zero motion vector, no residual)
					for (uint32 s = 0; s < skip_count && mbx < dec->mb_width; s++) {
						// Copy Y blocks
						for (int b = 0; b < 4; b++) {
							uint32 bx = mbx * 16 + ydx[b];
							uint32 by = mby * 16 + ydy[b];
							for (int r = 0; r < 8; r++)
								for (int c = 0; c < 8; c++)
									if (bx+c < w && by+r < h)
										dst->y[(by+r)*w + bx+c] =
											fwd_ref->y[(by+r)*w + bx+c];
						}
						// Copy chroma
						uint32 cx = mbx * 8, cy = mby * 8;
						for (int r = 0; r < 8; r++)
							for (int c = 0; c < 8; c++)
								if (cx+c < cw && cy+r < ch) {
									dst->cb[(cy+r)*cw + cx+c] =
										fwd_ref->cb[(cy+r)*cw + cx+c];
									dst->cr[(cy+r)*cw + cx+c] =
										fwd_ref->cr[(cy+r)*cw + cx+c];
								}
						mbx++;
						mb_decoded++;
					}
				} else {
					mbx += skip_count;
				}
			}

			// Update quantiser scale
			dec->quantiser_scale = mpeg2_compute_quantiser_scale(
				mb.quantiser_scale_code, dec->pic_ext.q_scale_type);

			if (mb.mb_type & MB_INTRA) {
				// Intra MB: decode all 6 blocks, no MC
				// Reset MV prediction on intra MB in P/B-frame
				if (is_p || is_b) {
					dec->mv_pred_h = 0;
					dec->mv_pred_v = 0;
					dec->mv_backward_pred_h = 0;
					dec->mv_backward_pred_v = 0;
				}
				for (int b = 0; b < 4; b++) {
					uint8 pix[64];
					decode_intra_pixels(mb.blocks[b], pix);
					write_block(dst->y, w, w, h,
						mbx*16 + ydx[b], mby*16 + ydy[b], pix, 8, 8);
				}
				for (int c = 0; c < 2; c++) {
					uint8 pix[64];
					decode_intra_pixels(mb.blocks[4+c], pix);
					write_block(c == 0 ? dst->cb : dst->cr, cw, cw, ch,
						mbx*8, mby*8, pix, 8, 8);
				}
			} else if (is_p && fwd_ref) {
				// P-frame inter MB with forward motion compensation
				int16 mv_h = mb.motion_h;
				int16 mv_v = mb.motion_v;

				// Y blocks: 4 blocks of 8x8 within the 16x16 MB
				for (int b = 0; b < 4; b++) {
					uint32 bx = mbx * 16 + ydx[b];
					uint32 by = mby * 16 + ydy[b];
					uint8 mc_ref_block[64];
					mc_block(fwd_ref->y, w, h, bx, by, mv_h, mv_v,
						mc_ref_block, 8, 8);

					if (mb.coded_block_pattern & (1 << (5 - b))) {
						uint8 pix[64];
						decode_inter_pixels(mb.blocks[b], mc_ref_block, pix);
						write_block(dst->y, w, w, h, bx, by, pix, 8, 8);
					} else {
						write_block(dst->y, w, w, h, bx, by,
							mc_ref_block, 8, 8);
					}
				}

				// Chroma: motion vectors scaled by /2
				int16 cmv_h = mv_h / 2;
				int16 cmv_v = mv_v / 2;
				for (int c = 0; c < 2; c++) {
					uint8* plane = (c == 0) ? dst->cb : dst->cr;
					const uint8* ref_plane = (c == 0) ? fwd_ref->cb : fwd_ref->cr;
					uint32 cx = mbx * 8, cy = mby * 8;
					uint8 mc_ref_block[64];
					mc_block(ref_plane, cw, ch, cx, cy, cmv_h, cmv_v,
						mc_ref_block, 8, 8);

					if (mb.coded_block_pattern & (1 << (1 - c))) {
						uint8 pix[64];
						decode_inter_pixels(mb.blocks[4+c],
							mc_ref_block, pix);
						write_block(plane, cw, cw, ch, cx, cy, pix, 8, 8);
					} else {
						write_block(plane, cw, cw, ch, cx, cy,
							mc_ref_block, 8, 8);
					}
				}
			} else if (is_b && fwd_ref && bwd_ref) {
				// B-frame inter MB with forward/backward/bidir MC
				int16 fwd_mv_h = mb.motion_h;
				int16 fwd_mv_v = mb.motion_v;
				int16 bwd_mv_h = mb.motion_backward_h;
				int16 bwd_mv_v = mb.motion_backward_v;
				bool has_fwd = (mb.mb_type & MB_MOTION_FORWARD) != 0;
				bool has_bwd = (mb.mb_type & MB_MOTION_BACKWARD) != 0;

				// Y blocks
				for (int b = 0; b < 4; b++) {
					uint32 bx = mbx * 16 + ydx[b];
					uint32 by = mby * 16 + ydy[b];
					uint8 mc_ref_block[64];

					if (has_fwd && has_bwd) {
						mc_block_bidir(fwd_ref->y, bwd_ref->y,
							w, h, bx, by,
							fwd_mv_h, fwd_mv_v,
							bwd_mv_h, bwd_mv_v,
							mc_ref_block, 8, 8);
					} else if (has_bwd) {
						mc_block(bwd_ref->y, w, h, bx, by,
							bwd_mv_h, bwd_mv_v,
							mc_ref_block, 8, 8);
					} else {
						mc_block(fwd_ref->y, w, h, bx, by,
							fwd_mv_h, fwd_mv_v,
							mc_ref_block, 8, 8);
					}

					if (mb.coded_block_pattern & (1 << (5 - b))) {
						uint8 pix[64];
						decode_inter_pixels(mb.blocks[b], mc_ref_block, pix);
						write_block(dst->y, w, w, h, bx, by, pix, 8, 8);
					} else {
						write_block(dst->y, w, w, h, bx, by,
							mc_ref_block, 8, 8);
					}
				}

				// Chroma: motion vectors scaled by /2
				int16 fwd_cmv_h = fwd_mv_h / 2;
				int16 fwd_cmv_v = fwd_mv_v / 2;
				int16 bwd_cmv_h = bwd_mv_h / 2;
				int16 bwd_cmv_v = bwd_mv_v / 2;
				for (int c = 0; c < 2; c++) {
					uint8* plane = (c == 0) ? dst->cb : dst->cr;
					uint32 cx = mbx * 8, cy = mby * 8;
					uint8 mc_ref_block[64];

					if (has_fwd && has_bwd) {
						const uint8* fwd_plane = (c == 0) ? fwd_ref->cb : fwd_ref->cr;
						const uint8* bwd_plane = (c == 0) ? bwd_ref->cb : bwd_ref->cr;
						mc_block_bidir(fwd_plane, bwd_plane,
							cw, ch, cx, cy,
							fwd_cmv_h, fwd_cmv_v,
							bwd_cmv_h, bwd_cmv_v,
							mc_ref_block, 8, 8);
					} else if (has_bwd) {
						const uint8* ref_plane = (c == 0) ? bwd_ref->cb : bwd_ref->cr;
						mc_block(ref_plane, cw, ch, cx, cy,
							bwd_cmv_h, bwd_cmv_v,
							mc_ref_block, 8, 8);
					} else {
						const uint8* ref_plane = (c == 0) ? fwd_ref->cb : fwd_ref->cr;
						mc_block(ref_plane, cw, ch, cx, cy,
							fwd_cmv_h, fwd_cmv_v,
							mc_ref_block, 8, 8);
					}

					if (mb.coded_block_pattern & (1 << (1 - c))) {
						uint8 pix[64];
						decode_inter_pixels(mb.blocks[4+c],
							mc_ref_block, pix);
						write_block(plane, cw, cw, ch, cx, cy, pix, 8, 8);
					} else {
						write_block(plane, cw, cw, ch, cx, cy,
							mc_ref_block, 8, 8);
					}
				}
			}

			mb_decoded++;
			mbx++;
		}
	}

	return mb_decoded;
}


// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
	const char* inpath = (argc > 1) ? argv[1] : "test_ip.m2v";
	const char* outdir = (argc > 2) ? argv[2] : "/tmp";

	FILE* f = fopen(inpath, "rb");
	if (!f) { printf("Cannot open %s\n", inpath); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(sz);
	fread(data, 1, sz, f); fclose(f);

	printf("Input: %s (%ld bytes)\n", inpath, sz);

	mpeg2_bits bs;
	mpeg2_bits_init(&bs, data, sz);
	mpeg2_decoder dec;
	memset(&dec, 0, sizeof(dec));

	frame_planes current, fwd_reference, bwd_reference;
	bool has_fwd_ref = false;
	bool has_bwd_ref = false;
	int frame_num = 0;

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;

		if (code == MPEG2_SEQUENCE_HEADER_CODE) {
			mpeg2_parse_sequence_header(&bs, &dec.seq);
		} else if (code == MPEG2_PICTURE_START_CODE) {
			mpeg2_parse_picture_header(&bs, &dec.pic);
		} else if (code == MPEG2_EXTENSION_START_CODE) {
			uint32 ext_id = mpeg2_bits_peek(&bs, 4);
			if (ext_id == 8) {
				mpeg2_bits_skip(&bs, 4);
				mpeg2_parse_picture_coding_extension(&bs, &dec.pic_ext);
				mpeg2_decoder_init(&dec);
			} else if (ext_id == 1) {
				mpeg2_bits_skip(&bs, 4 + 44);
			}
		} else if (code >= MPEG2_SLICE_START_MIN
			&& code <= MPEG2_SLICE_START_MAX) {

			if (!dec.initialized) continue;

			uint32 w = dec.seq.width, h = dec.seq.height;
			int pic_type = dec.pic.picture_coding_type;

			printf("\n--- Frame %d: %s-picture %ux%u ---\n",
				frame_num, pic_type == 1 ? "I" : pic_type == 2 ? "P" : "B",
				w, h);

			// Allocate current frame
			frame_alloc(&current, w, h);

			// For P-frame, start from forward reference (skipped MBs copy)
			if (pic_type == 2 && has_fwd_ref)
				frame_copy(&current, &fwd_reference);
			// For B-frame, start from forward reference too
			if (pic_type == 3 && has_fwd_ref)
				frame_copy(&current, &fwd_reference);

			// Push back so decode_picture can find the slice codes
			bs.byte_pos -= 4;
			bs.bit_pos = 0;

			// For B-frames: fwd_ref = older I/P, bwd_ref = newer I/P
			// In bitstream order, bwd_reference is the most recent I/P
			// and fwd_reference is the one before that.
			const frame_planes* fwd_ptr = NULL;
			const frame_planes* bwd_ptr = NULL;
			if (pic_type == 3) {
				// B-frame: fwd = older ref, bwd = newer ref
				fwd_ptr = has_bwd_ref ? &bwd_reference : NULL;
				bwd_ptr = has_fwd_ref ? &fwd_reference : NULL;
			} else {
				fwd_ptr = has_fwd_ref ? &fwd_reference : NULL;
			}

			uint32 mb_ok = decode_picture(&bs, &dec,
				&current, fwd_ptr, bwd_ptr);

			printf("Decoded %u MBs (expected %u)\n",
				mb_ok, dec.mb_count);

			// Write output
			char outpath[256];
			snprintf(outpath, sizeof(outpath), "%s/frame_%02d_%c.ppm",
				outdir, frame_num,
				pic_type == 1 ? 'I' : pic_type == 2 ? 'P' : 'B');
			write_ppm(outpath, &current);

			// Update reference frames: I and P become references,
			// B-frames do NOT update references.
			if (pic_type == 1 || pic_type == 2) {
				// Shift: old forward becomes backward
				if (has_fwd_ref) {
					if (has_bwd_ref) frame_free(&bwd_reference);
					frame_alloc(&bwd_reference, w, h);
					frame_copy(&bwd_reference, &fwd_reference);
					has_bwd_ref = true;
					frame_free(&fwd_reference);
				}
				frame_alloc(&fwd_reference, w, h);
				frame_copy(&fwd_reference, &current);
				has_fwd_ref = true;
			}

			frame_free(&current);
			frame_num++;
		}
	}

	if (has_fwd_ref) frame_free(&fwd_reference);
	if (has_bwd_ref) frame_free(&bwd_reference);
	free(data);
	printf("\nDone: %d frames decoded.\n", frame_num);
	return 0;
}
