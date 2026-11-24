#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Source and output use row-major RGB565 pixels. Handles landscape and portrait images.
   Caller must decode JPEG separately and provide source and destination buffers.
   Center-crop, nearest-neighbor scale to 480x480, keeping image aspect ratio.
   Destination needs at least 480*480 uint16_t pixels (450 KiB). */
#define SF_PHOTO_SIZE 480u
bool sf_crop_rgb565(const uint16_t *src, uint32_t width, uint32_t height,
                    uint16_t *out, size_t out_pixels);
/* Same center-crop + nearest-neighbor scale, to any out_w x out_h frame. */
bool sf_fit_rgb565(const uint16_t *src, uint32_t width, uint32_t height,
                   uint16_t *out, uint32_t out_w, uint32_t out_h);
