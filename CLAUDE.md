# SENTRY-RF

## Project Overview

SENTRY-RF is an open-source, ESP32-S3-based passive drone RF detector and GNSS jamming/spoofing monitor. It performs sub-GHz spectrum scanning (860-930 MHz), optional 2.4 GHz scanning (LR1121 boards), WiFi-based Remote ID detection (ASTM F3411), and real-time GNSS integrity monitoring using u-blox M10 UBX protocol. It is a personal open-source project (MIT license) under the Seaforged GitHub organization -- not a commercial product.

**Current version:** v1.5.2 (AAD persistence gate + corroborated scoring, 0.00% false positive rate in 15-min soak test, 4/4 detection modes passing, validated April 6 2026)

**Repository:** https://github.com/Seaforged/SENTRY-RF

## Tech Stack

- **Language:** C++ (Arduino framework)
- **Build system:** PlatformIO (multi-environment: `t3s3`, `heltec_v3`, `t3s3_lr1121`)
- **Platform:** Espressif32 (ESP32-S3), FreeRTOS dual-core
- **Key libraries:**
  - RadioLib v7+ (SX1262 and LR1121 LoRa/FSK radio control, spectrum scanning via RSSI reads)
  - SparkFun u-blox GNSS v3 (GPS UBX binary protocol, M10 configuration via VALSET/VALGET)
  - Adafruit SSD1306 + GFX (128x64 OLED display rendering)
  - QMC5883LCompass (magnetometer heading for directional bearing)
- **Test harness:** Python dual-device test scripts against JUH-MAK-IN JAMMER hardware

## Hardware Targets

| Board | PlatformIO Env | Radio | Sub-GHz | 2.4 GHz | SD Card | Compass |
|-------|---------------|-------|---------|---------|---------|---------|
| LilyGo T3S3 (SX1262) | `t3s3` | SX1262 | 860-930 MHz | WiFi only | Yes | Yes (QWIIC) |
| Heltec WiFi LoRa 32 V3 | `heltec_v3` | SX1262 | 860-930 MHz | WiFi only | No (SPIFFS) | No |
| LilyGo T3S3 (LR1121) | `t3s3_lr1121` | LR1121 | 860-930 MHz | 2400-2500 MHz | Yes | Yes (QWIIC) |

All boards use ESP32-S3 with SSD1306 128x64 OLED. GPS is u-blox M10 via UART on all boards. Pin mappings are in `include/board_config.h` -- never hardcode pins in source files.

### Board-Specific Gotchas

- **T3S3:** Uses native USB (`-DARDUINO_USB_CDC_ON_BOOT=1`). I2C is SDA=18/SCL=17. Requires `board_build.flash_size = 4MB` or it boot-loops.
- **Heltec V3:** Must call `radio.setTCXO(1.8)` before `radio.begin()` or SX1262 returns error -2. OLED needs Vext (GPIO 36) set LOW and reset pulse on GPIO 21. I2C is SDA=17/SCL=18 (swapped from T3S3).
- **T3S3 LR1121:** Same pin mapping as T3S3 SX1262. Radio uses `LR1121` RadioLib class (inherits from `LR11x0`). Band switching between sub-GHz and 2.4 GHz is transparent via `radio.setFrequency()`.

## Architecture

### FreeRTOS Dual-Core Task Layout

| Task | Core | Priority | Stack | Purpose |
|------|------|----------|-------|---------|
| `loRaScanTask` | Core 1 | 3 | 8192 | Sub-GHz (+ 2.4 GHz on LR1121) RSSI sweep, detection engine, data logging |
| `gpsReadTask` | Core 0 | 3 | 8192 | u-blox UART drain, GNSS integrity analysis, compass reading |
| `displayTask` | Core 0 | 1 | 8192 | 6-screen OLED UI (7 on LR1121), button input, battery monitoring |
| `alertTask` | Core 0 | 2 | 6144 | Detection event queue consumer, buzzer/LED control |
| `wifiScanTask` | Core 0 | 2 | 8192 | WiFi promiscuous mode, Remote ID capture, channel hopping |

### Shared State

