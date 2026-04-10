# SENTRY-RF LR1121 Comprehensive Development Plan
## From v1.5.3 to Full Dual-Band Detection Capability

**Date:** April 10, 2026
**Current State:** LR1121 board running v1.5.3 with working CAD (sub-GHz + 2.4 GHz), GFSK RSSI sweep (both bands), consecutiveHits >= 2 diversity gate, ambient warmup filter
**Hardware:** LilyGo T3-S3 LR1121 V1.2, VAS Mini RX2 side-feed u.fl antenna (on order)

> This document supersedes the LR1121 sections of `SENTRY-RF_Phased_Improvement_Plan.md`.

---

## What the LR1121 Can and Cannot Detect

### Detectable by LoRa CAD (sub-GHz 860-930 MHz)

| Protocol | Modulation | Detection Method | Confidence |
|----------|------------|------------------|------------|
| ELRS 900 MHz | LoRa FHSS, SF6-SF12, BW500 | CAD + frequency diversity | HIGH — chirp spread spectrum is exactly what CAD detects |
| TBS Crossfire 915 (50 Hz mode) | LoRa FHSS | CAD | HIGH |
| TBS Crossfire 868 (50 Hz mode) | LoRa FHSS | CAD | HIGH |
| FrSky R9 ACCESS 868/915 | LoRa FHSS, ~50 Hz | CAD | HIGH |
| mLRS 868/915 (LoRa modes) | LoRa FHSS, 19-50 Hz | CAD | HIGH |
| Autel SkyLink 2.0/3.0 900 MHz fallback | FHSS | RSSI energy only — not LoRa modulation | LOW |
| RFD900x telemetry | FHSS, ~1 MHz BW | RSSI energy only | LOW |
| MAVLink 915 MHz telemetry | FSK/FHSS | RSSI + FSK energy detection | LOW |
| TBS Crossfire 915 (150 Hz mode) | FSK 85.1 kbps | RSSI + Crossfire FSK scanner | MEDIUM |
| mLRS 915 (FSK 50 Hz mode) | FSK | RSSI energy only | LOW |
| LoRaWAN infrastructure | LoRa, infrequent | CAD hits, filtered by ambient warmup + consecutiveHits gate | FILTERED |

### Detectable by LoRa CAD (2.4 GHz 2400-2500 MHz) — NEW WITH LR1121

| Protocol | Modulation | Detection Method | Confidence |
|----------|------------|------------------|------------|
| ELRS 2.4 GHz (LoRa modes, 50-500 Hz) | LoRa FHSS, SF6-SF8, BW800 | CAD at BW800 | HIGH |
| ImmersionRC Ghost (Normal/LR modes) | LoRa CSS + Adaptive FHSS, 2406-2479 MHz | CAD | HIGH |
| TBS Tracer | LoRa FHSS, 2.4 GHz, up to 250 Hz | CAD | HIGH |
| mLRS 2.4 GHz (LoRa modes) | LoRa FHSS | CAD | HIGH |
| FlySky FRM302 | LoRa 2.4 GHz | CAD | MEDIUM — less common |

### Detectable by GFSK RSSI Sweep (2.4 GHz) — Energy Only

| Protocol | Modulation | What RSSI Shows | Confidence |
|----------|------------|------------------|------------|
| DJI OcuSync (all versions) C2 | FHSS + OFDM | Broadband energy across 2.4 GHz | MEDIUM — can see energy but can't identify protocol |
| Autel SkyLink (all versions) 2.4 GHz | FHSS + OFDM | Broadband energy | MEDIUM |
| Skydio WiFi-based links | 802.11ax/ac | WiFi-like energy patterns | LOW — hard to distinguish from regular WiFi |
| FrSky ACCESS/ACCST 2.4 GHz | FHSS, ~2 MHz BW | Narrow hopping energy | LOW |
| Spektrum DSMX | FHSS/DSSS, ~5 MHz BW | Wider energy pattern | LOW |
| FlySky AFHDS 2A | FHSS, ~2 MHz BW | Narrow hopping energy | LOW |
| ELRS 2.4 GHz (FLRC/1000 Hz mode) | FLRC | Energy detected, no CAD | LOW |
| Ghost Pure Race mode | FLRC/MSK | Energy detected, no CAD | LOW |
| Budget drone WiFi (Holy Stone, Potensic, etc.) | 802.11 | Standard WiFi energy | LOW — indistinguishable from laptops |

