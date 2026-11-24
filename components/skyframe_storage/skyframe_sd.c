#include "skyframe_sd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "skyframe_sd";

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool app_owned;            /* FAT mounted for SkyFrame and not being handed over */
static bool usb_owned;            /* the USB host owns the card */
static int busy;                  /* SkyFrame file operations in progress */
static uint32_t generation;
static bool present;
static tinyusb_msc_storage_handle_t storage;
static TaskHandle_t watchdog_task;

#define HANDOVER_WAIT_MS 10000    /* longest we wait for SkyFrame file access to finish */
#define UNPLUG_SUSPEND_MS 2000    /* bus silent this long while exposed = cable pulled */
#define RETRY_MS 1500             /* a failed hand-back is tried again after this */
#define RETRIES 3
/* Requests to the background task (task notification values). */
enum { REQ_SUSPENDED = 1, REQ_RESUMED, REQ_TO_USB, REQ_TO_APP };
static int app_retries;

bool sf_sd_begin(void)
{
    bool ok;
    taskENTER_CRITICAL(&state_lock);
    ok = app_owned;
    if (ok) busy++;
    taskEXIT_CRITICAL(&state_lock);
    return ok;
}

void sf_sd_end(void)
{
    taskENTER_CRITICAL(&state_lock);
    if (busy > 0) busy--;
    taskEXIT_CRITICAL(&state_lock);
}

bool sf_sd_present(void) { return present; }
bool sf_sd_is_ready(void) { return app_owned; }
bool sf_sd_usb_active(void) { return usb_owned; }
uint32_t sf_sd_generation(void) { return generation; }
void sf_sd_content_changed(void)
{
    taskENTER_CRITICAL(&state_lock);
    generation++;
    taskEXIT_CRITICAL(&state_lock);
}

/* Runs in the TinyUSB task around every ownership change. */
static void msc_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle; (void)arg;
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            /* About to unmount for the computer: refuse new access, let current access finish. */
            taskENTER_CRITICAL(&state_lock);
            app_owned = false;
            taskEXIT_CRITICAL(&state_lock);
            int waited = 0;
            while (waited < HANDOVER_WAIT_MS) {
                taskENTER_CRITICAL(&state_lock);
                int pending = busy;
                taskEXIT_CRITICAL(&state_lock);
                if (!pending) break;
                vTaskDelay(pdMS_TO_TICKS(10));
                waited += 10;
            }
            if (waited >= HANDOVER_WAIT_MS) ESP_LOGW(TAG, "SD access still busy; handing over anyway");
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        app_retries = 0;
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
            usb_owned = true;
            ESP_LOGI(TAG, "microSD card is now a USB drive on the computer");
        } else {
            taskENTER_CRITICAL(&state_lock);
            usb_owned = false;
            app_owned = true;
            generation++;
            taskEXIT_CRITICAL(&state_lock);
            ESP_LOGI(TAG, "microSD card mounted for SkyFrame");
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGW(TAG, "microSD mount event %d failed (owner %s)", (int)event->id,
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "SkyFrame");
        usb_owned = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB;
        /* Taking the card back failed (it then stays stuck on the USB notice): try again. */
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP && app_retries < RETRIES && watchdog_task) {
            app_retries++;
            usb_owned = true;   /* not SkyFrame's yet: keep the notice (and its Done button) up */
            xTaskNotify(watchdog_task, REQ_TO_APP, eSetValueWithOverwrite);
        }
        break;
    default:
        break;
    }
}

/* The board cannot sense VBUS on the native port when powered from the other port,
   so a pulled cable only shows up as a bus suspend. If the bus stays suspended while
   the computer owns the card, take the card back. It is not re-exposed on resume:
   the computer must re-enumerate (replug), so its view of the card is always fresh. */
