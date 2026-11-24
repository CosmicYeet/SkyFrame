#include "plane_library.h"
#include "skyframe_sd.h"
#include "skyframe_jpeg.h"
#include "skyframe_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PLANES_DIR SF_SD_ROOT "/planes"
#define TYPES_DIR  PLANES_DIR "/types"
#define SAVED_DIR  PLANES_DIR "/saved"
#define PHOTO_BYTES ((size_t)SKYFRAME_PHOTO_W * SKYFRAME_PHOTO_H * 2u)

static const char *TAG = "plane_library";

/* Saved-photo file: 8-byte header, then raw RGB565 pixels (no decoding needed). */
typedef struct { char magic[4]; uint16_t width, height; } saved_header_t;
static const char SAVED_MAGIC[4] = {'S', 'F', 'P', '1'};

/* File names come from network data: allow only short alphanumeric names. */
static bool safe_name(const char *s, size_t max_len, char *out, size_t cap, bool upper)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 2 || n > max_len || n >= cap) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c)) return false;
        out[i] = (char)(upper ? toupper(c) : c);
    }
    out[n] = 0;
    return true;
}

uint16_t *plane_library_load_saved(const char *icao)
{
    char name[8], path[64];
    if (!safe_name(icao, 6, name, sizeof(name), true)) return NULL;
    snprintf(path, sizeof(path), SAVED_DIR "/%s.rgb", name);
    if (!sf_sd_begin()) return NULL;
    uint16_t *pixels = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        saved_header_t h;
        if (fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, SAVED_MAGIC, 4) &&
            h.width == SKYFRAME_PHOTO_W && h.height == SKYFRAME_PHOTO_H) {
            pixels = heap_caps_malloc(PHOTO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pixels && fread(pixels, 1, PHOTO_BYTES, f) != PHOTO_BYTES) { free(pixels); pixels = NULL; }
        }
        fclose(f);
    }
    sf_sd_end();
    return pixels;
}

uint16_t *plane_library_load_type(const char *type_code)
{
    char name[12], path[64];
    if (!safe_name(type_code, 7, name, sizeof(name), true)) return NULL;
    snprintf(path, sizeof(path), TYPES_DIR "/%s.jpg", name);
    if (!sf_sd_begin()) return NULL;
    uint8_t *jpeg = NULL;
    size_t len = 0;
    struct stat st;
    esp_err_t e = stat(path, &st) == 0 ? sf_jpeg_read_file(path, &jpeg, &len) : ESP_ERR_NOT_FOUND;
    sf_sd_end();
    if (e != ESP_OK) return NULL;
    /* Decode after releasing the card. */
    uint16_t *pixels = NULL;
    if (sf_jpeg_decode_fit(jpeg, len, &pixels, SKYFRAME_PHOTO_W, SKYFRAME_PHOTO_H, path) != ESP_OK) pixels = NULL;
    free(jpeg);
    return pixels;
}

esp_err_t plane_library_save(const char *icao, const uint16_t *pixels)
{
    char name[8], path[64], tmp[64];
    if (!pixels || !safe_name(icao, 6, name, sizeof(name), true)) return ESP_ERR_INVALID_ARG;
    snprintf(path, sizeof(path), SAVED_DIR "/%s.rgb", name);
    snprintf(tmp, sizeof(tmp), SAVED_DIR "/%s.tmp", name);
    if (!sf_sd_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t rc = ESP_FAIL;
    if ((mkdir(PLANES_DIR, 0775) == 0 || errno == EEXIST) && (mkdir(SAVED_DIR, 0775) == 0 || errno == EEXIST)) {
        /* Write to a temp file and rename, so a half-written file is never loaded. */
        FILE *f = fopen(tmp, "wb");
        if (f) {
            saved_header_t h = { .width = SKYFRAME_PHOTO_W, .height = SKYFRAME_PHOTO_H };
            memcpy(h.magic, SAVED_MAGIC, 4);
            bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(pixels, 1, PHOTO_BYTES, f) == PHOTO_BYTES;
            ok = (fclose(f) == 0) && ok;
            remove(path);
            if (ok && rename(tmp, path) == 0) rc = ESP_OK;
            else remove(tmp);
        }
    }
    sf_sd_end();
    if (rc != ESP_OK) ESP_LOGW(TAG, "Could not save photo for %s", name);
    return rc;
}
