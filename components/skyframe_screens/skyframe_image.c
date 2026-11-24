#include "skyframe_image.h"
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
bool sf_fit_rgb565(const uint16_t *src, uint32_t width, uint32_t height,
                   uint16_t *out, uint32_t out_w, uint32_t out_h) {
    if (!src || !out || !width || !height || !out_w || !out_h ||
        width > 8192 || height > 8192 || out_w > 8192 || out_h > 8192 ||
        (uint64_t)width * height > SIZE_MAX / sizeof(uint16_t)) return false;
    /* Largest centered source window with the output's aspect ratio. */
    uint32_t win_w = width, win_h = (uint32_t)(((uint64_t)width * out_h) / out_w);
    if (win_h > height) { win_h = height; win_w = (uint32_t)(((uint64_t)height * out_w) / out_h); }
    if (!win_w) win_w = 1;
    if (!win_h) win_h = 1;
    uint32_t x0 = (width - win_w)/2, y0 = (height - win_h)/2;
    if (win_w == out_w && win_h == out_h) {   /* already the right size (prepared photos): copy rows */
        for (uint32_t y = 0; y < out_h; ++y)
            memcpy(&out[(size_t)y*out_w], &src[(size_t)(y0+y)*width + x0], (size_t)out_w * sizeof(uint16_t));
        return true;
    }
    /* Fixed point sampling avoids float and 32-bit overflow in coordinate calculation. */
    for (uint32_t y=0; y<out_h; ++y) {
        uint32_t sy=y0+(uint32_t)(((uint64_t)(2*y+1)*win_h)/(2*out_h));
        if(sy>=height)sy=height-1;
        for(uint32_t x=0;x<out_w;++x){
            uint32_t sx=x0+(uint32_t)(((uint64_t)(2*x+1)*win_w)/(2*out_w));
            if(sx>=width)sx=width-1;
            out[(size_t)y*out_w+x]=src[(size_t)sy*width+sx];
        }
    }
    return true;
}
bool sf_crop_rgb565(const uint16_t *src, uint32_t width, uint32_t height,
                    uint16_t *out, size_t out_pixels) {
    if (out_pixels < SF_PHOTO_SIZE * SF_PHOTO_SIZE) return false;
    return sf_fit_rgb565(src, width, height, out, SF_PHOTO_SIZE, SF_PHOTO_SIZE);
}
