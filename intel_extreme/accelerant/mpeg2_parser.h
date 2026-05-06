/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Minimal MPEG-2 bitstream parser for Gen5 GPU video decode.
 * Parses I-frame elementary streams and extracts per-macroblock
 * DCT coefficients for dispatch to the IQ+IDCT kernel.
 *
 * Reference: ISO/IEC 13818-2 (MPEG-2 Video)
 */

#ifndef MPEG2_PARSER_H
#define MPEG2_PARSER_H

#include <SupportDefs.h>


// MPEG-2 start codes
#define MPEG2_START_CODE_PREFIX		0x000001
#define MPEG2_PICTURE_START_CODE	0x00000100
#define MPEG2_SLICE_START_MIN		0x00000101
#define MPEG2_SLICE_START_MAX		0x000001AF
#define MPEG2_SEQUENCE_HEADER_CODE	0x000001B3
#define MPEG2_EXTENSION_START_CODE	0x000001B5
#define MPEG2_SEQUENCE_END_CODE		0x000001B7
#define MPEG2_GROUP_START_CODE		0x000001B8

// Picture coding types
#define MPEG2_I_PICTURE				1
#define MPEG2_P_PICTURE				2
#define MPEG2_B_PICTURE				3

// Chroma format (from sequence extension)
#define MPEG2_CHROMA_420			1
#define MPEG2_CHROMA_422			2
#define MPEG2_CHROMA_444			3


// ---------------------------------------------------------------------------
// Bitstream reader — reads bits from a byte buffer, MSB first.
// ---------------------------------------------------------------------------

struct mpeg2_bits {
	const uint8*	data;		// pointer to byte buffer
	uint32			size;		// total bytes
	uint32			byte_pos;	// current byte position
	uint8			bit_pos;	// bits consumed in current byte (0-7)
};

// Initialize bitstream reader.
void mpeg2_bits_init(mpeg2_bits* bs, const uint8* data, uint32 size);

// Read N bits (1..32) from the bitstream. Returns the value.
uint32 mpeg2_bits_read(mpeg2_bits* bs, uint32 n);

// Peek at the next N bits without advancing.
uint32 mpeg2_bits_peek(mpeg2_bits* bs, uint32 n);

// Skip N bits.
void mpeg2_bits_skip(mpeg2_bits* bs, uint32 n);

// Check if bitstream has at least N bits remaining.
bool mpeg2_bits_available(const mpeg2_bits* bs, uint32 n);

// Align to next byte boundary (skip remaining bits in current byte).
void mpeg2_bits_align(mpeg2_bits* bs);

// Current bit offset from start.
uint32 mpeg2_bits_offset(const mpeg2_bits* bs);


// ---------------------------------------------------------------------------
// Parsed MPEG-2 structures
// ---------------------------------------------------------------------------

struct mpeg2_sequence_header {
	uint32	width;
	uint32	height;
	uint8	aspect_ratio_info;		// 4 bits
	uint8	frame_rate_code;		// 4 bits
	uint32	bit_rate;				// 18 bits × 400 bps
	uint32	vbv_buffer_size;		// 10 bits
	bool	load_intra_quantiser_matrix;
	bool	load_non_intra_quantiser_matrix;
	uint8	intra_quantiser_matrix[64];		// zigzag order
	uint8	non_intra_quantiser_matrix[64];	// zigzag order
};

struct mpeg2_picture_header {
	uint16	temporal_reference;		// 10 bits
	uint8	picture_coding_type;	// 3 bits (I=1, P=2, B=3)
	uint16	vbv_delay;				// 16 bits
	// For P/B pictures: forward/backward motion vector info (not used for I)
};

struct mpeg2_picture_coding_extension {
	uint8	f_code[2][2];			// forward/backward, horizontal/vertical
	uint8	intra_dc_precision;		// 2 bits (0..3)
	uint8	picture_structure;		// 2 bits (1=top, 2=bottom, 3=frame)
	bool	top_field_first;
	bool	frame_pred_frame_dct;
	bool	concealment_motion_vectors;
	bool	q_scale_type;			// 0=linear, 1=non-linear
	bool	intra_vlc_format;
	bool	alternate_scan;
	bool	progressive_frame;
};

struct mpeg2_slice_header {
	uint32	slice_vertical_position;	// from start code (1-based)
	uint8	quantiser_scale_code;		// 5 bits
	// Extra information bytes (skipped)
};

