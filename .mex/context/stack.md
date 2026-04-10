---
name: stack
description: Technology stack, library choices, and the reasoning behind them. Load when working with specific technologies or making decisions about libraries and tools.
triggers:
  - "library"
  - "package"
  - "dependency"
  - "which tool"
  - "technology"
edges:
  - target: context/decisions.md
    condition: when the reasoning behind a tech choice is needed
  - target: context/conventions.md
    condition: when understanding how to use a technology in this codebase
last_updated: 2026-04-10
---

# Stack

## Core Technologies

- **C++ (Arduino framework)** — no STL containers in tight loops, `const` over `#define` for typed constants
- **PlatformIO** — multi-environment build (`t3s3`, `heltec_v3`, `t3s3_lr1121`), dependency management
- **Espressif32 platform** — ESP32-S3 target, FreeRTOS dual-core, native USB CDC on T3S3
- **FreeRTOS** — 5 tasks pinned across 2 cores, `stateMutex` + `serialMutex` semaphores, `detectionQueue` (depth 10)

## Key Libraries

- **RadioLib v7.6** (jgromes/RadioLib) — SX1262 and LR1121 radio driver. We use `scanChannel()` for CAD, `beginFSK()` / `beginGFSK()` for RSSI sweeps, raw SPI `SetPacketType` (opcode 0x8A) on SX1262 only for mode switching. On LR1121 the raw SPI shortcut corrupts RadioLib internal state — must use full `begin()` / `beginGFSK()` instead.
- **SparkFun u-blox GNSS v3** (not TinyGPS) — UBX binary protocol for M10 modules. Required for jamming/spoofing monitoring which TinyGPS does not support. NAV-SAT parser is stack-hungry.
- **Adafruit SSD1306 + GFX** — OLED rendering at 128x64 monochrome, 400kHz I2C
- **QMC5883LCompass** — magnetometer heading for directional bearing (T3S3 only, via QWIIC)

## What We Deliberately Do NOT Use

- **No LMIC** — we scan for drone signals, we don't transmit LoRaWAN. RadioLib is simpler and supports raw CAD.
- **No TinyGPS / TinyGPSPlus** — NMEA parsers don't expose u-blox jam/spoof indicators
- **No STL `<vector>` / `<map>`** in scan loops — fixed-size arrays (MAX_TAPS=32, MAX_DIVERSITY_SLOTS=32) for deterministic memory
- **No raw SPI packet type switching on LR1121** — RadioLib's LR11x0 driver state machine can't be shortcut. Use `radio.begin()` or `radio.beginGFSK()` even if it's slower.
- **No dynamic allocation in tasks** — all scan buffers are stack-allocated or static file-scope

## Version Constraints

- **RadioLib 7.6.0** — does NOT have the `getRSSI(bool packet, bool skipReceive)` overload for LR11x0. Use the `LR1121_RSSI` subclass in `rf_scanner.h` which exposes the protected `getRssiInst()` method. Upgrading RadioLib may remove this workaround.
- **PlatformIO Espressif32** — `board_build.flash_size = 4MB` and `board_build.partitions = default.csv` are REQUIRED for all T3S3 environments or the board boot-loops.
- **ESP32-S3 native USB CDC** — `-DARDUINO_USB_CDC_ON_BOOT=1` required in `build_flags` for T3S3 variants. Serial output during first 300ms after boot is unreliable — early `Serial.println` may be lost.
