/*
 * Standalone test for MPEG-2 parser — parses a real .m2v I-frame.
 *
 * Build:
 *   g++ -Wall -O2 -I.. -I/boot/system/develop/headers/os/support \
 *       -o test_parser test_parser.cpp ../mpeg2_parser.cpp
 *   ./test_parser [file.m2v]
 */

#include "mpeg2_parser.h"
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

static const char* kDefaultFile = "first_iframe.m2v";

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : kDefaultFile;

	FILE* f = fopen(path, "rb");
	if (!f) {
		printf("Cannot open %s\n", path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(fsize);
	fread(data, 1, fsize, f);
	fclose(f);

	printf("=== MPEG-2 Parser Test ===\n");
	printf("File: %s (%ld bytes)\n\n", path, fsize);

	mpeg2_bits bs;
	mpeg2_bits_init(&bs, data, fsize);

	mpeg2_decoder dec;
	memset(&dec, 0, sizeof(dec));

	uint32 total_mb = 0;
	uint32 failed_mb = 0;

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0)
			break;

		if (code == MPEG2_SEQUENCE_HEADER_CODE) {
			mpeg2_parse_sequence_header(&bs, &dec.seq);
			printf("Sequence: %ux%u\n", dec.seq.width, dec.seq.height);

		} else if (code == MPEG2_PICTURE_START_CODE) {
			mpeg2_parse_picture_header(&bs, &dec.pic);

		} else if (code == MPEG2_EXTENSION_START_CODE) {
			uint32 ext_id = mpeg2_bits_peek(&bs, 4);
			if (ext_id == 8) {
				mpeg2_bits_skip(&bs, 4);
				mpeg2_parse_picture_coding_extension(&bs, &dec.pic_ext);
				mpeg2_decoder_init(&dec);
			} else if (ext_id == 1) {
				// Sequence extension: skip ext_id(4) + 44 bits of data
				mpeg2_bits_skip(&bs, 4 + 44);
			}

		} else if (code >= MPEG2_SLICE_START_MIN
			&& code <= MPEG2_SLICE_START_MAX) {

			mpeg2_slice_header slice;
			mpeg2_parse_slice_header(&bs, code, &slice);

			uint16 qs = mpeg2_compute_quantiser_scale(
				slice.quantiser_scale_code, dec.pic_ext.q_scale_type);

			// Decode all macroblocks in this slice
			uint32 slice_mb = 0;
			while (mpeg2_bits_available(&bs, 8)) {
				// Check for start code (end of slice)
				if (mpeg2_bits_peek(&bs, 23) == 0)
					break;

				mpeg2_macroblock mb;
				mb.quantiser_scale_code = slice.quantiser_scale_code;
				status_t st = mpeg2_decode_intra_macroblock(
					&bs, &dec, &mb);

				if (st != B_OK) {
					failed_mb++;
					printf("  Slice %u MB %u: FAILED at bit %u\n",
						slice.slice_vertical_position,
						slice_mb, mpeg2_bits_offset(&bs));
					break;	// skip rest of slice
				}

				total_mb++;
				slice_mb++;

				// Print first MB of first slice
				if (total_mb == 1) {
					printf("  First MB DC values: "
						"Y0=%d Y1=%d Y2=%d Y3=%d Cb=%d Cr=%d\n",
						mb.blocks[0][0], mb.blocks[1][0],
						mb.blocks[2][0], mb.blocks[3][0],
						mb.blocks[4][0], mb.blocks[5][0]);
				}
			}
			if (slice_mb > 0 && failed_mb == 0) {
				printf("  Slice %u: %u MBs decoded (q_scale=%u)\n",
					slice.slice_vertical_position, slice_mb, qs);
			}
		}
	}

	printf("\n=== Summary ===\n");
	printf("Total MBs decoded: %u (expected: %u)\n",
		total_mb, dec.mb_count);
	printf("Failed MBs: %u\n", failed_mb);
	printf("Result: %s\n",
		(total_mb == dec.mb_count && failed_mb == 0)
		? "PASSED" : "PARTIAL/FAILED");

	free(data);
	return (failed_mb == 0) ? 0 : 1;
}
