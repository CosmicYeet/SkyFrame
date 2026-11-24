/* Over-the-air updates on the device: see skyframe_update.h. */
#include "skyframe_update.h"
#include "skyframe_net.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "skyframe_update";
#define NVS_NS "skyframe"
#define KEY_BAD "upd_bad"          /* a version that didn't prove itself: never offered again */
#define KEY_NEWS_VER "upd_news_v"  /* installed version whose notes are still to be shown */
#define KEY_NEWS "upd_news"
#define KEY_TRY_V "upd_try_v"      /* a version that went back once, and how often (KEY_TRY_N) */
#define KEY_TRY_N "upd_try_n"
#define STRIKES 2                  /* went back this often: never offered again */
#define INSTALL_LIMIT_US (10 * 60 * 1000000LL)   /* a download may take this long in all */

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static sf_update_status_t status = {.state = SF_UPDATE_IDLE};
static TaskHandle_t worker_task;
static volatile bool want_install;
static bool pending;               /* running a new version that hasn't proven itself */
static volatile bool reached;      /* this boot read the release information from GitHub */
static char bad_version[16];
static void (*restart_cb)(void);

static void set_status(sf_update_state_t state, int percent, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_status(sf_update_state_t state, int percent, const char *fmt, ...)
{
    char text[64];
    va_list a;
    va_start(a, fmt);
    vsnprintf(text, sizeof(text), fmt, a);
    va_end(a);
    taskENTER_CRITICAL(&lock);
    status.state = state;
    status.percent = percent;
    memcpy(status.text, text, sizeof(status.text));
    taskEXIT_CRITICAL(&lock);
    if (state != SF_UPDATE_DOWNLOADING && state != SF_UPDATE_CHECKING) ESP_LOGI(TAG, "%s", text);
}
void sf_update_status(sf_update_status_t *out)
{
    taskENTER_CRITICAL(&lock);
    *out = status;
    taskEXIT_CRITICAL(&lock);
}
const char *sf_update_version(void) { return esp_app_get_description()->version; }

static void nvs_put(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (value) nvs_set_str(h, key, value); else nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}
static bool nvs_take_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    bool ok = nvs_get_str(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    if (!ok) out[0] = 0;
    return ok;
}

/* ---- Checking and installing (worker task, CPU1) ---- */
/* An update is installed and the restart into it is on its way (from here or the web page):
   installing again would erase it. */
bool sf_update_restart_pending(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition(), *run = esp_ota_get_running_partition();
    return boot && run && boot != run;
}
static void install(const sf_update_manifest_t *m)
{
    if (sf_update_restart_pending()) { set_status(SF_UPDATE_INSTALLED, 100, "Installed; restarting"); return; }
    /* One TLS session at a time (Wi-Fi + LCD leave little internal RAM): hold the network
       lock for the whole download; traffic polls wait (or skip one update). */
    if (!sf_net_lock(60000)) { set_status(SF_UPDATE_FAILED, 0, "Busy; will try again later"); return; }
    set_status(SF_UPDATE_DOWNLOADING, 0, "Installing v%s...", m->version);
    /* GitHub answers with a redirect to a long signed address: big header buffers. */
    esp_http_client_config_t http = {
        .url = m->url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
        .buffer_size = 4096, .buffer_size_tx = 2048, .keep_alive_enable = true,
        .user_agent = "SkyFrame-updater",
    };
    esp_https_ota_config_t cfg = {.http_config = &http, .buffer_caps = MALLOC_CAP_SPIRAM};
    esp_https_ota_handle_t h = NULL;
    esp_err_t e = esp_https_ota_begin(&cfg, &h);
    if (e == ESP_OK) {
        esp_app_desc_t d;
        e = esp_https_ota_get_img_desc(h, &d);
        if (e == ESP_OK && (strcmp(d.project_name, "SkyFrame") || strcmp(d.version, m->version))) {
            ESP_LOGW(TAG, "Release is \"%s\" %s, expected SkyFrame %s", d.project_name, d.version, m->version);
            e = ESP_ERR_INVALID_VERSION;
        }
    }
    int size = h ? esp_https_ota_get_image_size(h) : 0, last = -1;
    const int64_t give_up = esp_timer_get_time() + INSTALL_LIMIT_US;   /* the network lock is held */
    while (e == ESP_OK) {
        if (esp_timer_get_time() > give_up) { e = ESP_ERR_TIMEOUT; break; }
        e = esp_https_ota_perform(h);
        if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        e = ESP_OK;
        int pct = size > 0 ? (int)((int64_t)esp_https_ota_get_image_len_read(h) * 100 / size) : 0;
        if (pct != last) { last = pct; set_status(SF_UPDATE_DOWNLOADING, pct, "Installing v%s... %d%%", m->version, pct); }
    }
    if (e == ESP_OK && !esp_https_ota_is_complete_data_received(h)) e = ESP_ERR_INVALID_SIZE;
    /* finish: checks the image and its signature, then makes it the one to boot */
    if (e == ESP_OK) e = esp_https_ota_finish(h);
    else if (h) esp_https_ota_abort(h);
    sf_net_unlock();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Update to %s failed: %s", m->version, esp_err_to_name(e));
        set_status(SF_UPDATE_FAILED, 0, "Update failed (%s)", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "Installed %s; restarting", m->version);
    sf_update_record_installed(m->version, m->notes);
    set_status(SF_UPDATE_INSTALLED, 100, "v%s installed; restarting", m->version);
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (restart_cb) restart_cb(); else esp_restart();
}
static void check(bool install_newer)
{
    set_status(SF_UPDATE_CHECKING, 0, "Checking for updates...");
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t e = sf_net_get_bytes(SF_UPDATE_MANIFEST_URL, SF_UPDATE_MANIFEST_MAX, &data, &len);
    if (e != ESP_OK) { set_status(SF_UPDATE_FAILED, 0, "Couldn't check (%s)", esp_err_to_name(e)); return; }
    sf_update_manifest_t m;
    bool ok = sf_update_parse_manifest((const char *)data, len, &m);
    free(data);
    if (!ok) { set_status(SF_UPDATE_FAILED, 0, "Release information unreadable"); return; }
    reached = true;   /* HTTPS, GitHub and the manifest work: this version can update itself */
    if (sf_version_compare(m.version, sf_update_version()) <= 0) {
        set_status(SF_UPDATE_CURRENT, 0, "Up to date (v%s)", sf_update_version());
        return;
    }
    if (!strcmp(m.version, bad_version)) {
        set_status(SF_UPDATE_FAILED, 0, "v%s skipped: it didn't start before", m.version);
        return;
    }
    if (pending) { set_status(SF_UPDATE_AVAILABLE, 0, "v%s available (after this one settles)", m.version); return; }
    if (!install_newer) { set_status(SF_UPDATE_AVAILABLE, 0, "v%s available", m.version); return; }
    install(&m);
}
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        check(want_install);
    }
}
void sf_update_check_now(bool install_newer)
{
    if (!worker_task) return;
    sf_update_status_t s;
    sf_update_status(&s);
    if (s.state == SF_UPDATE_CHECKING || s.state == SF_UPDATE_DOWNLOADING || s.state == SF_UPDATE_INSTALLED) return;
    want_install = install_newer;
    xTaskNotifyGive(worker_task);
}

