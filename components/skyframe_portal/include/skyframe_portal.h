#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include "skyframe_config.h"
#include "skyframe_engine.h"
/* Settings web page (on the "SkyFrame-XXXX" setup network and on the home network)
   plus the photo manager. Invoke only after NVS is initialized. */
typedef void (*sf_portal_saved_cb)(const sf_config_t *cfg);
esp_err_t sf_portal_start(sf_config_t *config,sf_portal_saved_cb callback);
void sf_portal_stop(void);
/* Turn the setup network on (first setup, settings screen, Wi-Fi trouble) or off. */
void sf_portal_set_ap(bool on);

/* Retrieve credentials so the screen can display first-run setup instructions. */
void sf_portal_get_credentials(char *ssid,size_t ssid_size,char *password,size_t pass_size);

/* Shared page styling, and the photo manager pages (/photos and its API). */
extern const char sf_portal_style[];
/* Shared page frame: starts a chunked HTML response with the style, the SkyFrame header
   and the Settings / Photos / Birthdays tabs (tab: which one is current, or -1), then
   opens <main>. Send the page body as chunks, then sf_portal_page_end. */
enum { SF_TAB_SETTINGS, SF_TAB_PHOTOS, SF_TAB_BIRTHDAYS };
void sf_portal_page_start(httpd_req_t *r, const char *title, int tab);
esp_err_t sf_portal_page_end(httpd_req_t *r);
/* True unless a browser says the request came from another site (CSRF guard). */
bool sf_portal_same_origin(httpd_req_t *r);
/* Reading too: data is only served to SkyFrame's own names (skyframe.local, its addresses).
   A web page elsewhere that points its own name at the device ("DNS rebinding") could
   otherwise read the location, Wi-Fi names, photos and birthdays. */
bool sf_portal_own_host(httpd_req_t *r);
/* For a page asked for by another name (a phone's captive-portal check on the setup network,
   or a rebinding page): redirect to the same path at the address the request arrived on. */
esp_err_t sf_portal_to_own_host(httpd_req_t *r, const char *path);
void sf_photo_web_register(httpd_handle_t server);
/* Birthdays page (/birthdays and its API): people, photos, owner and owner's message. */
void sf_birthday_web_register(httpd_handle_t server);
