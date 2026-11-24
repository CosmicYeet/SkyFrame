/* Firmware update page (http://skyframe.local/update): install a SkyFrame.bin from a computer on
   the same Wi-Fi, without publishing a release. The firmware must be signed with SkyFrame's
   key like any update (checked by esp_ota_end); the new version proves itself the same way. */
#include "skyframe_portal.h"
#include "skyframe_update.h"
#include "skyframe_wifi.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "update_web";
#define MAX_FIRMWARE (6 * 1024 * 1024)   /* one firmware slot */
#define CHUNK 4096

static const char page_html[] =
"<style>progress{width:100%;height:10px;margin-top:12px;accent-color:var(--acc)}"
".drop{position:relative;display:flex;flex-direction:column;align-items:center;gap:2px;padding:20px 12px;"
"border:2px dashed var(--acc);border-radius:14px;background:var(--soft);color:var(--acc);text-align:center;"
"cursor:pointer;font-size:19px}.drop input{position:absolute;width:1px;height:1px;opacity:0}</style>"
"<section class=card><h2>Firmware update</h2>"
"<p class=sub>SkyFrame installs new versions by itself overnight. To install a build now, choose its "
"<b>SkyFrame.bin</b> (in the project's <code>build</code> folder). Only firmware signed with SkyFrame's key is "
"accepted; if a new version doesn't start properly, SkyFrame goes back to this one.</p>"
"<p>Running now: <b id=v></b></p>"
"<label class=drop>Choose SkyFrame.bin<input type=file id=f accept='.bin'></label>"
"<progress id=pg hidden></progress><p id=s class=note></p></section><script>"
"const S=document.getElementById('s'),P=document.getElementById('pg');"
"function say(t,ok){S.textContent=t;S.className='note'+(ok===undefined?'':ok?' ok':' err')}"
"document.getElementById('f').onchange=e=>{const f=e.target.files[0];if(!f)return;"
"const x=new XMLHttpRequest();x.open('POST','/update');P.hidden=false;P.max=f.size;P.value=0;"
"x.upload.onprogress=v=>{P.value=v.loaded;say('Sending '+Math.round(v.loaded*100/f.size)+'%\\u2026')};"
"x.onload=()=>{P.hidden=true;if(x.status==200){say(x.responseText,true);setTimeout(()=>location.reload(),30000)}"
"else say(x.responseText||'Update failed',false)};"
"x.onerror=()=>{P.hidden=true;say('Connection lost during the upload',false)};"
"say('Sending\\u2026');x.send(f)};"
"</script>";

static esp_err_t update_page(httpd_req_t *r)
{
    if (!sf_portal_own_host(r)) return sf_portal_to_own_host(r, "/update");
    sf_portal_page_start(r, "SkyFrame firmware", -1);
    httpd_resp_send_chunk(r, page_html, HTTPD_RESP_USE_STRLEN);
    char v[96];
    snprintf(v, sizeof(v), "<script>document.getElementById('v').textContent='v%s'</script>", sf_update_version());
    httpd_resp_send_chunk(r, v, HTTPD_RESP_USE_STRLEN);
    return sf_portal_page_end(r);
}

static void restart_later(void *arg) { (void)arg; sf_planned_restart(); }
static esp_err_t fail(httpd_req_t *r, const char *status, const char *text)
{
    httpd_resp_set_status(r, status);
    return httpd_resp_sendstr(r, text);
}
static esp_err_t update_post(httpd_req_t *r)
{
    if (!sf_portal_same_origin(r)) return fail(r, "403 Forbidden", "Use the SkyFrame page to update");
    if (r->content_len <= 0 || r->content_len > MAX_FIRMWARE) return fail(r, "400 Bad Request", "That isn't a SkyFrame firmware file");
    if (sf_update_restart_pending()) return fail(r, "409 Conflict", "An update is already installed; SkyFrame is restarting");
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    esp_ota_handle_t h = 0;
    if (!slot || !buf || esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        free(buf);
        return fail(r, "500 Internal Server Error", "Couldn't prepare the update");
    }
    size_t got = 0;
    int timeouts = 0;
    const int64_t give_up = esp_timer_get_time() + 300 * 1000000LL;   /* five minutes for 6 MB at worst */
    esp_err_t e = ESP_OK;
    while (got < r->content_len && e == ESP_OK) {
        int n = httpd_req_recv(r, buf, r->content_len - got < CHUNK ? r->content_len - got : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (n <= 0 || esp_timer_get_time() > give_up) { e = ESP_ERR_TIMEOUT; break; }
        e = esp_ota_write(h, buf, (size_t)n);
        got += (size_t)n;
    }
    free(buf);
    if (e != ESP_OK) {
        esp_ota_abort(h);
        return fail(r, "400 Bad Request", e == ESP_ERR_TIMEOUT ? "The upload was interrupted" : "That isn't a SkyFrame firmware file");
    }
    /* esp_ota_end checks the image, and its signature (only SkyFrame's key). */
    e = esp_ota_end(h);
    esp_app_desc_t d;
    if (e == ESP_OK && (esp_ota_get_partition_description(slot, &d) != ESP_OK || strcmp(d.project_name, "SkyFrame")))
        e = ESP_ERR_INVALID_VERSION;
    if (e == ESP_OK) e = esp_ota_set_boot_partition(slot);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Uploaded firmware refused: %s", esp_err_to_name(e));
        return fail(r, "400 Bad Request", e == ESP_ERR_OTA_VALIDATE_FAILED || e == ESP_ERR_INVALID_VERSION
                    ? "Not installed: that isn't a SkyFrame firmware signed with SkyFrame's key"
                    : "Not installed: the firmware didn't check out");
    }
    ESP_LOGI(TAG, "Installed %s from the web page; restarting", d.version);
    sf_update_record_installed(d.version, "Installed from the web page.");
    char ok[128];
    snprintf(ok, sizeof(ok), "Installed v%.31s. SkyFrame is restarting; this page reloads in 30 seconds.", d.version);
    httpd_resp_sendstr(r, ok);
    /* Restart once the answer is on its way. */
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = restart_later, .name = "update_restart"};
    if (!t && esp_timer_create(&a, &t) != ESP_OK) t = NULL;
    if (t) esp_timer_start_once(t, 1500 * 1000);
    return ESP_OK;
}

void sf_update_web_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/update", .method = HTTP_GET, .handler = update_page},
        {.uri = "/update", .method = HTTP_POST, .handler = update_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(server, &uris[i]);
}
