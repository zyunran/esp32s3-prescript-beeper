# Prescript Beeper · ESP32-S3 Pager / Personal Terminal

English · [中文](README.md)

An ESP32-S3 firmware for a desktop **pager**-shaped personal terminal: 284×76 hand-drawn UI, three buttons + shake gestures, speaker/buzzer sound effects, integrating **clock & weather / glitch-decode instructions / gacha & coin battle / alarms & timers / todos / answer book / daily oracle push / OneNET cloud (MQTT)**; a phone or PC browser can provision WiFi, change every setting, and send instructions to the screen.

- Platform: ESP32-S3 (WROOM-1 N16R8) · ESP-IDF v5.5.5 · FreeRTOS · C
- Flash: 16MB (OTA dual partitions: ota_0 / ota_1, 2MB each)
- Most code started as vibe coding, then was hardened through four systematic audits (80+ fixes); solid engineering and docs throughout
- Reusable driver library: `D:/STM32Project/Library_SK/my_esp32_lib/esp32oder` (decoupled, with usage docs)

---


## Features

### Main menu

| Menu | Function |
|------|----------|
| **Oracle** | Pull a random instruction, full-screen glitch decode (inline `{RAND}`, `{#RRGGBB}` tags) |
| **TTL** | Alarm / Countdown / Pomodoro |
| **Todos** | Instruction log: `{TODO}` auto-captured, PASS marking, web sync |
| **Network** | Connect / Cloud (OneNET switch) / Weather / IP / AP provisioning / OTA update |
| **Gacha** | Ten-pull / Single / Coin Battle / Codex / Score |
| **Ask** | Answer book: answer / what to eat / drink / play (built-in + web-customized) |
| **User** | Switch the active user (web can add); each user receives their own private instructions |
| **Settings** | Volume/buzzer/key-sound; screen-off/theme (Soft Green/Cyber Cyan/Dark Gray/Standard B&W)/cursor/font; oracle/window; shake; system info, factory reset |
| *Loom (hidden)* | Easter egg unlocked by Konami gesture (Up Up Down Down Left Right Left Right): time acceleration, white-box filter, balance page |

### Highlights

- **Glitch decode**: full-garble jitter then per-char reveal; `{#RRGGBB}` color, `{RAND:min-max}`, `{TODO}` auto-todo; 16/24/32/64 px fonts; OK reveals the text instantly mid-decode.
- **Gacha**: ten-pull scanline animation, gold-voice typewriter, score pulls; **coin battle** mini-game with damage/streak scoring; 12 sinners × 120 identities codex.
- **Time management**: alarms (daily/weekdays/weekends/once/custom), countdown beep + auto wake, pomodoro cycles, scheduled daily oracle.
- **Offline-first**: DS1302 shows time instantly (battery-backed); every local feature works without network.
- **Light-sleep low power**: 50 ms tick sleep + suspended sensor task + WiFi off; reconnect is manual via Network → Connect (Cloud mode keeps the link alive instead).
- **Full web config**: WiFi/weather/library/theme/alarms/todos/users/decode params/codex/cloud — all persisted in NVS.

## Architecture & Engineering

```mermaid
flowchart LR
    K[Keys input_task] -->|key_q| U[ui_task UI state machine]
    M[MPU6050 task] -->|shake = key event| U
    W[httpd config page] -->|commands / web_dirty| U
    U -->|tick| A[INSTRUCTION glitch decode]
    U --> G[GACHA]
    U --> T[TIMER / ALARM / POMODORO]
    U --> N[NET on-demand WiFi + weather]
    U --> P[POWER screen-off / light sleep]
    C[CLOUD OneNET MQTT] -->|display_cmd| U
    U -.->|properties / events| C
```

- **Component layering**: 16 functional components + a `COMMON` shared header; UI framebuffer drawing APIs are reused by decode/gacha/timer full screens.
- **Event-driven tasks**: high-priority input polling + a single ui_task state machine owning all drawing (single LCD writer, no concurrent tearing); cloud_task runs the MQTT session independently.
- **Concurrency discipline**: mutexes on every httpd/UI-shared dataset; sound params passed atomically via a length-1 queue; drawing unified in ui_task.
- **Four systematic audits**: two full-codebase sweeps + two cloud-focused sub-agent cross reviews, 80+ fixes (web-save transactionalization, NVS write merging 37→≤8 commits, dual-core races, shake-source filtering, comment drift).
- **Security**: CSRF token on all web POSTs; WiFi/weather/OneNET credentials only in NVS, never in source; randomized AP password per device.
- **Release discipline**: `version.txt` single source of version; OTA dual partitions + SHA256 + boot rollback guard; GitHub Releases ship firmware + notes.

