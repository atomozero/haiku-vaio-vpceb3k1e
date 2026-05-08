/*
 * MPEG-2 I-frame viewer for Haiku.
 * Decodes an MPEG-2 file and displays it in a BWindow using BBitmap.
 * No reboot needed, no GPU — pure CPU decode + Haiku GUI.
 *
 * Build:
 *   g++ -Wall -O2 -I.. -std=c++17 \
 *       -o mpeg2_viewer mpeg2_viewer.cpp ../mpeg2_parser.cpp \
 *       -lbe -ltracker
 *   ./mpeg2_viewer [input.m2v]
 */

#include "mpeg2_parser.h"
#include "idct_ref.h"

#include <Application.h>
#include <Bitmap.h>
#include <Window.h>
#include <View.h>
#include <String.h>

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
	int16 iq[64];
	for (int i = 0; i < 64; i++) {
		int32 tmp = (int32)coeffs[i] * (int32)(uint32)qm[i];
		tmp *= (int32)(uint32)qs;
		tmp >>= 4;
		iq[i] = (int16)tmp;
	}
	iq[0] = (int16)((int32)coeffs[0] * (int32)(uint32)dc_mult);

	int16 idct[64];
	compute_idct_reference(iq, idct);
	for (int i = 0; i < 64; i++) {
		int32 v = idct[i];
		if (v < 0) v = 0;
		if (v > 255) v = 255;
		pixels[i] = (uint8)v;
	}
}


static inline uint8
clamp8(int32 v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8)v;
}


// Decode MPEG-2 I-frame to RGB32 bitmap
static BBitmap*
decode_mpeg2_to_bitmap(const char* path, uint32* out_decoded)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(sz);
	fread(data, 1, sz, f);
	fclose(f);

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
			uint32 id = mpeg2_bits_peek(&bs, 4);
			if (id == 8) {
				mpeg2_bits_skip(&bs, 4);
				mpeg2_parse_picture_coding_extension(&bs, &dec.pic_ext);
				mpeg2_decoder_init(&dec);
			} else {
				mpeg2_bits_skip(&bs, 4);
			}
		} else if (code >= MPEG2_SLICE_START_MIN
			&& code <= MPEG2_SLICE_START_MAX) {
			break;
		}
	}

	if (!dec.initialized) {
		free(data);
		return NULL;
	}

	uint32 w = dec.seq.width, h = dec.seq.height;
	uint32 cw = w / 2, ch = h / 2;

	uint8* y_plane = (uint8*)calloc(w * h, 1);
	uint8* cb_plane = (uint8*)malloc(cw * ch);
	memset(cb_plane, 128, cw * ch);
	uint8* cr_plane = (uint8*)malloc(cw * ch);
	memset(cr_plane, 128, cw * ch);

	// Parse slices and decode
	mpeg2_bits_init(&bs, data, sz);
	uint32 decoded = 0;
	static const int ydx[4] = {0, 8, 0, 8};
	static const int ydy[4] = {0, 0, 8, 8};

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;
		if (code < MPEG2_SLICE_START_MIN || code > MPEG2_SLICE_START_MAX)
			continue;

		mpeg2_slice_header sl;
		mpeg2_parse_slice_header(&bs, code, &sl);
		mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
		dec.quantiser_scale = mpeg2_compute_quantiser_scale(
			sl.quantiser_scale_code, dec.pic_ext.q_scale_type);

		uint32 mby = sl.slice_vertical_position - 1;
		uint32 mbx = 0;

		while (mbx < dec.mb_width && mpeg2_bits_available(&bs, 8)) {
			if (mpeg2_bits_peek(&bs, 23) == 0) break;

			mpeg2_macroblock mb;
			mb.quantiser_scale_code = sl.quantiser_scale_code;
			if (mpeg2_decode_intra_macroblock(&bs, &dec, &mb) != B_OK) {
				// Simple resync: skip to next byte-aligned position
				mpeg2_bits_align(&bs);
				mbx++;
				continue;
			}

			if (mb.address_increment > 1)
				mbx += mb.address_increment - 1;

			uint16 qs = dec.quantiser_scale;

			// Y blocks
			for (int b = 0; b < 4; b++) {
				uint8 pix[64];
				decode_block(mb.blocks[b], dec.seq.intra_quantiser_matrix,
					qs, dec.intra_dc_mult, pix);
				uint32 bx = mbx * 16 + ydx[b];
				uint32 by = mby * 16 + ydy[b];
				for (int r = 0; r < 8; r++)
					for (int c = 0; c < 8; c++)
						if (bx + c < w && by + r < h)
							y_plane[(by + r) * w + bx + c] = pix[r * 8 + c];
			}

			// Cb, Cr blocks
			for (int comp = 0; comp < 2; comp++) {
				uint8 pix[64];
				decode_block(mb.blocks[4 + comp],
					dec.seq.intra_quantiser_matrix,
					qs, dec.intra_dc_mult, pix);
				uint8* plane = (comp == 0) ? cb_plane : cr_plane;
				uint32 bx = mbx * 8, by = mby * 8;
				for (int r = 0; r < 8; r++)
					for (int c = 0; c < 8; c++)
						if (bx + c < cw && by + r < ch)
							plane[(by + r) * cw + bx + c] = pix[r * 8 + c];
			}

			decoded++;
			mbx++;
		}
	}

	if (out_decoded)
		*out_decoded = decoded;

	// Create BBitmap and convert YCbCr→RGB32
	BBitmap* bitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGB32);
	uint8* bits = (uint8*)bitmap->Bits();
	uint32 bpr = bitmap->BytesPerRow();

	for (uint32 y = 0; y < h; y++) {
		for (uint32 x = 0; x < w; x++) {
			int32 yv = (int32)y_plane[y * w + x] - 16;
			int32 cbv = (int32)cb_plane[(y / 2) * cw + (x / 2)] - 128;
			int32 crv = (int32)cr_plane[(y / 2) * cw + (x / 2)] - 128;

			// BT.601 YCbCr → RGB (studio range)
			int32 r = (1192 * yv + 1634 * crv) >> 10;
			int32 g = (1192 * yv - 401 * cbv - 832 * crv) >> 10;
			int32 b = (1192 * yv + 2066 * cbv) >> 10;

			// B_RGB32 is BGRA (little-endian)
			uint32 offset = y * bpr + x * 4;
			bits[offset + 0] = clamp8(b);	// B
			bits[offset + 1] = clamp8(g);	// G
			bits[offset + 2] = clamp8(r);	// R
			bits[offset + 3] = 255;			// A
		}
	}

	free(y_plane);
	free(cb_plane);
	free(cr_plane);
	free(data);

	return bitmap;
}


