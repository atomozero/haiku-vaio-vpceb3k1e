/*
 * Decode MPEG-2 I-frame to PPM image file (CPU-only, no reboot).
 * Proves the full pipeline visually: parse → IQ → IDCT → YCbCr→RGB → file.
 *
 * Build:
 *   g++ -Wall -O2 -I.. -I/boot/system/develop/headers/os/support \
 *       -o test_decode_to_ppm test_decode_to_ppm.cpp ../mpeg2_parser.cpp
 *   ./test_decode_to_ppm [input.m2v] [output.ppm]
 */

#include "mpeg2_parser.h"
#include "iq_intra_ref.h"
#include "idct_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int _sPrintf(const char* format, ...) {
	va_list args; va_start(args, format);
	int r = vprintf(format, args); va_end(args); return r;
}

static void
decode_block(const int16 coeffs[64], const uint8 qm[64],
	uint16 qs, uint16 dc_mult, uint8 pixels[64])
{
	// Parser now does IQ inline (with correct sign handling and
	// mismatch control). coeffs[] already contains IQ'd values.
	// Just do IDCT + clamp.
	(void)qm; (void)qs; (void)dc_mult;

	int16 idct[64];
	compute_idct_reference(coeffs, idct);
	for (int i = 0; i < 64; i++) {
		int32 v = idct[i];
		if (v < 0) v = 0;
		if (v > 255) v = 255;
		pixels[i] = (uint8)v;
	}
}

static inline uint8 clamp8(int32 v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8)v;
}

