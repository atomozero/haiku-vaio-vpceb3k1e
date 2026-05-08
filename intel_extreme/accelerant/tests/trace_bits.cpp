#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) { va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r; }
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
            printf("Start code 0x%08x consumed, bit_pos=%u (byte %u, bit %u)\n",
                code, mpeg2_bits_offset(&bs), mpeg2_bits_offset(&bs)/8, mpeg2_bits_offset(&bs)%8);
            
            // Read q_scale_code manually
            uint32 qsc = mpeg2_bits_read(&bs, 5);
            printf("q_scale_code=%u, bit_pos=%u\n", qsc, mpeg2_bits_offset(&bs));
            
            // extra_bit_slice
            while (mpeg2_bits_available(&bs,1) && mpeg2_bits_peek(&bs,1)==1) {
                printf("  extra_bit=1 at %u\n", mpeg2_bits_offset(&bs));
                mpeg2_bits_skip(&bs, 9);
            }
            uint32 ebs = mpeg2_bits_read(&bs, 1);
            printf("extra_bit_slice=%u at bit %u\n", ebs, mpeg2_bits_offset(&bs));
            
            // Now we should be at macroblock data
            printf("MB data starts at bit %u\n", mpeg2_bits_offset(&bs));
            
            // addr_inc
            printf("addr_inc peek1=%u\n", mpeg2_bits_peek(&bs,1));
            mpeg2_bits_skip(&bs, 1); // consume '1'
            printf("after addr_inc: bit %u\n", mpeg2_bits_offset(&bs));
            
            // mb_type
            printf("mb_type peek2=0b%u%u\n", 
                (mpeg2_bits_peek(&bs,2)>>1)&1, mpeg2_bits_peek(&bs,2)&1);
            mpeg2_bits_skip(&bs, 1); // consume '1' (intra)
            printf("after mb_type: bit %u\n", mpeg2_bits_offset(&bs));
            
            // DC size luminance
            uint32 dc_peek = mpeg2_bits_peek(&bs, 9);
            printf("DC luma peek9=0x%03x = ", dc_peek);
            for(int i=8;i>=0;i--) printf("%u",(dc_peek>>i)&1);
            printf("\n");
            
            break;
        }
    }
    free(d);
}
