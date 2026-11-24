#include "skyframe_image.h"
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
/* The scaler as first written (one 64-bit divide per pixel): the stepped one must match it. */
static void reference_fit(const uint16_t *src,uint32_t w,uint32_t h,uint16_t *out,uint32_t ow,uint32_t oh){
 uint32_t win_w=w,win_h=(uint32_t)(((uint64_t)w*oh)/ow);
 if(win_h>h){win_h=h;win_w=(uint32_t)(((uint64_t)h*ow)/oh);}
 if(!win_w)win_w=1;
 if(!win_h)win_h=1;
 uint32_t x0=(w-win_w)/2,y0=(h-win_h)/2;
 for(uint32_t y=0;y<oh;++y){
  uint32_t sy=y0+(uint32_t)(((uint64_t)(2*y+1)*win_h)/(2*oh));if(sy>=h)sy=h-1;
  for(uint32_t x=0;x<ow;++x){
   uint32_t sx=x0+(uint32_t)(((uint64_t)(2*x+1)*win_w)/(2*ow));if(sx>=w)sx=w-1;
   out[(size_t)y*ow+x]=src[(size_t)sy*w+sx];
  }
 }
}
static void test_matches_reference(void){
 static const uint32_t sizes[][4]={{8,4,480,480},{4,8,480,480},{1,1,280,156},{640,480,280,156},
  {1600,1200,480,480},{300,200,480,480},{3,1000,17,9},{8192,2,480,480},{1000,1000,1,1},{481,479,480,480}};
 for(size_t t=0;t<sizeof(sizes)/sizeof(sizes[0]);t++){
  uint32_t w=sizes[t][0],h=sizes[t][1],ow=sizes[t][2],oh=sizes[t][3];
  uint16_t *src=malloc((size_t)w*h*2),*a=malloc((size_t)ow*oh*2),*b=malloc((size_t)ow*oh*2);assert(src&&a&&b);
  for(size_t i=0;i<(size_t)w*h;i++)src[i]=(uint16_t)(i*2654435761u>>7);
  assert(sf_fit_rgb565(src,w,h,a,ow,oh));
  reference_fit(src,w,h,b,ow,oh);
  for(size_t i=0;i<(size_t)ow*oh;i++)if(a[i]!=b[i]){fprintf(stderr,"%ux%u->%ux%u differs at %zu\n",w,h,ow,oh,i);assert(0);}
  free(src);free(a);free(b);
 }
}
int main(void){
 test_matches_reference();
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