int main(int argc, char** argv)
{
	const char* inpath = (argc > 1) ? argv[1] : "test_gray_q31.m2v";
	const char* outpath = (argc > 2) ? argv[2]
		: "/tmp/decoded_frame.ppm";

	FILE* f = fopen(inpath, "rb");
	if (!f) { printf("Cannot open %s\n", inpath); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(sz); fread(data, 1, sz, f); fclose(f);

	printf("Input: %s (%ld bytes)\n", inpath, sz);

	mpeg2_bits bs;
	mpeg2_bits_init(&bs, data, sz);
	mpeg2_decoder dec;
	memset(&dec, 0, sizeof(dec));

	// Parse headers
	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;
		if (code == MPEG2_SEQUENCE_HEADER_CODE)
			mpeg2_parse_sequence_header(&bs, &dec.seq);
		else if (code == MPEG2_PICTURE_START_CODE)
			mpeg2_parse_picture_header(&bs, &dec.pic);
		else if (code == MPEG2_EXTENSION_START_CODE) {
			uint32 ext_id = mpeg2_bits_peek(&bs, 4);
			if (ext_id == 8) {
				mpeg2_bits_skip(&bs, 4);
				mpeg2_parse_picture_coding_extension(&bs, &dec.pic_ext);
				mpeg2_decoder_init(&dec);
			} else if (ext_id == 1) {
				mpeg2_bits_skip(&bs, 4 + 44);
			}
		} else if (code >= MPEG2_SLICE_START_MIN
			&& code <= MPEG2_SLICE_START_MAX) break;
	}
	if (!dec.initialized) {
		printf("Header parse failed\n");
		free(data); return 1;
	}

	uint32 w = dec.seq.width, h = dec.seq.height;
	uint32 cw = w/2, ch = h/2;
	printf("Frame: %ux%u (%ux%u MBs)\n", w, h, dec.mb_width, dec.mb_height);

	uint8* y_plane = (uint8*)calloc(w * h, 1);
	uint8* cb_plane = (uint8*)malloc(cw * ch); memset(cb_plane, 128, cw*ch);
	uint8* cr_plane = (uint8*)malloc(cw * ch); memset(cr_plane, 128, cw*ch);

	// Parse slices and decode all blocks
	mpeg2_bits_init(&bs, data, sz);
	uint32 mb_total = 0;
	static const int ydx[4]={0,8,0,8}, ydy[4]={0,0,8,8};

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;
		if (code < MPEG2_SLICE_START_MIN || code > MPEG2_SLICE_START_MAX)
			continue;

		mpeg2_slice_header sl;
		mpeg2_parse_slice_header(&bs, code, &sl);
		mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
		uint16 qs = mpeg2_compute_quantiser_scale(
			sl.quantiser_scale_code, dec.pic_ext.q_scale_type);

		uint32 mby = sl.slice_vertical_position - 1, mbx = 0;
		while (mbx < dec.mb_width && mpeg2_bits_available(&bs, 8)) {
			if (mpeg2_bits_peek(&bs, 23) == 0) break;
			mpeg2_macroblock mb;
			mb.quantiser_scale_code = sl.quantiser_scale_code;
			if (mpeg2_decode_intra_macroblock(&bs, &dec, &mb) != B_OK) {
				// Skip this MB but continue trying the next one.
				// The internal resync in decode_intra_macroblock already
				// attempted to find a valid position. If it failed,
				// the bitstream is likely past the error and may
				// recover at the next macroblock boundary.
				mbx++;
				continue;
			}

			// Handle address increment > 1 (skipped macroblocks).
			// Skipped MBs in I-frames keep DC prediction unchanged
			// and produce no output (black/zero in our buffer).
			if (mb.address_increment > 1)
				mbx += mb.address_increment - 1;

			// Decode 4 Y blocks
			for (int b = 0; b < 4; b++) {
				uint8 pix[64];
				decode_block(mb.blocks[b], dec.seq.intra_quantiser_matrix,
					qs, dec.intra_dc_mult, pix);
				uint32 bx = mbx*16 + ydx[b], by = mby*16 + ydy[b];
				for (int r = 0; r < 8; r++)
					for (int c = 0; c < 8; c++)
						if (bx+c < w && by+r < h)
							y_plane[(by+r)*w + bx+c] = pix[r*8+c];
			}

			// Decode Cb, Cr
			for (int c = 0; c < 2; c++) {
				uint8 pix[64];
				decode_block(mb.blocks[4+c],
					dec.seq.intra_quantiser_matrix,
					qs, dec.intra_dc_mult, pix);
				uint8* plane = (c == 0) ? cb_plane : cr_plane;
				uint32 bx = mbx*8, by = mby*8;
				for (int r = 0; r < 8; r++)
					for (int col = 0; col < 8; col++)
						if (bx+col < cw && by+r < ch)
							plane[(by+r)*cw + bx+col] = pix[r*8+col];
			}
			mb_total++;
			mbx++;
		}
	}
	printf("Decoded %u / %u MBs\n", mb_total, dec.mb_count);

	// Write PPM
	f = fopen(outpath, "wb");
	if (!f) { printf("Cannot write %s\n", outpath); free(data); return 1; }
	// BT.601 YCbCr → RGB conversion (studio range: Y 16-235, CbCr 16-240)
	// R = 1.164*(Y-16) + 1.596*(Cr-128)
	// G = 1.164*(Y-16) - 0.392*(Cb-128) - 0.813*(Cr-128)
	// B = 1.164*(Y-16) + 2.017*(Cb-128)
	// Fixed-point: multiply by 1024 (>> 10)
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (uint32 y = 0; y < h; y++) {
		for (uint32 x = 0; x < w; x++) {
			int32 yv = (int32)y_plane[y*w + x] - 16;
			int32 cbv = (int32)cb_plane[(y/2)*cw + (x/2)] - 128;
			int32 crv = (int32)cr_plane[(y/2)*cw + (x/2)] - 128;
			int32 r = (1192 * yv + 1634 * crv) >> 10;
			int32 g = (1192 * yv - 401 * cbv - 832 * crv) >> 10;
			int32 b = (1192 * yv + 2066 * cbv) >> 10;
			uint8 rgb[3] = { clamp8(r), clamp8(g), clamp8(b) };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	printf("Wrote %s (%ux%u)\n", outpath, w, h);

	free(y_plane); free(cb_plane); free(cr_plane); free(data);
	return 0;
}
