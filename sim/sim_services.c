/* SkyFrame simulator: Wi-Fi, saved settings, the setup portal and the microSD card / USB,
   standing in for skyframe_wifi.c, skyframe_portal.c, skyframe_nvs.c and skyframe_sd.c. */
#include <sys/stat.h>
#include "sim_esp.h"
#include "skyframe_config.h"
#include "skyframe_nvs.h"
#include "skyframe_wifi.h"
#include "skyframe_portal.h"
#include "skyframe_sd.h"

#define TAG "sim"
#define JOIN_MS 1800   /* "joining Wi-Fi" this long after start, so the startup screen shows */
extern double sim_latitude, sim_longitude;

/* ---- Settings: SkyFrame's defaults with one Wi-Fi network, so it starts normally ---- */
static sf_config_t saved;
static bool have_saved;
esp_err_t sf_config_nvs_init(void) { return ESP_OK; }
esp_err_t sf_config_nvs_load(sf_config_t *out) {
    if (!have_saved) {
        sf_config_defaults(&saved);
        snprintf(saved.wifi[0].ssid, sizeof(saved.wifi[0].ssid), "Simulator");
        if (sim_latitude || sim_longitude) { saved.latitude = sim_latitude; saved.longitude = sim_longitude; }
        have_saved = true;
    }
    *out = saved;
    return ESP_OK;
}
esp_err_t sf_config_nvs_save(const sf_config_t *cfg) {
    if (!sf_config_valid(cfg)) return ESP_ERR_INVALID_ARG;
    saved = *cfg;
    return ESP_OK;
}

/* ---- Wi-Fi ---------------------------------------------------------------------- */
esp_err_t sf_wifi_begin(sf_config_t *config) { (void)config; return ESP_OK; }
bool sf_wifi_is_connected(void) { return esp_timer_get_time() / 1000 >= JOIN_MS; }
void sf_wifi_kick(void) {}
bool sf_wifi_ip(char *out, size_t cap) {
    if (!sf_wifi_is_connected()) return false;
    snprintf(out, cap, "127.0.0.1");
    return true;
}
bool sf_wifi_needs_setup(void) { return false; }
uint8_t sf_wifi_home_channel(void) { return 6; }
void sf_planned_restart(void) { esp_restart(); }
bool sf_restart_was_planned(void) { return false; }

/* ---- Setup portal: no web server in the simulator --------------------------------- */
esp_err_t sf_portal_start(sf_config_t *config, sf_portal_saved_cb callback) { (void)config; (void)callback; return ESP_OK; }
void sf_portal_stop(void) {}
void sf_portal_set_ap(bool on) { (void)on; }
void sf_portal_get_credentials(char *ssid, size_t ssid_size, char *password, size_t pass_size) {
    snprintf(ssid, ssid_size, "SkyFrame-SIM");
    snprintf(password, pass_size, "skyframe");
}

/* ---- microSD card = the sim/sdcard folder; a computer is "always on the USB port" ---- */
static volatile bool usb_owned;
static volatile uint32_t generation = 1;
static bool present;
esp_err_t sf_sd_start(sdmmc_card_t *card) {
    (void)card;
    struct stat st;
    present = stat(SF_SD_ROOT, &st) == 0;
    if (!present) ESP_LOGW(TAG, "No %s folder: photos and saved plane pictures are off", SF_SD_ROOT);
    return present ? ESP_OK : ESP_FAIL;
}
bool sf_sd_begin(void) { return present && !usb_owned; }
void sf_sd_end(void) {}
bool sf_sd_present(void) { return present; }
bool sf_sd_is_ready(void) { return present && !usb_owned; }
bool sf_sd_usb_active(void) { return usb_owned; }
bool sf_sd_usb_host_connected(void) { return present; }
void sf_sd_usb_share(bool to_computer) {
    if (!present || usb_owned == to_computer) return;
    usb_owned = to_computer;
    if (!to_computer) generation++;   /* "remounted": the slideshow rescans */
    ESP_LOGI(TAG, "microSD card %s", to_computer ? "handed to the (pretend) computer" : "back with SkyFrame");
}
uint32_t sf_sd_generation(void) { return generation; }
void sf_sd_content_changed(void) { generation++; }