### Detectable by ESP32 WiFi Scanner (already implemented)

| Protocol | Method | Confidence |
|----------|--------|------------|
| Remote ID via WiFi NAN/Beacon (ASTM F3411) | 802.11 promiscuous mode, vendor IE parsing | HIGH — gives serial number, GPS, operator location |
| DJI DroneID (proprietary) | WiFi beacon vendor-specific IEs | HIGH |
| Remote ID via BLE 4 advertising | ESP32 BLE scan (if implemented) | HIGH — 3 advertising channels |

### NOT Detectable by LR1121 Hardware

| Protocol | Why Not | Alternative |
|----------|---------|-------------|
| DJI OcuSync 5.8 GHz video | LR1121 max frequency 2.5 GHz | Requires separate 5.8 GHz receiver |
| Analog FPV 5.8 GHz video | Out of LR1121 range | Requires separate 5.8 GHz receiver |
| DJI Digital FPV 5.8 GHz | Out of range | Requires separate 5.8 GHz receiver |
| Walksnail/HDZero 5.8 GHz | Out of range | Requires separate 5.8 GHz receiver |
| Autel SkyLink 5.2/5.8 GHz | Out of range | Requires wideband SDR (AntSDR E200) |
| Parrot Microhard 1.8 GHz | LR1121 covers 150-960 MHz + 2.4 GHz, gap at 1.8 GHz | AntSDR E200 |
| Cellular (4G/5G) drone links | Completely different technology | Network-level detection |

---

## Development Phases

### PHASE A: Hardware Completion (You — this week)

- [ ] Wire GPS module to GPIO 43/44 (UART) + GPIO 17/18 (I2C compass)
- [ ] Wire passive piezo buzzer to GPIO 16
- [ ] Verify spectrum bars rendering after `startReceive()` per-bin fix
- [ ] Flash SX1262 board (COM9) with v1.5.3 — regression test consecutiveHits gate
- [ ] Wait for LilyGo email response on GPIO 10/21 and antenna routing
- [ ] Wait for VAS Mini RX2 u.fl antenna — connect to IPX III port for 2.4 GHz

### PHASE B: LR1121 Feature Parity with SX1262 (CC — next session)

**Sprint B1: Version string fix + display verification**
- Fix version string in `main.cpp` (still shows v1.2.0-FIX4 somewhere)
- Verify spectrum bars render correctly on both bands
- Verify system screen text doesn't overlap page dots

**Sprint B2: Flash and regression test SX1262 board**
- Upload v1.5.3 to COM9
- Run JJ test suite against SX1262 to verify `consecutiveHits` gate doesn't break detection
- Capture serial output, compare diversity scores before/after

### PHASE C: 2.4 GHz Protocol Identification (CC — after Phase B)

**Sprint C1: Protocol signature matching for 2.4 GHz CAD hits**

When a CAD tap is detected at 2.4 GHz, classify it based on observed characteristics:

| Characteristic | ELRS 2.4 | Ghost | Tracer | Unknown LoRa |
|----------------|----------|-------|--------|--------------|
| Frequency range | 2400-2480 MHz | 2406-2479 MHz | 2400-2480 MHz | Any 2.4 GHz |
| SF range | SF6-SF8 | SF5-SF12 (mode dependent) | SF6-SF8 | Any |
| Hop rate (CAD hits/sec) | 50-500 Hz | 55-222 Hz | Up to 250 Hz | Variable |
| Channel count | ~80 | ~73 | ~80 | Variable |
| BW | 800 kHz | 800 kHz (LoRa mode) | 800 kHz | Variable |

