/* Simulator version of skyframe_sd.h: the "microSD card" is the folder sim/sdcard. The USB
   port acts as if a computer were plugged in, so file transfer can be tried from the screen. */
#pragma once
#include "sim_esp.h"
#ifndef SIM_SD_ROOT
#define SIM_SD_ROOT "sdcard"
#endif
#define SF_SD_ROOT SIM_SD_ROOT
esp_err_t sf_sd_start(sdmmc_card_t *card);
bool sf_sd_begin(void);
void sf_sd_end(void);
bool sf_sd_present(void);
bool sf_sd_is_ready(void);
bool sf_sd_usb_active(void);
bool sf_sd_usb_host_connected(void);
void sf_sd_usb_share(bool to_computer);
uint32_t sf_sd_generation(void);
void sf_sd_content_changed(void);
