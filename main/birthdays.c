#include "birthdays.h"
#include "skyframe_sd.h"
#include "skyframe_jpeg.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BIRTHDAY_FILE   SF_SD_ROOT "/birthdays.txt"
#define BIRTHDAY_PHOTOS SF_SD_ROOT "/birthdays"
#define FILE_MAX_BYTES  8192
#define ENTRIES_MAX     64

static const char *TAG = "birthdays";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int today_ymd;            /* yyyymmdd, 0 = unknown */
static volatile int preview_request;      /* 0 none, 1 owner, 2+n the n-th upcoming family birthday */
static volatile bool rebuild;
static birthday_list_t ready;             /* handed to the UI by birthdays_take() */
static bool ready_pending;
static TaskHandle_t task;

void birthdays_set_today(int year, int month, int day)
{
    int v = year * 10000 + month * 100 + day;
    if (v != today_ymd) {
        today_ymd = v;
        if (task) xTaskNotifyGive(task);
    }
}
void birthdays_preview_family(int index)
{
    taskENTER_CRITICAL(&lock);
    preview_request = 2 + (index < 0 ? 0 : index);
    taskEXIT_CRITICAL(&lock);
    if (task) xTaskNotifyGive(task);
}
void birthdays_preview(bool owner)
{
    taskENTER_CRITICAL(&lock);
    preview_request = owner ? 1 : 2;
    taskEXIT_CRITICAL(&lock);
    if (task) xTaskNotifyGive(task);
}
void birthdays_refresh(void)
{
    rebuild = true;
    if (task) xTaskNotifyGive(task);
}

