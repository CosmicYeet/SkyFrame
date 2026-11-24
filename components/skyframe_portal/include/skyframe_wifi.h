#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "skyframe_config.h"
/* Begin Wi-Fi and setup portal. With saved networks, joins the strongest one in range
   (several saved: scans first; the preferred one is favored), trying the others in turn
   and retrying with backoff (2 s doubling to 60 s) whenever the connection drops.
   Device app should use sf_wifi_is_connected() to gate HTTPS polling. */
esp_err_t sf_wifi_begin(sf_config_t *config);
bool sf_wifi_is_connected(void);
/* Start a connection attempt now if disconnected (after a Wi-Fi mode change). */
void sf_wifi_kick(void);
/* The device's address on the home network ("192.168.1.205"); false if not connected. */
bool sf_wifi_ip(char *out, size_t cap);
/* True when none of the saved networks has connected since boot after several tries:
   the settings are probably wrong, so the setup screen should be shown again. */
bool sf_wifi_needs_setup(void);
/* The home router's channel from the last connect (1-13), or 0 if unknown. */
uint8_t sf_wifi_home_channel(void);
/* Restart on purpose (settings saved, test menu), so the next boot can tell it from a crash. */
void sf_planned_restart(void);
/* True when this boot follows sf_planned_restart(). */
bool sf_restart_was_planned(void);
