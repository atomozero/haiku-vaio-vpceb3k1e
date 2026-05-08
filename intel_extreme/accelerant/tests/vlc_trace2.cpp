#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) {
    va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r;
}
extern int mpeg2_vlc_trace;
int main() {
    FILE* f = fopen("first_iframe.m2v","rb");
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
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
        } else if(code>=MPEG2_SLICE_START_MIN&&code<=MPEG2_SLICE_START_MAX) {
            mpeg2_slice_header sl;
            mpeg2_parse_slice_header(&bs,code,&sl);
            mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
            for(int mb=0; mb<=10; mb++) {
                if(mpeg2_bits_peek(&bs,23)==0) break;
                if(mb==9) mpeg2_vlc_trace=1;
                uint32 s=mpeg2_bits_offset(&bs);
                mpeg2_macroblock mbdata;
                mbdata.quantiser_scale_code=sl.quantiser_scale_code;
                status_t st=mpeg2_decode_intra_macroblock(&bs,&dec,&mbdata);
                mpeg2_vlc_trace=0;
                printf("MB%d: bit %u-%u (%u bits) %s\n",mb,s,mpeg2_bits_offset(&bs),mpeg2_bits_offset(&bs)-s,st==0?"OK":"FAIL");
                if(st!=0) break;
            }
            break;
        }
    }
    free(d);
}