- `SystemState` struct (defined in `include/detection_types.h`) protected by `stateMutex` (FreeRTOS mutex)
- `serialMutex` guards Serial.printf calls across tasks
- `detectionQueue` (depth 10) carries `DetectionEvent` structs from sensor tasks to alert handler
- Pattern: copy under lock, process outside lock

### Radio Architecture

- LoRa radio (SX1262 or LR1121) is on HSPI bus, owned exclusively by `loRaScanTask` on Core 1
- SD card is on FSPI bus (separate from LoRa) -- no SPI contention
- WiFi uses ESP32 internal radio (separate from external LoRa SPI) -- no contention
- Compass is on Wire1 (I2C bus 1, SDA=21/SCL=10), OLED is on Wire (I2C bus 0, SDA=18/SCL=17) -- no contention

### Sub-GHz Scanning

- 350 bins from 860-930 MHz at 200 kHz step
- Per-bin `startReceive()` + `getRSSI(false)` for accurate instantaneous RSSI
- ~2.4 second sweep cycle
- Multi-peak detection in 902-928 MHz ELRS US band with dual threshold (NF+15 dB relative, -85 dBm absolute)

### 2.4 GHz Scanning (LR1121 only)

- 100 bins from 2400-2500 MHz at 1 MHz step
- Alternates with sub-GHz sweep (band switch is transparent via `setFrequency()`)
- Detects ELRS 2.4 GHz, DJI OcuSync/O3/O4, FrSky, Spektrum, and 10+ drone protocols

## Detection Engine

### Threat Levels

| Level | Name | Trigger |
|-------|------|---------|
| 0 | CLEAR | No drone signals detected, GNSS healthy |
| 1 | ADVISORY | Persistent signal on known drone frequency (3+ consecutive sweeps) |
| 2 | WARNING | Drone signal + GNSS anomaly, OR signals on both sub-GHz and 2.4 GHz simultaneously |
| 3 | CRITICAL | Multiple persistent drone signals, OR drone signal + confirmed GNSS jamming/spoofing |

Threat level includes hysteresis (one step per sweep cycle) and 30-second cooldown decay.

### Detection Sources

- **RF sub-GHz:** Peak extraction, drone frequency signature matching (14 protocols), persistence tracking
- **RF 2.4 GHz (LR1121):** Same technique, filters out known WiFi AP beacons
- **WiFi Remote ID:** ASTM F3411 vendor-specific IE parsing (OUI FA:0B:BC), MAC OUI fingerprinting (DJI, Autel, Parrot)
- **GNSS integrity:** u-blox MON-HW jamming indicator, NAV-STATUS spoofing detection, per-satellite C/N0 uniformity analysis

## Key Modules

| Module | Header | Source | Purpose |
|--------|--------|--------|---------|
| RF Scanner | `rf_scanner.h` | `rf_scanner.cpp` | Sub-GHz + 2.4 GHz RSSI sweep engine |
| GPS Manager | `gps_manager.h` | `gps_manager.cpp` | u-blox M10 UART init, UBX parsing, NAV-PVT/SAT/STATUS polling |
| GNSS Integrity | `gnss_integrity.h` | `gnss_integrity.cpp` | Jamming/spoofing detection algorithms, C/N0 analysis |
| Detection Engine | `detection_engine.h` | `detection_engine.cpp` | Threat FSM, peak-to-protocol matching, persistence tracking |
| Drone Signatures | `drone_signatures.h` | `drone_signatures.cpp` | Frequency tables for ELRS, Crossfire, DJI, Tracer, FrSky, etc. |
| WiFi Scanner | `wifi_scanner.h` | `wifi_scanner.cpp` | Promiscuous mode Remote ID capture, MAC OUI matching |
| WiFi Dashboard | `wifi_dashboard.h` | `wifi_dashboard.cpp` | HTTP AP server with JSON API (toggle via 5-second BOOT hold) |
| Display | `display.h` | `display.cpp` | 6/7-screen OLED UI with custom boot splash |
| Alert Handler | `alert_handler.h` | `alert_handler.cpp` | Detection event queue consumer, LED + buzzer dispatch |
| Buzzer Manager | `buzzer_manager.h` | `buzzer_manager.cpp` | Non-blocking LEDC PWM tone patterns (7 patterns), mute/ACK |
| Data Logger | `data_logger.h` | `data_logger.cpp` | CSV logging to SD card (T3S3) or SPIFFS (Heltec, rotates at 100KB) |
| Compass | `compass.h` | `compass.cpp` | QMC5883L magnetometer heading, peak bearing tracking, calibration |
| Board Config | `board_config.h` | -- | All pin mappings and board capability flags |
| Task Config | `task_config.h` | -- | FreeRTOS core assignments, priorities, stack sizes |
| Detection Types | `detection_types.h` | -- | SystemState struct, ThreatLevel enum, DetectionEvent, queue/mutex externs |
| Version | `version.h` | -- | Firmware version string (`FW_VERSION`, `FW_NAME`, `FW_DATE`) |
| Splash Logo | `splash_logo.h` | -- | Custom boot logo bitmap (128x64 monochrome PROGMEM) |