// --- Haiku GUI ---

class FrameView : public BView {
public:
	FrameView(BRect frame, BBitmap* bitmap)
		: BView(frame, "frame", B_FOLLOW_ALL, B_WILL_DRAW)
		, fBitmap(bitmap)
	{
	}

	void Draw(BRect updateRect) override
	{
		if (fBitmap)
			DrawBitmap(fBitmap, Bounds());
	}

private:
	BBitmap* fBitmap;
};


class FrameWindow : public BWindow {
public:
	FrameWindow(BBitmap* bitmap, const char* title)
		: BWindow(
			BRect(100, 100,
				100 + bitmap->Bounds().Width(),
				100 + bitmap->Bounds().Height()),
			title, B_TITLED_WINDOW,
			B_NOT_RESIZABLE | B_QUIT_ON_WINDOW_CLOSE)
	{
		FrameView* view = new FrameView(Bounds(), bitmap);
		AddChild(view);
	}
};


// Decode to raw RGB32 buffer (no BBitmap, no app_server needed)
static uint8*
decode_mpeg2_to_rgb(const char* path, uint32* out_w, uint32* out_h,
	uint32* out_decoded)
{
	uint32 decoded = 0;
	// Temporarily create bitmap just to get the YCbCr data
	// Actually, decode to raw planes first
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(sz); fread(data, 1, sz, f); fclose(f);

	mpeg2_bits bs; mpeg2_bits_init(&bs, data, sz);
	mpeg2_decoder dec; memset(&dec, 0, sizeof(dec));

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;
		if (code == MPEG2_SEQUENCE_HEADER_CODE)
			mpeg2_parse_sequence_header(&bs, &dec.seq);
		else if (code == MPEG2_PICTURE_START_CODE)
			mpeg2_parse_picture_header(&bs, &dec.pic);
		else if (code == MPEG2_EXTENSION_START_CODE) {
			uint32 id = mpeg2_bits_peek(&bs, 4);
			if (id == 8) { mpeg2_bits_skip(&bs, 4);
				mpeg2_parse_picture_coding_extension(&bs, &dec.pic_ext);
				mpeg2_decoder_init(&dec);
			} else mpeg2_bits_skip(&bs, 4);
		} else if (code >= MPEG2_SLICE_START_MIN
			&& code <= MPEG2_SLICE_START_MAX) break;
	}
	if (!dec.initialized) { free(data); return NULL; }

	uint32 w = dec.seq.width, h = dec.seq.height;
	uint32 cw = w/2, ch = h/2;
	*out_w = w; *out_h = h;

	uint8* y_plane = (uint8*)calloc(w*h, 1);
	uint8* cb_plane = (uint8*)malloc(cw*ch); memset(cb_plane, 128, cw*ch);
	uint8* cr_plane = (uint8*)malloc(cw*ch); memset(cr_plane, 128, cw*ch);

	mpeg2_bits_init(&bs, data, sz);
	static const int ydx[4]={0,8,0,8}, ydy[4]={0,0,8,8};

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;
		if (code < MPEG2_SLICE_START_MIN || code > MPEG2_SLICE_START_MAX)
			continue;
		mpeg2_slice_header sl;
		mpeg2_parse_slice_header(&bs, code, &sl);
		mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
		dec.quantiser_scale = mpeg2_compute_quantiser_scale(
			sl.quantiser_scale_code, dec.pic_ext.q_scale_type);
		uint32 mby = sl.slice_vertical_position - 1, mbx = 0;

		while (mbx < dec.mb_width && mpeg2_bits_available(&bs, 8)) {
			if (mpeg2_bits_peek(&bs, 23) == 0) break;
			mpeg2_macroblock mb;
			mb.quantiser_scale_code = sl.quantiser_scale_code;
			if (mpeg2_decode_intra_macroblock(&bs, &dec, &mb) != B_OK) {
				mpeg2_bits_align(&bs); mbx++; continue;
			}
			if (mb.address_increment > 1)
				mbx += mb.address_increment - 1;
			uint16 qs = dec.quantiser_scale;
			for (int b = 0; b < 4; b++) {
				uint8 pix[64];
				decode_block(mb.blocks[b], dec.seq.intra_quantiser_matrix,
					qs, dec.intra_dc_mult, pix);
				uint32 bx = mbx*16+ydx[b], by = mby*16+ydy[b];
				for (int r=0;r<8;r++) for (int c=0;c<8;c++)
					if (bx+c<w && by+r<h) y_plane[(by+r)*w+bx+c]=pix[r*8+c];
			}
			for (int comp=0;comp<2;comp++) {
				uint8 pix[64];
				decode_block(mb.blocks[4+comp], dec.seq.intra_quantiser_matrix,
					qs, dec.intra_dc_mult, pix);
				uint8* plane = comp==0 ? cb_plane : cr_plane;
				uint32 bx=mbx*8, by=mby*8;
				for(int r=0;r<8;r++) for(int c=0;c<8;c++)
					if(bx+c<cw && by+r<ch) plane[(by+r)*cw+bx+c]=pix[r*8+c];
			}
			decoded++; mbx++;
		}
	}

	// Convert YCbCr → RGB32 (BGRA)
	uint8* rgb = (uint8*)malloc(w * h * 4);
	for (uint32 y = 0; y < h; y++) {
		for (uint32 x = 0; x < w; x++) {
			int32 yv = (int32)y_plane[y*w+x] - 16;
			int32 cbv = (int32)cb_plane[(y/2)*cw+(x/2)] - 128;
			int32 crv = (int32)cr_plane[(y/2)*cw+(x/2)] - 128;
			int32 r = (1192*yv + 1634*crv) >> 10;
			int32 g = (1192*yv - 401*cbv - 832*crv) >> 10;
			int32 b = (1192*yv + 2066*cbv) >> 10;
			uint32 off = (y*w+x)*4;
			rgb[off+0] = clamp8(b);
			rgb[off+1] = clamp8(g);
			rgb[off+2] = clamp8(r);
			rgb[off+3] = 255;
		}
	}

	free(y_plane); free(cb_plane); free(cr_plane); free(data);
	*out_decoded = decoded;
	return rgb;
}


