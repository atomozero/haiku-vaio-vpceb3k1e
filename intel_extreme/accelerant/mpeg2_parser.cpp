/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Minimal MPEG-2 bitstream parser.
 * Reference: ISO/IEC 13818-2 (MPEG-2 Video)
 */

#include "mpeg2_parser.h"

#include <string.h>

extern "C" int _sPrintf(const char* format, ...);

#define LOG(x...) _sPrintf("intel_extreme mpeg2: " x)


// ---------------------------------------------------------------------------
// Zigzag scan order (ISO/IEC 13818-2 Table 7-2, normal scan)
// ---------------------------------------------------------------------------

const uint8 mpeg2_zigzag_scan[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

// Alternate scan (used when alternate_scan flag is set)
static const uint8 mpeg2_alternate_scan[64] = {
	 0,  8, 16, 24,  1,  9,  2, 10,
	17, 25, 32, 40, 48, 56, 57, 49,
	41, 33, 26, 18,  3, 11,  4, 12,
	19, 27, 34, 42, 50, 58, 35, 43,
	51, 59, 20, 28,  5, 13,  6, 14,
	21, 29, 36, 44, 52, 60, 37, 45,
	53, 61, 22, 30,  7, 15, 23, 31,
	38, 46, 54, 62, 39, 47, 55, 63,
};


// ---------------------------------------------------------------------------
// Default intra quantiser matrix (ISO/IEC 13818-2 Table 6-4)
// ---------------------------------------------------------------------------

const uint8 mpeg2_default_intra_matrix[64] = {
	 8, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83,
};


// ---------------------------------------------------------------------------
// Bitstream reader
// ---------------------------------------------------------------------------

void
mpeg2_bits_init(mpeg2_bits* bs, const uint8* data, uint32 size)
{
	bs->data = data;
	bs->size = size;
	bs->byte_pos = 0;
	bs->bit_pos = 0;
}


uint32
mpeg2_bits_read(mpeg2_bits* bs, uint32 n)
{
	uint32 value = 0;
	while (n > 0) {
		if (bs->byte_pos >= bs->size)
			return value;
		uint32 bits_left = 8 - bs->bit_pos;
		uint32 bits_to_read = (n < bits_left) ? n : bits_left;
		uint32 mask = (1u << bits_to_read) - 1;
		uint32 shift = bits_left - bits_to_read;
		value = (value << bits_to_read)
			| ((bs->data[bs->byte_pos] >> shift) & mask);
		n -= bits_to_read;
		bs->bit_pos += bits_to_read;
		if (bs->bit_pos >= 8) {
			bs->bit_pos = 0;
			bs->byte_pos++;
		}
	}
	return value;
}


uint32
mpeg2_bits_peek(mpeg2_bits* bs, uint32 n)
{
	mpeg2_bits saved = *bs;
	uint32 val = mpeg2_bits_read(bs, n);
	*bs = saved;
	return val;
}


void
mpeg2_bits_skip(mpeg2_bits* bs, uint32 n)
{
	uint32 total_bits = bs->bit_pos + n;
	bs->byte_pos += total_bits / 8;
	bs->bit_pos = total_bits % 8;
}


bool
mpeg2_bits_available(const mpeg2_bits* bs, uint32 n)
{
	uint32 remaining = (bs->size - bs->byte_pos) * 8 - bs->bit_pos;
	return remaining >= n;
}


void
mpeg2_bits_align(mpeg2_bits* bs)
{
	if (bs->bit_pos > 0) {
		bs->bit_pos = 0;
		bs->byte_pos++;
	}
}


uint32
mpeg2_bits_offset(const mpeg2_bits* bs)
{
	return bs->byte_pos * 8 + bs->bit_pos;
}


// ---------------------------------------------------------------------------
// Start code scanner
// ---------------------------------------------------------------------------

uint32
mpeg2_find_start_code(mpeg2_bits* bs)
{
	mpeg2_bits_align(bs);

	while (bs->byte_pos + 3 < bs->size) {
		if (bs->data[bs->byte_pos] == 0
			&& bs->data[bs->byte_pos + 1] == 0
			&& bs->data[bs->byte_pos + 2] == 1) {
			uint32 code = 0x00000100u | bs->data[bs->byte_pos + 3];
			bs->byte_pos += 4;
			bs->bit_pos = 0;
			return code;
		}
		bs->byte_pos++;
	}
	return 0;
}


// Seek forward to the next slice start code (0x000001xx, xx in 0x01..0xAF).
// Positions the bitstream AT the start code prefix so that the next call
// to mpeg2_find_start_code() will consume it. Returns true if found.
bool
mpeg2_resync_to_next_slice(mpeg2_bits* bs)
{
	mpeg2_bits_align(bs);

	while (bs->byte_pos + 3 < bs->size) {
		if (bs->data[bs->byte_pos] == 0
			&& bs->data[bs->byte_pos + 1] == 0
			&& bs->data[bs->byte_pos + 2] == 1) {
			uint8 id = bs->data[bs->byte_pos + 3];
			if (id >= 0x01 && id <= 0xAF)
				return true;
			// Not a slice — skip past this start code
			bs->byte_pos += 4;
			continue;
		}
		bs->byte_pos++;
	}
	return false;
}


// ---------------------------------------------------------------------------
// Header parsers
// ---------------------------------------------------------------------------

status_t
mpeg2_parse_sequence_header(mpeg2_bits* bs, mpeg2_sequence_header* seq)
{
	if (!mpeg2_bits_available(bs, 64))
		return B_BAD_DATA;

	seq->width = mpeg2_bits_read(bs, 12);
	seq->height = mpeg2_bits_read(bs, 12);
	seq->aspect_ratio_info = mpeg2_bits_read(bs, 4);
	seq->frame_rate_code = mpeg2_bits_read(bs, 4);
	seq->bit_rate = mpeg2_bits_read(bs, 18);
	mpeg2_bits_skip(bs, 1);		// marker_bit
	seq->vbv_buffer_size = mpeg2_bits_read(bs, 10);
	mpeg2_bits_skip(bs, 1);		// constrained_parameters_flag

	// Intra quantiser matrix
	seq->load_intra_quantiser_matrix = mpeg2_bits_read(bs, 1);
	if (seq->load_intra_quantiser_matrix) {
		for (int i = 0; i < 64; i++)
			seq->intra_quantiser_matrix[mpeg2_zigzag_scan[i]]
				= mpeg2_bits_read(bs, 8);
	} else {
		memcpy(seq->intra_quantiser_matrix,
			mpeg2_default_intra_matrix, 64);
	}

	// Non-intra quantiser matrix
	seq->load_non_intra_quantiser_matrix = mpeg2_bits_read(bs, 1);
	if (seq->load_non_intra_quantiser_matrix) {
		for (int i = 0; i < 64; i++)
			seq->non_intra_quantiser_matrix[mpeg2_zigzag_scan[i]]
				= mpeg2_bits_read(bs, 8);
	} else {
		memset(seq->non_intra_quantiser_matrix, 16, 64);
	}

	LOG("sequence: %ux%u, aspect=%u, framerate=%u\n",
		seq->width, seq->height,
		seq->aspect_ratio_info, seq->frame_rate_code);

	return B_OK;
}


status_t
mpeg2_parse_picture_header(mpeg2_bits* bs, mpeg2_picture_header* pic)
{
	if (!mpeg2_bits_available(bs, 29))
		return B_BAD_DATA;

	pic->temporal_reference = mpeg2_bits_read(bs, 10);
	pic->picture_coding_type = mpeg2_bits_read(bs, 3);
	pic->vbv_delay = mpeg2_bits_read(bs, 16);

	// Skip forward/backward motion vector info for P/B pictures
	if (pic->picture_coding_type == MPEG2_P_PICTURE
		|| pic->picture_coding_type == MPEG2_B_PICTURE) {
		mpeg2_bits_skip(bs, 4);	// full_pel + forward_f_code
	}
	if (pic->picture_coding_type == MPEG2_B_PICTURE) {
		mpeg2_bits_skip(bs, 4);	// full_pel + backward_f_code
	}

	// Skip extra information
	while (mpeg2_bits_available(bs, 1) && mpeg2_bits_peek(bs, 1) == 1) {
		mpeg2_bits_skip(bs, 9);	// extra_bit + extra_information
	}
	mpeg2_bits_skip(bs, 1);		// extra_bit_picture = 0

	const char* type_str = (pic->picture_coding_type == 1) ? "I"
		: (pic->picture_coding_type == 2) ? "P"
		: (pic->picture_coding_type == 3) ? "B" : "?";
	LOG("picture: type=%s, temporal_ref=%u\n",
		type_str, pic->temporal_reference);

	return B_OK;
}


status_t
mpeg2_parse_picture_coding_extension(mpeg2_bits* bs,
	mpeg2_picture_coding_extension* ext)
{
	if (!mpeg2_bits_available(bs, 30))
		return B_BAD_DATA;

	ext->f_code[0][0] = mpeg2_bits_read(bs, 4);
	ext->f_code[0][1] = mpeg2_bits_read(bs, 4);
	ext->f_code[1][0] = mpeg2_bits_read(bs, 4);
	ext->f_code[1][1] = mpeg2_bits_read(bs, 4);
	ext->intra_dc_precision = mpeg2_bits_read(bs, 2);
	ext->picture_structure = mpeg2_bits_read(bs, 2);
	ext->top_field_first = mpeg2_bits_read(bs, 1);
	ext->frame_pred_frame_dct = mpeg2_bits_read(bs, 1);
	ext->concealment_motion_vectors = mpeg2_bits_read(bs, 1);
	ext->q_scale_type = mpeg2_bits_read(bs, 1);
	ext->intra_vlc_format = mpeg2_bits_read(bs, 1);
	ext->alternate_scan = mpeg2_bits_read(bs, 1);
	// Remaining fields (repeat_first_field, chroma_420_type, etc.)
	// are not needed for decode — skip them.
	ext->progressive_frame = false;

	LOG("pic_ext: dc_prec=%u, structure=%u, q_scale_type=%u, "
		"intra_vlc=%u, alt_scan=%u\n",
		ext->intra_dc_precision, ext->picture_structure,
		ext->q_scale_type ? 1 : 0,
		ext->intra_vlc_format ? 1 : 0,
		ext->alternate_scan ? 1 : 0);

	return B_OK;
}


status_t
mpeg2_parse_slice_header(mpeg2_bits* bs, uint32 slice_code,
	mpeg2_slice_header* slice)
{
	slice->slice_vertical_position = slice_code & 0xff;

	if (!mpeg2_bits_available(bs, 6))
		return B_BAD_DATA;

	slice->quantiser_scale_code = mpeg2_bits_read(bs, 5);

	// Skip extra information
	while (mpeg2_bits_available(bs, 1) && mpeg2_bits_peek(bs, 1) == 1) {
		mpeg2_bits_skip(bs, 9);
	}
	mpeg2_bits_skip(bs, 1);		// extra_bit_slice = 0

	return B_OK;
}


// ---------------------------------------------------------------------------
// Quantiser scale computation
// ---------------------------------------------------------------------------

// Non-linear quantiser scale table (ISO/IEC 13818-2 Table 7-6)
static const uint8 non_linear_scale[32] = {
	 0,  1,  2,  3,  4,  5,  6,  7,
	 8, 10, 12, 14, 16, 18, 20, 22,
	24, 28, 32, 36, 40, 44, 48, 52,
	56, 64, 72, 80, 88, 96, 104, 112,
};

uint16
mpeg2_compute_quantiser_scale(uint8 q_scale_code, bool q_scale_type)
{
	if (q_scale_code == 0)
		return 1;	// avoid zero (undefined in spec)
	if (!q_scale_type)
		return q_scale_code * 2;	// linear
	return non_linear_scale[q_scale_code];
}


uint16
mpeg2_compute_intra_dc_mult(uint8 intra_dc_precision)
{
	// intra_dc_mult = 8 >> intra_dc_precision
	// 0 → 8, 1 → 4, 2 → 2, 3 → 1
	return 8 >> intra_dc_precision;
}


void
mpeg2_decoder_init(mpeg2_decoder* dec)
{
	dec->mb_width = (dec->seq.width + 15) / 16;
	dec->mb_height = (dec->seq.height + 15) / 16;
	dec->mb_count = dec->mb_width * dec->mb_height;

	dec->quantiser_scale = 1;
	dec->intra_dc_mult = mpeg2_compute_intra_dc_mult(
		dec->pic_ext.intra_dc_precision);

	dec->initialized = true;

	LOG("decoder: %ux%u macroblocks (%u total), dc_mult=%u\n",
		dec->mb_width, dec->mb_height, dec->mb_count, dec->intra_dc_mult);
}


// ---------------------------------------------------------------------------
// VLC tables for DCT coefficient decoding
// ---------------------------------------------------------------------------

// DC size VLC tables (ISO/IEC 13818-2 Tables B-12, B-13)
// Format: { code_length, dc_size }

struct vlc_entry {
	uint8	length;		// number of bits
	int16	value;		// decoded value (dc_size or run/level)
};

// Intra DC luminance size table (Table B-12)
// dc_size 0..11, variable length 2..9 bits
static const vlc_entry dc_lum_size_tab[] = {
	// Indexed by lookup — we use sequential search for simplicity.
	// code → dc_size, length
	// 100        → 0, 3
	// 00         → 1, 2
	// 01         → 2, 2
	// 101        → 3, 3
	// 110        → 4, 3
	// 1110       → 5, 4
	// 11110      → 6, 5
	// 111110     → 7, 6
	// 1111110    → 8, 7
	// 11111110   → 9, 8
	// 111111110  → 10, 9
	// 111111111  → 11, 9
};

// Decode DC size for luminance (Table B-12).
static int
decode_dc_size_luminance(mpeg2_bits* bs)
{
	uint32 code = mpeg2_bits_peek(bs, 9);

	if ((code >> 7) == 0b00) {
		mpeg2_bits_skip(bs, 2); return 1;
	}
	if ((code >> 7) == 0b01) {
		mpeg2_bits_skip(bs, 2); return 2;
	}
	if ((code >> 6) == 0b100) {
		mpeg2_bits_skip(bs, 3); return 0;
	}
	if ((code >> 6) == 0b101) {
		mpeg2_bits_skip(bs, 3); return 3;
	}
	if ((code >> 6) == 0b110) {
		mpeg2_bits_skip(bs, 3); return 4;
	}
	if ((code >> 5) == 0b1110) {
		mpeg2_bits_skip(bs, 4); return 5;
	}
	if ((code >> 4) == 0b11110) {
		mpeg2_bits_skip(bs, 5); return 6;
	}
	if ((code >> 3) == 0b111110) {
		mpeg2_bits_skip(bs, 6); return 7;
	}
	if ((code >> 2) == 0b1111110) {
		mpeg2_bits_skip(bs, 7); return 8;
	}
	if ((code >> 1) == 0b11111110) {
		mpeg2_bits_skip(bs, 8); return 9;
	}
	if (code == 0b111111110) {
		mpeg2_bits_skip(bs, 9); return 10;
	}
	if (code == 0b111111111) {
		mpeg2_bits_skip(bs, 9); return 11;
	}
	return -1;	// error
}

// Decode DC size for chrominance (Table B-13).
static int
decode_dc_size_chrominance(mpeg2_bits* bs)
{
	uint32 code = mpeg2_bits_peek(bs, 10);

	if ((code >> 8) == 0b00) {
		mpeg2_bits_skip(bs, 2); return 0;
	}
	if ((code >> 8) == 0b01) {
		mpeg2_bits_skip(bs, 2); return 1;
	}
	if ((code >> 8) == 0b10) {
		mpeg2_bits_skip(bs, 2); return 2;
	}
	if ((code >> 7) == 0b110) {
		mpeg2_bits_skip(bs, 3); return 3;
	}
	if ((code >> 6) == 0b1110) {
		mpeg2_bits_skip(bs, 4); return 4;
	}
	if ((code >> 5) == 0b11110) {
		mpeg2_bits_skip(bs, 5); return 5;
	}
	if ((code >> 4) == 0b111110) {
		mpeg2_bits_skip(bs, 6); return 6;
	}
	if ((code >> 3) == 0b1111110) {
		mpeg2_bits_skip(bs, 7); return 7;
	}
	if ((code >> 2) == 0b11111110) {
		mpeg2_bits_skip(bs, 8); return 8;
	}
	if ((code >> 1) == 0b111111110) {
		mpeg2_bits_skip(bs, 9); return 9;
	}
	if (code == 0b1111111110) {
		mpeg2_bits_skip(bs, 10); return 10;
	}
	if (code == 0b1111111111) {
		mpeg2_bits_skip(bs, 10); return 11;
	}
	return -1;
}


// Decode DC differential value from dc_size bits.
static int16
decode_dc_differential(mpeg2_bits* bs, int dc_size)
{
	if (dc_size == 0)
		return 0;

	int16 half_range = 1 << (dc_size - 1);
	int16 val = mpeg2_bits_read(bs, dc_size);

	// If MSB is 0, value is negative (val - (2*half_range - 1))
	if (val < half_range)
		val = val - (2 * half_range - 1);

	return val;
}


// ---------------------------------------------------------------------------
// AC coefficient VLC decoding (Table B-14 / B-15)
// ---------------------------------------------------------------------------

// AC coefficient decoder using Table B-14 (non-intra VLC, or
// intra_vlc_format=0). For intra_vlc_format=1, use decode_ac_coeff_b15()
// which implements Table B-15.
//
// Returns: run in bits [13:8], level in bits [7:0], or -1 for EOB,
// -2 for escape, -3 for error.
//
// Table B-14 first entries (most common):
//   EOB: 10 (2 bits) — but first coefficient uses 1s for EOB...
//   Actually for MPEG-2 non-first coefficient: EOB = "10" (2 bits)
//   Escape: 000001 (6 bits)

// End of block marker
#define VLC_EOB		(-1)
#define VLC_ESCAPE	(-2)
#define VLC_ERROR	(-3)

// Debug: set to 1 to trace each AC coefficient decode
int mpeg2_vlc_trace = 0;

// Simple run/level entry
struct run_level {
	int8	run;
	int16	level;
};

// Table B-14 AC coefficient VLC (ISO/IEC 13818-2).
// Format: { bits (without sign), code_length, run, level }
struct ac_vlc_entry {
	uint16	code;
	uint8	length;
	int8	run;
	int16	level;
};

// Sorted by decreasing code length for longest-match-first decoding.
// The sign bit is NOT included — it's read separately after the code.
// EOB = run=-1; Escape = run=-2.
// CORRECTED: codes derived from ffmpeg's ff_mpeg1_vlc_table which matches
// the actual ISO/IEC 13818-2 Table B-14. The code-to-(run,level) mapping
// is NOT sequential — codes are assigned by statistical frequency.
static const ac_vlc_entry ac_table_b14[] = {
	// 2 bits
	{ 0b10,           2, -1, 0 },	// EOB (non-first coeff only)
	{ 0b11,           2,  0,  1 },	// run=0, level=1 (subsequent coeff)
	// Note: for the first coeff of non-intra blocks, '1s' is used instead
	// (handled via the first_coeff path in decode_ac_coeff_b14).
	// 3 bits
	{ 0b011,                3,  1,  1 },
	// 4 bits
	{ 0b0100,               4,  0,  2 },
	{ 0b0101,               4,  2,  1 },
	// 5 bits
	{ 0b00101,              5,  0,  3 },
	{ 0b00110,              5,  4,  1 },
	{ 0b00111,              5,  3,  1 },
	// 6 bits
	{ 0b000100,             6,  7,  1 },
	{ 0b000101,             6,  6,  1 },
	{ 0b000110,             6,  1,  2 },
	{ 0b000111,             6,  5,  1 },
	// 7 bits
	{ 0b0000100,            7,  2,  2 },
	{ 0b0000101,            7,  9,  1 },
	{ 0b0000110,            7,  0,  4 },
	{ 0b0000111,            7,  8,  1 },
	// 8 bits
	{ 0b00100000,           8, 13,  1 },
	{ 0b00100001,           8,  0,  6 },
	{ 0b00100010,           8, 12,  1 },
	{ 0b00100011,           8, 11,  1 },
	{ 0b00100100,           8,  3,  2 },
	{ 0b00100101,           8,  1,  3 },
	{ 0b00100110,           8,  0,  5 },
	{ 0b00100111,           8, 10,  1 },
	// 10 bits
	{ 0b0000001000,        10, 16,  1 },
	{ 0b0000001001,        10,  5,  2 },
	{ 0b0000001010,        10,  0,  7 },
	{ 0b0000001011,        10,  2,  3 },
	{ 0b0000001100,        10,  1,  4 },
	{ 0b0000001101,        10, 15,  1 },
	{ 0b0000001110,        10, 14,  1 },
	{ 0b0000001111,        10,  4,  2 },
	// 12 bits
	{ 0b000000010000,      12,  0, 11 },
	{ 0b000000010001,      12,  8,  2 },
	{ 0b000000010010,      12,  4,  3 },
	{ 0b000000010011,      12,  0, 10 },
	{ 0b000000010100,      12,  2,  4 },
	{ 0b000000010101,      12,  7,  2 },
	{ 0b000000010110,      12, 21,  1 },
	{ 0b000000010111,      12, 20,  1 },
	{ 0b000000011000,      12,  0,  9 },
	{ 0b000000011001,      12, 19,  1 },
	{ 0b000000011010,      12, 18,  1 },
	{ 0b000000011011,      12,  1,  5 },
	{ 0b000000011100,      12,  3,  3 },
	{ 0b000000011101,      12,  0,  8 },
	{ 0b000000011110,      12,  6,  2 },
	{ 0b000000011111,      12, 17,  1 },
	// 13 bits
	{ 0b0000000010000,     13, 10,  2 },
	{ 0b0000000010001,     13,  9,  2 },
	{ 0b0000000010010,     13,  5,  3 },
	{ 0b0000000010011,     13,  3,  4 },
	{ 0b0000000010100,     13,  2,  5 },
	{ 0b0000000010101,     13,  1,  7 },
	{ 0b0000000010110,     13,  1,  6 },
	{ 0b0000000010111,     13,  0, 15 },
	{ 0b0000000011000,     13,  0, 14 },
	{ 0b0000000011001,     13,  0, 13 },
	{ 0b0000000011010,     13,  0, 12 },
	{ 0b0000000011011,     13, 26,  1 },
	{ 0b0000000011100,     13, 25,  1 },
	{ 0b0000000011101,     13, 24,  1 },
	{ 0b0000000011110,     13, 23,  1 },
	{ 0b0000000011111,     13, 22,  1 },
	// 14 bits
	{ 0b00000000010000,    14,  0, 31 },
	{ 0b00000000010001,    14,  0, 30 },
	{ 0b00000000010010,    14,  0, 29 },
	{ 0b00000000010011,    14,  0, 28 },
	{ 0b00000000010100,    14,  0, 27 },
	{ 0b00000000010101,    14,  0, 26 },
	{ 0b00000000010110,    14,  0, 25 },
	{ 0b00000000010111,    14,  0, 24 },
	{ 0b00000000011000,    14,  0, 23 },
	{ 0b00000000011001,    14,  0, 22 },
	{ 0b00000000011010,    14,  0, 21 },
	{ 0b00000000011011,    14,  0, 20 },
	{ 0b00000000011100,    14,  0, 19 },
	{ 0b00000000011101,    14,  0, 18 },
	{ 0b00000000011110,    14,  0, 17 },
	{ 0b00000000011111,    14,  0, 16 },
	// 15 bits
	{ 0b000000000010000,   15,  0, 40 },
	{ 0b000000000010001,   15,  0, 39 },
	{ 0b000000000010010,   15,  0, 38 },
	{ 0b000000000010011,   15,  0, 37 },
	{ 0b000000000010100,   15,  0, 36 },
	{ 0b000000000010101,   15,  0, 35 },
	{ 0b000000000010110,   15,  0, 34 },
	{ 0b000000000010111,   15,  0, 33 },
	{ 0b000000000011000,   15,  0, 32 },
	{ 0b000000000011001,   15,  1, 14 },
	{ 0b000000000011010,   15,  1, 13 },
	{ 0b000000000011011,   15,  1, 12 },
	{ 0b000000000011100,   15,  1, 11 },
	{ 0b000000000011101,   15,  1, 10 },
	{ 0b000000000011110,   15,  1,  9 },
	{ 0b000000000011111,   15,  1,  8 },
	// 16 bits
	{ 0b0000000000010000,  16,  1, 18 },
	{ 0b0000000000010001,  16,  1, 17 },
	{ 0b0000000000010010,  16,  1, 16 },
	{ 0b0000000000010011,  16,  1, 15 },
	{ 0b0000000000010100,  16,  6,  3 },
	{ 0b0000000000010101,  16, 16,  2 },
	{ 0b0000000000010110,  16, 15,  2 },
	{ 0b0000000000010111,  16, 14,  2 },
	{ 0b0000000000011000,  16, 13,  2 },
	{ 0b0000000000011001,  16, 12,  2 },
	{ 0b0000000000011010,  16, 11,  2 },
	{ 0b0000000000011011,  16, 31,  1 },
	{ 0b0000000000011100,  16, 30,  1 },
	{ 0b0000000000011101,  16, 29,  1 },
	{ 0b0000000000011110,  16, 28,  1 },
	{ 0b0000000000011111,  16, 27,  1 },
	// Escape code
	{ 0b000001,            6, -2,  0 },
};

static const int ac_table_b14_count =
	sizeof(ac_table_b14) / sizeof(ac_table_b14[0]);

// Decode one AC coefficient using Table B-14.
static run_level
decode_ac_coeff_b14(mpeg2_bits* bs, bool first_coeff)
{
	run_level rl = { -3, 0 };

	if (!mpeg2_bits_available(bs, 2))
		return rl;

	uint32 bit_before = mpeg2_bits_offset(bs);

	// EOB: '10' (2 bits). For MPEG-2 intra blocks, DC is coded separately,
	// so all AC coefficients (including the first) use the "subsequent"
	// column where '10' = EOB. The first_coeff parameter is only relevant
	// for MPEG-1 or MPEG-2 non-intra blocks where DC is part of the VLC.
	if (mpeg2_bits_peek(bs, 2) == 0b10) {
		mpeg2_bits_skip(bs, 2);
		rl.run = -1;
		if (mpeg2_vlc_trace) LOG("  vlc @%u: EOB (2 bits)\n", bit_before);
		return rl;
	}

	// '1s' special code: ONLY for the first coefficient of a non-intra
	// block (ISO/IEC 13818-2 §B.5.1 Note). For all other positions,
	// '1' at the MSB is the start of a longer VLC code ('110s'=run 0
	// level 2, '111s'=run 1 level 1, etc.) and must NOT be intercepted.
	if (first_coeff && mpeg2_bits_peek(bs, 1) == 1) {
		mpeg2_bits_skip(bs, 1);
		uint32 sign = mpeg2_bits_read(bs, 1);
		rl.run = 0;
		rl.level = sign ? -1 : 1;
		if (mpeg2_vlc_trace) LOG("  vlc @%u: run=0 level=%d (2 bits, first_coeff)\n", bit_before, rl.level);
		return rl;
	}

	// Search the table for longest prefix match.
	uint32 peek = mpeg2_bits_peek(bs, 17);

	for (int i = 0; i < ac_table_b14_count; i++) {
		const ac_vlc_entry& e = ac_table_b14[i];
		// Skip EOB entry (handled above)
		if (e.run == -1)
			continue;

		uint32 shifted = peek >> (17 - e.length);
		if (shifted == e.code) {
			mpeg2_bits_skip(bs, e.length);

			if (e.run == -2) {
				// Escape: 6-bit run + 12-bit signed level
				rl.run = mpeg2_bits_read(bs, 6);
				int16 level = mpeg2_bits_read(bs, 12);
				if (level & 0x800)
					level |= 0xF000;
				rl.level = level;
				// Sanity check: escape codes at wrong positions
				// produce impossible run/level values. Treat as error
				// to prevent propagating the misalignment.
				if (rl.run >= 64 || rl.level == 0
					|| rl.level > 2047 || rl.level < -2048) {
					rl.run = -3;	// signal error
					return rl;
				}
			} else {
				uint32 sign = mpeg2_bits_read(bs, 1);
				rl.run = e.run;
				rl.level = sign ? -e.level : e.level;
			}
			if (mpeg2_vlc_trace)
				LOG("  vlc @%u: run=%d level=%d (%u bits)\n",
					bit_before, rl.run, rl.level,
					mpeg2_bits_offset(bs) - bit_before);
			return rl;
		}
	}

	// Check for start code prefix — 17 consecutive zero bits strongly
	// suggests we've reached byte-stuffing or a start code. Only
	// trigger on ALL zeros (peek==0) to avoid truncating legit codes.
	if (peek == 0) {
		rl.run = -1;	// treat as EOB
		return rl;
	}

	LOG("ac_vlc: unrecognized code at bit %u (peek17=0x%05x)\n",
		mpeg2_bits_offset(bs), peek);
	rl.run = -3;
	return rl;
}


// ---------------------------------------------------------------------------
// Table B-15 — AC coefficient VLC for intra blocks (intra_vlc_format=1)
// ---------------------------------------------------------------------------
// Derived from ffmpeg ff_mpeg2_vlc_table + ff_mpeg12_run + ff_mpeg12_level.
// Key differences from B-14:
//   - EOB = '0110' (4 bits) vs B-14 '10' (2 bits)
//   - NO '1s' special case for the first coefficient
//   - Different VLC codes for the same (run,level) pairs
//   - Escape = '000001' (6 bits) — same as B-14

static const ac_vlc_entry ac_table_b15[] = {
	// EOB: 4 bits
	{ 0b0110,                 4, -1, 0 },	// EOB
	// 2 bits
	{ 0b10,                   2,  0,  1 },	// run=0, level=1
	// 3 bits
	{ 0b110,                  3,  0,  2 },
	{ 0b010,                  3,  1,  1 },
	// 4 bits
	{ 0b0111,                 4,  0,  3 },
	// 5 bits
	{ 0b11100,                5,  0,  4 },
	{ 0b11101,                5,  0,  5 },
	{ 0b00101,                5,  2,  1 },
	{ 0b00110,                5,  1,  2 },
	{ 0b00111,                5,  3,  1 },
	// 6 bits
	{ 0b000101,               6,  0,  6 },
	{ 0b000100,               6,  0,  7 },
	{ 0b000001,               6, -2,  0 },	// Escape
	// 7 bits
	{ 0b0111011,              7,  1,  3 },
	{ 0b0111100,              7,  0,  8 },
	{ 0b1111000,              7,  9,  1 },
	{ 0b1111010,              7, 10,  1 },
	{ 0b0000111,              7,  2,  2 },
	{ 0b0000110,              7,  6,  1 },
	{ 0b0000100,              7,  8,  1 },
	{ 0b0000101,              7,  7,  1 },
	// 8 bits
	{ 0b00100011,             8,  0,  9 },
	{ 0b00100010,             8,  0, 10 },
	{ 0b11111010,             8,  0, 11 },
	{ 0b11111011,             8,  0, 12 },
	{ 0b11111110,             8,  0, 13 },
	{ 0b11111111,             8,  0, 14 },
	{ 0b00100111,             8,  1,  4 },
	{ 0b00100000,             8,  1,  5 },
	{ 0b11111100,             8,  2,  3 },
	{ 0b11111101,             8,  4,  1 },
	{ 0b00100110,             8,  3,  2 },
	{ 0b00100001,             8, 11,  1 },
	{ 0b00100101,             8, 12,  1 },
	{ 0b00100100,             8, 13,  1 },
	// 9 bits
	{ 0b000000100,            9,  5,  1 },
	{ 0b000000101,            9, 14,  1 },
	{ 0b000000111,            9, 15,  1 },
	// 10 bits
	{ 0b0000001100,          10,  2,  4 },
	{ 0b0000001101,          10, 16,  1 },
	// 12 bits
	{ 0b000000011100,        12,  3,  3 },
	{ 0b000000010010,        12,  4,  2 },
	{ 0b000000011110,        12,  6,  2 },
	{ 0b000000010101,        12,  7,  2 },
	{ 0b000000010001,        12,  8,  2 },
	{ 0b000000011111,        12, 17,  1 },
	{ 0b000000011010,        12, 18,  1 },
	{ 0b000000011001,        12, 19,  1 },
	{ 0b000000010111,        12, 20,  1 },
	{ 0b000000010110,        12, 21,  1 },
	// 13 bits
	{ 0b0000000010110,       13,  1,  6 },
	{ 0b0000000010101,       13,  1,  7 },
	{ 0b0000000010100,       13,  2,  5 },
	{ 0b0000000010011,       13,  3,  4 },
	{ 0b0000000010010,       13,  5,  2 },
	{ 0b0000000010001,       13,  9,  2 },
	{ 0b0000000010000,       13, 10,  2 },
	{ 0b0000000011111,       13, 22,  1 },
	{ 0b0000000011110,       13, 23,  1 },
	{ 0b0000000011101,       13, 24,  1 },
	{ 0b0000000011100,       13, 25,  1 },
	{ 0b0000000011011,       13, 26,  1 },
	// 14 bits
	{ 0b00000000011111,      14,  0, 16 },
	{ 0b00000000011110,      14,  0, 17 },
	{ 0b00000000011101,      14,  0, 18 },
	{ 0b00000000011100,      14,  0, 19 },
	{ 0b00000000011011,      14,  0, 20 },
	{ 0b00000000011010,      14,  0, 21 },
	{ 0b00000000011001,      14,  0, 22 },
	{ 0b00000000011000,      14,  0, 23 },
	{ 0b00000000010111,      14,  0, 24 },
	{ 0b00000000010110,      14,  0, 25 },
	{ 0b00000000010101,      14,  0, 26 },
	{ 0b00000000010100,      14,  0, 27 },
	{ 0b00000000010011,      14,  0, 28 },
	{ 0b00000000010010,      14,  0, 29 },
	{ 0b00000000010001,      14,  0, 30 },
	{ 0b00000000010000,      14,  0, 31 },
	// 15 bits
	{ 0b000000000011000,     15,  0, 32 },
	{ 0b000000000010111,     15,  0, 33 },
	{ 0b000000000010110,     15,  0, 34 },
	{ 0b000000000010101,     15,  0, 35 },
	{ 0b000000000010100,     15,  0, 36 },
	{ 0b000000000010011,     15,  0, 37 },
	{ 0b000000000010010,     15,  0, 38 },
	{ 0b000000000010001,     15,  0, 39 },
	{ 0b000000000010000,     15,  0, 40 },
	{ 0b000000000011111,     15,  1,  8 },
	{ 0b000000000011110,     15,  1,  9 },
	{ 0b000000000011101,     15,  1, 10 },
	{ 0b000000000011100,     15,  1, 11 },
	{ 0b000000000011011,     15,  1, 12 },
	{ 0b000000000011010,     15,  1, 13 },
	{ 0b000000000011001,     15,  1, 14 },
	// 16 bits
	{ 0b0000000000010011,    16,  1, 15 },
	{ 0b0000000000010010,    16,  1, 16 },
	{ 0b0000000000010001,    16,  1, 17 },
	{ 0b0000000000010000,    16,  1, 18 },
	{ 0b0000000000010100,    16,  6,  3 },
	{ 0b0000000000011010,    16, 11,  2 },
	{ 0b0000000000011001,    16, 12,  2 },
	{ 0b0000000000011000,    16, 13,  2 },
	{ 0b0000000000010111,    16, 14,  2 },
	{ 0b0000000000010110,    16, 15,  2 },
	{ 0b0000000000010101,    16, 16,  2 },
	{ 0b0000000000011111,    16, 27,  1 },
	{ 0b0000000000011110,    16, 28,  1 },
	{ 0b0000000000011101,    16, 29,  1 },
	{ 0b0000000000011100,    16, 30,  1 },
	{ 0b0000000000011011,    16, 31,  1 },
};

static const int ac_table_b15_count =
	sizeof(ac_table_b15) / sizeof(ac_table_b15[0]);


// Decode one AC coefficient using Table B-15 (intra_vlc_format=1).
// No special first_coeff handling — B-15 uses the same codes for all
// AC coefficients including the first.
static run_level
decode_ac_coeff_b15(mpeg2_bits* bs)
{
	run_level rl = { -3, 0 };

	if (!mpeg2_bits_available(bs, 2))
		return rl;

	uint32 bit_before = mpeg2_bits_offset(bs);

	// Search the table for longest prefix match (EOB included).
	uint32 peek = mpeg2_bits_peek(bs, 17);

	for (int i = 0; i < ac_table_b15_count; i++) {
		const ac_vlc_entry& e = ac_table_b15[i];

		uint32 shifted = peek >> (17 - e.length);
		if (shifted == e.code) {
			mpeg2_bits_skip(bs, e.length);

			if (e.run == -1) {
				// EOB
				rl.run = -1;
				if (mpeg2_vlc_trace)
					LOG("  vlc @%u: EOB (%u bits, B-15)\n",
						bit_before, e.length);
				return rl;
			}

			if (e.run == -2) {
				// Escape: 6-bit run + 12-bit signed level
				rl.run = mpeg2_bits_read(bs, 6);
				int16 level = mpeg2_bits_read(bs, 12);
				if (level & 0x800)
					level |= 0xF000;
				rl.level = level;
				if (rl.run >= 64 || rl.level == 0
					|| rl.level > 2047 || rl.level < -2048) {
					rl.run = -3;
					return rl;
				}
			} else {
				uint32 sign = mpeg2_bits_read(bs, 1);
				rl.run = e.run;
				rl.level = sign ? -e.level : e.level;
			}
			if (mpeg2_vlc_trace)
				LOG("  vlc @%u: run=%d level=%d (%u bits, B-15)\n",
					bit_before, rl.run, rl.level,
					mpeg2_bits_offset(bs) - bit_before);
			return rl;
		}
	}

	// Start code detection (same as B-14)
	if (peek == 0) {
		rl.run = -1;
		return rl;
	}

	LOG("ac_vlc_b15: unrecognized code at bit %u (peek17=0x%05x)\n",
		mpeg2_bits_offset(bs), peek);
	rl.run = -3;
	return rl;
}


// ---------------------------------------------------------------------------
// Intra macroblock decoder
// ---------------------------------------------------------------------------

// DC prediction state (reset per slice, one per component)
static int16 dc_pred[3] = { 0, 0, 0 };

// Reset DC prediction at the start of each slice.
static void
reset_dc_prediction(uint8 intra_dc_precision)
{
	// Initial DC predictor = 1 << (7 + intra_dc_precision)
	int16 init = 1 << (7 + intra_dc_precision);
	dc_pred[0] = init;	// Y
	dc_pred[1] = init;	// Cb
	dc_pred[2] = init;	// Cr
}


// Decode one 8x8 intra block with inline IQ (matching ffmpeg).
// cc = color component (0=Y, 1=Cb, 2=Cr)
// Output: block[0..63] contains IQ'd coefficients ready for IDCT.
//   DC: block[0] = dc_pred * intra_dc_mult
//   AC: block[j] = (|level| * qscale * quant_matrix[j]) >> 4, then negate if sign
// Sign is applied AFTER the >>4 shift (ffmpeg convention), NOT before.
// This avoids the ±1 rounding error from (-L*q*m)>>4 vs -(L*q*m>>4).
static status_t
decode_intra_block(mpeg2_bits* bs, const mpeg2_decoder* dec,
	int cc, int16 block[64])
{
	memset(block, 0, 64 * sizeof(int16));

	const uint8* scan = dec->pic_ext.alternate_scan
		? mpeg2_alternate_scan : mpeg2_zigzag_scan;
	const uint8* qmat = (cc == 0)
		? dec->seq.intra_quantiser_matrix
		: dec->seq.intra_quantiser_matrix;	// same matrix for chroma in spec
	uint16 qscale = dec->quantiser_scale;

	// DC coefficient
	uint32 block_start = mpeg2_bits_offset(bs);
	int dc_size;
	if (cc == 0)
		dc_size = decode_dc_size_luminance(bs);
	else
		dc_size = decode_dc_size_chrominance(bs);

	if (dc_size < 0)
		return B_BAD_DATA;

	int16 dc_diff = decode_dc_differential(bs, dc_size);
	if (mpeg2_vlc_trace)
		LOG("  blk cc=%d: DC @%u size=%d diff=%d (%u bits)\n",
			cc, block_start, dc_size, dc_diff,
			mpeg2_bits_offset(bs) - block_start);

	// Update DC predictor and apply IQ (dc * intra_dc_mult)
	dc_pred[cc] += dc_diff;
	block[0] = dc_pred[cc] * dec->intra_dc_mult;

	// AC coefficients with inline IQ (ffmpeg style).
	// Use Table B-15 when intra_vlc_format=1, Table B-14 otherwise.
	const bool use_b15 = dec->pic_ext.intra_vlc_format;
	int idx = 1;
	int32 mismatch = block[0];	// for mismatch control §7.4.3

	while (idx < 64) {
		// For intra blocks, first_coeff is always false — the '1s'
		// special code applies only to non-intra blocks (§B.5.1).
		run_level rl = use_b15 ? decode_ac_coeff_b15(bs)
		                       : decode_ac_coeff_b14(bs, false);
		if (rl.run == -1)	// EOB
			break;
		if (rl.run == -3) {
			// VLC error — absorb at block level: fill remaining
			// coefficients with zeros and end the block. The bit
			// position may be wrong, but continuing to the next block
			// gives the start-code check and EOB logic a chance to
			// resync without losing the entire macroblock/slice.
			break;
		}

		idx += rl.run;
		if (idx >= 64) {
			// Run overflows the block. The VLC bits were already consumed.
			// An EOB STILL follows in the bitstream (MPEG-2 §7.2.2.4
			// always includes EOB after the last coded coefficient,
			// even at index 63). Consume it to maintain alignment.
			run_level eob = use_b15 ? decode_ac_coeff_b15(bs)
			                        : decode_ac_coeff_b14(bs, false);
			(void)eob;
			break;
		}

		int j = scan[idx];

		// IQ with sign applied AFTER shift (ffmpeg convention):
		//   iq = (|level| * qscale * quant_matrix[j]) >> 4
		//   if (original was negative) iq = -iq
		int32 abs_level = rl.level < 0 ? -rl.level : rl.level;
		int32 iq = (abs_level * (int32)qscale * (int32)qmat[j]) >> 4;
		if (rl.level < 0)
			iq = -iq;

		block[j] = (int16)iq;
		mismatch ^= iq;
		idx++;
	}

	// If the loop exited because idx reached 64 (all coefficients coded),
	// there is STILL an EOB in the bitstream. MPEG-2 always places EOB
	// after the last AC coefficient, even when it's at index 63.
	// Consume it to maintain bitstream alignment.
	if (idx >= 64) {
		run_level eob = use_b15 ? decode_ac_coeff_b15(bs)
		                        : decode_ac_coeff_b14(bs, false);
		(void)eob;
	}

	// Mismatch control (§7.4.3): if the sum of all coefficients
	// (including DC) is even, flip the LSB of block[63].
	// This prevents ringing artifacts from IDCT precision limits.
	if ((mismatch & 1) == 0)
		block[63] ^= 1;

	return B_OK;
}


// ---------------------------------------------------------------------------
// Macroblock address increment VLC (Table B-1)
// ---------------------------------------------------------------------------

static int
decode_macroblock_address_increment(mpeg2_bits* bs)
{
	// ISO/IEC 13818-2 §6.2.3.1: nextbits() with 23 zero bits
	// indicates next_start_code() — end of slice macroblock data.
	// Return -2 (distinct from -1 error) to signal "end of slice".
	if (mpeg2_bits_available(bs, 23) && mpeg2_bits_peek(bs, 23) == 0) {
		return -2;	// end of slice (start code ahead)
	}

	// Most common: '1' → increment = 1
	if (mpeg2_bits_peek(bs, 1) == 1) {
		mpeg2_bits_skip(bs, 1);
		return 1;
	}
	// '011' → 2
	if (mpeg2_bits_peek(bs, 3) == 0b011) {
		mpeg2_bits_skip(bs, 3);
		return 2;
	}
	// '010' → 3
	if (mpeg2_bits_peek(bs, 3) == 0b010) {
		mpeg2_bits_skip(bs, 3);
		return 3;
	}
	// '0011' → 4
	if (mpeg2_bits_peek(bs, 4) == 0b0011) {
		mpeg2_bits_skip(bs, 4);
		return 4;
	}
	// '0010' → 5
	if (mpeg2_bits_peek(bs, 4) == 0b0010) {
		mpeg2_bits_skip(bs, 4);
		return 5;
	}
	// 5-bit codes
	if (mpeg2_bits_peek(bs, 5) == 0b00011) {
		mpeg2_bits_skip(bs, 5); return 6;
	}
	if (mpeg2_bits_peek(bs, 5) == 0b00010) {
		mpeg2_bits_skip(bs, 5); return 7;
	}
	// 7-bit codes
	uint32 peek7 = mpeg2_bits_peek(bs, 7);
	if (peek7 == 0b0000111) { mpeg2_bits_skip(bs, 7); return 8; }
	if (peek7 == 0b0000110) { mpeg2_bits_skip(bs, 7); return 9; }
	// 8-bit codes
	uint32 peek8 = mpeg2_bits_peek(bs, 8);
	if (peek8 == 0b00001011) { mpeg2_bits_skip(bs, 8); return 10; }
	if (peek8 == 0b00001010) { mpeg2_bits_skip(bs, 8); return 11; }
	if (peek8 == 0b00001001) { mpeg2_bits_skip(bs, 8); return 12; }
	if (peek8 == 0b00001000) { mpeg2_bits_skip(bs, 8); return 13; }
	if (peek8 == 0b00000111) { mpeg2_bits_skip(bs, 8); return 14; }
	if (peek8 == 0b00000110) { mpeg2_bits_skip(bs, 8); return 15; }
	// 10-bit codes
	uint32 peek10 = mpeg2_bits_peek(bs, 10);
	if (peek10 >= 0b0000010100 && peek10 <= 0b0000010111) {
		mpeg2_bits_skip(bs, 10); return 16 + (int)(peek10 - 0b0000010100);
	}
	if (peek10 >= 0b0000010000 && peek10 <= 0b0000010011) {
		mpeg2_bits_skip(bs, 10); return 20 + (int)(peek10 - 0b0000010000);
	}
	// 11-bit codes
	uint32 peek11 = mpeg2_bits_peek(bs, 11);
	if (peek11 >= 0b00000011000 && peek11 <= 0b00000011111) {
		mpeg2_bits_skip(bs, 11); return 24 + (int)(peek11 - 0b00000011000);
	}
	if (peek11 == 0b00000001000) {
		// macroblock_escape: add 33 and continue reading
		mpeg2_bits_skip(bs, 11);
		int next = decode_macroblock_address_increment(bs);
		return (next > 0) ? 33 + next : -1;
	}
	// macroblock_stuffing: '00000001111' (11 bits) — discard and retry
	if (peek11 == 0b00000001111) {
		mpeg2_bits_skip(bs, 11);
		return decode_macroblock_address_increment(bs);
	}
	LOG("mb_addr_inc: unrecognized at bit %u\n", mpeg2_bits_offset(bs));
	return -1;
}


// ---------------------------------------------------------------------------
// Macroblock type for I-pictures (Table B-2)
// ---------------------------------------------------------------------------

// I-picture macroblock types:
//   '1'  → intra (no quantiser scale change)
//   '01' → intra + quantiser (new quantiser_scale_code follows)
static int
decode_macroblock_type_i(mpeg2_bits* bs, bool* has_quant)
{
	if (mpeg2_bits_peek(bs, 1) == 1) {
		mpeg2_bits_skip(bs, 1);
		*has_quant = false;
		return 0;	// intra, no quant change
	}
	if (mpeg2_bits_peek(bs, 2) == 0b01) {
		mpeg2_bits_skip(bs, 2);
		*has_quant = true;
		return 0;	// intra + quant
	}
	LOG("mb_type_i: unrecognized at bit %u\n", mpeg2_bits_offset(bs));
	return -1;
}


void
mpeg2_reset_slice_state(uint8 intra_dc_precision)
{
	reset_dc_prediction(intra_dc_precision);
}


status_t
mpeg2_decode_intra_macroblock(mpeg2_bits* bs,
	mpeg2_decoder* dec, mpeg2_macroblock* mb)
{
	mb->valid = false;

	// 1. Macroblock address increment (Table B-1).
	mpeg2_bits saved_bs = *bs;
	int addr_inc = decode_macroblock_address_increment(bs);
	if (addr_inc == -2) {
		// End of slice — start code ahead. Not an error.
		return B_ENTRY_NOT_FOUND;	// signals "no more MBs in this slice"
	}
	if (addr_inc < 0) {
		// addr_inc unrecognized — try resync from saved position
		*bs = saved_bs;
		bool found = false;
		for (int d = 0; d <= 8 && mpeg2_bits_available(bs, 3); d++) {
			mpeg2_bits try_bs = saved_bs;
			if (d > 0) mpeg2_bits_skip(&try_bs, d);
			if (mpeg2_bits_peek(&try_bs, 1) == 1) {
				mpeg2_bits_skip(&try_bs, 1);
				bool tq = false;
				if (decode_macroblock_type_i(&try_bs, &tq) >= 0) {
					*bs = saved_bs;
					mpeg2_bits_skip(bs, d + 1);
					addr_inc = 1;
					found = true;
					break;
				}
			}
		}
		if (!found)
			return B_BAD_DATA;
	}
	mb->address_increment = (uint8)addr_inc;

	// 2. Macroblock type for I-picture (Table B-2).
	bool has_quant = false;
	if (decode_macroblock_type_i(bs, &has_quant) < 0) {
		// Resync: scan backward and forward from saved position
		// looking for addr_inc=1 + valid mb_type ('1'+'1' or '1'+'01').
		bool resynced = false;
		for (int delta = -6; delta <= 10; delta++) {
			int target = (int)mpeg2_bits_offset(&saved_bs) + delta;
			if (target < 0) continue;
			mpeg2_bits try_bs = saved_bs;
			if (delta > 0)
				mpeg2_bits_skip(&try_bs, delta);
			else if (delta < 0) {
				try_bs.byte_pos = target / 8;
				try_bs.bit_pos = target % 8;
			}
			if (!mpeg2_bits_available(&try_bs, 3)) continue;
			if (mpeg2_bits_peek(&try_bs, 1) == 1) {
				mpeg2_bits try2 = try_bs;
				mpeg2_bits_skip(&try2, 1);
				bool tq = false;
				if (decode_macroblock_type_i(&try2, &tq) >= 0) {
					*bs = try_bs;
					mpeg2_bits_skip(bs, 1);
					mb->address_increment = 1;
					has_quant = tq;
					resynced = true;
					break;
				}
			}
		}
		if (!resynced)
			return B_BAD_DATA;
	}

	// 3. If quant flag set, read new quantiser_scale_code (5 bits).
	if (has_quant) {
		mb->quantiser_scale_code = mpeg2_bits_read(bs, 5);
	}

	// Update decoder's quantiser_scale from the current q_scale_code.
	// This is used by the caller for IQ. The code comes either from
	// the slice header (initial) or from the MB's quant override.
	dec->quantiser_scale = mpeg2_compute_quantiser_scale(
		mb->quantiser_scale_code, dec->pic_ext.q_scale_type);

	// 4. Decode 6 blocks: Y0, Y1, Y2, Y3, Cb, Cr
	for (int b = 0; b < 6; b++) {
		// Check for start code (23+ zero bits = end of slice data).
		// This happens when the encoder ends a slice mid-macroblock
		// (before all 6 blocks), e.g., at a slice boundary. Remaining
		// blocks are implicitly zero (DC prediction carries forward).
		if (mpeg2_bits_available(bs, 23)
			&& mpeg2_bits_peek(bs, 23) == 0) {
			// Start code ahead — fill remaining blocks with zeros.
			for (int r = b; r < 6; r++)
				memset(mb->blocks[r], 0, 64 * sizeof(int16));
			break;
		}

		int cc = (b < 4) ? 0 : (b - 3);	// 0,0,0,0,1,2
		status_t st = decode_intra_block(bs, dec, cc, mb->blocks[b]);
		if (st != B_OK) {
			LOG("decode_mb: block %d failed at bit %u\n",
				b, mpeg2_bits_offset(bs));
			return st;
		}
	}

	mb->valid = true;
	return B_OK;
}


// ---------------------------------------------------------------------------
// P-picture macroblock type (Table B-3)
// ---------------------------------------------------------------------------

// P-picture macroblock types:
//   '1'     → MC, Not Coded (motion only, no DCT)
//   '01'    → MC + Pattern (motion + DCT residual)
//   '001'   → Intra
//   '00011' → MC + Pattern + Quant
//   '00010' → Intra + Quant
//   '00001' → MC (forward only, no pattern... skip-like)
static int
decode_macroblock_type_p(mpeg2_bits* bs, uint8* mb_type)
{
	if (mpeg2_bits_peek(bs, 1) == 1) {
		mpeg2_bits_skip(bs, 1);
		*mb_type = MB_MOTION_FORWARD;	// MC, not coded
		return 0;
	}
	if (mpeg2_bits_peek(bs, 2) == 0b01) {
		mpeg2_bits_skip(bs, 2);
		*mb_type = MB_MOTION_FORWARD | MB_PATTERN;
		return 0;
	}
	if (mpeg2_bits_peek(bs, 3) == 0b001) {
		mpeg2_bits_skip(bs, 3);
		*mb_type = MB_INTRA;
		return 0;
	}
	if (mpeg2_bits_peek(bs, 5) == 0b00011) {
		mpeg2_bits_skip(bs, 5);
		*mb_type = MB_MOTION_FORWARD | MB_PATTERN | MB_QUANT;
		return 0;
	}
	if (mpeg2_bits_peek(bs, 5) == 0b00010) {
		mpeg2_bits_skip(bs, 5);
		*mb_type = MB_INTRA | MB_QUANT;
		return 0;
	}
	if (mpeg2_bits_peek(bs, 5) == 0b00001) {
		mpeg2_bits_skip(bs, 5);
		*mb_type = MB_MOTION_FORWARD;
		return 0;
	}
	return -1;
}


// ---------------------------------------------------------------------------
// Motion vector decode (ISO 13818-2 Section 6.3.17.3)
// ---------------------------------------------------------------------------

// Motion code VLC (Table B-10)
static int
decode_motion_code(mpeg2_bits* bs)
{
	// ISO/IEC 13818-2 Table B-10: motion_code VLC.
	// VLC pattern (MSB first, excluding sign bit) + 1 sign bit.
	// Sign: 0=positive, 1=negative. motion_code=0 has no sign bit.
	if (mpeg2_bits_peek(bs, 1) == 1) {
		mpeg2_bits_skip(bs, 1);
		return 0;
	}

	// Peek enough bits for the longest code (11 VLC + 1 sign = 12).
	// We match on the VLC part, then read the sign separately.
	uint32 peek = mpeg2_bits_peek(bs, 12);

	// |mc|=1: VLC '01' (2 bits)
	if ((peek >> 10) == 0b01) {
		mpeg2_bits_skip(bs, 2);
		return mpeg2_bits_read(bs, 1) ? -1 : 1;
	}
	// |mc|=2: VLC '001' (3 bits)
	if ((peek >> 9) == 0b001) {
		mpeg2_bits_skip(bs, 3);
		return mpeg2_bits_read(bs, 1) ? -2 : 2;
	}
	// |mc|=3: VLC '0001' (4 bits)
	if ((peek >> 8) == 0b0001) {
		mpeg2_bits_skip(bs, 4);
		return mpeg2_bits_read(bs, 1) ? -3 : 3;
	}
	// |mc|=4: VLC '00011' (5 bits)
	if ((peek >> 7) == 0b00011) {
		mpeg2_bits_skip(bs, 5);
		return mpeg2_bits_read(bs, 1) ? -4 : 4;
	}
	// |mc|=5: VLC '00010' (5 bits)
	if ((peek >> 7) == 0b00010) {
		mpeg2_bits_skip(bs, 5);
		return mpeg2_bits_read(bs, 1) ? -5 : 5;
	}
	// |mc|=6: VLC '000011' (6 bits)
	if ((peek >> 6) == 0b000011) {
		mpeg2_bits_skip(bs, 6);
		return mpeg2_bits_read(bs, 1) ? -6 : 6;
	}
	// |mc|=7: VLC '000010' (6 bits)
	if ((peek >> 6) == 0b000010) {
		mpeg2_bits_skip(bs, 6);
		return mpeg2_bits_read(bs, 1) ? -7 : 7;
	}
	// |mc|=8: VLC '0000011' (7 bits)
	if ((peek >> 5) == 0b0000011) {
		mpeg2_bits_skip(bs, 7);
		return mpeg2_bits_read(bs, 1) ? -8 : 8;
	}
	// |mc|=9: VLC '0000010' (7 bits)
	if ((peek >> 5) == 0b0000010) {
		mpeg2_bits_skip(bs, 7);
		return mpeg2_bits_read(bs, 1) ? -9 : 9;
	}
	// |mc|=10: VLC '00000011' (8 bits)
	if ((peek >> 4) == 0b00000011) {
		mpeg2_bits_skip(bs, 8);
		return mpeg2_bits_read(bs, 1) ? -10 : 10;
	}
	// |mc|=11: VLC '00000010' (8 bits)
	if ((peek >> 4) == 0b00000010) {
		mpeg2_bits_skip(bs, 8);
		return mpeg2_bits_read(bs, 1) ? -11 : 11;
	}
	// |mc|=12: VLC '000000011' (9 bits)
	if ((peek >> 3) == 0b000000011) {
		mpeg2_bits_skip(bs, 9);
		return mpeg2_bits_read(bs, 1) ? -12 : 12;
	}
	// |mc|=13: VLC '000000010' (9 bits)
	if ((peek >> 3) == 0b000000010) {
		mpeg2_bits_skip(bs, 9);
		return mpeg2_bits_read(bs, 1) ? -13 : 13;
	}
	// |mc|=14: VLC '0000000011' (10 bits)
	if ((peek >> 2) == 0b0000000011) {
		mpeg2_bits_skip(bs, 10);
		return mpeg2_bits_read(bs, 1) ? -14 : 14;
	}
	// |mc|=15: VLC '0000000010' (10 bits)
	if ((peek >> 2) == 0b0000000010) {
		mpeg2_bits_skip(bs, 10);
		return mpeg2_bits_read(bs, 1) ? -15 : 15;
	}
	// |mc|=16: VLC '00000000010' (11 bits)
	if ((peek >> 1) == 0b00000000010) {
		mpeg2_bits_skip(bs, 11);
		return mpeg2_bits_read(bs, 1) ? -16 : 16;
	}

	LOG("motion_code: unrecognized at bit %u\n", mpeg2_bits_offset(bs));
	return -999;
}


// Decode full motion vector from motion_code + f_code
static void
decode_motion_vector(mpeg2_bits* bs, int16* mv_pred, uint8 f_code)
{
	int motion_code = decode_motion_code(bs);
	if (motion_code == -999) return;

	int r_size = f_code - 1;
	int motion_residual = 0;
	if (r_size > 0 && motion_code != 0)
		motion_residual = mpeg2_bits_read(bs, r_size);

	// Reconstruct full motion vector
	int range = 1 << (r_size + 4);	// = 16 * (1 << r_size)
	int delta;
	if (motion_code > 0)
		delta = ((motion_code - 1) << r_size) + motion_residual + 1;
	else if (motion_code < 0)
		delta = -(((- motion_code - 1) << r_size) + motion_residual + 1);
	else
		delta = 0;

	int pred = *mv_pred + delta;
	// Wrap to [-range, range-1]
	if (pred >= range) pred -= 2 * range;
	if (pred < -range) pred += 2 * range;
	*mv_pred = (int16)pred;
}


// ---------------------------------------------------------------------------
// Coded block pattern (Table B-9)
// ---------------------------------------------------------------------------

// Table B-9 — Coded block pattern VLC (ISO/IEC 13818-2)
// Index = CBP value (0-63), [0] = VLC code, [1] = code length in bits.
// From ffmpeg ff_mpeg12_mbPatTable[64][2].
static const uint8 cbp_table[64][2] = {
	// { code, length }
	{ 0x01, 9 }, // CBP  0
	{ 0x0b, 5 }, // CBP  1
	{ 0x09, 5 }, // CBP  2
	{ 0x0d, 6 }, // CBP  3
	{ 0x0d, 4 }, // CBP  4
	{ 0x17, 7 }, // CBP  5
	{ 0x13, 7 }, // CBP  6
	{ 0x1f, 8 }, // CBP  7
	{ 0x0c, 4 }, // CBP  8
	{ 0x16, 7 }, // CBP  9
	{ 0x12, 7 }, // CBP 10
	{ 0x1e, 8 }, // CBP 11
	{ 0x13, 5 }, // CBP 12
	{ 0x1b, 8 }, // CBP 13
	{ 0x17, 8 }, // CBP 14
	{ 0x13, 8 }, // CBP 15
	{ 0x0b, 4 }, // CBP 16
	{ 0x15, 7 }, // CBP 17
	{ 0x11, 7 }, // CBP 18
	{ 0x1d, 8 }, // CBP 19
	{ 0x11, 5 }, // CBP 20
	{ 0x19, 8 }, // CBP 21
	{ 0x15, 8 }, // CBP 22
	{ 0x11, 8 }, // CBP 23
	{ 0x0f, 6 }, // CBP 24
	{ 0x0f, 8 }, // CBP 25
	{ 0x0d, 8 }, // CBP 26
	{ 0x03, 9 }, // CBP 27
	{ 0x0f, 5 }, // CBP 28
	{ 0x0b, 8 }, // CBP 29
	{ 0x07, 8 }, // CBP 30
	{ 0x07, 9 }, // CBP 31
	{ 0x0a, 4 }, // CBP 32
	{ 0x14, 7 }, // CBP 33
	{ 0x10, 7 }, // CBP 34
	{ 0x1c, 8 }, // CBP 35
	{ 0x0e, 6 }, // CBP 36
	{ 0x0e, 8 }, // CBP 37
	{ 0x0c, 8 }, // CBP 38
	{ 0x02, 9 }, // CBP 39
	{ 0x10, 5 }, // CBP 40
	{ 0x18, 8 }, // CBP 41
	{ 0x14, 8 }, // CBP 42
	{ 0x10, 8 }, // CBP 43
	{ 0x0e, 5 }, // CBP 44
	{ 0x0a, 8 }, // CBP 45
	{ 0x06, 8 }, // CBP 46
	{ 0x06, 9 }, // CBP 47
	{ 0x12, 5 }, // CBP 48
	{ 0x1a, 8 }, // CBP 49
	{ 0x16, 8 }, // CBP 50
	{ 0x12, 8 }, // CBP 51
	{ 0x0d, 5 }, // CBP 52
	{ 0x09, 8 }, // CBP 53
	{ 0x05, 8 }, // CBP 54
	{ 0x05, 9 }, // CBP 55
	{ 0x0c, 5 }, // CBP 56
	{ 0x08, 8 }, // CBP 57
	{ 0x04, 8 }, // CBP 58
	{ 0x04, 9 }, // CBP 59
	{ 0x07, 3 }, // CBP 60
	{ 0x0a, 5 }, // CBP 61
	{ 0x08, 5 }, // CBP 62
	{ 0x0c, 6 }, // CBP 63
};

static int
decode_coded_block_pattern(mpeg2_bits* bs)
{
	uint32 peek = mpeg2_bits_peek(bs, 9);

	// Search all 64 CBP entries for a matching VLC code.
	// Codes range from 3 to 9 bits. Match longest prefix.
	for (int cbp = 0; cbp < 64; cbp++) {
		uint8 code = cbp_table[cbp][0];
		uint8 len = cbp_table[cbp][1];
		if ((peek >> (9 - len)) == code) {
			mpeg2_bits_skip(bs, len);
			return cbp;
		}
	}

	LOG("cbp: unrecognized at bit %u (peek9=0x%03x)\n",
		mpeg2_bits_offset(bs), peek);
	return -1;
}


// ---------------------------------------------------------------------------
// Non-intra block decode (for P-frame inter macroblocks)
// ---------------------------------------------------------------------------

static status_t
decode_non_intra_block(mpeg2_bits* bs, const mpeg2_decoder* dec,
	int16 block[64])
{
	memset(block, 0, 64 * sizeof(int16));

	const uint8* scan = dec->pic_ext.alternate_scan
		? mpeg2_alternate_scan : mpeg2_zigzag_scan;
	const uint8* qmat = dec->seq.non_intra_quantiser_matrix;
	uint16 qscale = dec->quantiser_scale;

	// Non-intra: no DC prediction, all coefficients use AC VLC.
	// First coefficient uses the '1s' special code from Table B-14
	// "first DCT coefficient" column.
	int idx = 0;
	while (idx < 64) {
		run_level rl = decode_ac_coeff_b14(bs, idx == 0);
		if (rl.run == -1)	// EOB
			break;
		if (rl.run == -3)	// error — forced EOB
			break;

		idx += rl.run;
		if (idx >= 64) {
			// Consume trailing EOB (always present per MPEG-2 spec)
			run_level eob = decode_ac_coeff_b14(bs, false);
			(void)eob;
			break;
		}

		// Apply inverse quantization (§7.4.2.2 for non-intra blocks):
		//   F''[v][u] = ((2 * level + Sign(level)) * W[w][v][u]
		//                * quantiser_scale) / 32
		// Simplified: sign applied AFTER the shift, matching ffmpeg.
		int j = scan[idx];
		int32 abs_level = rl.level < 0 ? -rl.level : rl.level;
		int32 iq = (((2 * abs_level + 1) * (int32)qscale
			* (int32)qmat[j]) >> 5);
		if (rl.level < 0)
			iq = -iq;
		block[j] = (int16)iq;
		idx++;
	}

	// If all 64 coefficients were coded, consume the trailing EOB.
	if (idx >= 64) {
		run_level eob = decode_ac_coeff_b14(bs, false);
		(void)eob;
	}

	// Mismatch control (§7.4.3)
	int32 sum = 0;
	for (int i = 0; i < 64; i++)
		sum += block[i];
	if ((sum & 1) == 0)
		block[63] ^= 1;

	return B_OK;
}


// ---------------------------------------------------------------------------
// P-picture macroblock decode
// ---------------------------------------------------------------------------

status_t
mpeg2_decode_p_macroblock(mpeg2_bits* bs,
	mpeg2_decoder* dec, mpeg2_macroblock* mb)
{
	mb->valid = false;
	memset(mb->blocks, 0, sizeof(mb->blocks));
	mb->motion_h = 0;
	mb->motion_v = 0;
	mb->coded_block_pattern = 0;

	// 1. Macroblock address increment
	mpeg2_bits saved_bs = *bs;
	int addr_inc = decode_macroblock_address_increment(bs);
	if (addr_inc < 0) {
		// Try resync
		*bs = saved_bs;
		bool found = false;
		for (int d = 0; d <= 8 && mpeg2_bits_available(bs, 3); d++) {
			mpeg2_bits try_bs = saved_bs;
			if (d > 0) mpeg2_bits_skip(&try_bs, d);
			if (mpeg2_bits_peek(&try_bs, 1) == 1) {
				*bs = saved_bs;
				mpeg2_bits_skip(bs, d);
				addr_inc = 1;
				found = true;
				break;
			}
		}
		if (!found) return B_BAD_DATA;
	}
	mb->address_increment = (uint8)addr_inc;

	// 2. Macroblock type (Table B-3)
	if (decode_macroblock_type_p(bs, &mb->mb_type) < 0) {
		// Resync
		bool found = false;
		for (int d = -4; d <= 6; d++) {
			int target = (int)mpeg2_bits_offset(&saved_bs) + d;
			if (target < 0) continue;
			mpeg2_bits try_bs;
			try_bs.data = bs->data; try_bs.size = bs->size;
			try_bs.byte_pos = target / 8; try_bs.bit_pos = target % 8;
			if (mpeg2_bits_peek(&try_bs, 1) == 1) {
				mpeg2_bits_skip(&try_bs, 1);
				uint8 ttype;
				if (decode_macroblock_type_p(&try_bs, &ttype) >= 0) {
					bs->byte_pos = target / 8; bs->bit_pos = target % 8;
					mpeg2_bits_skip(bs, 1);
					mb->address_increment = 1;
					mb->mb_type = ttype;
					found = true;
					break;
				}
			}
		}
		if (!found) return B_BAD_DATA;
	}

	// 3. Quantiser scale change
	if (mb->mb_type & MB_QUANT)
		mb->quantiser_scale_code = mpeg2_bits_read(bs, 5);

	// 4. Motion vectors (forward only for P-frame)
	if (mb->mb_type & MB_MOTION_FORWARD) {
		uint8 fcode_h = dec->pic_ext.f_code[0][0];
		uint8 fcode_v = dec->pic_ext.f_code[0][1];
		if (fcode_h == 0) fcode_h = 1;
		if (fcode_v == 0) fcode_v = 1;
		decode_motion_vector(bs, &dec->mv_pred_h, fcode_h);
		decode_motion_vector(bs, &dec->mv_pred_v, fcode_v);
		mb->motion_h = dec->mv_pred_h;
		mb->motion_v = dec->mv_pred_v;
	}

	// 5. Coded block pattern
	if (mb->mb_type & MB_PATTERN) {
		int cbp = decode_coded_block_pattern(bs);
		if (cbp < 0) {
			LOG("cbp decode failed in P-MB, assuming CBP=0\n");
			cbp = 0;
		}
		mb->coded_block_pattern = (uint8)cbp;
	} else if (mb->mb_type & MB_INTRA) {
		mb->coded_block_pattern = 0x3F;	// all blocks coded for intra
	}

	// 6. Decode blocks
	if (mb->mb_type & MB_INTRA) {
		// Intra: same as I-frame
		for (int b = 0; b < 6; b++) {
			int cc = (b < 4) ? 0 : (b - 3);
			status_t st = decode_intra_block(bs, dec, cc, mb->blocks[b]);
			if (st != B_OK) return st;
		}
		// §6.3.17.4: Intra MB resets motion vector prediction
		dec->mv_pred_h = 0;
		dec->mv_pred_v = 0;
	} else {
		// Inter: decode non-intra blocks for coded patterns
		// Reset DC predictors (non-intra MB breaks DC prediction)
		reset_dc_prediction(dec->pic_ext.intra_dc_precision);

		for (int b = 0; b < 6; b++) {
			if (mb->coded_block_pattern & (1 << (5 - b))) {
				status_t st = decode_non_intra_block(bs, dec,
					mb->blocks[b]);
				if (st != B_OK) return st;
			}
		}
	}

	mb->valid = true;
	return B_OK;
}
