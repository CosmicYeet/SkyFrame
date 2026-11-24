# SkyFrame simulator

SkyFrame's firmware running in a window on a Windows PC: the same radar, screens, rotation,
pull-down panel, alerts, transitions and test mode, with **live data** from the same services
the device uses (adsb.fi traffic, Open-Meteo weather, National Weather Service alerts,
BibleGateway, adsbdb aircraft details and photos). The mouse is your finger.

```powershell
.\sim\run.ps1                    # build what changed, then start
.\sim\run.ps1 39.18 -76.67       # center the radar somewhere else
```

## How it works

`main/main.c` and the components are compiled **unchanged**. Only the hardware is replaced:

| On the device | In the simulator |
|---|---|
| 480×480 round RGB panel, backlight | SDL window with a round mask; backlight dims the window |
| GT911 touch | mouse |
| FreeRTOS tasks, queues, notifications | threads (`sim_platform.c`) |
| ESP-IDF HTTPS client | libcurl |
| Wi-Fi | always connected (after ~2 s, so the startup screen shows) |
| NVS settings | in memory: defaults with one Wi-Fi network |
| microSD card | the `sim/sdcard` folder (copy the card's folders there) |
| USB file transfer | pretend computer: the USB button and Done work |

LVGL uses the firmware's own settings, read from `build/config/sdkconfig.h`, so run
`idf.py build` at least once first.

## Scripted input and screenshots

Set `SKYFRAME_SIM_SCRIPT` to timed steps (milliseconds since start; coordinates on the 480×480
panel) to drive the simulator without clicking. `shot NAME` saves `shot_NAME.bmp` in the
working folder:

```powershell
$env:SKYFRAME_SIM_SCRIPT = "6000:down 240,60;6040:move 240,180;6080:move 240,330;6120:up;7000:shot panel;7500:quit"
.\sim\run.ps1
```

Steps: `down x,y`, `move x,y`, `up`, `click x,y`, `shot NAME`, `quit`.

## Plane photos

`python tools/prepare_plane_photos.py sim\sdcard\planes` downloads the type photos (Wikipedia)
the same way as for the real card.

## Not in the simulator

- The web pages (settings, photos, birthdays): there is no web server.
- Speed: a PC is far faster than the ESP32-S3, so stutter that the device would show won't
  appear here. Check animations on the device.
- Battery voltage (shows `--`); CPU, RAM and chip temperature in test mode are placeholders.

## Requirements

MSYS2 (UCRT64) with `mingw-w64-ucrt-x86_64-gcc`, `-SDL2`, `-curl`, `-cmake` and `-ninja`.