class ViewerApp : public BApplication {
public:
	ViewerApp(const char* path)
		: BApplication("application/x-mpeg2-viewer")
		, fPath(path)
		, fRgb(NULL)
		, fBitmap(NULL)
	{
	}

	void ReadyToRun() override
	{
		uint32 w, h, decoded;
		fRgb = decode_mpeg2_to_rgb(fPath, &w, &h, &decoded);
		if (!fRgb) {
			printf("Failed to decode %s\n", fPath);
			PostMessage(B_QUIT_REQUESTED);
			return;
		}

		printf("Decoded %u MBs, frame %ux%u\n", decoded, w, h);

		fBitmap = new BBitmap(BRect(0, 0, w-1, h-1), B_RGB32);
		memcpy(fBitmap->Bits(), fRgb, w * h * 4);
		free(fRgb);
		fRgb = NULL;

		BString title("MPEG-2: ");
		title << fPath << " (" << decoded << " MBs)";

		FrameWindow* win = new FrameWindow(fBitmap, title.String());
		win->Show();
	}

private:
	const char* fPath;
	uint8* fRgb;
	BBitmap* fBitmap;
};


int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "test_dark_320.m2v";
	printf("MPEG-2 Viewer: %s\n", path);

	ViewerApp app(path);
	app.Run();

	return 0;
}
