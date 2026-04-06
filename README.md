![SENTRY-RF](SENTRY-RF%20Logo.png)

# SENTRY-RF

**Open-source passive drone RF detector and GNSS integrity monitor for ESP32-S3**

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![PlatformIO](https://img.shields.io/badge/Build-PlatformIO-orange.svg)
![Version: v1.5.3](https://img.shields.io/badge/Version-v1.5.3-blue.svg)
![Field Tested](https://img.shields.io/badge/Field_Tested-1km_at_10mW-brightgreen.svg)

SENTRY-RF is a pocket-sized passive drone detector ($25-75 BOM) built on ESP32-S3 with SX1262 LoRa radio. It detects drone FHSS control links using LoRa Channel Activity Detection, identifies drones broadcasting WiFi Remote ID, and monitors GNSS integrity for jamming and spoofing attacks.

**v1.5.3 field-validated performance:**

| Metric | Result |
|--------|--------|
| WARNING detection range (10 mW ELRS) | **842 meters** |
| ADVISORY detection range (10 mW ELRS) | **1,022 meters** |
| Time to WARNING | **6.4 seconds** |
| Time to CRITICAL | **11.2 seconds** |
| Return to CLEAR | **11.4 seconds** |
| False alarm rate (30-min soak, LoRa-dense bench) | **0.00%** |

---

## What It Does

### Sub-GHz RF Detection (860-930 MHz)

SENTRY-RF uses the SX1262's hardware LoRa Channel Activity Detection (CAD) to identify drone FHSS control links. CAD detects LoRa chirp modulation at the physics level -- LTE base stations, power line interference, and other non-LoRa signals physically cannot trigger it. The system scans across all spreading factors (SF6-SF12) at BW500, covering every ELRS packet rate from 200 Hz racing to 25 Hz long-range.

**Adaptive Ambient Discrimination (AAD)** separates drone FHSS from infrastructure LoRa (LoRaWAN, Meshtastic, Helium). Drones sustain high frequency diversity every scan cycle; infrastructure produces brief sporadic hits that don't persist. The sustained-diversity persistence gate requires 5 consecutive cycles of high diversity before counting toward the threat score, eliminating false alarms in any LoRa environment.

### WiFi Remote ID Detection

Captures ASTM F3411 Remote ID beacons in WiFi promiscuous mode. Detects any compliant drone (DJI, Autel, Parrot, and others) broadcasting location via vendor-specific Information Elements (OUI FA:0B:BC). Also fingerprints drone MAC OUI prefixes for additional identification.

### GNSS Integrity Monitoring

Connects to a u-blox M10 GPS module via UBX binary protocol. Monitors jamming indicators (MON-HW), spoofing detection state (NAV-STATUS), and per-satellite C/N0 uniformity (NAV-SAT). A spoofed GNSS constellation shows unnaturally uniform signal strength across all satellites -- real satellites at different ranges and elevations produce varied C/N0 values.

### Multi-Source Confidence Scoring

All detection sources feed a weighted confidence score:
- CAD confirmed taps (15 pts each, halved to 7 without sustained diversity) -- LoRa modulation verified
- Frequency diversity (8 pts per persistent frequency) -- FHSS pattern confirmed
- FSK detection (12 pts) -- Crossfire 150 Hz preamble
- RSSI persistence (5-10 pts) -- energy on known drone frequencies
- GNSS anomaly (15 pts) -- jamming or spoofing corroboration
- WiFi Remote ID (20 pts) -- drone identity confirmed
- Fast-detect bonus (20 pts) -- high persistent diversity + confirmed CAD after 5+ sustained cycles

Score thresholds: ADVISORY >= 8, WARNING >= 24, CRITICAL >= 40.

---

## Estimated Detection Range

Calibrated from field testing (10 mW ELRS at 842m WARNING in suburban USA). Path loss model: suburban corridor, n=2.8.

| TX Power | WARNING Range | ADVISORY Range | Typical Use |
|----------|---------------|----------------|-------------|
| **10 mW** | **842m** | **1,022m** | **Field tested** |
| 25 mW | ~1.2 km | ~1.4 km | ELRS racing quad |
| 100 mW | ~1.9 km | ~2.3 km | ELRS freestyle (common default) |
| 250 mW | ~2.7 km | ~3.2 km | ELRS / Crossfire long range |
| 500 mW | ~3.4 km | ~4.1 km | Crossfire max / ELRS boosted |
| 1000 mW | ~4.4 km | ~5.3 km | FCC Part 15 max (30 dBm) |

**Real-world drone scenarios:**

| System | TX Power | Est. WARNING | Est. ADVISORY |
|--------|----------|--------------|---------------|
| Racing quad (ELRS 25 mW) | 25 mW | ~1.2 km | ~1.4 km |
| Freestyle quad (ELRS 100 mW) | 100 mW | ~1.9 km | ~2.3 km |
| Long range wing (ELRS 250 mW) | 250 mW | ~2.7 km | ~3.2 km |
| TBS Crossfire 500 mW | 500 mW | ~3.4 km | ~4.1 km |
| ArduPilot SiK / RFD900x 1W | 1000 mW | ~4.4 km | ~5.3 km |
| TBS Crossfire 2W (max) | 2000 mW | ~5.6 km | ~6.8 km |

**Notes:** Open field / line-of-sight could be 2-3x further. Dense urban (concrete) could be 50% shorter. A directional antenna (6 dBi Yagi) would roughly double these ranges. DJI drones on 2.4/5.8 GHz are detected via WiFi Remote ID only (sub-GHz detection requires LR1121 board, coming soon).

---

## Detection Architecture

```
Every ~2.5 seconds (loRaScanTask, Core 1):

  PHASE 1:   Re-check active LoRa taps + adjacent channels
  PHASE 1.5: RSSI-guided CAD on elevated US-band bins
  PHASE 2:   Broad CAD scan -- SF6-SF12 rotating across 138 channels
             (up to 160 in pursuit mode when drone detected)
  PHASE 3:   FSK scan -- Crossfire 85.1 kbps on rotating channels
  PHASE 4:   RSSI sweep every 3rd cycle (350 bins, 860-930 MHz)

  -> Sustained-diversity persistence gate (5 consecutive high-div cycles)
  -> Confidence scoring (weighted multi-source, corroborated)
  -> Threat level assessment with hysteresis + rapid-clear

Concurrently (Core 0):
  GPS:    u-blox UBX polling, GNSS integrity analysis
  WiFi:   Promiscuous mode Remote ID capture, channel hopping
  Alert:  Buzzer patterns + LED blink based on threat level
  Display: 6-screen OLED UI at 2 Hz
```

### Protocols Detected

| Protocol | Band | Method | Confidence |
|----------|------|--------|------------|
| ELRS 868/915 | 860-928 MHz | CAD (LoRa SF6-SF12) | HIGH |
| TBS Crossfire LoRa | 860-928 MHz | CAD (LoRa SF6/SF12) | HIGH |
| TBS Crossfire FSK | 902-928 MHz | FSK preamble (85.1 kbps) | HIGH |
| mLRS | 860-928 MHz | CAD (LoRa SF6-SF11) | HIGH |
| SiK Radio (ArduPilot) | 902-928 MHz | RSSI + FSK energy | MEDIUM |
| FrSky R9 | 868/915 MHz | RSSI energy + protocol match | MEDIUM |
| Any ASTM F3411 drone | 2.4 GHz WiFi | Remote ID beacon parsing | HIGH |
| DJI / Autel / Parrot | 2.4 GHz WiFi | MAC OUI fingerprint | MEDIUM |
| ELRS 2.4 GHz | 2400-2500 MHz | RSSI (LR1121 only) | MEDIUM |
| DJI OcuSync/O3/O4 | 2400-2500 MHz | RSSI (LR1121 only) | MEDIUM |

---

## Field Test Results

### Suburban Drive Test (April 6, 2026)

**Environment:** Suburban USA -- residential neighborhood, houses, trees, parked cars
**JJ Config:** ELRS FCC915 200 Hz, ~10 mW (default power), stationary
**SENTRY-RF:** Driving away from JJ and back, 2.52 km track

| Metric | Result |
|--------|--------|
| Max distance from JJ | 1,022m |
| Furthest WARNING+ detection | 842m (outbound), 797m (return) |
| Furthest ADVISORY+ detection | 1,022m (at max range) |
| Peak score | 100 (at 823m, persistence gate activated) |
| Persistence gate activations | 3 (all legitimate) |
| False positives | 0 |
| GPS quality | 14-23 SVs, pDOP 1.16-2.23, hAcc 1-5m |

**Key finding:** The confirmed CAD tap pathway (conf x 7 pts at half-weight) is the primary long-range detection mechanism. At 300-700m, every WARNING-level detection came from confirmed taps with persDiv=0. The persistence gate only activated at close range and at 823m where sustained diversity was maintained for 5+ cycles. Detection range improved 4x over v1.4.0 firmware at the same power level.

Full results: [docs/FIELD_TEST_RESULTS_2026-04-06.md](docs/FIELD_TEST_RESULTS_2026-04-06.md)

### Rural Drive Test (April 1, 2026)

**Environment:** Rural USA -- open terrain with metal buildings
**JJ Config:** ELRS 200 Hz at 10 mW and 158 mW

| Test | TX Power | Range | Detection Probability | Max Diversity |
|------|----------|-------|-----------------------|---------------|
| Compound walk (NLOS, metal buildings) | 10 mW | 2-200m | **89%** | 9 |
| Rural road (driving) | 158 mW | 0-637m | **53%** | 14 |
| Baseline (no transmitter) | -- | -- | 0% false alarm | 2 |

Full results: [docs/FIELD_TEST_RESULTS_2026-04-01.md](docs/FIELD_TEST_RESULTS_2026-04-01.md)

---

## Supported Hardware

| Board | PIO Environment | Radio | Sub-GHz | 2.4 GHz | BOM |
|-------|----------------|-------|---------|---------|-----|
| **LilyGo T3S3 V1.3** | `t3s3` | SX1262 | 860-930 MHz | WiFi only | ~$25 |
| **Heltec WiFi LoRa 32 V3** | `heltec_v3` | SX1262 | 860-930 MHz | WiFi only | ~$20 |
| LilyGo T3S3 LR1121 | `t3s3_lr1121` | LR1121 | 860-930 MHz | 2400-2500 MHz | ~$35 |

All boards: ESP32-S3, FreeRTOS dual-core, 128x64 SSD1306 OLED.

### Optional Modules

| Module | Purpose | Connection |
|--------|---------|------------|
| **u-blox M10 GPS** (MAX-M10S or FlyFishRC M10QMC) | Position fix + GNSS integrity monitoring | UART (4 wires) |
| **QMC5883L compass** (e.g., FlyFishRC M10QMC-5883L) | Heading + directional bearing to signal | I2C on QWIIC (T3S3 only) |
| **KY-006 passive piezo buzzer** | Audible threat alerts (7 tone patterns) | GPIO 16 (T3S3) or GPIO 47 (Heltec) |
| **MicroSD card** (FAT32) | Detection event logging | SD slot (T3S3 only) |
| **18650 Li-Ion battery** (protected) | Portable operation (6-8 hours) | JST 1.25mm connector |

### GPS Wiring

| GPS Pin | T3S3 GPIO | Heltec V3 GPIO |
|---------|-----------|----------------|
| TX | GPIO 44 | GPIO 46 |
| RX | GPIO 43 | GPIO 45 |
| VCC | 3.3V | 3.3V |
| GND | GND | GND |

---

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- One supported ESP32-S3 board
- Sub-GHz antenna (868/915 MHz SMA + U.FL pigtail)

### Build and Flash

```bash
git clone https://github.com/Seaforged/Sentry-RF.git
cd Sentry-RF

# Build for your board
pio run -e t3s3          # LilyGo T3S3
pio run -e heltec_v3     # Heltec WiFi LoRa 32 V3

# Flash
pio run -e t3s3 --target upload

# Monitor
pio device monitor -b 115200
```

**Always connect the antenna before powering on.** Operating without an antenna can damage the SX1262 radio.

### Expected Boot Output

```
========== SENTRY-RF v1.5.3 ==========
[BOOT] Boot #1
[OLED] OK
[SCAN] FSK mode ready, 350 bins, 860.0-930.0 MHz
[GPS] Connected at 38400 baud — configuring
[WIFI] Promiscuous scanner active — channel hopping
[INIT] FreeRTOS tasks launched — LoRa:Core1, GPS+WiFi:Core0
[WARMUP] Complete after 20 cycles (51s). 12 ambient taps recorded
[CAD] cycle=21 conf=0 taps=1 div=1 persDiv=0 vel=0 sustainedCycles=0 score=5
```

After the 50-second ambient warmup, the system holds CLEAR with `persDiv=0` and `score=5`. When a drone appears, `persDiv` and `score` climb rapidly.

### Serial Output Key

The `[CAD]` line is the primary detection status:

| Field | Meaning |
|-------|---------|
| `cycle` | Scan cycle number since boot |
| `conf` | Confirmed CAD taps (3+ consecutive hits on same frequency) |
| `taps` | Total active taps in the tap list |
| `div` | Raw diversity: distinct frequencies with CAD hits in 3s window |
| `persDiv` | Persistent diversity: `div` when sustained >= 5 cycles, else 0 |
| `vel` | Diversity velocity: new persistent frequencies per window |
| `sustainedCycles` | Consecutive cycles with raw div >= 3 |
| `score` | Weighted confidence score (ADVISORY >= 8, WARNING >= 24, CRITICAL >= 40) |

**Baseline (no drone):** `persDiv=0, sustainedCycles=0, score=5`
**Drone detected:** `persDiv=25-32, sustainedCycles=5+, score=100`

---

## Configuration

All tunable detection constants in [`include/sentry_config.h`](include/sentry_config.h):

```cpp
// Detection thresholds
SCORE_ADVISORY          = 8;     // confidence score for ADVISORY
SCORE_WARNING           = 24;    // confidence score for WARNING
SCORE_CRITICAL          = 40;    // confidence score for CRITICAL

// AAD persistence gate
PERSISTENCE_MIN_CONSECUTIVE = 5;  // sustained high-diversity cycles required
PERSISTENCE_MIN_DIVERSITY   = 3;  // raw diversity threshold for "high"
DIVERSITY_WINDOW_MS         = 3000; // 3-second sliding window

// Response timing
COOLDOWN_MS             = 5000;  // 5s per threat level decay step
AMBIENT_WARMUP_MS       = 50000; // 50s boot warmup for ambient learning
AMBIENT_AUTOLEARN_MS    = 15000; // 15s post-warmup auto-learn for new ambient

// Fast-detect (requires sustained diversity confirmation)
FAST_DETECT_MIN_DIVERSITY = 5;   // persistent diversity for fast path
FAST_DETECT_MIN_CONF      = 1;   // confirmed CAD taps for fast path
WEIGHT_FAST_DETECT        = 20;  // bonus score points
```

---

## Companion Test Tool

[**JUH-MAK-IN JAMMER v2.0.0**](https://github.com/Seaforged/Juh-Mak-In-Jammer) is a companion signal simulator for SENTRY-RF validation testing. It runs on the same T3S3 hardware and simulates 16+ modes across 5 protocol families: ELRS FHSS (7 domains, 6 air rates), Crossfire FSK, SiK Radio TDM, mLRS, custom LoRa, WiFi Remote ID, and infrastructure false positive patterns (LoRaWAN, Meshtastic, Helium). All protocol parameters sourced from open-source firmware with citations in the [Protocol Emulation Reference v2](https://github.com/Seaforged/Juh-Mak-In-Jammer/blob/main/docs/JJ_Protocol_Emulation_Reference_v2.md).

---

## Project Status

**v1.5.3** -- Zero false positives in 30-minute soak test. 1 km detection range at 10 mW in suburban environment. All detection modes validated. LED alerts active.

### What Works
- Sub-GHz CAD detection with AAD persistence gate (zero false alarms in 30-min soak)
- ELRS FHSS detection in 6-11 seconds with confidence scoring
- Crossfire FSK detection via Phase 3 preamble scanning
- WiFi Remote ID capture (ASTM F3411)
- GNSS jamming/spoofing monitoring (u-blox M10)
- Buzzer alerts with 7 tone patterns (advisory, warning, critical, all-clear, etc.)
- LED threat indicators (slow blink, fast blink, solid)
- 6-screen OLED UI with auto-rotation
- SD card and SPIFFS data logging (CSV + JSONL)
- Confidence scoring with weighted multi-source fusion
- 11-second return to CLEAR after drone departs
- Field-validated: 842m WARNING at 10 mW through suburban environment

### Roadmap
- [ ] LR1121 2.4 GHz hardware validation (DJI OcuSync detection)
- [ ] AAD Sprint 2: continuous ambient catalog replacing fixed warmup
- [ ] Extended range testing (1+ km at higher power)
- [ ] Real drone flight testing
- [ ] Multi-device mesh networking
- [ ] OLED threat screen with AAD metrics (persDiv, score, sustainedCycles)
- [x] ~~Suburban field test — 1 km at 10 mW~~ -- v1.5.3
- [x] ~~Adaptive Ambient Discrimination~~ -- v1.5.0
- [x] ~~LED alert system~~ -- v1.5.1
- [x] ~~Fast detection response tuning~~ -- v1.5.1
- [x] ~~30-min soak test validation~~ -- v1.5.3
- [x] ~~Rural field test — 637m at 158 mW~~ -- April 2026
- [x] ~~CAD-first scan pipeline~~ -- v1.4.0
- [x] ~~Confidence scoring engine~~ -- v1.4.0
- [x] ~~Buzzer alert system~~ -- v1.4.0
- [x] ~~GNSS integrity monitoring~~ -- v1.2.1
- [x] ~~WiFi Remote ID (ASTM F3411)~~ -- v1.2.0

See [docs/](docs/) for architecture documents, sprint history, and the [Known Issues Tracker](docs/SENTRY-RF_Known_Issues_Tracker.md).

---

## Known Limitations

- **Single-channel scanner**: Checks one frequency at a time. Scan probability limits detection more than sensitivity.
- **No direction finding**: Omnidirectional antenna provides presence detection only. Compass + manual rotation gives rough bearing (~45 deg).
- **No 5.8 GHz**: Cannot detect analog/digital video links.
- **No 2.4 GHz RF scanning on SX1262 boards**: DJI OcuSync/O3/O4 requires LR1121 board (WiFi Remote ID still works).
- **Range estimates are modeled**: Only the 10 mW / 842m data point is field-tested. Higher power estimates use path loss modeling (n=2.8) and have not been validated.
- **3D printed case**: Attenuates signal ~3 dB. External SMA antenna connector recommended.

---

## License

[MIT](LICENSE) -- free to use, modify, and distribute.

## Contributing

Issues and PRs welcome. This is a veteran-owned open-source project under the [Seaforged](https://github.com/Seaforged) organization.

Developer context: [CLAUDE.md](CLAUDE.md) | Architecture docs: [docs/](docs/)
