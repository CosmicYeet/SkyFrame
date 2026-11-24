#pragma once
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>
#include <stdint.h>

/* microSD ownership: either SkyFrame (FAT mounted at SF_SD_ROOT) or a USB host
   (the card appears as a drive on the computer), never both at once.

   Plugging the native "USB" port into a computer hands the card to the computer;
   ejecting the drive (or unplugging) hands it back. Before the handover, new
   SkyFrame file access is refused and in-progress access is allowed to finish,
   so the two sides never write to the card at the same time. */
#define SF_SD_ROOT "/sdcard"

/* Mount an already-initialized card for SkyFrame and enable USB file transfer.
   Call once at boot; card may be NULL (no card: SD features stay unavailable). */
esp_err_t sf_sd_start(sdmmc_card_t *card);

/* Wrap every SkyFrame file operation: if sf_sd_begin() returns true the card is
   mounted and will stay mounted until sf_sd_end(). Keep these sections short. */
bool sf_sd_begin(void);
void sf_sd_end(void);

bool sf_sd_present(void);        /* a readable FAT card was mounted at boot */
bool sf_sd_is_ready(void);       /* SkyFrame currently owns the mounted card */
bool sf_sd_usb_active(void);     /* the computer currently owns the card */
/* A computer is connected to the native USB port (and has set the device up). */
bool sf_sd_usb_host_connected(void);
/* Hand the card to the connected computer (true) or take it back for SkyFrame (false).
   Runs in the background: the handover waits for SkyFrame's own file access to finish.
   Taking it back without ejecting on the computer first can lose files still being copied. */
void sf_sd_usb_share(bool to_computer);
/* Increments each time the card (re)mounts for SkyFrame, e.g. after a USB session,
   so content can be rescanned. 0 = never mounted. */
uint32_t sf_sd_generation(void);
/* Files were added or removed by SkyFrame itself (e.g. web upload): bump the
   generation so readers such as the slideshow rescan. */
void sf_sd_content_changed(void);
