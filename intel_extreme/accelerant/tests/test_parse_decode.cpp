/*
 * Integration test: parse MPEG-2 I-frame → CPU decode → verify pixels.
 * This validates the parser output against known pixel values (ffmpeg).
 * Once this works, the same coefficients feed the GPU kernel.
 *
 * Build:
 *   g++ -Wall -O2 -I.. -I/boot/system/develop/headers/os/support \
 *       -o test_parse_decode test_parse_decode.cpp ../mpeg2_parser.cpp
 */

#include "mpeg2_parser.h"
#include "iq_intra_ref.h"
#include "idct_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int _sPrintf(const char* format, ...) {
	va_list args;
	va_start(args, format);
	int r = vprintf(format, args);
	va_end(args);
	return r;
}


// Full decode of one intra block: IQ → IDCT → +128 → clamp → U8
static void
decode_block_to_pixels(const int16 coeffs[64],
	const uint8 quant_matrix[64], uint16 q_scale, uint16 dc_mult,
	uint8 pixels[64])
{
	// IQ
	int16 iq_out[64];
	for (int i = 0; i < 64; i++) {
		int32 tmp = (int32)coeffs[i] * (int32)(uint32)quant_matrix[i];
		tmp *= (int32)(uint32)q_scale;
		tmp >>= 4;
		iq_out[i] = (int16)tmp;
	}
	iq_out[0] = (int16)((int32)coeffs[0] * (int32)(uint32)dc_mult);

	// IDCT
	int16 idct_out[64];
	compute_idct_reference(iq_out, idct_out);

	// Clamp to [0,255]. NO +128 offset: the DC predictor init value
	// (2^(7+dc_prec)) already embeds the level shift.
	for (int i = 0; i < 64; i++) {
		int32 val = (int32)idct_out[i];
		if (val < 0) val = 0;
		if (val > 255) val = 255;
		pixels[i] = (uint8)val;
	}
}


int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "test_gray_q31.m2v";

	FILE* f = fopen(path, "rb");
	if (!f) { printf("Cannot open %s\n", path); return 1; }
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(fsize);
	fread(data, 1, fsize, f);
	fclose(f);

	printf("=== MPEG-2 Parse+Decode Test ===\n");
	printf("File: %s (%ld bytes)\n\n", path, fsize);

	mpeg2_bits bs;
	mpeg2_bits_init(&bs, data, fsize);

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
			}
		} else if (code >= MPEG2_SLICE_START_MIN
			&& code <= MPEG2_SLICE_START_MAX) {
			// First slice found — start decoding
			break;
		}
	}

	if (!dec.initialized) {
		printf("Failed to parse headers\n");
		free(data);
		return 1;
	}

	printf("Frame: %ux%u, %ux%u MBs\n",
		dec.seq.width, dec.seq.height, dec.mb_width, dec.mb_height);

	// Allocate Y plane
	uint32 y_w = dec.seq.width;
	uint32 y_h = dec.seq.height;
	uint8* y_plane = (uint8*)calloc(y_w * y_h, 1);

	// Re-parse slices and decode all macroblocks
	mpeg2_bits_init(&bs, data, fsize);
	uint32 total_mb = 0;
	uint32 mb_x = 0, mb_y = 0;

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;

		if (code < MPEG2_SLICE_START_MIN || code > MPEG2_SLICE_START_MAX)
			continue;

		mpeg2_slice_header slice;
		mpeg2_parse_slice_header(&bs, code, &slice);
		mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);

		uint16 q_scale = mpeg2_compute_quantiser_scale(
			slice.quantiser_scale_code, dec.pic_ext.q_scale_type);
		uint16 dc_mult = dec.intra_dc_mult;

		mb_y = slice.slice_vertical_position - 1;
		mb_x = 0;

		while (mpeg2_bits_available(&bs, 8)) {
			if (mpeg2_bits_peek(&bs, 23) == 0)
				break;

			mpeg2_macroblock mb;
			mb.quantiser_scale_code = slice.quantiser_scale_code;
			status_t st = mpeg2_decode_intra_macroblock(&bs, &dec, &mb);
			if (st != B_OK) break;

			// Skip MBs when address_increment > 1
			if (mb.address_increment > 1)
				mb_x += mb.address_increment - 1;

			// Decode Y blocks to pixels and write to Y plane
			static const int y_dx[4] = { 0, 8, 0, 8 };
			static const int y_dy[4] = { 0, 0, 8, 8 };

			for (int b = 0; b < 4; b++) {
				uint8 pixels[64];
				decode_block_to_pixels(mb.blocks[b],
					dec.seq.intra_quantiser_matrix,
					q_scale, dc_mult, pixels);

				uint32 px = mb_x * 16 + y_dx[b];
				uint32 py = mb_y * 16 + y_dy[b];

				for (int row = 0; row < 8; row++) {
					for (int col = 0; col < 8; col++) {
						if (px + col < y_w && py + row < y_h)
							y_plane[(py + row) * y_w + (px + col)]
								= pixels[row * 8 + col];
					}
				}
			}

			total_mb++;
			mb_x++;
			if (mb_x >= dec.mb_width) {
				mb_x = 0;
				mb_y++;
			}
		}
	}

	printf("Decoded %u / %u MBs\n\n", total_mb, dec.mb_count);

	// Verify: for the gray test, ffmpeg gives all 128. Our fixed-point
	// IDCT may differ by ±2 (IEEE 1180 allows ±1 per pixel, but our
	// cosine table has its own precision). Accept ±2 tolerance.
	uint32 exact = 0;
	uint32 close = 0;	// within ±2
	uint32 wrong = 0;
	for (uint32 i = 0; i < y_w * y_h; i++) {
		int diff = (int)y_plane[i] - 128;
		if (diff == 0) exact++;
		else if (diff >= -2 && diff <= 2) close++;
		else wrong++;
	}

	printf("Y plane verification (ffmpeg reference = all 128):\n");
	printf("  Exact match (128): %u / %u\n", exact, y_w * y_h);
	printf("  Within ±2:         %u\n", close);
	printf("  Wrong (>±2):       %u\n", wrong);

	// Print first 8x8 block
	printf("  Y[0..7][0..7]:\n");
	for (int r = 0; r < 8; r++) {
		printf("    ");
		for (int c = 0; c < 8; c++)
			printf("%3u ", y_plane[r * y_w + c]);
		printf("\n");
	}

	bool passed = (wrong == 0);
	printf("\n=== %s ===\n", passed ? "PASSED" : "FAILED");

	free(y_plane);
	free(data);
	return passed ? 0 : 1;
}