## Cloud OneNET (optional, v1.14)

"Remote online" is off by default and affects nothing locally. When enabled the device talks MQTT to China Mobile's OneNET IoT platform at `mqtt://mqtts.heclouds.com:1883` (clientId = device name, username = product ID, password = HMAC-SHA256 auth token):

| Link | Content | Cadence |
|------|---------|---------|
| Properties | `battery` / `rssi` / `version` / `alarm_cnt` | on connect + every 60 s |
| Events | `alarm_fire` / `todo_remind` / `daily_sign` (param `msg`) | on trigger |
| Downlink | `display_cmd` service (param `msg`) → glitch-decode on screen + reply | anytime |

### Platform side (OneNET console)

1. Register at [open.iot.10086.cn](https://open.iot.10086.cn) (real-name verification) → IoT Platform → create a product: **MQTT** access, **OneJSON** data protocol, **WiFi**.
2. Define the thing model exactly as the table above (**identifiers must match**): 4 properties, 3 events (`msg` text param), 1 service `display_cmd` (`msg` text, `timeout` int32).
3. Create a device; note the triplet **ProductID / DeviceName / DeviceKey**.
4. Optional pre-check on PC: `python tools/onenet_token.py <ProductID> <DeviceName> <DeviceKey>`, then connect MQTTX to `mqtt://mqtts.heclouds.com:1883` (clientId=DeviceName, username=ProductID, password=token) — "online" on the platform confirms the triplet.

### Device side

With WiFi connected, open the config page → "☁ Cloud OneNET" card: enter the triplet, enable, save. DeviceKey lives only in device NVS (masked in the web UI). The device-side Network → Cloud toggle flips the same switch.

> Note: while enabled the device stays online and skips light-sleep standby (higher idle current); a leaked DeviceKey can be reset on the platform. Offline downstream commands are dropped by design.

## Hardware

### Wiring (ESP32-S3)

| Module | Pins |
|--------|------|
| LCD ST7789 284×76 (SPI2 60MHz) | SCL=`7` SDA=`8` CS=`9` RST=`10` DC=`11` BLK=`12` (active-low backlight) |
| Keys Up / Down / OK (internal pull-ups, active-low; PCB rev.: Up=`5` Down=`6` OK=`4`) | see left |
| Active buzzer module (high-level trigger: high = beep, low = silent) | `15` |
| MAX98357A amp (I2S) | BCLK=`16` LRC=`17` DIN=`18` SD=`13` (low = muted) |
| DS1302 RTC (3-wire bit-bang) | CLK=`2` DAT=`14` CE/RST=`21` |
| MPU6050 IMU (software I2C) | SCL=`39` SDA=`38` |
| Battery ADC (1S Li-ion, 1:1 divider) | `1` (ADC1_CH0) |
| Serial log (115200) | `43`(TX) `44`(RX) |

> To move pins: keys in `components/UI/UI.h`; everything else is documented at the top of each component's `.c`. Avoid GPIO35/36/37 (PSRAM), GPIO48 (onboard RGB) and strapping pins on N16R8.

## Usage

### Build & flash

Environment: Windows + ESP-IDF v5.5.5, USB serial driver installed.

**Option A · ESP-IDF native (recommended)**

```powershell
cd oder
idf.py build
idf.py -p COM9 flash
```

**Option B · esptool direct**

```powershell
python -m esptool --chip esp32s3 -p COM9 -b 460800 --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\oder.bin
```

Artifact: `build/oder.bin` (~1.6 MB).

### Keys

- **Up / Down**: scroll; hold for repeat.
- **OK**: confirm / enter.
- **Long-press OK**: back / exit current screen.
- **Shake** (enable in Settings → Shake): shake up = scroll up, down = scroll down; left/right map to OK / long-OK.
- During a glitch decode, **OK reveals the text instantly**; press again to exit.

### Provisioning & web config

1. Main menu **Network → AP**: hotspot `ESP32ODERAP` starts (random password shown on screen).
2. Join it from a phone → the config page **pops up automatically** (captive portal), or open `http://192.168.4.1/`.
3. Scan & save your WiFi, set weather city and **weather API key (Seniverse)**; the device switches to station mode.
