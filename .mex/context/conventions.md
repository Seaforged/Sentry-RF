---
name: conventions
description: How code is written in this project — naming, structure, patterns, and style. Load when writing new code or reviewing existing code.
triggers:
  - "convention"
  - "pattern"
  - "naming"
  - "style"
  - "how should I"
  - "what's the right way"
edges:
  - target: context/architecture.md
    condition: when a convention depends on understanding the system structure
last_updated: 2026-04-10
---

# Conventions

## Naming

- **Files:** snake_case for .cpp/.h (`cad_scanner.cpp`, `detection_engine.h`), PascalCase for RadioLib-derived classes only
- **Functions:** camelCase, verb-first (`cadScannerInit`, `scannerSweep`, `detectionEngineUpdate`)
- **Structs:** PascalCase (`CadTap`, `SystemState`, `CadFskResult`)
- **Constants:** SCREAMING_SNAKE_CASE (`PERSISTENCE_MIN_CONSECUTIVE`, `SCORE_WARNING`)
- **GPIO pins:** `PIN_*` prefix, defined ONLY in `board_config.h` (`PIN_LORA_CS`, `PIN_GPS_RX`)
- **Board capability flags:** `HAS_*` prefix (`HAS_TCXO`, `HAS_COMPASS`, `HAS_24GHZ`)

## Structure

- **All pin numbers live in `include/board_config.h`** — never hardcode GPIO numbers in source files. Every file that touches hardware must `#include "board_config.h"`.
- **All detection thresholds live in `include/sentry_config.h`** — `SCORE_ADVISORY`, `PERSISTENCE_MIN_CONSECUTIVE`, `FSK_DETECT_THRESHOLD_DBM`, etc. Field calibration happens by editing this file, not hunting through code.
- **All FreeRTOS task config lives in `include/task_config.h`** — core assignments, priorities, stack sizes. LR1121 needs a 16KB LoRa task stack (larger RadioLib driver).
- **Board-specific code uses `#ifdef BOARD_T3S3 / BOARD_HELTEC_V3 / BOARD_T3S3_LR1121` guards.** Never `#ifdef ESP32` or other framework-level macros.
- **Shared state is protected by `stateMutex`** — copy under lock, process outside lock. Never hold the mutex during SPI transfers or blocking operations.
- **Serial output is protected by `serialMutex`** — prevents interleaved output from concurrent tasks.
- **Keep functions under ~40 lines.** Split if longer. The CAD scanner's scan loop is the exception.
- **Comment the "why", not the "what".** The code shows what; comments explain why a constant is what it is.

## Patterns

**Mutex-protected state access:**
```cpp
// Correct — copy under lock, process outside
GpsData snapGps = {};
if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snapGps = systemState.gps;
    xSemaphoreGive(stateMutex);
}
// process snapGps without holding the mutex

// Wrong — holds mutex during processing
if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    processGps(systemState.gps);  // blocks other tasks
    xSemaphoreGive(stateMutex);
}
```

**Board-specific radio object:**
```cpp
// Correct — declared in main.cpp with guard
#ifdef BOARD_T3S3_LR1121
LR1121_RSSI radio(&radioMod);    // subclass exposes getRssiInst()
#else
SX1262 radio(&radioMod);
#endif
```

**LR1121 instantaneous RSSI (not packet RSSI):**
```cpp
// Correct for LR1121 spectrum sweep
radio.setFrequency(freq);
radio.startReceive();              // required — setFrequency drops to standby
delayMicroseconds(SCAN_DWELL_US);
float rssi = radio.getInstantRSSI(); // custom wrapper on LR1121_RSSI

// Wrong — returns packet RSSI which is 0 if no packet received
float rssi = radio.getRSSI();        // useless for sweeping
```

## Verify Checklist

Before presenting any code:

- [ ] No hardcoded pin numbers — all pins reference `PIN_*` symbols from `board_config.h`
- [ ] Compiles clean for all three targets: `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121`
- [ ] Board-specific code is guarded by `#ifdef BOARD_T3S3 / BOARD_HELTEC_V3 / BOARD_T3S3_LR1121`
- [ ] `stateMutex` held only during copy, never during processing or SPI transfers
- [ ] Serial output guarded by `serialMutex` (except early boot before mutex creation)
- [ ] Any new constants live in `sentry_config.h` (detection) or `board_config.h` (hardware)
- [ ] LR1121 RSSI reads use `getInstantRSSI()` from the `LR1121_RSSI` subclass, not bare `getRSSI()`
- [ ] Antenna must be connected before flashing — document this if the change affects TX power
