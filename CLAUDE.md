# SkyFrame: notes for Claude

ESP-IDF 6.1.1 firmware (C, LVGL 8.2) for the Waveshare ESP32-S3-Touch-LCD-2.8C: a round 480×480
display showing a live air-traffic radar, weather, photos and more. It is a gift that runs 24/7 at
a relative's house, so robustness matters more than features. The whole project is built with
Claude Code; the owner reviews, flashes and releases.

## Rules

- **Never flash or open a monitor** (`idf.py flash`/`monitor`, COM ports). Build only, then give
  the owner the command: `idf.py -p COM4 flash monitor`. If the owner says COM4 is busy, stopping
  a stale `idf_monitor` python process is fine.
- **Commit and push only when asked.** Release only when asked (see Releases).
- **Never commit personal data**: real names, birthdays, home coordinates, Wi-Fi names or
  passwords, photos. Tests use the made-up point 39.15, -76.70 and a sample person "Alex". The repo
  is public. Commits use the GitHub noreply email (set in the repo's git config).
- **Never commit or print the signing key** (`../SkyFrame-signing-key.pem`, outside the repo;
  `*.pem` is git-ignored). Losing it means updates can only be installed over USB.
- **Don't reduce the radar's visual quality** to save CPU. Optimizations must look identical.
- Every new feature gets a **test in test mode** (hold any screen 5 s, then the settings screen
  5 s) and, where it is portable logic, a **unit test** in `tests/`.

## Commands (PowerShell)

```powershell
. C:\Espressif\tools\Microsoft.v6.1.1.PowerShell_profile.ps1 *> $null   # ESP-IDF environment
idf.py build                                                           # firmware (signed)
.\sim\run.ps1                                  # PC simulator with live data (MSYS2: C:\msys64)
$env:MSYSTEM="UCRT64"; C:\msys64\usr\bin\bash.exe -lc "cd '/c/ESP Project/SkyFrame' && sh tests/run_tests.sh"
```

The simulator compiles `main/main.c` and the components unchanged (hardware stand-ins in `sim/`).
Use it to check visual changes: `SKYFRAME_SIM_SCRIPT` drives it and saves screenshots (see
`sim/README.md`); convert the `.bmp` to `.png` with Pillow to look at it. It can't show speed: the
device is far slower than a PC.

## Architecture

- `main/main.c`: startup, the UI loop (the only thread that may call LVGL), worker tasks on CPU1
  (traffic, weather, verse, NWS alerts, aircraft info, Today's photo) that hand results over via
  the `mailbox` queue; screen rotation and transitions; test mode; sightings; updates.
- `components/`: `skyframe_ui` (radar), `skyframe_screens` (everything else on screen),
  `skyframe_engine` (pure logic, host-tested), `skyframe_net` (HTTPS), `skyframe_portal` (Wi-Fi
  and the web pages), `skyframe_config` (settings, versioned NVS record), `skyframe_update`
  (over-the-air updates), `skyframe_storage` (microSD and USB), photo player and JPEG.
- Internal RAM is scarce (Wi-Fi, TLS, LVGL objects); put big buffers in PSRAM
  (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`). Only one TLS session at a time (`skyframe_net`).
- Saved settings are a versioned record (`SF_CONFIG_RECORD_VERSION`). Adding a field means a new
  version, a frozen copy of the previous layout in `skyframe_config.c`, a migration, and a test
  that an old record still decodes.
- The flash layout (`partitions.csv`) is fixed: changing it can only be installed over USB, and
  `nvs` must stay at 0x9000 (settings live there).

## Releases (over-the-air updates)

Devices check `https://github.com/CosmicYeet/SkyFrame/releases/latest/download/latest.json` every
night in the 3 AM hour (and from test mode > Check for updates), install a newer signed version,
and roll back by themselves if it doesn't start properly.

To release, when the owner asks: commit the work first, then

```powershell
.\tools\release.ps1 0.5.1 "Short notes, about 40 characters"
```

It runs the tests, builds the simulator and the signed firmware, verifies the signature, commits
`version.txt`, tags `v0.5.1`, pushes, and creates the GitHub release (`SkyFrame.bin` +
`latest.json`). Version numbers: patch for fixes, minor for new features. The notes appear on the
device's "Updated to v…" card, so write them for the person who sees the screen.