## Known Limitations (v1.2.1)

- **LED disabled:** Ambient 868/915 MHz ISM traffic causes false escalation on bench. LED will be re-enabled after field testing with real drones establishes proper thresholds.
- **Single antenna:** No true angle-of-arrival direction finding. Compass + peak bearing tracking provides manual rotation-based approach only (~45 degree resolution).
- **SX1262 boards:** Cannot scan 2.4 GHz. Upgrade to LR1121 board for dual-band coverage.
- **WiFi scanner vs dashboard:** Cannot run both simultaneously. Default is scanner mode; dashboard requires manual 5-second BOOT button hold.
- **C/N0 threshold:** Production setting (15 dB-Hz) optimized for outdoor use. Lower to 6 for indoor bench testing.
- **No 5.8 GHz:** Cannot detect 5.8 GHz video links on any board.

## Roadmap

- LR1121 2.4 GHz hardware validation (board en route)
- QMC5883L compass field testing
- Detection threshold tuning against real drones (false alarm reduction)
- LED alert re-enablement after field-calibrated thresholds
- Multi-device mesh networking
- 3D-printed field enclosure design
- Deep sleep power management for long deployments
- Watchdog monitoring for all FreeRTOS tasks

See `docs/SENTRY-RF_UPDATED_ROADMAP.md` for the full sprint-by-sprint roadmap (Sprints 8-11).

## Development Conventions

- All code must compile for ALL three board targets: `pio run -e t3s3`, `pio run -e heltec_v3`, `pio run -e t3s3_lr1121`
- Never hardcode pin numbers -- always use symbols from `board_config.h`
- Use `#include "board_config.h"` in every file that touches hardware
- Prefer `const` over `#define` for typed constants
- Keep functions under ~40 lines; split if longer
- Comment the "why", not the "what"
- Test every new feature on serial monitor before touching OLED display
- Commit after each acceptance criterion passes
- Board-specific code uses `#ifdef BOARD_T3S3` / `#elif defined(BOARD_HELTEC_V3)` / `#elif defined(BOARD_T3S3_LR1121)` guards

## Build & Run

### Prerequisites

- PlatformIO (VS Code extension or CLI: `pip install platformio`)
- Git
- USB drivers (T3S3 uses native USB; Heltec V3 uses CP2102)

### Build

```bash
# Clone
git clone https://github.com/Seaforged/Sentry-RF.git
cd Sentry-RF

# Build for your board
pio run -e t3s3          # LilyGo T3S3 (SX1262)
pio run -e heltec_v3     # Heltec WiFi LoRa 32 V3
pio run -e t3s3_lr1121   # LilyGo T3S3 (LR1121 dual-band)
```

### Flash & Monitor

```bash
pio run -e t3s3 --target upload
pio device monitor -b 115200
```

If upload fails, hold BOOT while pressing RESET to enter download mode.

**Always connect the antenna before powering on.** Transmitting without an antenna can damage the SX1262/LR1121 radio module.

### Expected Boot Output