Implementation approach:
- Track CAD hit frequency distribution over a 3-second window
- Count distinct frequencies hit (frequency diversity per band)
- ELRS 2.4: high diversity (20+ distinct freqs in 3s), SF6-SF8, 2400-2480 range
- Ghost LoRa: similar diversity, 2406-2479 range (slightly narrower)
- Tracer: similar to ELRS pattern
- Label as "ELRS_2G4", "GHOST_2G4", "TRACER_2G4", or "UNKNOWN_LORA_2G4"
- Initially, all three LoRa-based 2.4 GHz protocols look nearly identical — classify as "LORA_2G4" until we can differentiate further with real captures

**Sprint C2: Per-band diversity tracking**

Separate the `FrequencyDiversityTracker` into two independent trackers:
- `diversitySub` — tracks sub-GHz (860-930 MHz) frequency diversity
- `diversity24` — tracks 2.4 GHz (2400-2500 MHz) frequency diversity
- Either band exceeding the diversity threshold independently triggers escalation
- Combined: if BOTH bands show diversity simultaneously, it's almost certainly a drone (dual-band correlation from D-TECT-R research — "paired signal detection")

**Sprint C3: RSSI-based non-LoRa 2.4 GHz detection**

Use the GFSK RSSI sweep at 2.4 GHz to detect non-LoRa signals:
- DJI OcuSync: Look for broadband OFDM energy (20-40 MHz wide, flat rectangular spectral shape)
- Distinguish from WiFi: WiFi is 20 MHz on fixed channels (1,6,11). OcuSync FHSS moves across the band.
- Track RSSI energy patterns over time — signals that appear/disappear together across the band suggest FHSS
- This is LOW confidence detection — flag as "POSSIBLE_OFDM_2G4" not "DJI_CONFIRMED"

### PHASE D: Advanced Detection Features (CC — after Phase C)

**Sprint D1: BLE Remote ID scanning**
- The ESP32-S3 has BLE capability alongside WiFi
- Scan BLE advertising channels (2402, 2426, 2480 MHz) for Open Drone ID service UUID 0xFFFA
- Parse ASTM F3411 message types: Basic ID, Location/Vector, System, Operator ID
- Correlate with RF CAD/RSSI detections — if BLE RID reports a drone AND CAD detects LoRa hopping, confirmed detection with identification

**Sprint D2: Dual-band correlation engine**
- From D-TECT-R research: "Most real drones emit on TWO bands simultaneously"
- If sub-GHz CAD detects ELRS 900 AND 2.4 GHz RSSI shows energy (possible OcuSync video), flag as "MULTI-BAND DRONE"
- If 2.4 GHz CAD detects ELRS 2.4 AND WiFi Remote ID broadcasts appear, confirmed with identification
- Temporal correlation: signals that appear/disappear within 2 seconds of each other across bands are likely the same source

**Sprint D3: Boot self-test + antenna quality check**
- Radio health test: RSSI reads at 10 frequencies, verify not all -127.5
- Antenna quality: if all readings below -130 dBm, warn "ANTENNA CHECK"
- 2.4 GHz antenna test: same pattern on 2400-2480 MHz
- GPS health check: 120-second timeout for first fix

### PHASE E: Operational Modes (CC — after Phase D)

**Sprint E1: STANDARD / COVERT / HIGH ALERT modes**
- STANDARD: Full CAD + RSSI + WiFi scanning (current behavior)
- COVERT: RF scanning only, WiFi OFF, OLED dim/off, buzzer off, LED off
- HIGH ALERT: CAD-only rapid scan, skip RSSI sweep, immediate buzzer on any confirmed tap
- Button state machine: single=screen cycle, double=HIGH ALERT, triple=COVERT, long=mute

### PHASE F: Performance Optimization (CC — after field testing)

**Sprint F1: Reduce scan cycle time**
- Current: ~3.7s per cycle (CAD + RSSI sweep every 3rd cycle)
- RSSI sweep takes ~4s due to `startReceive()` per bin — investigate if batch mode is possible
- CAD sweep takes ~2.5s — investigate reducing channel count in quiet conditions
- Target: <2s cycle time for faster response

