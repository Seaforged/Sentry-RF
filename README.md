# SENTRY-RF

**Open-source passive drone RF detector + GNSS jamming/spoofing monitor**

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![Platform: ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue.svg)
![Version: v1.1.0](https://img.shields.io/badge/Version-v1.1.0-orange.svg)

## What It Does

- **Sub-GHz Spectrum Scanning** (860-930 MHz): Sweeps 700 frequency bins in ~460ms, detects ELRS, Crossfire, and other drone control links with protocol identification and channel matching
- **2.4 GHz Dual-Band Scanning** (LR1121 boards): Adds 2400-2500 MHz sweep for ELRS 2.4, DJI OcuSync/O3/O4, FrSky, Spektrum, and 10+ drone protocols
- **GNSS Integrity Monitoring**: u-blox M10 GPS with UBX protocol — monitors jamming indicator, spoofing detection state, per-satellite C/N0, and AGC levels in real-time
- **WiFi Remote ID Detection**: ESP32 promiscuous mode captures drone beacon frames, matches manufacturer MAC OUI prefixes (DJI, Autel, Parrot), detects ASTM F3411 Remote ID

## Threat Levels

| Level | Name | Trigger |
|-------|------|---------|
| 0 | **CLEAR** | No drone signals detected, GNSS healthy |
| 1 | **ADVISORY** | Persistent signal on a known drone frequency (3+ consecutive sweeps) |
| 2 | **WARNING** | Drone signal + GNSS anomaly, OR signals on both sub-GHz and 2.4 GHz simultaneously |
| 3 | **CRITICAL** | Multiple persistent drone signals, OR drone signal + confirmed GNSS jamming/spoofing |

Threat level includes hysteresis (one step per sweep cycle) and 30-second cooldown decay.

## Supported Hardware

| Board | Radio | Sub-GHz | 2.4 GHz | GPS | SD Card | OLED |
|-------|-------|---------|---------|-----|---------|------|
| LilyGo T3S3 | SX1262 | 860-930 MHz | WiFi only | UART | Yes | 128x64 |
| Heltec WiFi LoRa 32 V3 | SX1262 | 860-930 MHz | WiFi only | UART | No (SPIFFS) | 128x64 |
| LilyGo T3S3 LR1121 | LR1121 | 860-930 MHz | 2400-2500 MHz | UART | Yes | 128x64 |

All boards use ESP32-S3 with FreeRTOS dual-core: LoRa scanning on Core 1, GPS + WiFi on Core 0.

## Hardware Requirements

**Required:**
- One of the supported ESP32-S3 boards listed above
- u-blox M10 GPS module (MAX-M10S or NEO-M10S) — connected via UART

**Optional:**
- QMC5883L compass module — connected via QWIIC/I2C for RF direction finding (T3S3 boards only)
- SD card — for CSV data logging (T3S3 boards)

### Wiring — GPS Module (FlyFishRC M10QMC or similar)

| GPS Module Pin | T3S3 GPIO | Heltec V3 GPIO | Notes |
|----------------|-----------|----------------|-------|
| TX | 44 (RX) | 46 (RX) | GPS transmit → ESP32 receive |
| RX | 43 (TX) | 45 (TX) | GPS receive ← ESP32 transmit |
| SDA | 21 (Wire1) | N/A | Compass I2C — T3S3 only |
| SCL | 10 (Wire1) | N/A | Compass I2C — T3S3 only |
| VCC | 3.3V | 3.3V | |
| GND | GND | GND | |

If using a GPS module without a built-in compass, only TX/RX/VCC/GND are needed.

## Quick Start

```bash
# Clone
git clone https://github.com/Ndwoo10/Sentry-RF.git
cd Sentry-RF

# Build for your board
pio run -e t3s3          # LilyGo T3S3 (SX1262)
pio run -e heltec_v3     # Heltec WiFi LoRa 32 V3
pio run -e t3s3_lr1121   # LilyGo T3S3 (LR1121 dual-band)

# Flash
pio run -e t3s3 --target upload

# Monitor
pio device monitor -b 115200
```

## OLED Screens

The device has 6 screens (7 on LR1121), cycled by the BOOT button or auto-rotating every 5 seconds:

1. **Dashboard** — System overview: threat level, mini spectrum, GPS status, battery, WiFi mode
2. **Sub-GHz Spectrum** — 860-930 MHz bar chart with peak frequency/RSSI
3. **GPS** — Fix type, satellite count, coordinates, accuracy, compass heading
4. **GNSS Integrity** — Jamming state, spoofing detection, C/N0 standard deviation, AGC
5. **Threat Detail** — Current threat level, active detections, peak bearing
6. **System** — Firmware version, uptime, free heap, compass status
7. **2.4 GHz Spectrum** *(LR1121 only)* — 2400-2500 MHz bar chart

## WiFi Dashboard

Hold the BOOT button for 5 seconds to switch from WiFi scanner mode to dashboard mode.

- **SSID:** `SENTRY-RF`
- **Password:** `SentryP@ssword`
- **Dashboard:** http://192.168.4.1
- **JSON API:** http://192.168.4.1/api/status

Power cycle to return to WiFi scanner mode.

## Detection Capabilities

### What It CAN Detect
- ELRS 868/915 MHz and 2.4 GHz control links
- TBS Crossfire 868/915 MHz control links
- TBS Tracer, FrSky, Spektrum, Futaba, FlySky, ImmersionRC Ghost (2.4 GHz, LR1121 only)
- DJI OcuSync/O3/O4 approximate channel presence (2.4 GHz, LR1121 only)
- WiFi-based drone Remote ID (ASTM F3411) beacon frames
- DJI, Autel, Parrot controller WiFi signatures via MAC OUI
- GNSS jamming (via u-blox MON-HW jamming indicator)
- GNSS spoofing (via u-blox NAV-STATUS spoofing detection + C/N0 uniformity analysis)

### What It CANNOT Detect
- DJI OcuSync/O3/O4 on SX1262 boards (requires LR1121 for 2.4 GHz)
- 5.8 GHz video links (no hardware support on any board)
- Non-compliant drones without Remote ID transmitters
- Frequency-hopping spread spectrum content (detects energy, not protocol data)
- Drones using encrypted or proprietary protocols at very low power

## Known Limitations

- **Single antenna**: No true angle-of-arrival direction finding. The compass + peak bearing tracking provides a manual rotation-based approach.
- **SX1262 boards**: Limited to sub-GHz (860-930 MHz). Upgrade to LR1121 for 2.4 GHz coverage.
- **C/N0 threshold tradeoff**: Production setting (15 dB-Hz) optimized for outdoor use. Lower to 6 for indoor bench testing.
- **WiFi scanner vs dashboard**: Cannot run both simultaneously. Default is scanner mode; dashboard requires manual activation.

## Data Logging

CSV data is logged automatically:
- **T3S3 boards**: SD card (`/log_NNNN.csv`, new file each boot)
- **Heltec V3**: SPIFFS (`/log.csv`, rotates at 100KB)

Columns: `timestamp_ms, sweep_num, threat_level, peak_freq_mhz, peak_rssi_dbm, gps_lat, gps_lon, fix_type, num_sv, jam_ind, spoof_state, cno_stddev`

## Roadmap

- [ ] LR1121 2.4 GHz hardware validation (board en route)
- [ ] QMC5883L compass field testing
- [ ] Live drone detection validation (ELRS + Crossfire)
- [ ] OpenDroneID full frame parsing (ASTM F3411)
- [ ] Multi-device mesh networking
- [ ] 3D-printed field enclosure design

## Contributing

Issues and pull requests welcome. This is a personal open-source project (MIT license) — not a commercial product.

## License

MIT License. See [LICENSE](LICENSE) for details.