/* ---- Proving a new version ---- */
bool sf_update_pending_verify(void) { return pending; }
bool sf_update_server_reached(void) { return reached; }
void sf_update_confirm(void)
{
    if (!pending) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        pending = false;
        ESP_LOGI(TAG, "Version %s confirmed", sf_update_version());
    }
}
void sf_update_reject(void)
{
    if (!pending) return;
    ESP_LOGW(TAG, "Version %s couldn't reach the updates: going back to the previous version", sf_update_version());
    esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();   /* returns only on failure */
    ESP_LOGE(TAG, "Going back failed (%s): keeping %s", esp_err_to_name(e), sf_update_version());
    pending = false;   /* don't retry every second; the bootloader still rolls back on a crash */
}

/* ---- What's new ---- */
void sf_update_record_installed(const char *version, const char *notes)
{
    nvs_put(KEY_NEWS_VER, version);
    nvs_put(KEY_NEWS, notes && notes[0] ? notes : "");
}
bool sf_update_take_news(char *version, size_t vcap, char *notes, size_t ncap)
{
    char v[16];
    if (!nvs_take_str(KEY_NEWS_VER, v, sizeof(v))) return false;
    bool ours = !strcmp(v, sf_update_version());   /* not ours: that update rolled back */
    if (ours) {
        snprintf(version, vcap, "%s", v);
        nvs_take_str(KEY_NEWS, notes, ncap);
    }
    nvs_put(KEY_NEWS_VER, NULL);
    nvs_put(KEY_NEWS, NULL);
    return ours;
}

/* ---- Crash reports ---- */
bool sf_update_crash_summary(char *out, size_t cap)
{
    out[0] = 0;
    if (esp_core_dump_image_check() != ESP_OK) return false;
    esp_core_dump_summary_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    bool ok = s && esp_core_dump_get_summary(s) == ESP_OK;
    if (ok) snprintf(out, cap, "%.15s at 0x%08lx", s->exc_task, (unsigned long)s->exc_pc);
    free(s);
    return ok;
}

void sf_update_init(void (*restart)(void))
{
    restart_cb = restart;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (pending) ESP_LOGI(TAG, "Running new version %s: waiting for it to prove itself", sf_update_version());
    nvs_take_str(KEY_BAD, bad_version, sizeof(bad_version));
    /* A version that went back before proving itself (crash, freeze, no updates, power cut):
       try it once more on another night; the second time, never again. The "installed" note
       (KEY_NEWS_VER) marks the first boot after it went back, so each counts once. */
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    char installed[16];
    if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK && strcmp(d.version, sf_update_version())) {
        if (nvs_take_str(KEY_NEWS_VER, installed, sizeof(installed)) && !strcmp(installed, d.version)) {
            char tried[16];
            uint8_t n = 0;
            nvs_handle_t h;
            if (nvs_take_str(KEY_TRY_V, tried, sizeof(tried)) && !strcmp(tried, d.version) &&
                nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
                nvs_get_u8(h, KEY_TRY_N, &n);
                nvs_close(h);
            }
            n++;
            ESP_LOGW(TAG, "Version %s went back (%u of %d); kept %s", d.version, n, STRIKES, sf_update_version());
            nvs_put(KEY_TRY_V, d.version);
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, KEY_TRY_N, n); nvs_commit(h); nvs_close(h); }
            if (n >= STRIKES) { snprintf(bad_version, sizeof(bad_version), "%.15s", d.version); nvs_put(KEY_BAD, bad_version); }
            nvs_put(KEY_NEWS_VER, NULL);
            nvs_put(KEY_NEWS, NULL);
        }
        if (!strcmp(d.version, bad_version)) set_status(SF_UPDATE_FAILED, 0, "v%s didn't start; kept v%s", d.version, sf_update_version());
        else set_status(SF_UPDATE_FAILED, 0, "v%s went back; will try again", d.version);
    }
    char crash[48];
    if (sf_update_crash_summary(crash, sizeof(crash))) ESP_LOGW(TAG, "Last crash saved: %s", crash);
    if (!worker_task) xTaskCreatePinnedToCore(worker, "sf_update", 8192, NULL, 3, &worker_task, 1);
}