// Macroblock type flags (from Tables B-2, B-3, B-4)
#define MB_QUANT			0x01
#define MB_MOTION_FORWARD	0x02
#define MB_MOTION_BACKWARD	0x04
#define MB_PATTERN			0x08
#define MB_INTRA			0x10

// One decoded macroblock (intra or inter).
struct mpeg2_macroblock {
	int16	blocks[6][64];		// 6 blocks × 64 DCT coefficients
	uint8	quantiser_scale_code;
	uint8	address_increment;	// 1 = normal, >1 = skipped MBs before this one
	uint8	mb_type;			// MB_* flags
	int16	motion_h;			// forward horizontal motion vector (half-pel)
	int16	motion_v;			// forward vertical motion vector (half-pel)
	uint8	coded_block_pattern;	// 6 bits for 4:2:0 (Y0,Y1,Y2,Y3,Cb,Cr)
	bool	valid;
};

// Decoder context — holds sequence/picture state for decoding.
struct mpeg2_decoder {
	mpeg2_sequence_header			seq;
	mpeg2_picture_header			pic;
	mpeg2_picture_coding_extension	pic_ext;

	// Derived values
	uint32	mb_width;			// macroblocks per row
	uint32	mb_height;			// macroblock rows
	uint32	mb_count;			// total macroblocks

	// Quantiser scale (computed from q_scale_code + q_scale_type)
	uint16	quantiser_scale;

	// Intra DC multiplier (computed from intra_dc_precision)
	uint16	intra_dc_mult;

	// Motion vector prediction state (P/B frames)
	int16	mv_pred_h;
	int16	mv_pred_v;

	bool	initialized;
};


// ---------------------------------------------------------------------------
// Parser functions
// ---------------------------------------------------------------------------

// Find the next start code in the bitstream. Returns the 32-bit start
// code value (0x000001xx) and advances past it, or 0 if not found.
uint32 mpeg2_find_start_code(mpeg2_bits* bs);

// Seek forward to the next slice start code for error recovery.
// Positions bitstream at the start code prefix (does not consume it).
// Returns true if a slice start code was found, false at end of stream.
bool mpeg2_resync_to_next_slice(mpeg2_bits* bs);

// Parse a sequence header (after start code has been consumed).
status_t mpeg2_parse_sequence_header(mpeg2_bits* bs,
	mpeg2_sequence_header* seq);

// Parse a picture header (after start code has been consumed).
status_t mpeg2_parse_picture_header(mpeg2_bits* bs,
	mpeg2_picture_header* pic);

// Parse picture coding extension (after extension start code + ID).
status_t mpeg2_parse_picture_coding_extension(mpeg2_bits* bs,
	mpeg2_picture_coding_extension* ext);

// Parse a slice header (after start code has been consumed).
// slice_code is the raw start code (0x00000101..0x000001AF).
status_t mpeg2_parse_slice_header(mpeg2_bits* bs, uint32 slice_code,
	mpeg2_slice_header* slice);

// Decode one intra macroblock from the current bitstream position.
status_t mpeg2_decode_intra_macroblock(mpeg2_bits* bs,
	mpeg2_decoder* dec, mpeg2_macroblock* mb);

// Decode one macroblock from a P-picture (inter or intra).
status_t mpeg2_decode_p_macroblock(mpeg2_bits* bs,
	mpeg2_decoder* dec, mpeg2_macroblock* mb);

// Initialize decoder context from parsed headers.
void mpeg2_decoder_init(mpeg2_decoder* dec);

// Compute quantiser_scale from q_scale_code and q_scale_type.
uint16 mpeg2_compute_quantiser_scale(uint8 q_scale_code,
	bool q_scale_type);

// Compute intra_dc_mult from intra_dc_precision.
uint16 mpeg2_compute_intra_dc_mult(uint8 intra_dc_precision);

// Reset DC prediction state at the start of each slice.
void mpeg2_reset_slice_state(uint8 intra_dc_precision);


// ---------------------------------------------------------------------------
// MPEG-2 zigzag scan order (normal scan)
// ---------------------------------------------------------------------------

extern const uint8 mpeg2_zigzag_scan[64];

// Default intra quantiser matrix (ISO/IEC 13818-2 Table 6-4)
extern const uint8 mpeg2_default_intra_matrix[64];


#endif // MPEG2_PARSER_H
