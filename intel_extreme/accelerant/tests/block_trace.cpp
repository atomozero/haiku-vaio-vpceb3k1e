#include "mpeg2_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) {
    va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r;
}
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
        } else if(code>=MPEG2_SLICE_START_MIN && code<=MPEG2_SLICE_START_MAX) {
            mpeg2_slice_header sl;
            mpeg2_parse_slice_header(&bs,code,&sl);
            mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
            
            // Decode MBs 0-9, trace block-by-block for MB 9
            for(int mb=0; mb<10; mb++) {
                if(mpeg2_bits_peek(&bs,23)==0) break;
                
                uint32 mb_start = mpeg2_bits_offset(&bs);
                
                if(mb == 9) {
                    // Manual decode with per-field tracing
                    // addr_inc
                    uint32 p1 = mpeg2_bits_offset(&bs);
                    int ai = (mpeg2_bits_peek(&bs,1)==1) ? 1 : -1;
                    if(ai==1) mpeg2_bits_skip(&bs,1);
                    printf("MB9 addr_inc: %d (%u bits)\n", ai, mpeg2_bits_offset(&bs)-p1);
                    
                    // mb_type
                    p1 = mpeg2_bits_offset(&bs);
                    int mt = mpeg2_bits_peek(&bs,1);
                    if(mt==1) { mpeg2_bits_skip(&bs,1); printf("MB9 mb_type: intra (1 bit)\n"); }
                    else { mpeg2_bits_skip(&bs,2); printf("MB9 mb_type: intra+quant (2 bits)\n"); }
                    
                    // Decode 6 blocks with per-block bit tracking
                    // (reusing the parser's decode)
                    mpeg2_macroblock mbdata;
                    mbdata.quantiser_scale_code = sl.quantiser_scale_code;
                    
                    // I need to call decode blocks individually
                    // but our API decodes all 6 at once.
                    // Instead: record bit pos before/after full decode
                    // by reading back from the bitstream position.
                    
                    // Save bitstream state
                    mpeg2_bits saved = bs;
                    
                    // Try decoding and measure total
                    status_t st = mpeg2_decode_intra_macroblock(&saved, &dec, &mbdata);
                    uint32 total = mpeg2_bits_offset(&saved) - mb_start;
                    printf("MB9 total: %u bits (addr_inc+mb_type+6blocks)\n", total);
                    printf("MB9 overhead (addr+type): %u bits\n", 
                        mpeg2_bits_offset(&bs) - mb_start);
                    
                    // Now decode block by block manually
                    // Actually, can't easily do this without modifying the parser.
                    // Instead, compare total bits vs expected.
                    
                    // For each block: DC (dc_size_vlc + dc_size bits) + AC (vlc+sign per coeff + EOB)
                    // block[0-2]: DC only (0 AC) → ~5 bits each (dc_size=0 or 1)
                    // block[3]: 9 AC → many bits
                    // block[4]: 7 AC → many bits
                    // block[5]: 1 AC → few bits
                    
                    // Use the saved decode
                    bs = saved;
                    break;
                }
                
                mpeg2_macroblock mbdata;
                mbdata.quantiser_scale_code = sl.quantiser_scale_code;
                mpeg2_decode_intra_macroblock(&bs,&dec,&mbdata);
                printf("MB%d: %u bits\n", mb, mpeg2_bits_offset(&bs)-mb_start);
            }
            
            // Now check: where should MB10 actually start?
            printf("\nMB10 should be at: %u\n", mpeg2_bits_offset(&bs));
            printf("Peek at that pos: ");
            for(int i=0;i<16;i++) printf("%d", (mpeg2_bits_peek(&bs,i+1)>>(0))&1);
            printf("\n");
            
            // Try offsets -3 to +3
            for(int off=-3; off<=3; off++) {
                mpeg2_bits test = bs;
                // Adjust
                if(off > 0) mpeg2_bits_skip(&test, off);
                else {
                    test.byte_pos = (mpeg2_bits_offset(&bs) + off) / 8;
                    test.bit_pos = (mpeg2_bits_offset(&bs) + off) % 8;
                }
                uint32 peek2 = mpeg2_bits_peek(&test, 2);
                uint32 peek1 = mpeg2_bits_peek(&test, 1);
                printf("offset %+d (bit %u): peek1=%d peek2=%02b → %s\n",
                    off, mpeg2_bits_offset(&test), peek1, peek2,
                    peek1==1 ? "addr_inc=1 ✓" :
                    peek2==0b01 ? "addr_inc+quant?" : "no match");
            }
            
            break;
        }
    }
    free(d);
}
