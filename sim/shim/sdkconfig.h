/* Simulator: settings for the JPEG decoder (the device uses the copy in its ROM). */
#pragma once
#define CONFIG_JD_SZBUF 512
#define CONFIG_JD_FORMAT 0
#define CONFIG_JD_USE_SCALE 1
#define CONFIG_JD_TBLCLIP 1
#define CONFIG_JD_FASTDECODE 2   /* table decoding: its larger work area also fits 64-bit pointers */
