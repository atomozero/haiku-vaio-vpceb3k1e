#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) {
    va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r;
}

// Hook: trace every AC coeff decode
static int g_trace_mb = -1;
static int g_trace_block = -1;

// Wrap the parser: decode MB 0-11, trace MB 9-10
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
            if(mpeg2_bits_peek(&bs,4)==8) { mpeg2_bits_skip(&bs,4);
                mpeg2_parse_picture_coding_extension(&bs,&dec.pic_ext);
                mpeg2_decoder_init(&dec); }
        } else if(code>=MPEG2_SLICE_START_MIN && code<=MPEG2_SLICE_START_MAX) {
            mpeg2_slice_header sl;
            mpeg2_parse_slice_header(&bs,code,&sl);
            mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
            
            for(int mb=0; mb<12; mb++) {
                uint32 start = mpeg2_bits_offset(&bs);
                if(mpeg2_bits_peek(&bs,23)==0) break;
                
                // For MB 8-10: manually trace the decode
                if(mb >= 8) {
                    printf("\n=== MB %d at bit %u ===\n", mb, start);
                    
                    // addr_inc
                    uint32 ai_start = mpeg2_bits_offset(&bs);
                    // Just peek to see what's coming
                    printf("  peek8: %08b\n", mpeg2_bits_peek(&bs,8));
                }
                
                mpeg2_macroblock mbdata;
                mbdata.quantiser_scale_code = sl.quantiser_scale_code;
                status_t st = mpeg2_decode_intra_macroblock(&bs,&dec,&mbdata);
                uint32 end = mpeg2_bits_offset(&bs);
                
                if(mb >= 8) {
                    printf("  result: %s, %u bits, DC=[%d,%d,%d,%d,%d,%d]\n",
                        st==0?"OK":"FAIL", end-start,
                        mbdata.blocks[0][0], mbdata.blocks[1][0],
                        mbdata.blocks[2][0], mbdata.blocks[3][0],
                        mbdata.blocks[4][0], mbdata.blocks[5][0]);
                    
                    if(st==0) {
                        // Count non-zero AC per block
                        for(int b=0;b<6;b++) {
                            int nz=0;
                            for(int i=1;i<64;i++) if(mbdata.blocks[b][i]) nz++;
                            if(nz) {
                                printf("  block[%d]: %d AC coeffs:", b, nz);
                                for(int i=1;i<64;i++)
                                    if(mbdata.blocks[b][i])
                                        printf(" [%d]=%d", i, mbdata.blocks[b][i]);
                                printf("\n");
                            }
                        }
                    }
                }
                if(st!=0) break;
            }
            break;
        }
    }
    free(d);
}
