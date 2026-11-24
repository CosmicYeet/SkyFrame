#include "skyframe_photos.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
static bool jpeg(const char *name){
    size_t n=strlen(name);
    if(n<5||name[0]=='.')return false;   /* hidden files, incl. macOS "._photo.jpg" companions */
    const char *dot=strrchr(name,'.');if(!dot)return false;
    char ext[6]={0};size_t len=strlen(dot);if(len>5)return false;
    for(size_t i=0;i<len;i++)ext[i]=(char)tolower((unsigned char)dot[i]);
    return !strcmp(ext,".jpg")||!strcmp(ext,".jpeg");
}
bool sf_photos_scan(sf_photo_catalog_t *c,const char *dir){
    if(!c||!dir||!dir[0])return false;
    memset(c,0,sizeof(*c));
    DIR *d=opendir(dir);if(!d)return false;
    struct dirent *entry;
    while((entry=readdir(d))&&c->count<SF_MAX_PHOTOS){
        if(!jpeg(entry->d_name))continue;
        int n=snprintf(c->paths[c->count],SF_MAX_PHOTO_PATH,"%s/%s",dir,entry->d_name);
        if(n<0||n>=SF_MAX_PHOTO_PATH)continue;
        c->count++;
    }
    closedir(d);
    for(size_t i=0;i<c->count;i++)c->order[i]=(uint16_t)i;
    c->last=SIZE_MAX;
    return true;
}
static uint32_t rnd(sf_photo_catalog_t *c){   /* xorshift32 */
    uint32_t x=c->rng?c->rng:0x9E3779B9u;
    x^=x<<13;x^=x>>17;x^=x<<5;
    return c->rng=x;
}
const char *sf_photos_next(sf_photo_catalog_t *c){
    if(!c||!c->count)return NULL;
    if(c->next==0&&c->count>1){
        /* New pass: Fisher-Yates shuffle, then make sure it doesn't repeat the last photo. */
        for(size_t i=c->count-1;i>0;i--){
            size_t j=rnd(c)%(i+1);
            uint16_t t=c->order[i];c->order[i]=c->order[j];c->order[j]=t;
        }
        if(c->order[0]==c->last){
            size_t j=1+rnd(c)%(c->count-1);
            uint16_t t=c->order[0];c->order[0]=c->order[j];c->order[j]=t;
        }
    }
    size_t index=c->order[c->next];
    c->next=(c->next+1)%c->count;
    c->last=index;
    return c->paths[index];
}
