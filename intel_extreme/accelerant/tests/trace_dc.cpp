#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) { va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r; }

// Replicate DC decode with verbose logging
static int16 dc_pred_trace[3];
static void trace_dc_decode(mpeg2_bits* bs, int cc, const char* name) {
    uint32 bit_before = mpeg2_bits_offset(bs);
    int dc_size;
    if (cc == 0) {
        // Luminance DC size
        uint32 code = mpeg2_bits_peek(bs, 9);
        printf("    %s: peek9=0x%03x (%09b)\n", name, code, code);
        // Try decode
        if ((code >> 7) == 0b00) { dc_size = 1; }
        else if ((code >> 7) == 0b01) { dc_size = 2; }
        else if ((code >> 6) == 0b100) { dc_size = 0; }
        else if ((code >> 6) == 0b101) { dc_size = 3; }
        else if ((code >> 6) == 0b110) { dc_size = 4; }
        else if ((code >> 5) == 0b1110) { dc_size = 5; }
        else if ((code >> 4) == 0b11110) { dc_size = 6; }
        else if ((code >> 3) == 0b111110) { dc_size = 7; }
        else if ((code >> 2) == 0b1111110) { dc_size = 8; }
        else if ((code >> 1) == 0b11111110) { dc_size = 9; }
        else if (code == 0b111111110) { dc_size = 10; }
        else if (code == 0b111111111) { dc_size = 11; }
        else { printf("    ERROR: unknown dc_size\n"); return; }
    } else {
        uint32 code = mpeg2_bits_peek(bs, 10);
        printf("    %s: chroma peek10=0x%03x\n", name, code);
        if ((code >> 8) == 0b00) dc_size = 0;
        else if ((code >> 8) == 0b01) dc_size = 1;
        else if ((code >> 8) == 0b10) dc_size = 2;
        else if ((code >> 7) == 0b110) dc_size = 3;
        else { printf("    dc_size > 3, skipping detail\n"); dc_size = -1; return; }
    }
    printf("    dc_size=%d\n", dc_size);
    
    if (dc_size == 0) {
        printf("    dc_diff=0, dc_pred stays at %d\n", dc_pred_trace[cc]);
    } else {
        // Read dc_size bits for differential
        // (don't actually consume - just peek)
        int16 half = 1 << (dc_size - 1);
        printf("    half_range=%d, will read %d bits for differential\n", half, dc_size);
        
        // Compute expected dc_diff
        // For black (pixel=0): dc_pred should become 0
        // dc_diff = 0 - dc_pred_trace[cc]
        printf("    dc_pred_before=%d, for pixel=0 need dc_diff=%d\n", 
            dc_pred_trace[cc], -dc_pred_trace[cc]);
    }
}

int main() {
    FILE* f = fopen("first_iframe.m2v","rb");
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8* d=(uint8*)malloc(sz); fread(d,1,sz,f); fclose(f);
    mpeg2_bits bs; mpeg2_bits_init(&bs,d,sz);
    mpeg2_decoder dec; memset(&dec,0,sizeof(dec));
    
    while(mpeg2_bits_available(&bs,32)) {
        uint32 code=mpeg2_find_start_code(&bs);
        if(!code) break;
        if(code==MPEG2_SEQUENCE_HEADER_CODE) mpeg2_parse_sequence_header(&bs,&dec.seq);
        else if(code==MPEG2_PICTURE_START_CODE) mpeg2_parse_picture_header(&bs,&dec.pic);
        else if(code==MPEG2_EXTENSION_START_CODE) {
            if(mpeg2_bits_peek(&bs,4)==8) { mpeg2_bits_skip(&bs,4); mpeg2_parse_picture_coding_extension(&bs,&dec.pic_ext); mpeg2_decoder_init(&dec); }
        } else if(code>=MPEG2_SLICE_START_MIN && code<=MPEG2_SLICE_START_MAX) {
            mpeg2_slice_header sl;
            mpeg2_parse_slice_header(&bs,code,&sl);
            mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
            
            // Initialize trace DC predictors
            int16 init = 1 << (7 + dec.pic_ext.intra_dc_precision);
            dc_pred_trace[0] = dc_pred_trace[1] = dc_pred_trace[2] = init;
            printf("DC pred init = %d (dc_prec=%u)\n", init, dec.pic_ext.intra_dc_precision);
            
            // Parse addr_inc + mb_type  
            uint32 bit_start = mpeg2_bits_offset(&bs);
            printf("Slice data at bit %u\n", bit_start);
            printf("Peek16: 0x%04x\n", mpeg2_bits_peek(&bs, 16));
            
            // Decode first MB manually
            mpeg2_macroblock mb;
            mb.quantiser_scale_code = sl.quantiser_scale_code;
            
            // Before decoding: show what the DC should be
            printf("\nFirst MB - tracing Y0 DC:\n");
            
            // Skip addr_inc and mb_type manually
            // addr_inc '1' = 1 bit
            printf("  addr_inc peek: bit=%d\n", mpeg2_bits_peek(&bs,1));
            
            // Actually just decode the whole MB and check result
            status_t st = mpeg2_decode_intra_macroblock(&bs, &dec, &mb);
            printf("  MB decode: %s\n", st==0?"OK":"FAIL");
            printf("  Y0 DC coeff (block[0][0]) = %d\n", mb.blocks[0][0]);
            printf("  Y0 DC * dc_mult = %d * %d = %d\n", 
                mb.blocks[0][0], dec.intra_dc_mult, 
                mb.blocks[0][0] * dec.intra_dc_mult);
            
            // Compute expected pixel from this DC
            int32 dc_iq = mb.blocks[0][0] * dec.intra_dc_mult;
            // IDCT of [dc_iq, 0...] ≈ dc_iq * 16383^2 / 2^31
            double pixel_approx = (double)dc_iq * 16383.0 * 16383.0 / (1LL << 31);
            printf("  Approx pixel from DC = %.1f (ffmpeg says 0)\n", pixel_approx);
            printf("  For pixel=0: need DC_IQ=0, so dc_pred should be 0\n");
            printf("  dc_pred was %d + dc_diff = %d\n", init, mb.blocks[0][0]);
            printf("  dc_diff = %d - %d = %d\n", mb.blocks[0][0], init, mb.blocks[0][0] - init);
            break;
        }
    }
    free(d);
}
