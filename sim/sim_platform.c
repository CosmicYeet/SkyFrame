/* SkyFrame simulator: ESP-IDF / FreeRTOS stand-ins on a PC, the SDL window that plays the
   round 480x480 display (mouse = finger), and main(). SkyFrame's own app_main() runs on
   the main thread, exactly as on the device; its worker tasks run as threads. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include "sim_esp.h"
#include "sim_board.h"
#include "lvgl.h"

void app_main(void);
#define TAG "sim"

/* ---- Time, logging, errors --------------------------------------------------- */
static struct timespec t0;
int64_t esp_timer_get_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)(t.tv_sec - t0.tv_sec) * 1000000 + (t.tv_nsec - t0.tv_nsec) / 1000;
}
TickType_t xTaskGetTickCount(void) { return (TickType_t)(esp_timer_get_time() / 1000); }
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
void sim_log(char level, const char *tag, const char *fmt, ...) {
    pthread_mutex_lock(&log_lock);
    printf("%c (%lu) %s: ", level, (unsigned long)(esp_timer_get_time() / 1000), tag);
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    printf("\n"); fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}
const char *esp_err_to_name(esp_err_t e) {
    switch (e) {
    case ESP_OK: return "ESP_OK"; case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM"; case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE"; case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND"; case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT"; case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    default: return "ESP_ERR";
    }
}
/* Memory figures for the test menu: the device's sizes, not the PC's. */
size_t heap_caps_get_free_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 6u << 20 : 120u << 10; }
size_t heap_caps_get_total_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 8u << 20 : 320u << 10; }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 5u << 20 : 96u << 10; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 4u << 20 : 64u << 10; }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void esp_restart(void) { sim_log('I', TAG, "Restart requested: closing the simulator"); exit(0); }
uint32_t esp_random(void) { return ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ (uint32_t)esp_timer_get_time(); }
const esp_app_desc_t *esp_app_get_description(void) {
    static esp_app_desc_t d = { .version = SIM_VERSION, .project_name = "SkyFrame",
                                .time = __TIME__, .date = __DATE__, .idf_ver = "simulator" };
    return &d;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    memset(ap, 0, sizeof(*ap));
    strcpy((char *)ap->ssid, "Simulator");
    ap->rssi = -58; ap->primary = 6;
    return ESP_OK;
}