/* NVS key per person: "bd" + FNV-1a hash of name and date. Value: year dismissed. */
static void dismiss_key(const sf_birthday_t *b, char key[16])
{
    uint32_t h = 2166136261u;
    for (const char *p = b->name; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= (uint32_t)(b->month * 32 + b->day); h *= 16777619u;
    snprintf(key, 16, "bd%08lx", (unsigned long)h);
}
static bool dismissed(const sf_birthday_t *b, int year)
{
    nvs_handle_t h;
    int32_t v = 0;
    char key[16];
    dismiss_key(b, key);
    if (nvs_open("skyframe", NVS_READONLY, &h) != ESP_OK) return false;
    bool yes = nvs_get_i32(h, key, &v) == ESP_OK && v == year;
    nvs_close(h);
    return yes;
}
void birthdays_dismiss(const sf_birthday_t *who, int year)
{
    nvs_handle_t h;
    char key[16];
    if (!who) return;
    dismiss_key(who, key);
    if (nvs_open("skyframe", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, year);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Reminder for %s dismissed until next year", who->name);
}

static uint16_t *load_photo(const char *file)
{
    char path[96];
    if (!file[0]) return NULL;
    snprintf(path, sizeof(path), BIRTHDAY_PHOTOS "/%s", file);
    uint8_t *jpeg = NULL;
    size_t len = 0;
    if (!sf_sd_begin()) return NULL;
    esp_err_t e = sf_jpeg_read_file(path, &jpeg, &len);
    sf_sd_end();
    if (e != ESP_OK) { ESP_LOGW(TAG, "Photo %s not found or unreadable", path); return NULL; }
    uint16_t *px = NULL;
    if (sf_jpeg_decode_fit(jpeg, len, &px, BIRTHDAY_PHOTO_SIZE, BIRTHDAY_PHOTO_SIZE, path) != ESP_OK) px = NULL;
    free(jpeg);
    return px;
}

/* Everyone in the file, the owner (the first line marked "owner") first. Also picks up
   the optional "message: ..." line for the owner's screen. */
static int load_entries(sf_birthday_t *all, int cap, char *message, size_t message_cap)
{
    int n = 0;
    bool have_owner = false;
    char *text = NULL;
    snprintf(message, message_cap, "%s", DEFAULT_OWNER_MESSAGE);
    if (sf_sd_begin()) {                  /* card held only for the read */
        FILE *f = fopen(BIRTHDAY_FILE, "r");
        if (f) {
            text = heap_caps_calloc(1, FILE_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);   /* keep internal RAM for Wi-Fi */
            if (text && fread(text, 1, FILE_MAX_BYTES, f) == FILE_MAX_BYTES) {
                /* Longer than the limit: drop the cut-off last line rather than misread it. */
                char *end = strrchr(text, '\n');
                if (end) end[1] = 0; else text[0] = 0;
            }
            fclose(f);
        }
        sf_sd_end();
    }
    char *save = NULL;
    for (char *line = text ? strtok_r(text, "\r\n", &save) : NULL; line && n < cap; line = strtok_r(NULL, "\r\n", &save)) {
        if (sf_birthday_parse_message(line, message, message_cap)) continue;
        sf_birthday_t b;
        if (!sf_birthday_parse_line(line, &b)) continue;
        if (b.owner && have_owner) b.owner = false;      /* only one owner */
        if (b.owner) {                                   /* owner goes first */
            have_owner = true;
            memmove(&all[1], &all[0], sizeof(all[0]) * (size_t)n);
            all[0] = b;
        } else all[n] = b;
        n++;
    }
    free(text);
    return n;
}
static void add(birthday_list_t *out, const sf_birthday_t *b, int year)
{
    if (out->count >= BIRTHDAYS_MAX) return;
    birthday_today_t *t = &out->list[out->count++];
    t->who = *b;
    t->age = sf_birthday_age(b, year);
    t->pixels = load_photo(b->photo);
}

/* Everyone in the file, shared by both builders (they run on the same task). In PSRAM:
   64 entries are about 6 KB, too much to keep in internal RAM for an occasional rebuild. */
static sf_birthday_t *entries(void)
{
    static sf_birthday_t *all;
    if (!all) all = heap_caps_calloc(ENTRIES_MAX, sizeof(*all), MALLOC_CAP_SPIRAM);
    return all;
}

/* Today's birthdays that haven't been dismissed this year. */
static void build_today(int ymd, birthday_list_t *out)
{
    int y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    sf_birthday_t *all = entries();
    memset(out, 0, sizeof(*out));
    int n = all ? load_entries(all, ENTRIES_MAX, out->message, sizeof(out->message)) : 0;
    for (int i = 0; i < n; i++)
        if (sf_birthday_is_today(&all[i], y, m, d) && !dismissed(&all[i], y)) add(out, &all[i], y);
    if (out->count) ESP_LOGI(TAG, "%d birthday(s) today", out->count);
}

/* Test mode: the owner's birthday, or the skip-th upcoming family birthday (counting from
   today, in date order, wrapping around), as it will look. */
static void build_preview(int ymd, bool owner, int skip, birthday_list_t *out)
{
    int y = ymd / 10000, today = ymd % 10000;   /* today as MMDD */
    sf_birthday_t *all = entries();
    memset(out, 0, sizeof(*out));
    int n = all ? load_entries(all, ENTRIES_MAX, out->message, sizeof(out->message)) : 0;
    out->preview = true;
    int pick = -1;
    if (owner) pick = (n && all[0].owner) ? 0 : -1;
    else {
        int family = 0;
        for (int i = 0; i < n; i++) if (!all[i].owner) family++;
        /* The (skip mod family)-th soonest: rank each by days until it, file order on ties. */
        int want = family ? skip % family : 0;
        for (int i = 0; i < n && pick < 0; i++) {
            if (all[i].owner) continue;
            int mmdd = all[i].month * 100 + all[i].day;
            int until = mmdd >= today ? mmdd - today : mmdd + 1300 - today;
            int rank = 0;
            for (int j = 0; j < n; j++) {
                if (all[j].owner || j == i) continue;
                int mj = all[j].month * 100 + all[j].day;
                int uj = mj >= today ? mj - today : mj + 1300 - today;
                if (uj < until || (uj == until && j < i)) rank++;
            }
            if (rank == want) pick = i;
        }
    }
    if (pick < 0) {                             /* nobody suitable in the file: a sample */
        sf_birthday_t sample = { .name = "Sample", .year = y - 30, .month = 1, .day = 1, .owner = owner };
        add(out, &sample, y);
        return;
    }
    /* The age they'll turn at that birthday (next year's if it has passed). */
    int year = y;
    if (all[pick].month * 100 + all[pick].day < today) year++;
    add(out, &all[pick], year);
}

static void free_list(birthday_list_t *l)
{
    for (int i = 0; i < l->count; i++) free(l->list[i].pixels);
    memset(l, 0, sizeof(*l));
}

bool birthdays_take(birthday_list_t *out)
{
    bool have = false;
    taskENTER_CRITICAL(&lock);
    if (ready_pending) { *out = ready; memset(&ready, 0, sizeof(ready)); ready_pending = false; have = true; }
    taskEXIT_CRITICAL(&lock);
    return have;
}

static void publish(birthday_list_t *next)
{
    birthday_list_t stale = {0};
    taskENTER_CRITICAL(&lock);
    if (ready_pending) stale = ready;     /* replace a list the UI hasn't picked up */
    ready = *next;
    ready_pending = true;
    taskEXIT_CRITICAL(&lock);
    free_list(&stale);
    memset(next, 0, sizeof(*next));
}

static void worker(void *arg)
{
    (void)arg;
    int built_ymd = 0;
    uint32_t built_gen = UINT32_MAX;
    birthday_list_t *next = calloc(1, sizeof(*next));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        int ymd = today_ymd;
        if (!next) continue;
        taskENTER_CRITICAL(&lock);          /* take the request atomically */
        int preview = preview_request;
        preview_request = 0;
        taskEXIT_CRITICAL(&lock);
        if (preview) {
            build_preview(ymd ? ymd : 20260101, preview == 1, preview >= 2 ? preview - 2 : 0, next);
            publish(next);
            continue;
        }
        uint32_t gen = sf_sd_generation();
        if (!ymd || (ymd == built_ymd && gen == built_gen && !rebuild)) continue;
        rebuild = false;
        build_today(ymd, next);
        built_ymd = ymd;
        built_gen = gen;
        publish(next);
    }
}

void birthdays_start(void)
{
    if (!task) xTaskCreatePinnedToCore(worker, "birthdays", 6144, NULL, 2, &task, 1);
}
