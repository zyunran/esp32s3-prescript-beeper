# Prescript Beeper · ESP32-S3 Pager / Personal Terminal

English · [中文](README.md)

An ESP32-S3 firmware for a desktop **pager (BB-machine)**-shaped personal terminal: 284×76 hand-drawn UI, three buttons + shake gestures, speaker / buzzer sound effects, integrating **clock & weather / "glitch" instruction decoder / gacha & dice / alarm & countdown / todos / answer book / daily oracle push**; you can provision the network offline, change all settings, and send instructions from a phone or PC browser.

- Platform: ESP32-S3 (WROOM-1 N16R8) · ESP-IDF v5.5.5 · FreeRTOS · C
- Flash: 16MB (app partition 4MB, OTA not enabled)
- Most code is vibe-coded; v1.06 fixes the main stability issues, and refactoring continues
- Reusable driver library: `D:/STM32Project/Library_SK/my_esp32_lib/esp32oder` (decoupled, with usage docs)
---

## v1.06 Changes

- Fixed: Pomodoro timer never ran (`POM_Tick` was not wired into the main loop)
- Fixed: Pomodoro pause accounting (timer kept running while paused / double-counted after resume)
- Fixed: web-saved instruction library did not take effect immediately (added independent 64px preset read/write)
- Fixed: web theme save drew from the HTTP task, risking LCD tearing
- Fixed: saving before the web page finished loading could clear all alarms
- Fixed: custom theme colors were lost after reboot
- Hardened: instruction library read/write concurrency; reads now copy into caller buffers
- Added: `pomodoro_drv` / `power_drv` reusable drivers (see driver library path above)

## Features

### Main menu

| Menu | Function |
|------|----------|
| **Oracle** | Draw a random instruction and reveal it as a full-screen glitch effect, word by word (supports inline tags like `{RAND}`, `{#RRGGBB}`) |
| **Ask** | Book of answers: "what to say / eat / drink / play" (built-in + editable on the web page) |
| **Observe** | Card gacha: 10-pull / dice battle / single pull / points / album |
| **Todo** | Instruction log: `{TODO}` instructions are auto-added, PASS to mark done, synced with the web page |
| **User** | Switch the active user (add more on the web page); each user receives their own private instructions |
| **Settings** | Screen timeouts / volume / buzzer / shake / oracle / glitch font size / cursor; system info, balance, factory reset |
| **Network** | Connect to WiFi / start provisioning hotspot / check weather |
| **Loom** | Easter egg: "Weaving Time" time-acceleration, "Weaving Memory" full-screen white-frame filter |
| **TTL protocol** | Countdown / alarm / Pomodoro |

### Highlights

- **Glitch instruction decoder**: starts as full-screen random noise, then reveals text character by character; supports `{#RRGGBB}` colors, `{RAND:min-max}` random values, `{TODO}` auto-insert into todos; font sizes 16/24/32/64 px.
- **Card gacha (Limbus-style)**: 10-pull scan-line animation, golden-persona voice typewriter, points-based single pull; coin **dice-battle** mini-game with damage points / win streak; collectible album of 12 sinners × 120 personas. (The dice battle still has some issues.)
- **Time management**: alarms (daily / workdays / weekends / one-shot / custom weekdays), countdown that beeps and wakes the screen when done, Pomodoro (work/break auto-rotation), scheduled daily oracle push.
- **Works offline**: DS1302 shows the time immediately at power-up and keeps it on its backup battery when unpowered; every local feature runs without a network.
- **Light-sleep low power**: 50 ms tick-based light sleep + suspended sampling task + WiFi off; on wake it reconnects in the background and SNTP resyncs right away.
- **Full web configuration**: WiFi / weather / instruction library / theme / alarms / todos / users / glitch parameters / album — all persisted in NVS and survive reboots.

## Hardware

### Wiring (ESP32-S3)

| Module | Pins |
|--------|------|
| LCD ST7789 284×76 (SPI2 60MHz) | SCL=`7` SDA=`8` CS=`9` RST=`10` DC=`11` BLK=`12` (backlight on when LOW) |
| Buttons Up / Down / OK (internal pull-up, pressed = LOW; PCB rev. wiring: Up=`5`, Down=`6`, OK=`4`) | see left |
| Active buzzer (sounds when LOW) | `15` |
| MAX98357A amp (I2S) | BCLK=`16` LRC=`17` DIN=`18` SD=`13` (LOW = off to avoid pop) |
| DS1302 RTC (3-wire bit-bang) | CLK=`2` DAT=`14` CE/RST=`21` |
| MPU6050 6-axis (software I2C) | SCL=`39` SDA=`38` |
| Battery voltage ADC (1S LiPo, 1:1 divider) | `1` (ADC1_CH0) |
| Serial log (115200) | `43`(TX) `44`(RX) |

> To change pins: buttons are in `components/UI/UI.h`, the rest is noted in comments at the top of each component's `.c`. Avoid GPIO35/36/37 on N16R8 (occupied by internal PSRAM), GPIO48 (on-board RGB) and the strapping pins.

## Usage

### Build & flash

Environment: Windows + ESP-IDF v5.5.5, USB serial driver installed.

**Option 1 · Native ESP-IDF (recommended)**

```powershell
cd oder
idf.py build
idf.py -p COM9 flash
```

**Option 2 · Direct esptool** (`--before default_reset` + `--after hard_reset` = normal double boot; `python` is the activated ESP-IDF environment)

```powershell
python -m esptool --chip esp32s3 -p COM9 -b 460800 --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\oder.bin
```

Artifact: `build/oder.bin` (~1.57 MB).

### Button operations

- **Up / Down**: scroll the menu; long-press for auto-repeat (fast scrolling).
- **OK**: confirm / enter the current item.
- **Long press OK**: go back one level / exit the current screen.
- **Shake** (enable in Settings → Shake): shake up = scroll up, shake down = scroll down, shake left = confirm, shake right = long-press-OK (exit).

### Provisioning & web configuration

1. From the main menu go to **Network → Start provisioning** to turn on the hotspot `ESP32ODERAP` (the password is generated randomly per device and shown on the screen when the hotspot is enabled).
2. Connect your phone to the hotspot — the config page pops up **automatically** (captive portal), or open `http://192.168.4.1/` manually.
3. On the config page scan/save your WiFi, enter your city and **weather API key (Seniverse / 心知天气)**; the device switches to network mode.
4. Afterwards, when the device and your phone/PC are on the same network, open the device's IP in a browser (shown on the status page) for full management.

### Notes

1. **Currently everything runs on a breadboard with modules and is incomplete; a PCB version is in progress.**

> **Privacy**: this repository does not contain any personal WiFi SSID/password or weather API key — on first use they must be entered on the config page (or written into the device's NVS `net` namespace); the source defaults to empty.