```
========== SENTRY-RF v1.2.1 ==========
[BOOT] Boot #1
[OLED] OK
[SCAN] FSK mode ready, 350 bins, 860.0-930.0 MHz
[GPS] FAIL: no response at 9600 or 38400    <- normal if no GPS connected
[INIT] GPS init failed - continuing without GPS
[WIFI] Promiscuous scanner active - channel hopping
[COMPASS] Not detected - continuing without compass
[INIT] FreeRTOS tasks launched - LoRa:Core1, GPS+WiFi:Core0
[SCAN] Peak: 881.2 MHz @ -72.5 dBm (2407 ms) | Threat: CLEAR
```

### WiFi Dashboard

Hold BOOT button 5 seconds to switch to dashboard mode:
- SSID: `SENTRY-RF`, Password: `SentryP@ssword`
- Dashboard: http://192.168.4.1
- JSON API: http://192.168.4.1/api/status

## File Structure

```
Sentry-RF/
  platformio.ini              # 3 build envs: t3s3, heltec_v3, t3s3_lr1121
  include/
    board_config.h            # Pin mappings + capability flags for all boards
    task_config.h             # FreeRTOS core/priority/stack configuration
    detection_types.h         # SystemState, ThreatLevel, DetectionEvent, mutexes
    version.h                 # Firmware version string
    splash_logo.h             # Boot logo bitmap (128x64 monochrome)
    rf_scanner.h              # Sub-GHz + 2.4 GHz scan structs and functions
    gps_manager.h             # GpsData struct, init/process/update functions
    gnss_integrity.h          # IntegrityStatus struct, jamming/spoofing detection
    detection_engine.h        # Threat FSM update function
    drone_signatures.h        # Protocol frequency table declarations
    wifi_scanner.h            # Promiscuous mode Remote ID, DroneOUI struct
    wifi_dashboard.h          # HTTP AP web server
    display.h                 # Screen rendering functions
    alert_handler.h           # Alert task, mute/ACK interface
    buzzer_manager.h          # Non-blocking tone pattern player (7 patterns)
    data_logger.h             # SD/SPIFFS CSV logging
    compass.h                 # QMC5883L heading + peak bearing tracking
  src/
    main.cpp                  # Hardware init, FreeRTOS task creation, Arduino entry
    rf_scanner.cpp            # RSSI sweep engine (sub-GHz + 2.4 GHz on LR1121)
    gps_manager.cpp           # u-blox M10 UART/UBX protocol handling
    gnss_integrity.cpp        # Jamming/spoofing detection algorithms
    detection_engine.cpp      # Threat FSM + drone protocol matching
    drone_signatures.cpp      # Frequency tables (14 protocols)
    wifi_scanner.cpp          # WiFi promiscuous + ASTM F3411 IE parsing
    wifi_dashboard.cpp        # AP mode web server with JSON API
    display.cpp               # 6/7-screen OLED UI
    alert_handler.cpp         # Detection queue consumer, buzzer/LED dispatch
    buzzer_manager.cpp        # LEDC PWM tone patterns
    data_logger.cpp           # CSV logging (SD or SPIFFS)
    compass.cpp               # Magnetometer read + calibration
  docs/
    BUILD_GUIDE.md            # Bill of materials, wiring, flashing instructions
    DEV_CONTEXT.md            # Developer context, architecture, gotchas
    SENTRY-RF_UPDATED_ROADMAP.md  # Sprint 8-11 roadmap
  LICENSE                     # MIT
  README.md                   # Project overview and capabilities
  CLAUDE.md                   # This file
```

## Required Skills & Workflow

When working on this project, invoke the appropriate skills:

- **embedded-systems** -- Use for ALL firmware development work (C++ Arduino, PlatformIO builds, FreeRTOS task architecture, ESP32 peripherals, SPI/I2C/UART configuration, GPIO management)
- **rf-fundamentals** -- Use for RF scanning logic, antenna considerations, spectrum analysis, RSSI interpretation, frequency planning, drone signal characterization, and any radio hardware work
- **seaforged-brand** -- Use when creating any branded materials, logos, visual assets, or public-facing content for the Seaforged organization
- **scientific-visualization / matplotlib** -- Use for creating detection metric visualizations, spectrum plots, RSSI heatmaps, C/N0 distribution charts, and any data analysis figures from logged CSV data
- **document-skills** -- Use for writing user guides, datasheets, build documentation, and any formatted documents (PDF, DOCX, PPTX) related to the project
