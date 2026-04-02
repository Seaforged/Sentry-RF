![SENTRY-RF Splash Screen](SENTRY-RF%20Logo.png)

# SENTRY-RF

**Passive drone RF detection + GNSS jamming/spoofing monitor for ESP32-S3**

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![Platform: ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue.svg)
![Version: v1.4.0](https://img.shields.io/badge/Version-v1.4.0-orange.svg)
![Field Tested](https://img.shields.io/badge/Field_Tested-637m_at_158mW-brightgreen.svg)

## What It Does

SENTRY-RF is a pocket-sized ($25-75 BOM) passive drone detector that identifies FHSS drone control links by measuring **frequency diversity** -- the number of distinct frequencies producing LoRa CAD hits within a sliding time window. Drones hop across dozens of frequencies per second; infrastructure LoRa uses 1-3 fixed channels. The spread is the signature.

- **Frequency Diversity Detection**: Counts distinct CAD-hit frequencies in a 3-second window. Baseline div=0-2, drone div=3-14. Clean separation enables fast, false-alarm-free alerting
- **CAD-First Scan Pipeline**: 121-channel LoRa CAD scan every ~1 second across SF6-SF12/BW500, with RSSI-guided priority scanning on elevated bins
- **Field-Validated Performance**: 89% detection probability at 2-200m through metal buildings (10 mW), 637m max detection while driving (158 mW). See [field test results](docs/FIELD_TEST_RESULTS_2026-04-01.md)
- **Sub-GHz Spectrum Scanning** (860-930 MHz): 350 bins at 200 kHz step, protocol matching across 14 drone protocols
- **GNSS Integrity Monitoring**: u-blox M10 GPS -- jamming detection, spoofing detection, per-satellite C/N0 uniformity
- **WiFi Remote ID Detection**: ASTM F3411 vendor-specific IE parsing from any drone
- **Rapid-Clear**: Threat drops to CLEAR within seconds of the drone leaving (4 clean cycles)

## Field Test Results (April 2026)

Tested in rural North Carolina against [JUH-MAK-IN JAMMER](https://github.com/Seaforged/Juh-Mak-In-Jammer) ELRS transmitter:

| Test | TX Power | Range | Pd (div>=3) | Max div |
|------|----------|-------|-------------|---------|
| Compound walk (NLOS) | 10 mW | 2-200m | **89%** | 9 |
| Rural road drive | 158 mW | 0-637m | **53%** | 14 |
| Baseline (no TX) | -- | -- | 0% false | max 2 |

**Key finding**: A $25 passive detector reliably detects a 10 mW ELRS drone signal through metal buildings at 200m, and a 158 mW signal at 637m while driving. Zero false alarms in rural environments.

Full results: [docs/FIELD_TEST_RESULTS_2026-04-01.md](docs/FIELD_TEST_RESULTS_2026-04-01.md)

## Threat Levels

| Level | Name | Trigger | Typical Time |
|-------|------|---------|--------------|
| 0 | **CLEAR** | No drone signals, GNSS healthy | -- |
| 1 | **ADVISORY** | Any CAD diversity, or RSSI persistence | ~2s |
| 2 | **WARNING** | 3+ distinct frequencies in 3s window | **~3-5s** |
| 3 | **CRITICAL** | 5+ distinct frequencies, or WARNING + RSSI + GNSS | ~10-15s |

Includes hysteresis (one step per cycle), 15-second cooldown decay, and rapid-clear (4 consecutive clean cycles = immediate CLEAR).

## Supported Hardware

| Board | PIO Env | Radio | Sub-GHz | 2.4 GHz | Est. BOM |
|-------|---------|-------|---------|---------|----------|
| LilyGo T3S3 v1.3 | `t3s3` | SX1262 | 860-930 MHz | WiFi only | ~$25 |
| Heltec WiFi LoRa 32 V3 | `heltec_v3` | SX1262 | 860-930 MHz | WiFi only | ~$20 |
| LilyGo T3S3 LR1121 | `t3s3_lr1121` | LR1121 | 860-930 MHz | 2400-2500 MHz | ~$35 |

All boards: ESP32-S3, FreeRTOS dual-core, 128x64 OLED, u-blox M10 GPS via UART.

## Quick Start

```bash
git clone https://github.com/Seaforged/Sentry-RF.git
cd Sentry-RF

pio run -e t3s3              # Build
pio run -e t3s3 -t upload    # Flash
pio device monitor -b 115200 # Monitor — watch for div=N in [CAD] lines
```

## Hardware Requirements

**Required:** One supported ESP32-S3 board + u-blox M10 GPS module

**Optional:** QMC5883L compass (QWIIC/I2C), SD card for logging

### GPS Wiring

| GPS Pin | T3S3 GPIO | Heltec V3 GPIO |
|---------|-----------|----------------|
| TX | 44 (RX) | 46 (RX) |
| RX | 43 (TX) | 45 (TX) |
| VCC | 3.3V | 3.3V |
| GND | GND | GND |

## Detection Architecture

```
Every ~1 second:
  Phase 1:   Re-check active LoRa taps + adjacent channels
  Phase 1.5: RSSI-guided CAD on elevated US-band bins
  Phase 2:   Broad 121-channel CAD scan (SF6:60, SF7:30, SF8-12)
  Phase 3:   FSK scan on Crossfire channels (infrastructure, disabled)
  -> Frequency diversity count -> Threat assessment -> Rapid-clear check

Every 3rd cycle:
  Full 350-bin RSSI sweep (860-930 MHz)
```

The **frequency diversity score** (`div=N` in serial output) is the primary FHSS discriminator. Infrastructure LoRa hits 1-3 fixed frequencies regardless of time. A drone's FHSS hits many different frequencies every second. Short 3-second window prevents ambient accumulation.

## Detection Capabilities

### Detects
- **Sub-GHz LoRa** (CAD): ELRS 868/915, Crossfire LoRa, mLRS -- modulation-confirmed
- **Sub-GHz FSK** (RSSI): Crossfire 150Hz, FrSky R9 -- energy + frequency match
- **2.4 GHz** (LR1121 only): ELRS 2.4, DJI OcuSync/O3/O4, FrSky, Spektrum, +10 protocols
- **WiFi Remote ID**: ASTM F3411 beacons (vendor-agnostic)
- **GNSS Jamming/Spoofing**: u-blox MON-HW + NAV-STATUS + C/N0 analysis

### Cannot Detect
- DJI on SX1262 boards (requires LR1121 for 2.4 GHz)
- 5.8 GHz video links
- Custom military protocols outside 860-930 MHz / 2.4 GHz

## Architecture

```
Core 0                          Core 1
+-----------------------+       +---------------------------+
| gpsReadTask (pri 3)   |       | loRaScanTask (pri 3)      |
|  u-blox UART + GNSS   |       |  CAD scan (121 ch/cycle)  |
|  Compass heading      |       |  RSSI-guided CAD          |
+-----------------------+       |  Diversity tracking        |
| wifiScanTask (pri 2)  |       |  Threat assessment        |
|  Remote ID capture    |       |  Data logging             |
+-----------------------+       +---------------------------+
| displayTask (pri 1)   |
|  6-screen OLED UI     |       All config in sentry_config.h
+-----------------------+       Centralized tunable constants
| alertTask (pri 2)     |
|  Buzzer + LED control |
+-----------------------+
```

## Configuration

All tunable constants in [`include/sentry_config.h`](include/sentry_config.h) -- one file for field calibration:

```cpp
DIVERSITY_WARNING  = 3;   // distinct freqs for WARNING (field: 3, bench: 5)
DIVERSITY_CRITICAL = 5;   // distinct freqs for CRITICAL (field: 5, bench: 8)
DIVERSITY_WINDOW_MS = 3000; // 3-second sliding window
COOLDOWN_MS = 15000;        // 15s per threat level decay
AMBIENT_WARMUP_MS = 50000;  // 50s boot warmup
```

## Data Logging

- **SD card** (T3S3): CSV (`/log_NNNN.csv`) + JSONL (`/field_NNNN.jsonl`) per boot
- **SPIFFS** (Heltec): CSV (`/log.csv`, rotates at 100KB)
- **Analysis**: `python tools/analyze_field_test.py field_0001.jsonl`

## Known Limitations

- **Single-channel scanner**: Checks one frequency at a time -- scan probability limits Pd more than sensitivity
- **No direction finding**: Omnidirectional antenna, presence detection only
- **3D printed case**: Attenuates signal ~3 dB -- external antenna connector recommended
- **Scan probability bottleneck**: 42+ dB margin at 637m but only 53% Pd -- need wideband capture for >90% Pd

## Roadmap

- [ ] External antenna connector for 3D printed case
- [ ] LR1121 2.4 GHz hardware validation
- [ ] Extended range testing (1+ km at 500 mW)
- [ ] Multi-device mesh networking
- [ ] Real drone flight testing
- [x] ~~Field test validation~~ -- April 2026 (89% Pd at 200m, 637m max range)
- [x] ~~Frequency diversity detection engine~~ -- v1.4.0
- [x] ~~CAD-first scan pipeline~~ -- v1.4.0
- [x] ~~Ambient warmup filter~~ -- v1.4.0
- [x] ~~GNSS integrity monitoring~~ -- v1.2.1
- [x] ~~WiFi Remote ID (ASTM F3411)~~ -- v1.2.0
- [x] ~~Buzzer alert system~~ -- v1.2.0

## Contributing

Issues and PRs welcome. MIT license, [Seaforged](https://github.com/Seaforged) organization.

See [CLAUDE.md](CLAUDE.md) for developer context and [docs/](docs/) for architecture documents.
