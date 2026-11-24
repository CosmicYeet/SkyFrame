/* SkyFrame simulator: the small part of ESP-IDF and FreeRTOS that SkyFrame's code uses,
   implemented for a PC (sim_platform.c). Every ESP-IDF header name SkyFrame includes is a
   one-line file in this folder that includes this one. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- esp_err.h ------------------------------------------------------------- */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10A
#define ESP_ERR_NOT_FINISHED 0x10C
#define ESP_ERR_NOT_ALLOWED 0x10D
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
const char *esp_err_to_name(esp_err_t code);
#define ESP_ERROR_CHECK(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { \
    fprintf(stderr, "ESP_ERROR_CHECK failed: %s at %s:%d\n", esp_err_to_name(_e), __FILE__, __LINE__); abort(); } } while (0)

/* ---- esp_log.h ------------------------------------------------------------- */
void sim_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) sim_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) sim_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) sim_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGV(tag, ...) do { (void)(tag); } while (0)

/* ---- esp_check.h ----------------------------------------------------------- */
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t _e = (x); if (_e != ESP_OK) { ESP_LOGE(tag, __VA_ARGS__); return _e; } } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) do { if (!(a)) { ESP_LOGE(tag, __VA_ARGS__); return (err); } } while (0)
#define ESP_GOTO_ON_FALSE(a, err, label, tag, ...) do { if (!(a)) { ESP_LOGE(tag, __VA_ARGS__); ret = (err); goto label; } } while (0)
#define ESP_GOTO_ON_ERROR(x, label, tag, ...) do { esp_err_t _e = (x); if (_e != ESP_OK) { ESP_LOGE(tag, __VA_ARGS__); ret = _e; goto label; } } while (0)

/* ---- esp_attr.h / heap ----------------------------------------------------- */
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define RTC_NOINIT_ATTR
#define MALLOC_CAP_DEFAULT (1u << 12)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, uint32_t caps) { (void)caps; return calloc(c, n); }
static inline void *heap_caps_realloc(void *p, size_t n, uint32_t caps) { (void)caps; return realloc(p, n); }
static inline void *heap_caps_malloc_prefer(size_t n, size_t num, ...) { (void)num; return malloc(n); }
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);

/* ---- esp_timer / system ---------------------------------------------------- */
int64_t esp_timer_get_time(void);   /* microseconds since start */
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
               ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO,
               ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void) __attribute__((noreturn));
