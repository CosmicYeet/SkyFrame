#pragma once
/* Over-the-air updates.

   Releases are published on GitHub (tools/release.ps1): a signed SkyFrame.bin and latest.json,
   which names the newest version, where its firmware is, and a line of release notes.

   - SkyFrame checks once a night and on request (test mode). A newer version is downloaded
     into the other firmware slot, its signature checked (only SkyFrame's key is accepted),
     and the device restarts into it.
   - The new version must prove itself: Wi-Fi joined and a minute of running. Until then it
     is "pending"; if it crashes or freezes first, the bootloader goes back to the previous
     version, and that version is never offered again.
   - After an update the release notes are shown once ("Updated to v0.5.1"). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SF_UPDATE_MANIFEST_URL "https://github.com/CosmicYeet/SkyFrame/releases/latest/download/latest.json"
#define SF_UPDATE_MANIFEST_MAX 8192

/* ---- Portable (host-tested) ---- */
typedef struct {
    char version[16];        /* "0.5.1" */
    char url[256];           /* the firmware (.bin), HTTPS */
    char notes[160];         /* one or two lines for the "Updated" card */
} sf_update_manifest_t;
/* Compare "major.minor.patch" versions: <0, 0 or >0 (missing parts count as 0; a leading 'v'
   is ignored). */
int sf_version_compare(const char *a, const char *b);
/* latest.json -> manifest. False if a field is missing or unusable (e.g. not HTTPS). */
bool sf_update_parse_manifest(const char *json, size_t len, sf_update_manifest_t *out);

/* ---- On the device ---- */
typedef enum { SF_UPDATE_IDLE, SF_UPDATE_CHECKING, SF_UPDATE_CURRENT, SF_UPDATE_AVAILABLE,
               SF_UPDATE_DOWNLOADING, SF_UPDATE_INSTALLED, SF_UPDATE_FAILED } sf_update_state_t;
typedef struct {
    sf_update_state_t state;
    int percent;             /* while downloading */
    char text[64];           /* for people: "Up to date (v0.5.0)", "Installing v0.5.1... 40%" */
} sf_update_status_t;

/* At boot, after NVS: rollback bookkeeping, the last crash report; starts the worker (CPU1).
   restart: how to restart once an update is installed (the planned-restart helper). */
void sf_update_init(void (*restart)(void));
const char *sf_update_version(void);          /* the running version, "0.5.0" */
/* Check now in the background (needs Wi-Fi); install a newer version if `install`. */
void sf_update_check_now(bool install);
void sf_update_status(sf_update_status_t *out);
/* Running a newly installed version that hasn't proven itself yet. */
bool sf_update_pending_verify(void);
/* This boot has read the release information: HTTPS and the updater work. */
bool sf_update_server_reached(void);
/* An update is installed and waiting for its restart. */
bool sf_update_restart_pending(void);
void sf_update_confirm(void);                 /* it works: keep it */
void sf_update_reject(void);                  /* it doesn't: back to the previous one (restarts) */
/* What's new, once after an update (false otherwise). */
bool sf_update_take_news(char *version, size_t vcap, char *notes, size_t ncap);
/* An update installed some other way (the web page): show it after the restart. */
void sf_update_record_installed(const char *version, const char *notes);
/* The last crash saved in flash, "skyframe_net at 0x4201a2b3", or false if none. */
bool sf_update_crash_summary(char *out, size_t cap);
