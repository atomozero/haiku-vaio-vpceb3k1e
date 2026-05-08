#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* format, ...) {
	va_list args; va_start(args, format);
	int r = vprintf(format, args); va_end(args); return r;
}
int main() {
	FILE* f = fopen("test_gray_q31.m2v", "rb");
	if (!f) { printf("cannot open file\n"); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8* d = (uint8*)malloc(sz); fread(d, 1, sz, f); fclose(f);
	mpeg2_bits bs; mpeg2_bits_init(&bs, d, sz);
	mpeg2_decoder dec; memset(&dec, 0, sizeof(dec));
	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;
		if (code == MPEG2_SEQUENCE_HEADER_CODE)
			mpeg2_parse_sequence_header(&bs, &dec.seq);
		else if (code == MPEG2_PICTURE_START_CODE)
			mpeg2_parse_picture_header(&bs, &dec.pic);
		else if (code == MPEG2_EXTENSION_START_CODE) {
			if (mpeg2_bits_peek(&bs, 4) == 8) {
				mpeg2_bits_skip(&bs, 4);
				mpeg2_parse_picture_coding_extension(&bs, &dec.pic_ext);
				mpeg2_decoder_init(&dec);
			}
		} else if (code >= MPEG2_SLICE_START_MIN && code <= MPEG2_SLICE_START_MAX) {
			mpeg2_slice_header sl;
			mpeg2_parse_slice_header(&bs, code, &sl);
			mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
			uint16 qs = mpeg2_compute_quantiser_scale(sl.quantiser_scale_code, dec.pic_ext.q_scale_type);
			printf("Slice %u: q_scale=%u, dc_mult=%u, dc_prec=%u\n",
				sl.slice_vertical_position, qs, dec.intra_dc_mult, dec.pic_ext.intra_dc_precision);
			if (mpeg2_bits_peek(&bs, 23) == 0) continue;
			mpeg2_macroblock mb;
			mb.quantiser_scale_code = sl.quantiser_scale_code;
			if (mpeg2_decode_intra_macroblock(&bs, &dec, &mb) == B_OK) {
				for (int b = 0; b < 6; b++)
					printf("  block[%d] coeff[0]=%d\n", b, mb.blocks[b][0]);
				// The DC coeff in the block IS the raw dc_pred value.
				// Check: dc_pred_init = 1 << (7 + dc_prec) = 128 for dc_prec=0
				// The MPEG-2 spec says pixel = IDCT(F'') + 128
				// F''[0][0] = dc_pred * dc_mult = 126 * 8 = 1008
				// But for pixel=128: we need IDCT to give 0, so F''=0, so dc_pred=0
				// This means dc_pred=128 already represents the +128 offset!
				// So we should NOT add +128 after IDCT when dc_pred includes it.
				//
				// Actually: dc_pred init = 128 = 2^7. dc_mult = 8 = 2^3.
				// 128 * 8 = 1024. IDCT of 1024 = 128. So IDCT(dc_pred*dc_mult) = 128.
				// If we also add +128 we get 256 -> clamp 255. WRONG.
				// The +128 is already embedded in dc_pred_init!
				printf("\n  CONCLUSION: dc_pred_init=128 already embeds the +128 level shift.\n");
				printf("  DO NOT add +128 after IDCT for intra blocks.\n");
				printf("  Expected pixel = IDCT(coeff[0]*dc_mult, rest) = ~%d\n",
					(int)((long long)mb.blocks[0][0] * dec.intra_dc_mult * 16383LL * 16383LL / (1LL << 31)));
			}
			break;
		}
	}
	free(d);
}
