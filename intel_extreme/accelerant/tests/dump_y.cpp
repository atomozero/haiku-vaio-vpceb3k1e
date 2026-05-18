#include "mpeg2_parser.h"
#include "idct_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" int _sPrintf(const char* fmt, ...) { va_list a; va_start(a,fmt); int r=vprintf(fmt,a); va_end(a); return r; }
static void decode_block(const int16 c[64],const uint8 qm[64],uint16 qs,uint16 dm,uint8 p[64]){
    int16 iq[64]; for(int i=0;i<64;i++){int32 t=(int32)c[i]*(int32)(uint32)qm[i]; t*=(int32)(uint32)qs; t>>=4; iq[i]=(int16)t;}
    iq[0]=(int16)((int32)c[0]*(int32)(uint32)dm);
    int16 id[64]; compute_idct_reference(iq,id);
    for(int i=0;i<64;i++){int32 v=id[i]; if(v<0)v=0; if(v>255)v=255; p[i]=(uint8)v;}
}
int main(int argc, char** argv) {
    const char* in = argc>1?argv[1]:"test_real_q31.m2v";
    FILE* f=fopen(in,"rb"); fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    uint8* d=(uint8*)malloc(sz);fread(d,1,sz,f);fclose(f);
    mpeg2_bits bs; mpeg2_bits_init(&bs,d,sz);
    mpeg2_decoder dec; memset(&dec,0,sizeof(dec));
    while(mpeg2_bits_available(&bs,32)){
        uint32 code=mpeg2_find_start_code(&bs); if(!code)break;
        if(code==MPEG2_SEQUENCE_HEADER_CODE) mpeg2_parse_sequence_header(&bs,&dec.seq);
        else if(code==MPEG2_PICTURE_START_CODE) mpeg2_parse_picture_header(&bs,&dec.pic);
        else if(code==MPEG2_EXTENSION_START_CODE){if(mpeg2_bits_peek(&bs,4)==8){mpeg2_bits_skip(&bs,4);mpeg2_parse_picture_coding_extension(&bs,&dec.pic_ext);mpeg2_decoder_init(&dec);}}
        else if(code>=MPEG2_SLICE_START_MIN&&code<=MPEG2_SLICE_START_MAX) break;
    }
    uint32 w=dec.seq.width,h=dec.seq.height;
    uint8* y=(uint8*)calloc(w*h,1);
    mpeg2_bits_init(&bs,d,sz);
    static const int ydx[4]={0,8,0,8},ydy[4]={0,0,8,8};
    uint32 mbt=0;
    while(mpeg2_bits_available(&bs,32)){
        uint32 code=mpeg2_find_start_code(&bs); if(!code)break;
        if(code<MPEG2_SLICE_START_MIN||code>MPEG2_SLICE_START_MAX) continue;
        mpeg2_slice_header sl; mpeg2_parse_slice_header(&bs,code,&sl);
        mpeg2_reset_slice_state(dec.pic_ext.intra_dc_precision);
        uint16 qs=mpeg2_compute_quantiser_scale(sl.quantiser_scale_code,dec.pic_ext.q_scale_type);
        uint32 mby=sl.slice_vertical_position-1,mbx=0;
        while(mpeg2_bits_available(&bs,8)){
            if(mpeg2_bits_peek(&bs,23)==0)break;
            mpeg2_macroblock mb; mb.quantiser_scale_code=sl.quantiser_scale_code;
            if(mpeg2_decode_intra_macroblock(&bs,&dec,&mb)!=B_OK)break;
            if(mb.address_increment>1) mbx+=mb.address_increment-1;
            for(int b=0;b<4;b++){
                uint8 pix[64]; decode_block(mb.blocks[b],dec.seq.intra_quantiser_matrix,qs,dec.intra_dc_mult,pix);
                uint32 bx=mbx*16+ydx[b],by=mby*16+ydy[b];
                for(int r=0;r<8;r++)for(int c=0;c<8;c++)
                    if(bx+c<w&&by+r<h) y[(by+r)*w+bx+c]=pix[r*8+c];
            }
            mbt++; mbx++;
        }
    }
    printf("Decoded %u/%u MBs, writing Y-only PGM\n",mbt,dec.mb_count);
    f=fopen("/tmp/y_only.pgm","wb");
    fprintf(f,"P5\n%u %u\n255\n",w,h);
    fwrite(y,1,w*h,f); fclose(f);
    // Also print first MB's Y values
    printf("Y[0..7]: %u %u %u %u %u %u %u %u\n",y[0],y[1],y[2],y[3],y[4],y[5],y[6],y[7]);
    free(y);free(d);
}