/* ---- NVS: in memory ---------------------------------------------------------- */
typedef struct { char key[32]; void *data; size_t len; } nvs_item_t;
static nvs_item_t nvs_items[64];
static pthread_mutex_t nvs_lock = PTHREAD_MUTEX_INITIALIZER;
static char nvs_ns[8][16];
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) {
    (void)mode;
    for (int i = 0; i < 8; i++) {
        if (!nvs_ns[i][0]) snprintf(nvs_ns[i], sizeof(nvs_ns[i]), "%s", ns);
        if (!strcmp(nvs_ns[i], ns)) { *h = (nvs_handle_t)i + 1; return ESP_OK; }
    }
    return ESP_FAIL;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static nvs_item_t *nvs_find(nvs_handle_t h, const char *key, bool create) {
    char k[32]; snprintf(k, sizeof(k), "%u/%s", (unsigned)h, key);
    nvs_item_t *free_slot = NULL;
    for (int i = 0; i < 64; i++) {
        if (!strcmp(nvs_items[i].key, k)) return &nvs_items[i];
        if (!nvs_items[i].key[0] && !free_slot) free_slot = &nvs_items[i];
    }
    if (create && free_slot) snprintf(free_slot->key, sizeof(free_slot->key), "%s", k);
    return create ? free_slot : NULL;
}
static esp_err_t nvs_put(nvs_handle_t h, const char *key, const void *v, size_t len) {
    pthread_mutex_lock(&nvs_lock);
    nvs_item_t *it = nvs_find(h, key, true);
    if (it) { free(it->data); it->data = malloc(len ? len : 1); memcpy(it->data, v, len); it->len = len; }
    pthread_mutex_unlock(&nvs_lock);
    return it ? ESP_OK : ESP_ERR_NO_MEM;
}
static esp_err_t nvs_take(nvs_handle_t h, const char *key, void *out, size_t *len, bool exact) {
    pthread_mutex_lock(&nvs_lock);
    nvs_item_t *it = nvs_find(h, key, false);
    esp_err_t e = ESP_ERR_NVS_NOT_FOUND;
    if (it) {
        if (!out) { *len = it->len; e = ESP_OK; }
        else if ((exact && *len != it->len) || *len < it->len) e = ESP_ERR_INVALID_SIZE;
        else { memcpy(out, it->data, it->len); *len = it->len; e = ESP_OK; }
    }
    pthread_mutex_unlock(&nvs_lock);
    return e;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) { return nvs_take(h, key, out, len, false); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len) { return nvs_put(h, key, v, len); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) { return nvs_take(h, key, out, len, false); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { return nvs_put(h, key, v, strlen(v) + 1); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    pthread_mutex_lock(&nvs_lock);
    nvs_item_t *it = nvs_find(h, key, false);
    if (it) { free(it->data); memset(it, 0, sizeof(*it)); }
    pthread_mutex_unlock(&nvs_lock);
    return it ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
#define SIM_NVS_INT(T, N) \
    esp_err_t nvs_get_##N(nvs_handle_t h, const char *key, T *out) { size_t n = sizeof(T); return nvs_take(h, key, out, &n, true); } \
    esp_err_t nvs_set_##N(nvs_handle_t h, const char *key, T v) { return nvs_put(h, key, &v, sizeof(T)); }
SIM_NVS_INT(uint8_t, u8) SIM_NVS_INT(int8_t, i8) SIM_NVS_INT(uint16_t, u16) SIM_NVS_INT(int16_t, i16)
SIM_NVS_INT(uint32_t, u32) SIM_NVS_INT(int32_t, i32) SIM_NVS_INT(uint64_t, u64) SIM_NVS_INT(int64_t, i64)

/* ---- FreeRTOS on threads ----------------------------------------------------- */
struct sim_task {
    pthread_t thread;
    TaskFunction_t fn; void *arg;
    char name[16];
    pthread_mutex_t lock; pthread_cond_t cond;
    uint32_t value; bool pending;
};
static __thread struct sim_task *current_task;
static pthread_t main_thread;
static void sim_pump(void);
static struct sim_task *task_new(const char *name) {
    struct sim_task *t = calloc(1, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "task");
    pthread_mutex_init(&t->lock, NULL); pthread_cond_init(&t->cond, NULL);
    return t;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    if (!current_task) current_task = task_new("main");
    return current_task;
}
static void *task_entry(void *p) {
    current_task = p;
    current_task->fn(current_task->arg);
    return NULL;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core) {
    (void)stack; (void)prio; (void)core;
    struct sim_task *t = task_new(name);
    t->fn = fn; t->arg = arg;
    if (out) *out = t;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 20);
    int r = pthread_create(&t->thread, &at, task_entry, t);
    pthread_attr_destroy(&at);
    if (!r) pthread_detach(t->thread);
    return r ? pdFAIL : pdPASS;
}
void vTaskDelete(TaskHandle_t t) { if (!t || t == current_task) pthread_exit(NULL); }
static void sleep_ms(uint32_t ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
void vTaskDelay(TickType_t ticks) {
    if (pthread_equal(pthread_self(), main_thread)) {
        /* app_main's loop: keep the window alive while it waits. */
        int64_t until = esp_timer_get_time() + (int64_t)ticks * 1000;
        do { sim_pump(); if (esp_timer_get_time() < until) sleep_ms(1); } while (esp_timer_get_time() < until);
    } else sleep_ms(ticks);
}
static void deadline(struct timespec *ts, TickType_t wait) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += wait / 1000; ts->tv_nsec += (long)(wait % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}
/* Wait (with t->lock held) until pending; false on timeout. */
static bool wait_pending(struct sim_task *t, TickType_t wait) {
    struct timespec ts; deadline(&ts, wait);
    while (!t->pending) {
        if (wait == portMAX_DELAY) pthread_cond_wait(&t->cond, &t->lock);
        else if (pthread_cond_timedwait(&t->cond, &t->lock, &ts)) return t->pending;
    }
    return true;
}
BaseType_t xTaskNotify(TaskHandle_t t, uint32_t v, eNotifyAction action) {
    if (!t) return pdFAIL;
    pthread_mutex_lock(&t->lock);
    switch (action) {
    case eSetBits: t->value |= v; break;
    case eIncrement: t->value++; break;
    case eSetValueWithOverwrite: case eSetValueWithoutOverwrite: t->value = v; break;
    default: break;
    }
    t->pending = true;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t t) { return xTaskNotify(t, 0, eIncrement); }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    struct sim_task *t = xTaskGetCurrentTaskHandle();
    pthread_mutex_lock(&t->lock);
    if (!t->value) { t->pending = false; wait_pending(t, wait); }
    uint32_t v = t->value;
    if (v) t->value = clear ? 0 : v - 1;
    t->pending = t->value != 0;
    pthread_mutex_unlock(&t->lock);
    return v;
}
BaseType_t xTaskNotifyWait(uint32_t clear_entry, uint32_t clear_exit, uint32_t *value, TickType_t wait) {
    struct sim_task *t = xTaskGetCurrentTaskHandle();
    pthread_mutex_lock(&t->lock);
    if (!t->pending) t->value &= ~clear_entry;
    bool got = wait_pending(t, wait);
    if (got) { if (value) *value = t->value; t->value &= ~clear_exit; t->pending = false; }
    pthread_mutex_unlock(&t->lock);
    return got ? pdTRUE : pdFALSE;
}
/* CPU load for the test menu: pretend the idle tasks ran 60% of the time. */
void vTaskGetInfo(TaskHandle_t t, TaskStatus_t *s, BaseType_t stack, eTaskState state) {
    (void)t; (void)stack; (void)state;
    s->ulRunTimeCounter = (uint32_t)(esp_timer_get_time() * 6 / 10);
}
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core) { (void)core; return NULL; }

struct sim_queue { pthread_mutex_t lock; pthread_cond_t cond; uint8_t *buf; unsigned len, item, head, count; };
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item) {
    struct sim_queue *q = calloc(1, sizeof(*q));
    q->buf = malloc((size_t)len * item); q->len = len; q->item = item;
    pthread_mutex_init(&q->lock, NULL); pthread_cond_init(&q->cond, NULL);
    return q;
}
static BaseType_t queue_wait(struct sim_queue *q, bool for_space, TickType_t wait) {
    struct timespec ts; deadline(&ts, wait);
    while (for_space ? q->count == q->len : q->count == 0) {
        if (!wait) return pdFAIL;
        if (wait == portMAX_DELAY) pthread_cond_wait(&q->cond, &q->lock);
        else if (pthread_cond_timedwait(&q->cond, &q->lock, &ts)) return (for_space ? q->count < q->len : q->count > 0) ? pdPASS : pdFAIL;
    }
    return pdPASS;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    pthread_mutex_lock(&q->lock);
    BaseType_t ok = queue_wait(q, true, wait);
    if (ok) {
        memcpy(q->buf + ((q->head + q->count) % q->len) * q->item, item, q->item);
        q->count++; pthread_cond_broadcast(&q->cond);
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item) {
    pthread_mutex_lock(&q->lock);
    if (q->count == q->len) { q->head = (q->head + 1) % q->len; q->count--; }
    memcpy(q->buf + ((q->head + q->count) % q->len) * q->item, item, q->item);
    q->count++; pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait) {
    pthread_mutex_lock(&q->lock);
    BaseType_t ok = queue_wait(q, false, wait);
    if (ok) {
        memcpy(out, q->buf + q->head * q->item, q->item);
        q->head = (q->head + 1) % q->len; q->count--; pthread_cond_broadcast(&q->cond);
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    pthread_mutex_lock(&q->lock); unsigned n = q->count; pthread_mutex_unlock(&q->lock); return n;
}
struct sim_mutex { pthread_mutex_t m; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct sim_mutex *s = calloc(1, sizeof(*s)); pthread_mutex_init(&s->m, NULL); return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    if (wait == portMAX_DELAY) return pthread_mutex_lock(&s->m) ? pdFAIL : pdPASS;
    struct timespec ts; deadline(&ts, wait);
    return pthread_mutex_timedlock(&s->m, &ts) ? pdFAIL : pdPASS;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { pthread_mutex_unlock(&s->m); return pdPASS; }
static pthread_mutex_t critical;
void sim_critical_enter(void) { pthread_mutex_lock(&critical); }
void sim_critical_exit(void) { pthread_mutex_unlock(&critical); }

/* ---- HTTP client on libcurl -------------------------------------------------- */
struct sim_http { esp_http_client_config_t cfg; char *url; char *agent; CURL *curl; long status; bool headers_sent; };
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    struct sim_http *c = calloc(1, sizeof(*c));
    c->cfg = *cfg;
    c->url = strdup(cfg->url ? cfg->url : "");
    c->agent = strdup(cfg->user_agent ? cfg->user_agent : "SkyFrame simulator");
    c->curl = curl_easy_init();
    return c;
}
static size_t http_write(char *data, size_t size, size_t n, void *p) {
    struct sim_http *c = p;
    size_t bytes = size * n;
    curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &c->status);
    if (c->status >= 300 && c->status < 400) return bytes;   /* a redirect's own body */
    if (!c->cfg.event_handler) return bytes;
    esp_http_client_event_t e = { .client = c, .user_data = c->cfg.user_data };
    if (!c->headers_sent) {
        c->headers_sent = true;
        e.event_id = HTTP_EVENT_ON_HEADERS_COMPLETE;
        c->cfg.event_handler(&e);
    }
    e.event_id = HTTP_EVENT_ON_DATA; e.data = data; e.data_len = (int)bytes;
    return c->cfg.event_handler(&e) == ESP_OK ? bytes : 0;
}
/* The device's client writes "GET <path> HTTP/1.1" into a buffer_size_tx buffer (512 bytes
   unless set) and fails when it doesn't fit: GitHub's signed redirect addresses are ~900. */
static bool request_line_fits(const struct sim_http *c, const char *url) {
    const char *p = strstr(url, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    size_t line = 4 + (p ? strlen(p) : 1) + 9, cap = c->cfg.buffer_size_tx > 0 ? (size_t)c->cfg.buffer_size_tx : 512;
    if (line < cap) return true;
    sim_log('W', TAG, "HTTP request line of %u bytes doesn't fit buffer_size_tx %u (the device fails here)",
            (unsigned)line, (unsigned)cap);
    return false;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    if (!c || !c->curl) return ESP_FAIL;
    curl_easy_setopt(c->curl, CURLOPT_USERAGENT, c->agent);
    curl_easy_setopt(c->curl, CURLOPT_FOLLOWLOCATION, 0L);   /* followed below, like the device */
    curl_easy_setopt(c->curl, CURLOPT_TIMEOUT_MS, (long)(c->cfg.timeout_ms ? c->cfg.timeout_ms : 12000));
    curl_easy_setopt(c->curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c->curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, c);
    CURLcode r = CURLE_OK;
    char *url = strdup(c->url);
    for (int hops = 0; hops <= 5; hops++) {
        if (!request_line_fits(c, url)) { free(url); return ESP_FAIL; }
        curl_easy_setopt(c->curl, CURLOPT_URL, url);
        r = curl_easy_perform(c->curl);
        curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &c->status);
        char *next = NULL;
        if (r != CURLE_OK || c->status < 300 || c->status >= 400 ||
            curl_easy_getinfo(c->curl, CURLINFO_REDIRECT_URL, &next) != CURLE_OK || !next) break;
        free(url);
        url = strdup(next);
    }
    free(url);
    if (r != CURLE_OK) sim_log('W', TAG, "HTTP %s: %s", c->url, curl_easy_strerror(r));
    return r == CURLE_OK ? ESP_OK : ESP_FAIL;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c ? (int)c->status : 0; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    if (!c) return ESP_OK;
    curl_easy_cleanup(c->curl); free(c->url); free(c->agent); free(c);
    return ESP_OK;
}

/* ---- The display: an SDL window ---------------------------------------------- */
#define PANEL 480
#define BEZEL 24                      /* dark frame around the round glass */
#define WIN (PANEL + 2 * BEZEL)
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screen_tex, *mask_tex;
static uint16_t *framebuffer;
static bool frame_dirty, mouse_down;
static int mouse_x, mouse_y, backlight = 80;
static uint32_t last_tick_ms;
sdmmc_card_t *SD_Card;
static sdmmc_card_t sim_card;
void I2C_Init(void) {}
void EXIO_Init(void) {}
void LCD_Init(void) {}
void Touch_Init(void) {}
void SD_Init(void) { SD_Card = &sim_card; }
void Set_Backlight(uint8_t light) { backlight = light > 100 ? 100 : light; frame_dirty = true; }
uint32_t LCD_Vsync_Count(void) { return (uint32_t)(esp_timer_get_time() / 16667); }   /* 60 Hz panel */
const void *LCD_Frame_Buffer(void) { return framebuffer; }

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(framebuffer + y * PANEL + a->x1, px + (y - a->y1) * w, (size_t)w * 2);
    frame_dirty = true;
    lv_disp_flush_ready(drv);
}
static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *d) {
    (void)drv;
    d->point.x = (lv_coord_t)(mouse_x - BEZEL);
    d->point.y = (lv_coord_t)(mouse_y - BEZEL);
    d->state = mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
/* Everything outside the round glass is black, with a soft edge. */
static void make_mask(void) {
    mask_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, PANEL, PANEL);
    uint32_t *m = malloc(PANEL * PANEL * 4);
    for (int y = 0; y < PANEL; y++) for (int x = 0; x < PANEL; x++) {
        float d = hypotf(x + 0.5f - PANEL / 2.f, y + 0.5f - PANEL / 2.f) - (PANEL / 2.f - 0.5f);
        float a = d <= 0 ? 0 : d >= 1 ? 1 : d;
        m[y * PANEL + x] = (uint32_t)(a * 255) << 24 | 0x000000;
    }
    SDL_UpdateTexture(mask_tex, NULL, m, PANEL * 4);
    SDL_SetTextureBlendMode(mask_tex, SDL_BLENDMODE_BLEND);
    free(m);
}
void LVGL_Init(void) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); exit(1); }
    window = SDL_CreateWindow("SkyFrame simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN, WIN, 0);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    screen_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, PANEL, PANEL);
    make_mask();
    framebuffer = calloc(PANEL * PANEL, 2);

    lv_init();
    static lv_disp_draw_buf_t buf;
    lv_color_t *b1 = malloc(PANEL * PANEL * sizeof(lv_color_t)), *b2 = malloc(PANEL * PANEL * sizeof(lv_color_t));
    lv_disp_draw_buf_init(&buf, b1, b2, PANEL * PANEL);
    static lv_disp_drv_t disp;
    lv_disp_drv_init(&disp);
    disp.hor_res = PANEL; disp.ver_res = PANEL;
    disp.flush_cb = flush; disp.draw_buf = &buf;
    lv_disp_drv_register(&disp);
    static lv_indev_drv_t touch;
    lv_indev_drv_init(&touch);
    touch.type = LV_INDEV_TYPE_POINTER; touch.read_cb = touch_read;
    lv_indev_drv_register(&touch);
    last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000);
}
static void present(void) {
    SDL_SetRenderDrawColor(renderer, 18, 20, 24, 255);
    SDL_RenderClear(renderer);
    SDL_UpdateTexture(screen_tex, NULL, framebuffer, PANEL * 2);
    SDL_Rect r = { BEZEL, BEZEL, PANEL, PANEL };
    SDL_RenderCopy(renderer, screen_tex, NULL, &r);
    /* Backlight: the panel shows through less as it dims (never fully black above 0). */
    int shade = backlight <= 0 ? 255 : (int)(255 * (1.f - (0.18f + 0.82f * backlight / 100.f)));
    if (shade > 0) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (uint8_t)shade);
        SDL_RenderFillRect(renderer, &r);
    }
    SDL_RenderCopy(renderer, mask_tex, NULL, &r);
    SDL_RenderPresent(renderer);
}
/* Scripted input and screenshots, for checking changes without clicking:
     SKYFRAME_SIM_SCRIPT="3000:down 240,60;3150:move 240,320;3300:up;5000:click 281,142;9000:shot today"
   Times are ms since start; coordinates are on the 480x480 panel. "shot NAME" saves
   sim/out/shot_NAME.bmp. "quit" closes the simulator. */
