#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) { va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r; }
int main(int argc, char** argv) {
    const char* in = argc>1 ? argv[1] : "first_iframe.m2v";
    FILE* f=fopen(in,"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8* d=(uint8*)malloc(sz); fread(d,1,sz,f); fclose(f);
    mpeg2_bits bs; mpeg2_bits_init(&bs,d,sz);
    mpeg2_decoder dec; memset(&dec,0,sizeof(dec));
    while(mpeg2_bits_available(&bs,32)) {
        uint32 code=mpeg2_find_start_code(&bs); if(!code) break;
        if(code==MPEG2_SEQUENCE_HEADER_CODE) mpeg2_parse_sequence_header(&bs,&dec.seq);
        else if(code==MPEG2_PICTURE_START_CODE) mpeg2_parse_picture_header(&bs,&dec.pic);
        else if(code==MPEG2_EXTENSION_START_CODE) {
            if(mpeg2_bits_peek(&bs,4)==8) { mpeg2_bits_skip(&bs,4);
                mpeg2_parse_picture_coding_extension(&bs,&dec.pic_ext);
                mpeg2_decoder_init(&dec); }
        } else if(code>=MPEG2_SLICE_START_MIN && code<=MPEG2_SLICE_START_MAX) break;
    }
    uint32 total=0;
    mpeg2_bits_init(&bs,d,sz);
    while(mpeg2_bits_available(&bs,32)) {
        uint32 code=mpeg2_find_start_code(&bs); if(!code) break;
        if(code<MPEG2_SLICE_START_MIN||code>MPEG2_SLICE_START_MAX) continue;
        mpeg2_slice_header sl; mpeg2_parse_slice_header(&bs,code,&sl);
        mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
        uint32 smb=0;
        while(smb < dec.mb_width && mpeg2_bits_available(&bs,8)) {
            if(mpeg2_bits_peek(&bs,23)==0) break;
            mpeg2_macroblock mb; mb.quantiser_scale_code=sl.quantiser_scale_code;
            if(mpeg2_decode_intra_macroblock(&bs,&dec,&mb)!=B_OK) break;
            smb++; total++;
        }
        printf("Slice %2u: %2u/%u MBs %s\n", sl.slice_vertical_position,
            smb, dec.mb_width, smb==dec.mb_width?"✓":"✗");
    }
    printf("\nTotal: %u/%u (%u%%)\n", total, dec.mb_count, total*100/dec.mb_count);
    free(d);
}
