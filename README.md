![SENTRY-RF Splash Screen](SENTRY-RF%20Logo.png)

# SENTRY-RF

**Passive drone RF detection + GNSS jamming/spoofing monitor for ESP32-S3**

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![Platform: ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue.svg)
![Version: v1.4.0](https://img.shields.io/badge/Version-v1.4.0-orange.svg)

## What It Does

SENTRY-RF is a pocket-sized ($25-75 BOM) passive drone detector that fuses three independent detection methods: sub-GHz RSSI spectrum scanning, LoRa Channel Activity Detection (CAD) for modulation discrimination, and WiFi Remote ID capture. It runs on ESP32-S3 hardware with an external SX1262 or LR1121 LoRa radio.

- **CAD-First Scan Pipeline**: LoRa Channel Activity Detection runs every ~575ms, providing first-alert in **8 seconds**. RSSI spectrum sweep runs every 3rd cycle as background maintenance. CAD scans SF6-SF12 at BW500 across 66 channels per cycle
- **Sub-GHz Spectrum Scanning** (860-930 MHz): 350 frequency bins at 200 kHz step. Detects ELRS, Crossfire, mLRS, and FrSky R9 control links with protocol identification across 14 drone protocols
- **Band Energy Trending**: Rolling 10-cycle average of 902-928 MHz RSSI detects aggregate FHSS energy independent of catching any specific hop
- **Ambient Warmup Filter**: 50-second boot warmup learns ambient LoRa infrastructure (LoRaWAN gateways, Meshtastic nodes) and excludes them from detection counts. Continuous post-warmup auto-learning catches infrastructure that appears after boot
- **2.4 GHz Dual-Band Scanning** (LR1121 boards): 2400-2500 MHz sweep for ELRS 2.4, DJI OcuSync/O3/O4, FrSky, Spektrum, and 10+ drone protocols
- **GNSS Integrity Monitoring**: u-blox M10 GPS with UBX binary protocol -- jamming indicator, spoofing detection, per-satellite C/N0 uniformity, AGC levels
- **WiFi Remote ID Detection**: ESP32 promiscuous mode captures ASTM F3411 vendor-specific Information Elements (OUI FA:0B:BC) from any drone
- **Audible Buzzer Alerts**: 7 tone patterns, edge-triggered escalation, operator ACK via double-press, 5-minute mute with auto-unmute

## Detection Architecture

SENTRY-RF uses a **CAD-first, RSSI-background** scan pipeline with sensor fusion:

```
Every ~575ms (CAD-only cycle):
  1. Switch SX1262 to LoRa mode (SPI opcode 0x8A)
  2. Phase 1: Re-check active taps + adjacent channels (+-325 kHz)
  3. Phase 2: Broad CAD scan - 66 channels across SF6-SF12
  4. Switch back to FSK mode
  5. Evaluate threat immediately with CAD results + last RSSI data

Every 3rd cycle (~3s, CAD + RSSI):
  6. Full 350-bin RSSI sweep (860-930 MHz)
  7. Re-evaluate threat with fresh RSSI data
```

The CAD scan uses the SX1262's hardware chirp correlator to detect LoRa modulation. CAD has near-zero false alarm rate against non-LoRa signals (LTE, ISM noise), but cannot distinguish drone LoRa from infrastructure LoRa (LoRaWAN, Meshtastic). The **persistence filter** (3 consecutive hits to confirm) and **ambient warmup filter** (50s boot learning) provide that discrimination -- drones transmit continuously via FHSS, infrastructure transmits intermittently on fixed frequencies.

## Threat Levels

| Level | Name | Trigger | Typical Time |
|-------|------|---------|--------------|
| 0 | **CLEAR** | No drone signals, GNSS healthy | -- |
| 1 | **ADVISORY** | Persistent RSSI on drone frequency in EU overlap band (869-886 MHz) | ~5s |
| 2 | **WARNING** | Confirmed CAD tap in 902-928 MHz, OR persistent RSSI in US band with CAD corroboration | **~8s** |
| 3 | **CRITICAL** | 2+ confirmed non-ambient CAD taps (FHSS pattern), OR GNSS anomaly + WARNING | ~45s |

Threat level includes hysteresis (one step per scan cycle) and 30-second cooldown decay. The 50-second warmup period establishes ambient baselines before allowing escalation above ADVISORY.

## Validated Detection Performance

Tested against [JUH-MAK-IN JAMMER](https://github.com/Seaforged/Juh-Mak-In-Jammer) (ESP32-S3 + SX1262 signal generator, ELRS mode at 130 Hz hop rate, SF6/BW500, 80 channels, 10 dBm):

| Metric | Value | Notes |
|--------|-------|-------|
| Time to WARNING (ELRS) | **8 seconds** | CAD-first pipeline, confirmed tap + RSSI |
| CAD cycle time | **~575 ms** | 66-channel scan across all SFs |
| CAD hit rate (SF6/BW500) | 0.3% per scan | Theoretical max for 80ch FHSS preamble timing |
| CAD false alarm rate | ~0% | Near-zero against non-LoRa signals |
| RSSI detection (CW) | 100% | Any signal above -73 dBm in 902-928 MHz |
| Baseline false positive | **0 false WARNING** | 120s test on LoRa-rich bench (ambient filter) |
| CAD channels per cycle | 66 | SF6:40, SF7:15, SF8:5, SF9:3, SF10-12:1 each |

### JAMMER Test Suite Results (8/8 passing)

| Test | Mode | Result | Key Metric |
|------|------|--------|------------|
| CW Tone | 915 MHz, +22 dBm | **PASS** | 915.0 MHz @ -60 dBm |
| ELRS FHSS | 80ch, 130 Hz, SF6/BW500 | **PASS** | WARNING at 8s |
| Band Sweep | 860-930 MHz | **PASS** | Moving peaks across full band |
| Remote ID | WiFi ASTM F3411 | **PASS** | IE parsing, any MAC |
| Mixed FP | LoRaWAN + ELRS | **PASS** | 0 false positives |
| Combined | RID + ELRS dual-core | **PASS** | Both detected simultaneously |
| Drone Swarm | 4 virtual drones | **PASS** | All 4 MACs detected |
| Baseline | No TX, 120s | **PASS** | Max ADVISORY, zero false WARNING/CRITICAL |

## Supported Hardware

| Board | PIO Env | Radio | Sub-GHz | 2.4 GHz | SD Card | Est. BOM |
|-------|---------|-------|---------|---------|---------|----------|
| LilyGo T3S3 v1.3 | `t3s3` | SX1262 | 860-930 MHz | WiFi only | Yes | ~$25 |
| Heltec WiFi LoRa 32 V3 | `heltec_v3` | SX1262 | 860-930 MHz | WiFi only | No (SPIFFS) | ~$20 |
| LilyGo T3S3 LR1121 | `t3s3_lr1121` | LR1121 | 860-930 MHz | 2400-2500 MHz | Yes | ~$35 |

All boards: ESP32-S3, FreeRTOS dual-core (LoRa on Core 1, GPS + WiFi on Core 0), 128x64 OLED, u-blox M10 GPS via UART.

## Hardware Requirements

**Required:**
- One of the supported ESP32-S3 boards listed above
- u-blox M10 GPS module (MAX-M10S or NEO-M10S) via UART

**Optional:**
- QMC5883L compass module via QWIIC/I2C (T3S3 boards only)
- SD card for CSV data logging (T3S3 boards)

### Wiring -- GPS Module (FlyFishRC M10QMC or similar)

| GPS Module Pin | T3S3 GPIO | Heltec V3 GPIO | Notes |
|----------------|-----------|----------------|-------|
| TX | 44 (RX) | 46 (RX) | GPS transmit -> ESP32 receive |
| RX | 43 (TX) | 45 (TX) | GPS receive <- ESP32 transmit |
| SDA | 21 (Wire1) | N/A | Compass I2C -- T3S3 only |
| SCL | 10 (Wire1) | N/A | Compass I2C -- T3S3 only |
| VCC | 3.3V | 3.3V | |
| GND | GND | GND | |

## Quick Start

```bash
git clone https://github.com/Seaforged/Sentry-RF.git
cd Sentry-RF

# Build for your board
pio run -e t3s3          # LilyGo T3S3 (SX1262)
pio run -e heltec_v3     # Heltec WiFi LoRa 32 V3
pio run -e t3s3_lr1121   # LilyGo T3S3 (LR1121 dual-band)

# Flash and monitor
pio run -e t3s3 --target upload
pio device monitor -b 115200
```

## OLED Screens

6 screens (7 on LR1121), cycled by BOOT button or auto-rotating every 5 seconds:

1. **Dashboard** -- Threat level, mini spectrum, GPS status, battery, WiFi mode
2. **Sub-GHz Spectrum** -- 860-930 MHz bar chart with peak frequency/RSSI
3. **GPS** -- Fix type, satellite count, coordinates, accuracy, compass heading
4. **GNSS Integrity** -- Jamming state, spoofing detection, C/N0 standard deviation
5. **Threat Detail** -- Current threat level, active detections, peak bearing
6. **System** -- Firmware version, uptime, free heap, compass status
7. **2.4 GHz Spectrum** *(LR1121 only)* -- 2400-2500 MHz bar chart

## WiFi Dashboard

Hold BOOT button for 5 seconds to switch from WiFi scanner to dashboard mode.

- **SSID:** `SENTRY-RF`
- **Password:** `SentryP@ssword`
- **Dashboard:** http://192.168.4.1
- **JSON API:** http://192.168.4.1/api/status

Power cycle to return to scanner mode.

## Detection Capabilities

### What It Detects
- **Sub-GHz LoRa** (CAD + RSSI): ELRS 868/915, Crossfire LoRa modes, mLRS -- modulation-confirmed via hardware chirp correlator
- **Sub-GHz FSK** (RSSI): Crossfire 150Hz, FrSky R9 -- energy detection with protocol matching
- **2.4 GHz** (LR1121 only): ELRS 2.4, DJI OcuSync/O3/O4, FrSky, Spektrum, Tracer, Ghost, Futaba, FlySky
- **WiFi Remote ID**: ASTM F3411 beacon frames from any drone (vendor-agnostic IE parsing)
- **WiFi Signatures**: DJI, Autel, Parrot controller MACs via OUI matching
- **GNSS Jamming**: u-blox MON-HW jamming indicator + AGC monitoring
- **GNSS Spoofing**: u-blox NAV-STATUS spoofing flag + per-satellite C/N0 uniformity analysis

### What It Cannot Detect
- DJI OcuSync/O3/O4 on SX1262 boards (requires LR1121 for 2.4 GHz)
- 5.8 GHz video links (no hardware support)
- Drones with Remote ID disabled (illegal but trivial to do)
- Custom military frequency-agile protocols outside 860-930 MHz / 2.4 GHz
- Protocol-level identification (CAD confirms LoRa modulation type, not specific protocol)

## Architecture

```
Core 0                          Core 1
+-----------------------+       +---------------------------+
| gpsReadTask (pri 3)   |       | loRaScanTask (pri 3)      |
|  u-blox UART drain    |       |  CAD scan FIRST (~575ms)  |
|  GNSS integrity       |       |  -> Threat assessment     |
|  Compass heading      |       |  RSSI sweep every 3rd     |
+-----------------------+       |  Band energy trending     |
| wifiScanTask (pri 2)  |       |  Detection engine         |
|  Promiscuous mode     |       |  Data logger (SD/SPIFFS)  |
|  Remote ID capture    |       +---------------------------+
+-----------------------+
| displayTask (pri 1)   |       Shared: SystemState + mutex
|  6-screen OLED UI     |       detectionQueue (depth 10)
+-----------------------+
| alertTask (pri 2)     |
|  Buzzer + LED control |
+-----------------------+
```

The LoRa radio (SX1262/LR1121) is on HSPI, owned exclusively by `loRaScanTask`. WiFi uses the ESP32 internal radio (no contention). SD card is on FSPI (separate SPI bus). Compass is on Wire1, OLED on Wire (separate I2C buses).

## Ambient Filtering

SENTRY-RF operates in the 902-928 MHz US ISM band which contains legitimate LoRa infrastructure (LoRaWAN gateways, Meshtastic mesh nodes, IoT sensors). The ambient filter prevents these from causing false alerts:

1. **Boot warmup** (50 seconds): All LoRa CAD taps detected during warmup are recorded as ambient infrastructure. Threat escalation is capped at ADVISORY during this period.
2. **Continuous auto-learning**: After warmup, any confirmed tap that persists on a fixed frequency for 10+ seconds is auto-classified as infrastructure. Drone FHSS taps expire in ~2 seconds per frequency (3 consecutive misses as the drone hops away).
3. **Ambient exclusion**: All ambient-tagged taps are excluded from confirmed, strong-pending, and active-tap counts that feed threat escalation.

## Known Limitations

- **Single-channel scanning receiver**: Checks one frequency at a time. Against 80-channel FHSS, per-scan CAD probability is ~0.3%. Mitigated by CAD-first pipeline (575ms cycles) and sensor fusion
- **No direction finding**: Single omnidirectional antenna provides presence detection, not bearing. Compass + manual rotation gives rough direction only
- **~575ms CAD update, ~3s RSSI update**: A drone at 150 km/h covers ~30m between CAD cycles. Commercial systems update at 10-100 Hz with wideband capture
- **SX1262 sensitivity**: ~6-7 dB noise figure vs 1-2 dB for purpose-built receivers. Detection range is roughly 40% less than commercial systems at same TX power
- **LoRa-rich environments**: Bench testing in areas with many LoRaWAN gateways may produce WARNING-level false alerts from infrastructure LoRa that appears after the warmup window
- **WiFi scanner vs dashboard**: Cannot run both simultaneously

## Data Logging

CSV data logged automatically:
- **T3S3 boards**: SD card (`/log_NNNN.csv`, new file each boot)
- **Heltec V3**: SPIFFS (`/log.csv`, rotates at 100KB)

Columns: `timestamp_ms, sweep_num, threat_level, peak_freq_mhz, peak_rssi_dbm, gps_lat, gps_lon, fix_type, num_sv, jam_ind, spoof_state, cno_stddev`

## Roadmap

- [ ] LR1121 2.4 GHz hardware validation (board en route)
- [ ] Field testing against real drones for Pd measurement at range
- [ ] Multi-device mesh networking (network Pd >> single-unit Pd)
- [ ] 3D-printed field enclosure design
- [ ] LED alert re-enablement with field-calibrated thresholds
- [x] ~~CAD-first scan pipeline (575ms first-alert)~~ -- v1.4.0
- [x] ~~Ambient warmup filter with continuous auto-learning~~ -- v1.4.0
- [x] ~~CAD-as-discriminator detection engine~~ -- v1.3.0
- [x] ~~LoRa CAD modulation detection (SF6-SF12)~~ -- v1.2.1
- [x] ~~OpenDroneID full frame parsing (ASTM F3411)~~ -- v1.2.0
- [x] ~~Buzzer alert system with ACK/mute~~ -- v1.2.0

## Contributing

Issues and pull requests welcome. This is a personal open-source project (MIT license) under the [Seaforged](https://github.com/Seaforged) organization -- not a commercial product.

See [CLAUDE.md](CLAUDE.md) for developer context, architecture details, and coding conventions.

## License

MIT License. See [LICENSE](LICENSE) for details.
