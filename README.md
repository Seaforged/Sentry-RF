![SENTRY-RF](SENTRY-RF%20Logo.png)

# SENTRY-RF

**Passive drone RF detection + GNSS jamming/spoofing monitor for ESP32-S3**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![GitHub release](https://img.shields.io/github/v/tag/Seaforged/Sentry-RF?label=release)](https://github.com/Seaforged/Sentry-RF/tags)

SENTRY-RF is an open-source counter-UAS trip-wire that detects drone control links and monitors GPS integrity using low-cost ESP32 hardware. No SDR required, no laptop required, no subscription. Built by [Seaforged](https://seaforged.io), a veteran-owned company.

---

## What It Does

**RF Drone Detection** — Passive scanning across 860–930 MHz (sub-GHz) and 2400–2500 MHz (2.4 GHz on LR1121 boards). Detects ExpressLRS, TBS Crossfire, SiK telemetry, mLRS, and other drone control links by matching FHSS patterns against known protocol signatures. Protocol-agnostic energy detection catches custom and encrypted links too.

**WiFi Remote ID** — Parses ASTM F3411 WiFi Beacon frames in real-time using the ESP32-S3's built-in WiFi in promiscuous mode. Identifies drones broadcasting FAA-mandated Remote ID with no extra hardware.

**GNSS Integrity Monitoring** — Reads u-blox M10 jamming indicators, spoofing detection state, and per-satellite C/N0 uniformity via UBX binary protocol. Emits standalone alerts on rising GNSS threat level (jam / spoof / position-jump / C/N0 uniformity anomaly); the candidate engine additionally uses GNSS anomalies as a confirm-score boost when RF detection has been active in the last 30 s. Position-jump detection compares consecutive NAV-PVT reports for >100 m teleports with tight hAcc — a signature of rebroadcast spoofing.

**Threat Classification** — Correlates RF detections with GNSS anomalies to produce actionable threat levels: `CLEAR → ADVISORY → WARNING → CRITICAL`. Buzzer alerts on WARNING+.

---

## Detection Performance (Bench Validated)

Tested against [Juh-Mak-In Jammer](https://github.com/Seaforged/Juh-Mak-In-Jammer) v2.0.0 drone signal emulator, April 2026:

| Protocol | Time to WARNING | Time to CRITICAL |
|---|---|---|
| ELRS 200Hz FCC915 | 3–12 s | 12–20 s |
| ELRS 25Hz FCC915 | ~71 s | — |
| TBS Crossfire FSK | ~34 s | — |
| SiK Telemetry | ~6 s | — |
| WiFi Remote ID | ~24 s | ~83 s |
| LoRaWAN (false positive test) | — stays ADVISORY | PASS |
| Meshtastic (false positive test) | — stays ADVISORY | PASS |

Field tested (v1.5.3, SX1262): 842m WARNING, 1022m ADVISORY against 158mW ELRS transmitter in suburban environment.

---

## Supported Hardware

| Board | Radio | Bands | Display | Status |
|---|---|---|---|---|
| **LilyGo T3S3** | Semtech SX1262 | 860–930 MHz | SSD1306 128×64 OLED | ✅ Field tested |
| **LilyGo T3S3 LR1121** | Semtech LR1121 | 860–930 MHz + 2400–2500 MHz | SSD1306 128×64 OLED | ✅ Bench validated |
| **Heltec WiFi LoRa 32 V3** | Semtech SX1262 | 860–930 MHz | SSD1306 128×64 OLED | ✅ Builds clean |

**GPS:** u-blox M10 module (HGLRC M100 Mini, FlyFishRC M10QMC, or similar) connected via UART. Auto-detects baud rate (115200/38400/9600).

**Optional:** QMC5883L compass for signal bearing, passive piezo buzzer for alerts, SD card for logging.

---

## Quick Start

### Prerequisites
- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- One of the supported boards
- u-blox M10 GPS module + 4 jumper wires

### Build and Flash

```bash
git clone https://github.com/Seaforged/Sentry-RF.git
cd Sentry-RF

# Build for your board
pio run -e t3s3            # LilyGo T3S3 (SX1262)
pio run -e t3s3_lr1121     # LilyGo T3S3 LR1121 (dual-band)
pio run -e heltec_v3       # Heltec WiFi LoRa 32 V3

# Flash
pio run -e t3s3 --target upload

# Monitor serial output
pio device monitor
```

### GPS Wiring

| GPS Pin | T3S3 SX1262 | T3S3 LR1121 | Heltec V3 |
|---|---|---|---|
| TX → board RX | GPIO 44 | GPIO 43 | GPIO 46 |
| RX → board TX | GPIO 43 | GPIO 44 | GPIO 45 |
| VCC | 3.3V | 3.3V (QWIIC) | 3.3V |
| GND | GND | GND (QWIIC) | GND |

> **Note:** LR1121 QWIIC connector reverses data pins compared to the SX1262 sister board.

---

## OLED Display

7 screens, cycled via BOOT button press:

| Screen | Content |
|---|---|
| **Dashboard** | Threat level, GPS status, WiFi channel activity chart, peak RF signal, battery |
| **Sub-GHz Spectrum** | 860–930 MHz waterfall with peak frequency/power |
| **GPS** | Fix type, satellites, PDOP, coordinates, altitude, accuracy |
| **GNSS Integrity** | Jamming state, jamming indicator, spoofing detection, C/N0 deviation |
| **Threat Detail** | RF peak, GPS+jam+spoof status, buzzer state, bearing |
| **System** | Version, uptime, heap, buzzer/compass status |
| **2.4 GHz Spectrum** | 2400–2500 MHz waterfall (LR1121 boards only) |

See [`docs/OLED_Screen_Glossary.md`](docs/OLED_Screen_Glossary.md) for field-by-field reference.

---

## Architecture

- **FreeRTOS dual-core:** LoRa scanning on Core 1, GPS/WiFi on Core 0
- **Detection engine:** CAD (Channel Activity Detection) for LoRa preamble detection + RSSI sweep for energy detection + FHSS frequency-spread tracker for hopping signal classification
- **Protocol classifier:** Matches detected signals against 220+ drone protocol signatures
- **Ambient discrimination:** Learns background RF during warmup, filters LoRaWAN/Meshtastic/smart meter infrastructure from drone detections
- **Threat scoring:** Weighted combination of CAD confidence, frequency diversity, persistence, velocity, RSSI, and GNSS integrity

---

## Companion Projects

| Project | Description |
|---|---|
| [Juh-Mak-In Jammer](https://github.com/Seaforged/Juh-Mak-In-Jammer) | Drone signal emulator for testing SENTRY-RF. Emulates ELRS, Crossfire, SiK, mLRS, Remote ID, and infrastructure protocols. |
| **D-TECT-R** | Commercial counter-UAS product (separate from SENTRY-RF) |
| **TESTKIT-GPS** | GPS spoofing/jamming test tool using AntSDR E200 (planned) |

---

## Project Status

**v1.6.1-rc1** — Bench validated, April 2026.

Recent milestones:
- LR1121 dual-band board fully operational (CAD detection fix, 2.4 GHz bandwidth fix)
- FHSS frequency-spread tracker for hopping signal detection
- WiFi Remote ID detection via ESP32-S3 promiscuous scanning
- GPS auto-baud detection (115200/38400/9600)
- Boot-time antenna self-test with soft-warn
- 7-screen OLED UI with WiFi channel activity dashboard

Known limitations:
- mLRS detection stays at ADVISORY (narrow hop set, tuning gap)
- LR1121 ambient warmup list saturates in noisy RF environments
- 2.4 GHz CAD BW800 fix untested (needs 2.4 GHz transmitter)
- De-escalation after signal stops takes ~15 seconds (by design)

See [`docs/SENTRY-RF_Known_Issues_Tracker.md`](docs/SENTRY-RF_Known_Issues_Tracker.md) for full backlog.

---

## Documentation

| Document | Description |
|---|---|
| [`docs/OLED_Screen_Glossary.md`](docs/OLED_Screen_Glossary.md) | Field-by-field reference for every OLED display element |
| [`docs/Protocol_RF_Reference.md`](docs/Protocol_RF_Reference.md) | 220+ drone protocol signatures |
| [`docs/SENTRY-RF_Known_Issues_Tracker.md`](docs/SENTRY-RF_Known_Issues_Tracker.md) | Known issues and backlog |
| [`CLAUDE.md`](CLAUDE.md) | Claude Code project context |

---

## License

[MIT](LICENSE) — use it however you want.

## Contributing

Issues and PRs welcome. This is built in public by a veteran-owned company. If you see something that could be better, open an issue.

## Contact

- **Website:** [seaforged.io](https://seaforged.io)
- **GitHub:** [github.com/Seaforged](https://github.com/Seaforged)
