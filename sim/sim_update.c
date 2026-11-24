/* SkyFrame simulator: the updater (skyframe_update.c) stands in here. There is no second
   firmware slot on a PC; "Check for updates" reads the real release information from GitHub
   and says what the device would do, without installing anything. */
#include <stdarg.h>
#include "sim_esp.h"
#include "skyframe_update.h"
#include "skyframe_net.h"

static sf_update_status_t status;
static void set_status(sf_update_state_t st, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    status.state = st;
    vsnprintf(status.text, sizeof(status.text), fmt, a);
    va_end(a);
}
static void checker(void *arg)
{
    (void)arg;
    uint8_t *data = NULL;
    size_t len = 0;
    sf_update_manifest_t m;
    if (sf_net_get_bytes(SF_UPDATE_MANIFEST_URL, SF_UPDATE_MANIFEST_MAX, &data, &len) != ESP_OK) {
        set_status(SF_UPDATE_FAILED, "No release published yet");
    } else if (!sf_update_parse_manifest((const char *)data, len, &m)) {
        set_status(SF_UPDATE_FAILED, "Release information unreadable");
    } else if (sf_version_compare(m.version, sf_update_version()) <= 0) {
        set_status(SF_UPDATE_CURRENT, "Up to date (v%s)", sf_update_version());
    } else {
        set_status(SF_UPDATE_AVAILABLE, "v%s available (simulator: not installed)", m.version);
    }
    free(data);
    vTaskDelete(NULL);
}
void sf_update_init(void (*restart)(void)) { (void)restart; }
const char *sf_update_version(void) { return esp_app_get_description()->version; }
void sf_update_check_now(bool install)
{
    (void)install;
    set_status(SF_UPDATE_CHECKING, "Checking for updates...");
    xTaskCreatePinnedToCore(checker, "sim_update", 8192, NULL, 3, NULL, 1);
}
void sf_update_status(sf_update_status_t *out) { *out = status; }
bool sf_update_pending_verify(void) { return false; }
void sf_update_confirm(void) {}
void sf_update_reject(void) {}
bool sf_update_take_news(char *version, size_t vcap, char *notes, size_t ncap)
{
    /* SKYFRAME_SIM_NEWS=1 shows the "Updated to ..." card once, as after a real update. */
    const char *e = getenv("SKYFRAME_SIM_NEWS");
    if (!e || *e != '1') return false;
    snprintf(version, vcap, "%s", sf_update_version());
    snprintf(notes, ncap, "Sample release notes: a smoother radar and a new Today's sky.");
    return true;
}
void sf_update_record_installed(const char *version, const char *notes) { (void)version; (void)notes; }
bool sf_update_crash_summary(char *out, size_t cap) { if (cap) out[0] = 0; return false; }