**Sprint F2: LR1121-specific GFSK optimization**
- The LR1121 `beginGFSK` max deviation is 200 kHz (vs SX1262's 234.3)
- Current `rxBw=156.2` kHz may be suboptimal — test wider values
- Investigate whether `startReceive()` is truly needed per-bin or if there's a continuous RX mode for LR1121

**Sprint F3: Field threshold calibration**
- All current thresholds are bench-optimized
- Need field testing with actual drones (ELRS 900, ELRS 2.4, Crossfire)
- `GPS_MIN_CNO` must be raised from 6 to 15-20 for outdoor use
- Document detection ranges achieved vs D-TECT-R research benchmarks

---

## Key Learnings from D-TECT-R Research Applied to SENTRY-RF

1. **Paired signal detection is a strong drone indicator** — detecting correlated signals on two bands simultaneously. The LR1121 can check sub-GHz AND 2.4 GHz, making this possible on a single chip.
2. **OcuSync FHSS patterns differ between versions** (O1/O2/O3/O4). While we can't demodulate OFDM, we CAN detect the FHSS C2 component's energy pattern at 2.4 GHz via RSSI sweep.
3. **Power levels as discriminators** — ELRS 2.4 at 10-1000 mW, DJI at 100-400 mW, enterprise at up to 1W. RSSI at known distance could estimate TX power to distinguish drone class.
4. **Remote ID is free ground truth** — any FAA-compliant drone broadcasts its identity. This is labeled training data for the detection engine. When Remote ID + CAD agree, confidence is maximum.
5. **LoRa chirp spread spectrum is the key differentiator** — all major FPV control protocols (ELRS, Ghost, Tracer, Crossfire) use LoRa modulation at 2.4 GHz and/or 900 MHz. CAD is purpose-built to detect this. DJI/Autel use OFDM+FHSS which CAD won't see but RSSI will.
6. **The LR1121 lacks GNSS and WiFi scanning** (stripped from LR1120 for cost). Our external u-blox GPS and ESP32 WiFi scanner are the correct architectural choices.
7. **Detection range benchmarks from academic research**: DJI consumer drones detectable at 1.3-3.7 km with proper SDR hardware. SENTRY-RF's SX1262/LR1121 is lower sensitivity than a USRP but higher than nothing — field testing will determine actual range.

---

## Hardware Notes (Must Not Lose)

- LR1121 board is **V1.2** (silkscreen confirmed, barcode: `H750 T3S3 LR1121 830 945/2.4G`)
- **TCXO voltage: 3.0V** (proven in hello sketch, passed as 7th param to `beginGFSK`)
- **`beginGFSK` signature: 7 args** — freq, br, dev, rxBw, power, preamble, tcxo
- **Max freq deviation: 200 kHz** (SX1262 allows 234.3)
- **`getRSSI()` returns packet RSSI (useless)** — must use `getInstantRSSI()` via `LR1121_RSSI` subclass
- **`setFrequency()` drops to standby** — must call `startReceive()` before each RSSI read
- **CAD return codes:** `-702 = DETECTED`, `-15 = FREE` (valid, not errors)
- **RF switch table required:** DIO5/DIO6 must be configured before radio init
- **GPIO 10/21 status unknown** on V1.2 — awaiting LilyGo response
- **3 antenna connectors:** SMA (sub-GHz), IPX III (2.4 GHz LR1121), IPX (ESP32 WiFi)
- **VAS Mini RX2 side-feed u.fl ordered** — covers 890-950 + 2360-2510 MHz on single antenna for IPX III port
- `GPS_MIN_CNO = 6` for indoor testing — MUST raise to 15-20 before field deployment
- **Battery connector: PH2.0mm** (not JST 1.25mm)

---

*This document supersedes the LR1121 sections of SENTRY-RF_Phased_Improvement_Plan.md*
*Last updated: April 10, 2026*
