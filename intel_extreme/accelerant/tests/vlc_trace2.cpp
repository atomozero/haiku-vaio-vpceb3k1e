#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) {
    va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r;
}
extern int mpeg2_vlc_trace;
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "first_iframe.m2v";
    FILE* f = fopen(path,"rb");
    if (!f) { printf("Cannot open %s\n", path); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8* d=(uint8*)malloc(sz); fread(d,1,sz,f); fclose(f);
    mpeg2_bits bs; mpeg2_bits_init(&bs,d,sz);
    mpeg2_decoder dec; memset(&dec,0,sizeof(dec));
    int total=0, failed=0;
    while(mpeg2_bits_available(&bs,32)) {
        uint32 code=mpeg2_find_start_code(&bs); if(!code) break;
        printf("[%06u] start_code=0x%02x\n", mpeg2_bits_offset(&bs), code & 0xFF);
        if(code==MPEG2_SEQUENCE_HEADER_CODE) {
            mpeg2_parse_sequence_header(&bs,&dec.seq);
            printf("  → seq %ux%u, next_bit=%u\n", dec.seq.width, dec.seq.height, mpeg2_bits_offset(&bs));
        } else if(code==MPEG2_PICTURE_START_CODE) {
            mpeg2_parse_picture_header(&bs,&dec.pic);
            printf("  → pic type=%d, next_bit=%u\n", dec.pic.picture_coding_type, mpeg2_bits_offset(&bs));
        } else if(code==MPEG2_EXTENSION_START_CODE) {
            uint32 ext_id = mpeg2_bits_peek(&bs,4);
            printf("  → ext_id=%u, bit=%u\n", ext_id, mpeg2_bits_offset(&bs));
            if(ext_id==8) {
                mpeg2_bits_skip(&bs,4);
                mpeg2_parse_picture_coding_extension(&bs,&dec.pic_ext);
                mpeg2_decoder_init(&dec);
                printf("  → pic_ext parsed, next_bit=%u\n", mpeg2_bits_offset(&bs));
            } else if(ext_id==1) {
                // Sequence extension — skip 4-bit id + parse remaining bits
                // profile_and_level(8) + progressive(1) + chroma(2) + h_ext(2)
                // + v_ext(2) + bit_rate_ext(12) + marker(1) + vbv_size_ext(8)
                // + low_delay(1) + frame_rate_ext_n(2) + frame_rate_ext_d(5) = 44 bits
                mpeg2_bits_skip(&bs, 4 + 44);
                printf("  → seq_ext skipped, next_bit=%u\n", mpeg2_bits_offset(&bs));
            }
        } else if(code>=MPEG2_SLICE_START_MIN && code<=MPEG2_SLICE_START_MAX) {
            mpeg2_slice_header sl;
            mpeg2_parse_slice_header(&bs,code,&sl);
            uint32 slice_start = mpeg2_bits_offset(&bs);
            printf("  → slice %u, qsc=%u, data_start_bit=%u\n",
                sl.slice_vertical_position, sl.quantiser_scale_code, slice_start);
            mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
            for(int mb=0; mb<1200; mb++) {
                if(mpeg2_bits_peek(&bs,23)==0) break;
                uint32 s=mpeg2_bits_offset(&bs);
                mpeg2_macroblock mbdata;
                mbdata.quantiser_scale_code=sl.quantiser_scale_code;
                status_t st=mpeg2_decode_intra_macroblock(&bs,&dec,&mbdata);
                uint32 e=mpeg2_bits_offset(&bs);
                if(st!=0) {
                    printf("  MB%d: bit %u FAIL (consumed %u bits)\n",mb,s,e-s);
                    failed++;
                    break;
                }
                total++;
                if(mb < 12 || st != 0)
                    printf("  MB%d: bit %u-%u (%u bits) OK\n",mb,s,e,e-s);
            }
            // Only process first slice for debug
            if(failed > 0) break;
        }
    }
    printf("\nTotal OK: %d, Failed: %d\n", total, failed);
    free(d);
}
