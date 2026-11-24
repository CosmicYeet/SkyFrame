#include "skyframe_photos.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static sf_photo_catalog_t c;
int main(void){
    assert(!sf_photos_scan(&c,"/nonexistent_skyframe_path"));assert(sf_photos_next(&c)==NULL);
    /* Shuffled passes: each photo once per pass, never the same photo twice in a row. */
    memset(&c,0,sizeof(c));
    c.count=7;c.last=SIZE_MAX;c.rng=12345;
    for(size_t i=0;i<c.count;i++){snprintf(c.paths[i],SF_MAX_PHOTO_PATH,"%zu",i);c.order[i]=(uint16_t)i;}
    const char *prev=NULL;
    bool reordered=false;
    for(int pass=0;pass<50;pass++){
        int seen[7]={0};
        for(size_t k=0;k<c.count;k++){
            const char *p=sf_photos_next(&c);
            assert(p!=prev);prev=p;
            int n=p[0]-'0';seen[n]++;
            if((size_t)n!=k)reordered=true;
        }
        for(int i=0;i<7;i++)assert(seen[i]==1);
    }
    assert(reordered);
    puts("Photo catalog tests passed");return 0;
}