uint32_t esp_random(void);
static inline uint32_t esp_rom_get_cpu_ticks_per_us(void) { return 240; }
static inline esp_err_t esp_task_wdt_add(void *task) { (void)task; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
typedef struct { char version[32]; char project_name[32]; char time[16]; char date[16]; char idf_ver[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
static inline esp_err_t esp_vfs_fat_info(const char *path, uint64_t *total, uint64_t *free_bytes) {
    (void)path; *total = 31914983424ull; *free_bytes = 21474836480ull; return ESP_OK; }

/* ---- ADC / temperature sensor: no hardware (battery shows "--") ------------ */
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef enum { ADC_UNIT_1, ADC_UNIT_2 } adc_unit_t;
typedef enum { ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3, ADC_CHANNEL_4 } adc_channel_t;
typedef enum { ADC_ATTEN_DB_0, ADC_ATTEN_DB_2_5, ADC_ATTEN_DB_6, ADC_ATTEN_DB_12 } adc_atten_t;
typedef enum { ADC_BITWIDTH_DEFAULT, ADC_BITWIDTH_12 = 12 } adc_bitwidth_t;
typedef struct { adc_unit_t unit_id; int clk_src; int ulp_mode; } adc_oneshot_unit_init_cfg_t;
typedef struct { adc_atten_t atten; adc_bitwidth_t bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { adc_unit_t unit_id; adc_channel_t chan; adc_atten_t atten; adc_bitwidth_t bitwidth; } adc_cali_curve_fitting_config_t;
static inline esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h) { (void)c; *h = NULL; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch, const adc_oneshot_chan_cfg_t *c) { (void)h; (void)ch; (void)c; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *h) { (void)c; *h = NULL; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *raw) { (void)h; (void)ch; *raw = 0; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) { (void)h; (void)raw; *mv = 0; return ESP_ERR_NOT_SUPPORTED; }
typedef void *temperature_sensor_handle_t;
typedef struct { int range_min, range_max; } temperature_sensor_config_t;
#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(min, max) { .range_min = (min), .range_max = (max) }
static inline esp_err_t temperature_sensor_install(const temperature_sensor_config_t *c, temperature_sensor_handle_t *h) { (void)c; *h = (void *)1; return ESP_OK; }
static inline esp_err_t temperature_sensor_enable(temperature_sensor_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t h, float *c) { (void)h; *c = 43.f; return ESP_OK; }

/* ---- NVS: kept in memory for the session ----------------------------------- */
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
#define SIM_NVS_INT(T, N) esp_err_t nvs_get_##N(nvs_handle_t h, const char *key, T *out); \
                          esp_err_t nvs_set_##N(nvs_handle_t h, const char *key, T v);
SIM_NVS_INT(uint8_t, u8) SIM_NVS_INT(int8_t, i8) SIM_NVS_INT(uint16_t, u16) SIM_NVS_INT(int16_t, i16)
SIM_NVS_INT(uint32_t, u32) SIM_NVS_INT(int32_t, i32) SIM_NVS_INT(uint64_t, u64) SIM_NVS_INT(int64_t, i64)
#undef SIM_NVS_INT

/* ---- Wi-Fi: always "connected" with a fair signal -------------------------- */
typedef struct { uint8_t ssid[33]; int8_t rssi; uint8_t primary; } wifi_ap_record_t;
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);

/* ---- FreeRTOS (threads, 1 ms ticks) ---------------------------------------- */
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configTICK_RATE_HZ 1000
typedef struct sim_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0)
void vTaskDelete(TaskHandle_t t);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
typedef enum { eNoAction, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite } eNotifyAction;
BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
BaseType_t xTaskNotifyWait(uint32_t clear_entry, uint32_t clear_exit, uint32_t *value, TickType_t wait);
typedef enum { eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
typedef struct { uint32_t ulRunTimeCounter; } TaskStatus_t;
void vTaskGetInfo(TaskHandle_t t, TaskStatus_t *s, BaseType_t stack, eTaskState state);
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core);

typedef struct sim_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item);
#define xQueueCreateWithCaps(len, item, caps) xQueueCreate(len, item)
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
#define xQueueSendToBack xQueueSend
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item);
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);

typedef struct sim_mutex *SemaphoreHandle_t;
typedef struct { void *p; } StaticSemaphore_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
#define xSemaphoreCreateMutexStatic(buf) ((void)(buf), xSemaphoreCreateMutex())
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t m);

/* Critical sections: one process-wide recursive lock. */
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
void sim_critical_enter(void);
void sim_critical_exit(void);
#define taskENTER_CRITICAL(m) do { (void)(m); sim_critical_enter(); } while (0)
#define taskEXIT_CRITICAL(m) do { (void)(m); sim_critical_exit(); } while (0)
#define portENTER_CRITICAL taskENTER_CRITICAL
#define portEXIT_CRITICAL taskEXIT_CRITICAL

/* ---- HTTP client (libcurl underneath) -------------------------------------- */
typedef struct sim_http *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ERROR, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_HEADER_SENT = HTTP_EVENT_HEADERS_SENT,
               HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_HEADERS_COMPLETE, HTTP_EVENT_ON_STATUS_CODE, HTTP_EVENT_ON_DATA,
               HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED, HTTP_EVENT_REDIRECT } esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key, *header_value;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);
typedef struct {
    const char *url;
    int timeout_ms;
    http_event_handle_cb event_handler;
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int buffer_size;
    const char *user_agent;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
esp_err_t esp_crt_bundle_attach(void *conf);

/* ---- Web server types (the portal itself isn't part of the simulator) ------ */
typedef struct httpd_req httpd_req_t;
typedef void *httpd_handle_t;

/* ---- SD card ---------------------------------------------------------------- */
typedef struct { int unused; } sdmmc_card_t;

#ifdef __cplusplus
}
#endif
