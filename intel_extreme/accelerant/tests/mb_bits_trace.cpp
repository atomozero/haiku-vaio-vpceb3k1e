#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) {
    va_list a; va_start(a,fmt); int r=vfprintf(stderr,fmt,a); va_end(a); return r;
}
int main(int argc, char** argv) {
    if(argc < 2) { fprintf(stderr,"Usage: %s file.m2v\n",argv[0]); return 1; }
    FILE* f = fopen(argv[1],"rb");
    if(!f) { perror(argv[1]); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8* d=(uint8*)malloc(sz); fread(d,1,sz,f); fclose(f);
    mpeg2_bits bs; mpeg2_bits_init(&bs,d,sz);
    mpeg2_decoder dec; memset(&dec,0,sizeof(dec));
    while(mpeg2_bits_available(&bs,32)) {
        uint32 code=mpeg2_find_start_code(&bs);
        if(!code) break;
        if(code==MPEG2_SEQUENCE_HEADER_CODE)
            mpeg2_parse_sequence_header(&bs,&dec.seq);
        else if(code==MPEG2_PICTURE_START_CODE)
            mpeg2_parse_picture_header(&bs,&dec.pic);
        else if(code==MPEG2_EXTENSION_START_CODE) {
            if(mpeg2_bits_peek(&bs,4)==8) {
                mpeg2_bits_skip(&bs,4);
                mpeg2_parse_picture_coding_extension(&bs,&dec.pic_ext);
                mpeg2_decoder_init(&dec);
            }
        } else if(code>=MPEG2_SLICE_START_MIN && code<=MPEG2_SLICE_START_MAX) {
            mpeg2_slice_header sl;
            mpeg2_parse_slice_header(&bs,code,&sl);
            dec.quantiser_scale = mpeg2_compute_quantiser_scale(
                sl.quantiser_scale_code, dec.pic_ext.q_scale_type);
            mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
            printf("Slice %u (qsc=%u qs=%u):\n",
                code & 0xff, sl.quantiser_scale_code, dec.quantiser_scale);
            int mb_count = 0;
            while(mpeg2_bits_available(&bs, 23)) {
                if(mpeg2_bits_peek(&bs,23) == 0) break;
                uint32 mb_start = mpeg2_bits_offset(&bs);
                mpeg2_macroblock mb;
                memset(&mb,0,sizeof(mb));
                mb.quantiser_scale_code = sl.quantiser_scale_code;
                status_t st = mpeg2_decode_intra_macroblock(&bs, &dec, &mb);
                uint32 mb_end = mpeg2_bits_offset(&bs);
                if(st == B_ENTRY_NOT_FOUND) break;
                if(st != B_OK) {
                    printf("  MB %2d: FAIL bits %5u-%5u (%3u bits) peek=0x%04x\n",
                        mb_count, mb_start, mb_end, mb_end-mb_start,
                        mpeg2_bits_available(&bs,16)?(uint32)mpeg2_bits_peek(&bs,16):0u);
                    mb_count++;
                    continue;
                }
                printf("  MB %2d: OK   bits %5u-%5u (%3u bits)\n",
                    mb_count, mb_start, mb_end, mb_end-mb_start);
                mb_count++;
            }
        }
    }
    free(d);
    return 0;
}
