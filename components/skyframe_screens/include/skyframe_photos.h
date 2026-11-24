#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define SF_MAX_PHOTOS 256
#define SF_MAX_PHOTO_PATH 256
/* Photos play in shuffled passes: every photo once per pass, a new order each pass. */
typedef struct {
    char paths[SF_MAX_PHOTOS][SF_MAX_PHOTO_PATH];
    size_t count;
    size_t next;                    /* position in the current pass */
    uint16_t order[SF_MAX_PHOTOS];  /* this pass's play order (indexes into paths) */
    size_t last;                    /* index shown last; a new pass never starts with it */
    uint32_t rng;                   /* shuffle state; seed after scanning (0 = fixed seed) */
} sf_photo_catalog_t;
/* Enumerates .jpg/.jpeg files; mount SD at /sdcard before calling. */
bool sf_photos_scan(sf_photo_catalog_t *catalog, const char *dir);
const char *sf_photos_next(sf_photo_catalog_t *catalog);
