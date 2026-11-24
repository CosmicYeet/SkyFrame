#include "skyframe_image.h"
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
int main(void){
 uint16_t *out=calloc(480u*480u,sizeof(uint16_t));assert(out);
 uint16_t src[8*4];for(int y=0;y<4;y++)for(int x=0;x<8;x++)src[y*8+x]=(uint16_t)x;
 assert(sf_crop_rgb565(src,8,4,out,480u*480u));
 assert(out[0]==2&&out[479]==5&&out[479*480]==2);
 assert(!sf_crop_rgb565(src,8,4,out,3));
 uint16_t portrait[4*8];for(int y=0;y<8;y++)for(int x=0;x<4;x++)portrait[y*4+x]=(uint16_t)y;
 assert(sf_crop_rgb565(portrait,4,8,out,480u*480u));
 assert(out[0]==2&&out[479*480]==5);
 free(out);puts("image crop tests passed");return 0;
}
