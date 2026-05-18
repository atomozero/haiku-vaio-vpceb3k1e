/*
 * Standalone MPEG-2 video viewer with GPU-accelerated IDCT.
 * Decodes I and P frames with motion compensation. IDCT runs on the
 * Intel Gen5 EU array via MEDIA_OBJECT dispatch when available,
 * falling back to CPU reference when GPU is not accessible.
 *
 * Usage: mpeg2_viewer input.m2v
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Bitmap.h>
#include <MessageRunner.h>
#include <View.h>
#include <Window.h>

#include "mpeg2_parser.h"
#include "idct_ref.h"
#include "gpu_idct.h"


// ---------------------------------------------------------------------------
// GPU IDCT batching: collect blocks during decode, flush periodically
// ---------------------------------------------------------------------------

#define IDCT_BATCH_SIZE 64

static int16 sIdctInputBatch[IDCT_BATCH_SIZE][64];
static int16 sIdctOutputBatch[IDCT_BATCH_SIZE][64];
static uint32 sIdctBatchCount = 0;
static uint32 sIdctGpuBlocks = 0;
static uint32 sIdctCpuBlocks = 0;

static void
flush_idct_batch(void)
{
	if (sIdctBatchCount == 0)
		return;

	if (gpu_idct_available()) {
		if (gpu_idct_process(sIdctInputBatch, sIdctOutputBatch,
				sIdctBatchCount) == B_OK) {
			sIdctGpuBlocks += sIdctBatchCount;
		} else {
			// GPU failed, fall back to CPU for this batch
			for (uint32 i = 0; i < sIdctBatchCount; i++)
				compute_idct_reference(sIdctInputBatch[i], sIdctOutputBatch[i]);
			sIdctCpuBlocks += sIdctBatchCount;
		}
	} else {
		for (uint32 i = 0; i < sIdctBatchCount; i++)
			compute_idct_reference(sIdctInputBatch[i], sIdctOutputBatch[i]);
		sIdctCpuBlocks += sIdctBatchCount;
	}
	sIdctBatchCount = 0;
}

// Submit one block for IDCT. Returns pointer to the output (valid
// after flush_idct_batch or immediately if batch is full).
static int16*
submit_idct_block(const int16 coeffs[64])
{
	uint32 idx = sIdctBatchCount;
	memcpy(sIdctInputBatch[idx], coeffs, 128);
	sIdctBatchCount++;

	if (sIdctBatchCount >= IDCT_BATCH_SIZE)
		flush_idct_batch();

	return sIdctOutputBatch[idx];
}


static inline uint8
clamp8(int32 v)
{
	return (v < 0) ? 0 : (v > 255) ? 255 : (uint8)v;
}


// ---------------------------------------------------------------------------
// Frame planes (Y + Cb + Cr, 4:2:0)
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
	f->cb = (uint8*)malloc(w / 2 * h / 2);
	f->cr = (uint8*)malloc(w / 2 * h / 2);
	memset(f->cb, 128, w / 2 * h / 2);
	memset(f->cr, 128, w / 2 * h / 2);
}

static void
frame_free(frame_planes* f)
{
	free(f->y);
	free(f->cb);
	free(f->cr);
	f->y = f->cb = f->cr = NULL;
}

static void
frame_copy(frame_planes* dst, const frame_planes* src)
{
	memcpy(dst->y, src->y, src->w * src->h);
	memcpy(dst->cb, src->cb, src->w / 2 * src->h / 2);
	memcpy(dst->cr, src->cr, src->w / 2 * src->h / 2);
}


// ---------------------------------------------------------------------------
// Block decode helpers
// ---------------------------------------------------------------------------

static void
decode_intra_pixels(const int16 coeffs[64], uint8 pixels[64])
{
	// Enqueue + flush immediately (result needed now)
	uint32 idx = sIdctBatchCount;
	memcpy(sIdctInputBatch[idx], coeffs, 128);
	sIdctBatchCount++;
	flush_idct_batch();
	for (int i = 0; i < 64; i++)
		pixels[i] = clamp8(sIdctOutputBatch[idx][i]);
}

static void
decode_inter_pixels(const int16 coeffs[64], const uint8 mc_ref[64],
	uint8 pixels[64])
{
	uint32 idx = sIdctBatchCount;
	memcpy(sIdctInputBatch[idx], coeffs, 128);
	sIdctBatchCount++;
	flush_idct_batch();
	for (int i = 0; i < 64; i++)
		pixels[i] = clamp8((int32)mc_ref[i] + (int32)sIdctOutputBatch[idx][i]);
}

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
// Motion Compensation: half-pel bilinear interpolation
// ---------------------------------------------------------------------------

static inline uint8
ref_pixel(const uint8* plane, int x, int y, int w, int h)
{
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= w) x = w - 1;
	if (y >= h) y = h - 1;
	return plane[y * w + x];
}

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


// ---------------------------------------------------------------------------
// Decode one picture (I or P)
// ---------------------------------------------------------------------------

static const int ydx[4] = {0, 8, 0, 8};
static const int ydy[4] = {0, 0, 8, 8};

static uint32
decode_picture(mpeg2_bits* bs, mpeg2_decoder* dec,
	frame_planes* dst, const frame_planes* ref)
{
	uint32 w = dst->w, h = dst->h;
	uint32 cw = w / 2, ch = h / 2;
	bool is_p = (dec->pic.picture_coding_type == 2);
	uint32 mb_decoded = 0;

	while (mpeg2_bits_available(bs, 32)) {
		uint32 code = mpeg2_find_start_code(bs);
		if (code == 0) break;
		if (code < MPEG2_SLICE_START_MIN || code > MPEG2_SLICE_START_MAX) {
			bs->byte_pos = bs->byte_pos - 4;
			bs->bit_pos = 0;
			break;
		}

		mpeg2_slice_header sl;
		mpeg2_parse_slice_header(bs, code, &sl);
		mpeg2_reset_slice_state(dec->pic_ext.intra_dc_precision);
		dec->mv_pred_h = 0;
		dec->mv_pred_v = 0;

		uint32 mby = sl.slice_vertical_position - 1;
		uint32 mbx = 0;

		while (mbx < dec->mb_width && mpeg2_bits_available(bs, 8)) {
			if (mpeg2_bits_peek(bs, 23) == 0) break;

			mpeg2_macroblock mb;
			mb.quantiser_scale_code = sl.quantiser_scale_code;
			status_t st;

			if (is_p)
				st = mpeg2_decode_p_macroblock(bs, dec, &mb);
			else
				st = mpeg2_decode_intra_macroblock(bs, dec, &mb);

			if (st != B_OK) { mbx++; continue; }

			// Skipped MBs
			if (mb.address_increment > 1) {
				uint32 skip_count = mb.address_increment - 1;
				if (is_p && ref) {
					for (uint32 s = 0; s < skip_count && mbx < dec->mb_width; s++) {
						for (int b = 0; b < 4; b++) {
							uint32 bx = mbx * 16 + ydx[b];
							uint32 by = mby * 16 + ydy[b];
							for (int r = 0; r < 8; r++)
								for (int c = 0; c < 8; c++)
									if (bx+c < w && by+r < h)
										dst->y[(by+r)*w + bx+c] =
											ref->y[(by+r)*w + bx+c];
						}
						uint32 cx = mbx * 8, cy = mby * 8;
						for (int r = 0; r < 8; r++)
							for (int c = 0; c < 8; c++)
								if (cx+c < cw && cy+r < ch) {
									dst->cb[(cy+r)*cw + cx+c] =
										ref->cb[(cy+r)*cw + cx+c];
									dst->cr[(cy+r)*cw + cx+c] =
										ref->cr[(cy+r)*cw + cx+c];
								}
						mbx++;
						mb_decoded++;
					}
				} else {
					mbx += skip_count;
				}
			}

			dec->quantiser_scale = mpeg2_compute_quantiser_scale(
				mb.quantiser_scale_code, dec->pic_ext.q_scale_type);

			if (mb.mb_type & MB_INTRA) {
				if (is_p) {
					dec->mv_pred_h = 0;
					dec->mv_pred_v = 0;
				}
				for (int b = 0; b < 4; b++) {
					uint8 pix[64];
					decode_intra_pixels(mb.blocks[b], pix);
					write_block(dst->y, w, w, h,
						mbx*16 + ydx[b], mby*16 + ydy[b], pix, 8, 8);
				}
				for (int cc = 0; cc < 2; cc++) {
					uint8 pix[64];
					decode_intra_pixels(mb.blocks[4 + cc], pix);
					write_block(cc == 0 ? dst->cb : dst->cr, cw, cw, ch,
						mbx*8, mby*8, pix, 8, 8);
				}
			} else if (is_p && ref) {
				int16 mv_h = mb.motion_h;
				int16 mv_v = mb.motion_v;

				for (int b = 0; b < 4; b++) {
					uint32 bx = mbx * 16 + ydx[b];
					uint32 by = mby * 16 + ydy[b];
					uint8 mc_ref_block[64];
					mc_block(ref->y, w, h, bx, by, mv_h, mv_v,
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

				int16 cmv_h = mv_h / 2;
				int16 cmv_v = mv_v / 2;
				for (int cc = 0; cc < 2; cc++) {
					uint8* plane = (cc == 0) ? dst->cb : dst->cr;
					const uint8* ref_plane = (cc == 0) ? ref->cb : ref->cr;
					uint32 cx = mbx * 8, cy = mby * 8;
					uint8 mc_ref_block[64];
					mc_block(ref_plane, cw, ch, cx, cy, cmv_h, cmv_v,
						mc_ref_block, 8, 8);

					if (mb.coded_block_pattern & (1 << (1 - cc))) {
						uint8 pix[64];
						decode_inter_pixels(mb.blocks[4 + cc],
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
// BView for video display
// ---------------------------------------------------------------------------

class VideoView : public BView {
public:
	VideoView(BRect frame, BBitmap* bitmap)
		: BView(frame, "video", B_FOLLOW_ALL, B_WILL_DRAW),
		  fBitmap(bitmap) {}

	void Draw(BRect updateRect)
	{
		if (fBitmap)
			DrawBitmap(fBitmap, Bounds());
	}

	void SetBitmap(BBitmap* bitmap) { fBitmap = bitmap; }

	BBitmap* fBitmap;
};


// ---------------------------------------------------------------------------
// YCbCr → RGB32 conversion (BT.601) into a BBitmap
// ---------------------------------------------------------------------------

static void
frame_to_bitmap(const frame_planes* f, BBitmap* bitmap)
{
	uint32 w = f->w, h = f->h, cw = w / 2;
	uint32 bpr = bitmap->BytesPerRow();
	uint8* bits = (uint8*)bitmap->Bits();

	for (uint32 y = 0; y < h; y++) {
		for (uint32 x = 0; x < w; x++) {
			int32 yv = (int32)f->y[y * w + x] - 16;
			int32 cbv = (int32)f->cb[(y / 2) * cw + (x / 2)] - 128;
			int32 crv = (int32)f->cr[(y / 2) * cw + (x / 2)] - 128;
			int32 r = (1192 * yv + 1634 * crv) >> 10;
			int32 g = (1192 * yv - 401 * cbv - 832 * crv) >> 10;
			int32 b = (1192 * yv + 2066 * cbv) >> 10;
			bits[y * bpr + x * 4 + 0] = clamp8(b);
			bits[y * bpr + x * 4 + 1] = clamp8(g);
			bits[y * bpr + x * 4 + 2] = clamp8(r);
			bits[y * bpr + x * 4 + 3] = 255;
		}
	}
}


// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1]
		: "test.m2v";

	// Initialize GPU IDCT (falls back to CPU if unavailable)
	if (gpu_idct_init() == B_OK)
		printf("GPU IDCT: ENABLED (Intel Gen5 hardware acceleration)\n");
	else
		printf("GPU IDCT: disabled (CPU fallback)\n");

	FILE* f = fopen(path, "rb");
	if (!f) { printf("Cannot open %s\n", path); return 1; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(sz);
	fread(data, 1, sz, f);
	fclose(f);

	// First pass: parse headers to get dimensions
	mpeg2_bits bs;
	mpeg2_bits_init(&bs, data, sz);
	mpeg2_decoder dec;
	memset(&dec, 0, sizeof(dec));

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (!code) break;
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
			&& code <= MPEG2_SLICE_START_MAX) {
			break;
		}
	}

	if (!dec.initialized) {
		printf("Failed to parse MPEG-2 headers\n");
		free(data);
		return 1;
	}

	uint32 w = dec.seq.width, h = dec.seq.height;
	printf("Video: %ux%u, %ux%u MBs\n", w, h, dec.mb_width, dec.mb_height);

	// Decode all frames
	mpeg2_bits_init(&bs, data, sz);
	memset(&dec, 0, sizeof(dec));

	frame_planes current, reference;
	memset(&current, 0, sizeof(current));
	memset(&reference, 0, sizeof(reference));
	bool has_ref = false;

	// Collect decoded frames for display
	struct decoded_frame {
		uint8*	rgb;	// RGB32 data (w*h*4 bytes)
		int		type;	// 1=I, 2=P, 3=B
	};

	decoded_frame* frames = NULL;
	int frame_count = 0;
	int frame_capacity = 0;

	// Create a temporary bitmap for conversion
	BApplication app("application/x-vnd.Haiku-MPEG2Viewer");
	BBitmap* tmpBitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGB32);
	uint32 bpr = tmpBitmap->BytesPerRow();

	while (mpeg2_bits_available(&bs, 32)) {
		uint32 code = mpeg2_find_start_code(&bs);
		if (code == 0) break;

		if (code == MPEG2_SEQUENCE_HEADER_CODE) {
			mpeg2_parse_sequence_header(&bs, &dec.seq);
		} else if (code == MPEG2_PICTURE_START_CODE) {
			mpeg2_parse_picture_header(&bs, &dec.pic);
		} else if (code == MPEG2_EXTENSION_START_CODE) {
			if (mpeg2_bits_peek(&bs, 4) == 8) {
				mpeg2_bits_skip(&bs, 4);
				mpeg2_parse_picture_coding_extension(&bs, &dec.pic_ext);
				mpeg2_decoder_init(&dec);
			}
		} else if (code >= MPEG2_SLICE_START_MIN
			&& code <= MPEG2_SLICE_START_MAX) {

			if (!dec.initialized) continue;

			int pic_type = dec.pic.picture_coding_type;

			// Skip B-frames for now
			if (pic_type == 3) continue;

			frame_alloc(&current, w, h);
			if (pic_type == 2 && has_ref)
				frame_copy(&current, &reference);

			bs.byte_pos -= 4;
			bs.bit_pos = 0;

			uint32 mb_ok = decode_picture(&bs, &dec,
				&current, has_ref ? &reference : NULL);

			printf("Frame %d: %c-picture, %u MBs decoded\n",
				frame_count,
				pic_type == 1 ? 'I' : pic_type == 2 ? 'P' : 'B',
				mb_ok);

			// Convert to RGB and store
			frame_to_bitmap(&current, tmpBitmap);

			if (frame_count >= frame_capacity) {
				frame_capacity = frame_capacity ? frame_capacity * 2 : 16;
				frames = (decoded_frame*)realloc(frames,
					frame_capacity * sizeof(decoded_frame));
			}
			frames[frame_count].rgb = (uint8*)malloc(bpr * h);
			memcpy(frames[frame_count].rgb, tmpBitmap->Bits(), bpr * h);
			frames[frame_count].type = pic_type;
			frame_count++;

			// Update reference
			if (pic_type == 1 || pic_type == 2) {
				if (has_ref) frame_free(&reference);
				frame_alloc(&reference, w, h);
				frame_copy(&reference, &current);
				has_ref = true;
			}

			frame_free(&current);
		}
	}

	if (has_ref) frame_free(&reference);
	free(data);

	if (frame_count == 0) {
		printf("No frames decoded\n");
		delete tmpBitmap;
		return 1;
	}

	printf("Decoded %d frames total.\n", frame_count);
	printf("IDCT stats: %u blocks GPU, %u blocks CPU\n",
		sIdctGpuBlocks, sIdctCpuBlocks);

	// Display first frame
	memcpy(tmpBitmap->Bits(), frames[0].rgb, bpr * h);

	char title[128];
	snprintf(title, sizeof(title),
		"MPEG-2 Viewer — Frame 0/%d (%c) [%ux%u]",
		frame_count,
		frames[0].type == 1 ? 'I' : frames[0].type == 2 ? 'P' : 'B',
		w, h);

	BWindow* window = new BWindow(BRect(50, 50, 50 + w - 1, 50 + h - 1),
		title, B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE);
	VideoView* view = new VideoView(window->Bounds(), tmpBitmap);
	window->AddChild(view);
	window->Show();

	// If multiple frames, cycle through them with a timer
	if (frame_count > 1) {
		printf("Playing %d frames at ~10 fps. Close window to stop.\n",
			frame_count);

		// Simple frame cycling in a thread
		struct play_data {
			decoded_frame*	frames;
			int				count;
			BBitmap*		bitmap;
			BWindow*		window;
			VideoView*		view;
			uint32			bpr;
			uint32			h;
			uint32			w;
		};

		static play_data pd;
		pd.frames = frames;
		pd.count = frame_count;
		pd.bitmap = tmpBitmap;
		pd.window = window;
		pd.view = view;
		pd.bpr = bpr;
		pd.h = h;
		pd.w = w;

		thread_id player = spawn_thread([](void* arg) -> status_t {
			play_data* p = (play_data*)arg;
			int idx = 0;
			while (p->window->LockWithTimeout(100000) == B_OK) {
				p->window->Unlock();

				idx = (idx + 1) % p->count;
				memcpy(p->bitmap->Bits(), p->frames[idx].rgb,
					p->bpr * p->h);

				char title[128];
				snprintf(title, sizeof(title),
					"MPEG-2 Viewer — Frame %d/%d (%c) [%ux%u]",
					idx, p->count,
					p->frames[idx].type == 1 ? 'I'
						: p->frames[idx].type == 2 ? 'P' : 'B',
					p->w, p->h);

				if (p->window->LockWithTimeout(100000) == B_OK) {
					p->window->SetTitle(title);
					p->view->Invalidate();
					p->window->Unlock();
				}

				snooze(100000);	// ~10 fps
			}
			return B_OK;
		}, "player", B_NORMAL_PRIORITY, &pd);

		resume_thread(player);
	} else {
		printf("Displaying single frame. Close window to exit.\n");
	}

	app.Run();

	// Cleanup
	for (int i = 0; i < frame_count; i++)
		free(frames[i].rgb);
	free(frames);
	delete tmpBitmap;
	gpu_idct_uninit();
	return 0;
}
