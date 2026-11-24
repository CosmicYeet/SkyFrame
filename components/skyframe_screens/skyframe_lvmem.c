/* LVGL's allocator: see skyframe_lvmem.h. Device only (the simulator uses malloc). */
#include "skyframe_lvmem.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

static int prefer_psram;   /* UI thread only */

void sf_lvmem_prefer_psram(bool on)
{
    prefer_psram += on ? 1 : -1;
    if (prefer_psram < 0) prefer_psram = 0;
}
void *sf_lv_malloc(size_t size)
{
    if (prefer_psram) return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
    return malloc(size);
}
void *sf_lv_realloc(void *ptr, size_t size)
{
    /* A block that lives in PSRAM stays there (a test-mode label's text changing later). */
    if (prefer_psram || (ptr && esp_ptr_external_ram(ptr)))
        return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
    return realloc(ptr, size);
}
void sf_lv_free(void *ptr) { free(ptr); }