static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_SUSPENDED && watchdog_task) xTaskNotify(watchdog_task, REQ_SUSPENDED, eSetValueWithOverwrite);
    if (event->id == TINYUSB_EVENT_RESUMED && watchdog_task) xTaskNotify(watchdog_task, REQ_RESUMED, eSetValueWithOverwrite);
}
/* Give the card back to SkyFrame. A failed mount still leaves the driver's mount point at
   APP, and the driver ignores requests for the mount point it already has; every later
   hand-back (retry, Done, eject, unplug) would then do nothing and the card would stay lost
   until a restart. So if the driver says APP but SkyFrame doesn't have the card, step it
   back to USB first: the APP request below then really mounts. */
static void take_back(void)
{
    tinyusb_msc_mount_point_t mp;
    if (!app_owned && tinyusb_msc_get_storage_mount_point(storage, &mp) == ESP_OK && mp == TINYUSB_MSC_STORAGE_MOUNT_APP)
        tinyusb_msc_set_storage_mount_point(storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
    tinyusb_msc_set_storage_mount_point(storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
}
/* Every ownership change requested here happens in this task: a handover can wait up to
   HANDOVER_WAIT_MS for file access to finish, which must never stall the display. */
static void unplug_watchdog(void *arg)
{
    (void)arg;
    uint32_t what = 0;
    bool have = false;    /* a request arrived while waiting for something else */
    for (;;) {
        if (!have) xTaskNotifyWait(0, UINT32_MAX, &what, portMAX_DELAY);
        have = false;
        if (what == REQ_TO_USB) {
            if (!usb_owned && tud_mounted()) {
                ESP_LOGI(TAG, "File transfer started from the screen");
                tinyusb_msc_set_storage_mount_point(storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
            }
            continue;
        }
        if (what == REQ_TO_APP) {
            if (app_retries) vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            if (usb_owned || !app_owned) {
                ESP_LOGI(TAG, "Returning the microSD card to SkyFrame%s", app_retries ? " (retry)" : "");
                take_back();
            }
            continue;
        }
        if (what != REQ_SUSPENDED) continue;
        /* Suspended: wait to see whether it resumes. */
        if (xTaskNotifyWait(0, UINT32_MAX, &what, pdMS_TO_TICKS(UNPLUG_SUSPEND_MS)) == pdTRUE) {
            if (what != REQ_RESUMED) have = true;   /* some other request: handle it next */
            continue;
        }
        if (usb_owned) {
            ESP_LOGI(TAG, "USB host went silent; returning the microSD card to SkyFrame");
            take_back();
        }
    }
}
bool sf_sd_usb_host_connected(void) { return present && tud_mounted(); }
void sf_sd_usb_share(bool to_computer)
{
    if (!present || !watchdog_task) return;
    if (!to_computer) app_retries = 0;
    xTaskNotify(watchdog_task, to_computer ? REQ_TO_USB : REQ_TO_APP, eSetValueWithOverwrite);
}

esp_err_t sf_sd_start(sdmmc_card_t *card)
{
    if (!card) return ESP_ERR_INVALID_ARG;
    const tinyusb_msc_driver_config_t driver_cfg = {
        .callback = msc_event,
    };
    esp_err_t e = tinyusb_msc_install_driver(&driver_cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "MSC driver install failed (%s)", esp_err_to_name(e)); return e; }
    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = card,
        .fat_fs = {
            .base_path = SF_SD_ROOT,
            .config = { .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024 },
            .do_not_format = true,      /* never erase someone's card */
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    e = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &storage);
    /* No FAT filesystem (or unreadable): treat as no card rather than waiting forever. */
    if (e != ESP_OK || !app_owned) {
        ESP_LOGE(TAG, "microSD card could not be mounted (%s); it must be FAT32", esp_err_to_name(e));
        return e != ESP_OK ? e : ESP_FAIL;
    }
    present = true;

    /* USB is optional: if it fails, SkyFrame keeps the card mounted for itself. */
    xTaskCreatePinnedToCore(unplug_watchdog, "sd_usb_watch", 4096, NULL, 3, &watchdog_task, 1);
    tinyusb_config_t usb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event);
    e = tinyusb_driver_install(&usb_cfg);
    if (e != ESP_OK) ESP_LOGW(TAG, "USB file transfer unavailable (%s)", esp_err_to_name(e));
    return ESP_OK;
}
