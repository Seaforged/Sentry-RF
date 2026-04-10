---
name: setup
description: Dev environment setup and commands. Load when setting up the project for the first time or when environment issues arise.
triggers:
  - "setup"
  - "install"
  - "environment"
  - "getting started"
  - "how do I run"
  - "local development"
edges:
  - target: context/stack.md
    condition: when specific technology versions or library details are needed
  - target: context/architecture.md
    condition: when understanding how components connect during setup
last_updated: 2026-04-10
---

# Setup

## Prerequisites

- **PlatformIO** — VS Code extension or CLI (`pip install platformio`)
- **Git**
- **USB drivers** — T3S3 uses native USB CDC (no driver needed on Windows 10+), Heltec V3 uses CP2102
- **Sub-GHz antenna** — 868/915 MHz SMA + U.FL pigtail. **Connect before powering on — transmitting without an antenna damages the SX1262/LR1121.**
- **(Optional)** u-blox M10 GPS module (MAX-M10S or FlyFishRC M10QMC), KY-006 passive piezo buzzer, 18650 Li-Ion cell

## First-time Setup

1. `git clone https://github.com/Seaforged/Sentry-RF.git && cd Sentry-RF`
2. Open in VS Code with the PlatformIO extension, or use `pio run` from CLI
3. Build for your target: `pio run -e t3s3` (or `heltec_v3`, `t3s3_lr1121`)
4. Connect the antenna, then connect the board via USB-C
5. Flash: `pio run -e t3s3 --target upload`
6. Monitor: `pio device monitor -b 115200`
7. First boot runs a ~50s ambient warmup to learn local LoRa infrastructure — don't introduce a test drone during this window

## Environment Variables

This is a firmware project — no environment variables. All configuration is compile-time:

- **`include/board_config.h`** — pins and hardware capabilities
- **`include/sentry_config.h`** — detection thresholds, scoring weights, timing
- **`include/task_config.h`** — FreeRTOS task priorities, stack sizes, core assignments
- **`platformio.ini`** `build_flags` — board selection (`-DBOARD_T3S3`, `-DBOARD_HELTEC_V3`, `-DBOARD_T3S3_LR1121`)

## Common Commands

- `pio run -e t3s3` — build for LilyGo T3S3 SX1262
- `pio run -e heltec_v3` — build for Heltec WiFi LoRa 32 V3
- `pio run -e t3s3_lr1121` — build for LilyGo T3S3 LR1121 (dual-band)
- `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121` — build ALL three (required before any commit)
- `pio run -e t3s3 --target upload --upload-port COM9` — flash T3S3 on COM9
- `pio run -e t3s3_lr1121 --target clean` — clean LR1121 build cache (needed after partition changes)
- `pio device list` — enumerate connected boards and COM ports
- `pio device monitor -b 115200 -p COM9` — serial monitor on COM9

## Common Issues

**T3S3 boot-loops after flashing:** `platformio.ini` is missing `board_build.flash_size = 4MB` and `board_build.partitions = default.csv` for that environment. Both T3S3 variants need these.

**T3S3 upload fails with "Couldn't find a board on the selected port":** Native USB CDC reset is unreliable. Hold BOOT, tap RESET, release BOOT — this puts the board in download mode. Then retry `pio run --target upload`.

**Heltec V3 radio init returns -2 (CHIP_NOT_FOUND):** Missing `radio.setTCXO(1.8)` call before `radio.begin()`. The Heltec has a 1.8V TCXO that must be powered first.

**LR1121 radio init returns -707 (CHIP_NOT_FOUND):** Wrong pin for DIO9 (it's GPIO 36, NOT DIO1 on GPIO 33), or `setTCXO()` called before `beginGFSK()` (the LR1121 SPI isn't live until `beginGFSK()` runs — pass TCXO voltage as the 7th argument instead). See `context/decisions.md`.

**LR1121 `getRSSI()` returns -0.0 dBm forever:** Using `getRSSI()` instead of `LR1121_RSSI::getInstantRSSI()`. The base method returns packet RSSI which is 0 when no packet has been received. See `context/decisions.md`.

**Python serial reader fails during long soak tests:** GPS print rate is too high. `gpsReadTask` should only print once every 5 seconds — check that the rate limiter is still in place in `main.cpp`.

**SD card on T3S3 doesn't register:** Known hardware issue on our V1.3 board. Extensive testing couldn't get the SD slot to initialise reliably. Logs go to serial (captured via `soak_test.py` on the host). SPIFFS is used on Heltec V3 instead.

**Serial output missing during first ~300ms after boot:** T3S3 native USB CDC enumeration takes time. Early `Serial.println` calls are lost. Add a `delay(500)` after `Serial.begin(115200)` if you need to see boot diagnostics.