static void save_shot(const char *name) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(framebuffer, PANEL, PANEL, 16, PANEL * 2, SDL_PIXELFORMAT_RGB565);
    char path[160]; snprintf(path, sizeof(path), "shot_%s.bmp", name);
    if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); }
    sim_log('I', TAG, "Saved %s", path);
}
static void run_script(void) {
    static const char *next;
    static bool started;
    if (!started) { started = true; next = getenv("SKYFRAME_SIM_SCRIPT"); }
    while (next && *next) {
        unsigned at = 0; int n = 0;
        if (sscanf(next, "%u:%n", &at, &n) != 1) { next = NULL; return; }
        if (esp_timer_get_time() / 1000 < at) return;
        const char *cmd = next + n;
        const char *end = strchr(cmd, ';');
        char step[64]; size_t len = end ? (size_t)(end - cmd) : strlen(cmd);
        if (len >= sizeof(step)) len = sizeof(step) - 1;
        memcpy(step, cmd, len); step[len] = 0;
        next = end ? end + 1 : NULL;
        int x, y; char name[40];
        if (sscanf(step, "down %d,%d", &x, &y) == 2) { mouse_x = x + BEZEL; mouse_y = y + BEZEL; mouse_down = true; }
        else if (sscanf(step, "move %d,%d", &x, &y) == 2) { mouse_x = x + BEZEL; mouse_y = y + BEZEL; }
        else if (!strcmp(step, "up")) mouse_down = false;
        else if (sscanf(step, "click %d,%d", &x, &y) == 2) {
            mouse_x = x + BEZEL; mouse_y = y + BEZEL; mouse_down = true;
            for (int i = 0; i < 6; i++) { lv_timer_handler(); sleep_ms(10); lv_tick_inc(10); }
            mouse_down = false;
        }
        else if (sscanf(step, "shot %39s", name) == 1) save_shot(name);
        else if (!strcmp(step, "quit")) exit(0);
    }
}
static void sim_pump(void) {
    run_script();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: exit(0);
        case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) mouse_down = true; mouse_x = e.button.x; mouse_y = e.button.y; break;
        case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) mouse_down = false; mouse_x = e.button.x; mouse_y = e.button.y; break;
        case SDL_MOUSEMOTION: mouse_x = e.motion.x; mouse_y = e.motion.y; break;
        case SDL_WINDOWEVENT: frame_dirty = true; break;
        default: break;
        }
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now != last_tick_ms) { lv_tick_inc(now - last_tick_ms); last_tick_ms = now; }
    if (frame_dirty) { frame_dirty = false; present(); }
}

/* ---- main ---------------------------------------------------------------------- */
double sim_latitude = 0, sim_longitude = 0;   /* from the command line; 0 = SkyFrame's default */
int main(int argc, char **argv) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    srand((unsigned)time(NULL));
    main_thread = pthread_self();
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma); pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&critical, &ma);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc >= 3) { sim_latitude = atof(argv[1]); sim_longitude = atof(argv[2]); }
    printf("SkyFrame simulator. Mouse = finger. Close the window to quit.\n");
    app_main();
    return 0;
}
