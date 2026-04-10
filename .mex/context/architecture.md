---
name: architecture
description: How the major pieces of this project connect and flow. Load when working on system design, integrations, or understanding how components interact.
triggers:
  - "architecture"
  - "system design"
  - "how does X connect to Y"
  - "integration"
  - "flow"
edges:
  - target: context/stack.md
    condition: when specific technology details are needed
  - target: context/decisions.md
    condition: when understanding why the architecture is structured this way
last_updated: 2026-04-10
---

# Architecture

## System Overview

FreeRTOS dual-core pipeline. Every ~2.5s, `loRaScanTask` on Core 1 runs a CAD sweep (SF6-SF12, rotating channels) → RSSI-guided CAD on elevated US-band bins → broad CAD scan → (LR1121 only) 2.4 GHz CAD at BW800 → switch to FSK for Crossfire scan → RSSI sweep every 3rd cycle → CAD results feed the Detection Engine which computes a weighted confidence score → threat FSM updates `systemState.threatLevel` → `alertTask` on Core 0 consumes detection events, drives LED + buzzer → `displayTask` on Core 0 reads `systemState` under mutex and renders the current OLED screen at 2 Hz.

Concurrently: `gpsReadTask` on Core 0 drains the u-blox M10 UART continuously, parses UBX messages (NAV-PVT, NAV-SAT, NAV-STATUS, MON-HW), and updates `systemState.gps` + `systemState.integrity`. `wifiScanTask` on Core 0 runs WiFi promiscuous mode, captures ASTM F3411 Remote ID beacons, and writes to `systemState`.

## Key Components

- **`loRaScanTask`** (Core 1, prio 3, 8KB stack — 16KB on LR1121) — Owns LoRa SPI exclusively. Runs CAD scanner, RSSI sweeper, calls Detection Engine, writes `systemState.spectrum` + `systemState.threatLevel` under `stateMutex`.
- **`gpsReadTask`** (Core 0, prio 3, 8KB stack) — Owns GPS UART. Parses UBX binary, runs integrity analysis (C/N0 uniformity, jam indicator, spoof state), writes `systemState.gps` + `systemState.integrity`. Prints status every 5s (rate-limited).
- **`displayTask`** (Core 0, prio 1, 8KB stack) — Renders 6 OLED screens (7 on LR1121 with 2.4 GHz spectrum). Reads `systemState` under `stateMutex`, writes nothing. Rotates on BOOT button short press.
- **`alertTask`** (Core 0, prio 2, 6KB stack) — Consumes `detectionQueue` (depth 10), drives buzzer + LED based on threat level. Handles mute (3s BOOT hold) and ACK (1s BOOT hold).
- **`wifiScanTask`** (Core 0, prio 2, 8KB stack) — WiFi promiscuous mode for Remote ID capture, channel hopping. Uses ESP32 internal radio (separate from LoRa SPI).
- **Detection Engine** (`src/detection_engine.cpp`) — Stateful. Tracks persistent CAD taps, computes frequency diversity over a 3s window, applies AAD persistence gate (5 consecutive high-diversity cycles), computes weighted confidence score.
- **CAD Scanner** (`src/cad_scanner.cpp`) — SF6-SF12 LoRa CAD with rotating channel plans per SF, ambient warmup filter, frequency diversity tracker with `consecutiveHits >= 2` gate.
- **GNSS Integrity** (`src/gnss_integrity.cpp`) — u-blox MON-HW jam indicator, NAV-STATUS spoof state, per-satellite C/N0 uniformity (detects spoofing via unnaturally uniform signal strength).

## External Dependencies

- **RadioLib v7.6** — SX1262 and LR1121 radio control. We use CAD (`scanChannel`), GFSK RSSI sweeps, raw SPI packet type switches on SX1262 only. LR1121 needs the `LR1121_RSSI` subclass for instantaneous RSSI (protected method `getRssiInst()`).
- **SparkFun u-blox GNSS v3** — UBX binary parsing for M10 modules. We use NAV-PVT, NAV-SAT, NAV-STATUS, MON-HW. Parser is stack-hungry (hence 8KB stack on GPS task).
- **Adafruit SSD1306 + GFX** — 128x64 OLED rendering on I2C. Different I2C pins per board (SDA/SCL swap between T3S3 and Heltec V3).
- **ESP32 Arduino framework** (Espressif32 platform via PlatformIO) — FreeRTOS, WiFi, BLE (not yet used), LEDC PWM for buzzer, HSPI/FSPI for LoRa + SD card.

## What Does NOT Exist Here

- **No 5.8 GHz support** — LR1121 max frequency is 2.5 GHz. DJI OcuSync video and analog FPV 5.8 GHz require separate hardware.
- **No direction finding** — single omni antenna. Compass + manual rotation provides ~45° bearing estimate only.
- **No 2.4 GHz CAD scanning on SX1262 boards** — only the LR1121 build has dual-band CAD. SX1262 boards detect 2.4 GHz drones via WiFi Remote ID only.
- **No SD card logging on T3S3 in practice** — hardware testing shows the SD slot doesn't register reliably. Logs go to serial only. SPIFFS is used on Heltec V3.
- **No BLE Remote ID yet** — hardware supports it (ESP32-S3 BLE), code is not written.
- **No cellular or network-level drone detection** — RF-only project.
