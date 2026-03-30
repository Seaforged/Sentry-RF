# SENTRY-RF Detection Engine Redesign: Research & Specification Document

## Executive Summary

This document provides the technical foundation for redesigning SENTRY-RF's sub-GHz detection engine from an RSSI energy-based system to a **modulation-aware matched detection** architecture. The core change: use the SX1262's built-in Channel Activity Detection (CAD) as the primary LoRa drone detector, FSK packet detection for FSK-based drone protocols, and retain RSSI energy scanning for spectrum awareness and unknown protocol anomaly detection.

### Why This Redesign Is Necessary

The current RSSI-only detection engine cannot distinguish between LTE cell tower band-edge energy and drone control signals in the 869–886 MHz overlap zone. After 10+ iterations of statistical and behavioral filtering (ambient baseline, spectral width, frequency diversity, protocol novelty), none achieved zero false positives without also suppressing real drone detection. The fundamental problem: **RSSI energy detection has no knowledge of signal modulation — it treats all RF energy identically regardless of source.**

### The Solution

The SX1262 hardware contains a **correlation-based LoRa detector (CAD)** that identifies LoRa chirp spread spectrum modulation patterns. LTE OFDM, GSM, power line noise, and other non-LoRa interference will never trigger CAD because they use fundamentally different modulation. Similarly, the SX1262's FSK receive mode can detect FSK-modulated packets that match specific baud rate and deviation parameters. Together, these modulation-aware detectors provide high-confidence drone classification that energy detection cannot achieve.

---

## Part 1: Protocols in the 860–930 MHz Band

### 1.1 Drone Control Protocols (Detection Targets)

#### ExpressLRS (ELRS) 900 MHz
- **Frequency bands:** 868 MHz (EU, 50 channels, 860–886 MHz) and 915 MHz (US/FCC, 80 channels, 902–928 MHz)
- **Modulation (SX127x hardware):** LoRa at ALL packet rates (50 Hz, 100 Hz, 150 Hz, 200 Hz). Uses SF6–SF12 with BW500.
  - 200 Hz: SF6 BW500 (sensitivity -108 dBm)
  - 150 Hz: SF7 BW500 (sensitivity -112 dBm)  
  - 100 Hz: SF8 BW500 (sensitivity -115 dBm)
  - 50 Hz: SF9 BW500 (sensitivity -120 dBm)
  - 25 Hz: SF10 BW500 (sensitivity -123 dBm)
  - D50 Hz: SF12 BW500 redundant transmit mode (sensitivity -128 dBm)
- **Modulation (LR1121 hardware):** FSK at high packet rates (K1000, K500). LoRa at lower rates. FSK uses custom parameters.
- **Frequency hopping:** FHSS with pseudo-random sequence seeded by bind phrase
- **Hop rate:** Equal to packet rate (e.g., 200 Hz = 200 hops/sec, 5 ms per channel)
- **Packet size:** 8 bytes payload, very short air time
- **Channel spacing:** ~520 kHz (868 EU), ~325 kHz (915 US)
- **TX power:** Configurable, 10 mW to 1 W typical

**CAD detection strategy:** CAD at SF6/BW500 catches 200 Hz mode (most common for FPV). CAD at SF7/BW500 catches 150 Hz. CAD at SF9/BW500 catches 50 Hz (long range). Prioritize SF6/BW500 and SF7/BW500 as these are the most commonly used modes.

#### TBS Crossfire
- **Frequency band:** 868 MHz (EU) and 915 MHz (US), 100 channels, 260 kHz spacing
- **Modulation:**
  - 50 Hz mode: **LoRa** (SF6 BW500) — detectable by CAD
  - 4 Hz mode: **LoRa** (higher SF, long range) — detectable by CAD
  - 150 Hz mode: **FSK** at 85.1 kbaud — NOT detectable by CAD, requires FSK packet detection
- **Hop rate:** 150 channels/sec at 150 Hz mode
- **TX power:** Up to 2 W (33 dBm)
- **Encryption:** Yes, Crossfire is the only hobby RC protocol with data encryption

**Detection strategy:** CAD catches 50 Hz and 4 Hz LoRa modes. FSK packet detection at 85.1 kbaud catches 150 Hz mode.

#### mLRS
- **Frequency band:** 868/915 MHz and 433 MHz
- **Modulation:** LoRa at all packet rates (19 Hz, 31 Hz, 50 Hz)
  - 19 Hz: SF11 BW500 (SX127x compatibility mode)
  - 31 Hz: SF7 BW500 (SX126x mode, higher performance)
  - 50 Hz: SF6 BW500
- **Hardware:** STM32 + SX1262/SX127x based

**Detection strategy:** CAD at SF6/BW500 and SF7/BW500 catches most modes. SF11/BW500 for 19 Hz mode if needed.

#### FrSky R9 System
- **Frequency band:** 868/915 MHz
- **Modulation:** **FSK only** — no LoRa modulation used
- **Baud rate:** Proprietary FSK parameters (approximately 50 kbaud)
- **TX power:** Up to 1 W

**Detection strategy:** FSK packet detection only. CAD will NOT detect FrSky R9. RSSI energy detection is the fallback. In the 902–928 MHz US band, there is no cell tower overlap, so RSSI-based detection works without the false positive problem.

#### ImmersionRC Ghost
- **Frequency band:** 2.4 GHz only
- **Not relevant to sub-GHz detection** — handled by 2.4 GHz scanner (LR1121 / WiFi)

#### TBS Tracer
- **Frequency band:** 2.4 GHz only
- **Not relevant to sub-GHz detection** — handled by 2.4 GHz scanner (LR1121 / WiFi)

### 1.2 Non-Drone LoRa Signals (Must NOT Trigger Drone Alerts)

#### LoRaWAN (IoT sensors, smart meters)
- **Modulation:** LoRa at SF7–SF12, **BW125** (not BW500)
- **US channels:** 903.9, 904.1, 904.3, 904.5, 904.7, 904.9, 905.1, 905.3 MHz (8 uplink), 904.6 MHz (downlink)
- **EU channels:** 868.1, 868.3, 868.5 MHz (3 uplink)
- **Duty cycle:** Very low — transmits briefly then silent for seconds/minutes
- **CAD differentiation:** LoRaWAN uses BW125. Drone protocols use BW500. **CAD configured for BW500 will NOT trigger on BW125 LoRaWAN signals.** The chirp bandwidth mismatch causes correlation failure.

#### Meshtastic
- **Modulation:** LoRa, configurable SF and BW
- **Common settings:** LongFast = SF11 BW250, MediumSlow = SF11 BW500, ShortFast = SF7 BW250
- **CAD differentiation:** Most Meshtastic presets use BW250, not BW500. CAD at BW500 won't trigger on BW250 signals. Exception: MediumSlow (SF11 BW500) would match a CAD scan at SF11/BW500 — but we don't need to CAD at SF11 for drone detection since no drone protocol uses SF11/BW500.

#### Reticulum
- **Modulation:** LoRa, typically SF7–SF10 at BW125 or BW250
- **CAD differentiation:** BW125/BW250 won't trigger CAD at BW500.

### 1.3 Interference Sources (Must Be Rejected)

#### LTE Cell Towers (Band 5, Band 8)
- **US Band 5 downlink:** 869–894 MHz (25 MHz wide)
- **EU Band 8 downlink:** 925–960 MHz (partial overlap at top of scan range)
- **Modulation:** OFDM (Orthogonal Frequency Division Multiplexing)
- **Bandwidth:** 10 MHz or 20 MHz per carrier
- **CAD rejection:** OFDM is not LoRa chirp spread spectrum. CAD will ALWAYS return "channel clear" for LTE signals regardless of signal strength. This is the definitive solution to the cell tower false positive problem.

#### Power Lines / Distribution Stations
- **Characteristics:** Broadband electromagnetic interference, harmonics of 50/60 Hz
- **Spectrum signature:** Wideband energy spread across many MHz, no narrowband structure
- **Rejection:** Spectral width classifier (existing) + CAD returns "clear" (no LoRa modulation)

#### Other ISM Devices (garage doors, weather stations, industrial sensors)
- **Modulation:** Typically OOK (On-Off Keying) or simple FSK at low baud rates
- **CAD rejection:** OOK is not LoRa. Simple FSK at non-matching baud rates won't decode as drone FSK.

---

## Part 2: SX1262 CAD Technical Specification

### 2.1 How CAD Works

Channel Activity Detection performs a correlation operation on received samples, looking for LoRa chirp preamble patterns (and on SX1262, the full packet). The CAD operation:

1. Radio is configured with target frequency, SF, and BW
2. Radio enters CAD mode via `SetCad()` command
3. Radio listens for one symbol period (T_symbol + 32/BW milliseconds)
4. Hardware correlator compares received samples against ideal LoRa preamble waveform
5. CadDone interrupt fires, CadDetected flag indicates result

### 2.2 CAD Timing

| SF | BW (kHz) | Symbol Time (ms) | CAD Time (ms) | Notes |
|----|----------|-------------------|---------------|-------|
| SF6 | 500 | 0.128 | ~0.19 | Fastest, catches ELRS 200Hz / CRSF 50Hz |
| SF7 | 500 | 0.256 | ~0.32 | Catches ELRS 150Hz |
| SF8 | 500 | 0.512 | ~0.58 | Catches ELRS 100Hz |
| SF9 | 500 | 1.024 | ~1.09 | Catches ELRS 50Hz long-range |
| SF7 | 125 | 1.024 | ~1.28 | Would catch LoRaWAN — DO NOT USE for drone detection |

### 2.3 CAD Parameters on SX1262

RadioLib exposes CAD via:
```cpp
int16_t state = radio.startChannelScan();  // Non-blocking CAD start
// ... wait for interrupt or poll ...
int16_t result = radio.getChannelScanResult();  // Returns RADIOLIB_LORA_DETECTED or RADIOLIB_CHANNEL_FREE

// Or blocking version:
int16_t result = radio.scanChannel();  // Blocks until CAD completes
```

CAD configuration parameters (SetCadParams):
- `cadSymbolNum`: Number of symbols for CAD detection (1, 2, 4, 8, 16). More symbols = higher Pd but longer time.
- `cadDetPeak`: Peak detection threshold. Higher = fewer false positives but lower Pd.
- `cadDetMin`: Minimum detection threshold.
- `cadExitMode`: What to do after CAD — return to standby or enter RX.

Recommended starting parameters for drone detection:
- `cadSymbolNum = LORA_CAD_02_SYMBOL` (2 symbols — fast, sufficient for strong drone signals)
- `cadDetPeak = SF + 13` (Semtech recommended default)
- `cadDetMin = 10` (Semtech recommended default)

### 2.4 CAD Probability of Detection (Pd) and False Alarm (Pfa)

From Semtech AN1200.48 and academic literature:
- At SNR > 0 dB: Pd > 99%, Pfa < 1%
- At SNR = -5 dB: Pd > 95%, Pfa < 1% (LoRa can decode below noise floor)
- At SNR = -10 dB: Pd drops to ~80%, Pfa < 1%
- Pfa for non-LoRa signals (LTE, FSK, noise): effectively 0% — the correlation never matches

### 2.5 CAD Limitations

- **SF and BW must match the target signal.** CAD at SF6/BW500 will NOT detect a signal at SF7/BW500 or SF6/BW125. This is both a limitation and an advantage — it provides built-in protocol discrimination.
- **CAD detects LoRa modulation, not specific protocols.** It cannot tell ELRS from Meshtastic if both use the same SF/BW. Differentiation comes from the SF/BW combination chosen for CAD.
- **CAD takes time.** Each CAD operation takes ~0.2 to ~1.1 ms depending on SF. Scanning many channels and multiple SF values multiplies this.
- **CAD requires the signal to be present during the CAD window.** For FHSS signals, the drone must be transmitting on the scanned channel at the exact moment CAD runs. Probability of catching a single hop is low, but scanning multiple channels per sweep cycle increases cumulative Pd.

---

## Part 3: FSK Packet Detection for Non-LoRa Drone Protocols

### 3.1 Crossfire FSK Mode (150 Hz)

- **Baud rate:** 85.1 kbaud
- **Modulation:** GFSK
- **Deviation:** ~25 kHz (estimated from channel spacing)
- **Channel spacing:** 260 kHz
- **Preamble:** Standard GFSK preamble (0xAA pattern)

The SX1262 can be configured in FSK receive mode with matching baud rate and deviation. If a valid preamble is detected, the chip generates an interrupt. This provides confirmation that a real FSK packet is present, not just energy.

RadioLib FSK receive:
```cpp
radio.beginFSK(freq, 85.1, 25.0, 234.3, 10, 16);
radio.setFrequency(target_freq);
radio.startReceive();
// Wait briefly (1-2 ms)
// Check for preamble detection via interrupt or RSSI
```

### 3.2 FrSky R9 FSK Mode

- **Baud rate:** ~50 kbaud (proprietary, approximate)
- **Modulation:** FSK
- **Lower confidence detection** — exact parameters are proprietary. RSSI energy detection in the 902–928 MHz band (no cell tower overlap in US) is the primary detection method.

---

## Part 4: Redesigned Detection Architecture

### 4.1 Detection Layers (Priority Order)

**Layer 1 — LoRa CAD Scan (PRIMARY for LoRa drone protocols)**
- Scan 10–20 rotating channels per sweep cycle at known ELRS/Crossfire frequencies
- CAD at SF6/BW500 (catches ELRS 200 Hz, Crossfire 50 Hz, mLRS 50 Hz)
- CAD at SF7/BW500 (catches ELRS 150 Hz, mLRS 31 Hz)
- Optional: CAD at SF9/BW500 (catches ELRS 50 Hz long-range mode)
- Time cost: ~4–8 ms per sweep cycle (20 channels × 2 SF settings × ~0.2 ms)
- **CAD detection = HIGH CONFIDENCE drone C2**
- Inherently rejects: LTE, LoRaWAN (BW125), Meshtastic (BW250), power lines, all non-LoRa signals

**Layer 2 — FSK Packet Detection (for FSK drone protocols)**
- Listen on 2–4 Crossfire FSK channels at 85.1 kbaud
- Brief dwell per channel (~2–5 ms) looking for preamble detection
- Time cost: ~10–20 ms per sweep cycle
- **FSK preamble detection = HIGH CONFIDENCE drone C2**
- Inherently rejects: LTE (OFDM, not FSK), LoRa signals, noise

**Layer 3 — RSSI Energy Sweep (spectrum awareness + unknown protocols)**
- Existing 860–930 MHz sweep with strongest-peak selection
- Provides spectrum visualization for operator situational awareness
- Catches energy from unknown/custom protocols that don't match any CAD or FSK signature
- **RSSI-only detection = LOW CONFIDENCE — ADVISORY only from sub-GHz alone**
- Exception: RSSI peaks in 902–928 MHz (no cell tower overlap in US) with protocol match and persistence = MEDIUM CONFIDENCE → can escalate to WARNING

**Layer 4 — WiFi Remote ID (independent, packet-level)**
- ASTM F3411 vendor-specific IE parsing in promiscuous mode
- Runs continuously on Core 0
- **WiFi RID detection = HIGH CONFIDENCE — escalates independently**

**Layer 5 — GNSS Integrity Monitor (independent sensor)**
- u-blox M10 jamming/spoofing indicators
- Runs continuously on Core 0
- **GNSS anomaly correlated with any RF detection = escalates one tier**

### 4.2 Threat Escalation Rules

| Detection | Confidence | Max Threat (alone) | With Corroboration |
|-----------|-----------|-------------------|-------------------|
| CAD-confirmed LoRa at drone SF/BW | HIGH | CRITICAL | — |
| FSK packet detected at drone baud rate | HIGH | CRITICAL | — |
| WiFi Remote ID beacon | HIGH | CRITICAL | — |
| RSSI peak at 902–928 MHz + protocol match + persistence | MEDIUM | WARNING | CRITICAL with CAD/WiFi/GNSS |
| RSSI peak at 869–886 MHz without CAD confirmation | LOW | ADVISORY | WARNING with WiFi/GNSS |
| GNSS jamming/spoofing alone | MEDIUM | WARNING | CRITICAL with any RF detection |

### 4.3 FreeRTOS Task Integration

The CAD and FSK detection scans should be integrated into the existing LoRaScanTask on Core 1. The sweep cycle becomes:

```
LoRaScanTask cycle (~2.5 seconds):
  1. RSSI sweep 860–930 MHz [~2.4s] (existing, with strongest-peak selection)
  2. CAD scan: 20 channels × SF6/BW500 [~4 ms]
  3. CAD scan: 10 channels × SF7/BW500 [~3 ms]  
  4. FSK listen: 4 channels × 85.1 kbaud [~16 ms]
  5. Process all results → feed detection engine
  Total added time: ~23 ms (< 1% of sweep cycle)
```

### 4.4 What Gets Removed

- Protocol novelty detection (AmbientProtocol, ambientProtosLocked, recordAmbientProto, isNovelProtocol, isNovelFrequency) — replaced by CAD modulation discrimination
- Frequency diversity tracking (uniqueBins, MIN_UNIQUE_BINS) — not needed with CAD
- Second-pass baseline — not needed with CAD
- **KEEP:** Ambient baseline lock (still useful for stable interference sources)
- **KEEP:** Spectral width classifier (still useful for broadband interference)
- **KEEP:** Warmup cap at ADVISORY (still useful for the first 10 sweeps)
- **KEEP:** Strongest-peak selection in extractPeaks (correct regardless of detection method)
- **KEEP:** Protocol tracker with per-protocol persistence (works alongside CAD results)

---

## Part 5: Implementation Notes for Claude Code

### 5.1 RadioLib CAD API

```cpp
// Configure LoRa mode for CAD
radio.begin(freq, BW, SF, CR, syncWord, outputPower, preambleLength);

// Or reconfigure on the fly:
radio.setSpreadingFactor(6);
radio.setBandwidth(500.0);
radio.setFrequency(915.0);

// Blocking CAD (simplest):
int16_t result = radio.scanChannel();
if (result == RADIOLIB_LORA_DETECTED) {
    // LoRa signal detected at this freq/SF/BW
} else if (result == RADIOLIB_CHANNEL_FREE) {
    // No LoRa signal (or non-LoRa interference)
}

// Non-blocking CAD (for integration with FreeRTOS):
radio.startChannelScan();
// ... do other work or wait for DIO1 interrupt ...
int16_t result = radio.getChannelScanResult();
```

### 5.2 Mode Switching

The SX1262 must switch between FSK mode (for RSSI sweeps) and LoRa mode (for CAD scans). This requires:
1. After RSSI sweep completes: switch to LoRa mode with `radio.begin()` or reconfigure
2. Set SF and BW for drone detection
3. Run CAD scans across target frequencies
4. Switch back to FSK mode for next sweep

Mode switching takes ~1–2 ms (PLL settling + calibration). This is acceptable given the 2.4s sweep cycle.

### 5.3 Channel Selection Strategy

Rather than CAD-scanning all 80 ELRS channels (which would take too long), scan a **rotating subset** each cycle:
- Divide 80 channels into 4 groups of 20
- Each sweep cycle, CAD-scan one group
- Full coverage every 4 cycles (~10 seconds)
- A drone hopping at 200 Hz hits each channel ~2.5 times per second — high probability of being present on at least one channel in any group of 20

### 5.4 CAD Result Integration with Detection Engine

Create a new data structure for CAD results:
```cpp
struct CadDetection {
    float frequency;
    uint8_t sf;
    float bw;
    unsigned long timestamp;
};
```

Feed CAD detections into the existing protocol tracker alongside RSSI peaks. CAD detections are pre-classified as HIGH CONFIDENCE — they bypass the ambient baseline, spectral width, and all other RSSI-based filters because the modulation match is definitive.

### 5.5 Testing with JUH-MAK-IN JAMMER

The JAMMER's ELRS FHSS mode transmits LoRa packets at SF6 BW500 with FHSS hopping — this is exactly what CAD at SF6/BW500 should detect. The JAMMER's CW mode transmits an unmodulated carrier — CAD should NOT detect this (it's not LoRa), which validates the modulation discrimination. The JAMMER's Crossfire FSK mode transmits FSK at 85.1 kbaud — the FSK packet detector should catch this.

Expected test results with the new architecture:
- CW (c): RSSI detects energy, CAD returns "clear" → ADVISORY (RSSI-only, 902–928 MHz, no cell tower overlap) or WARNING with persistence
- ELRS (e): RSSI detects energy AND CAD detects LoRa → CRITICAL
- Crossfire FSK (n): RSSI detects energy AND FSK detects packets → CRITICAL  
- Baseline (q): RSSI sees cell tower energy, CAD returns "clear" → ADVISORY max (RSSI-only in overlap zone)
- RID (r): WiFi scanner detects Remote ID → CRITICAL independently
- All WiFi modes: unaffected by sub-GHz changes

---

## Part 6: Distributed Sensor Network Considerations

### 6.1 One Device Per Soldier

When every soldier carries a SENTRY-RF unit, the system transitions from single-point detection to a **distributed sensor network**. Each node independently detects and can share detections via LoRa mesh.

### 6.2 Advantages of Distribution

- **Direction finding:** Two nodes with different RSSI readings from the same detection → bearing estimate
- **Triangulation:** Three+ nodes → position estimate of the drone/operator
- **Redundancy:** If one node fails or is jammed, others continue operating
- **Coverage:** Multiple nodes cover more spectrum more frequently
- **Validation:** A detection confirmed by multiple nodes has higher confidence than single-node detection

### 6.3 Future Architecture Implications

- Detection events should be structured as serializable messages (they already are: DetectionEvent struct)
- LoRa mesh networking (Meshtastic-style) could relay detections between nodes
- **Important:** SENTRY-RF's LoRa radio is shared between scanning and potential mesh communication — time-division between detection and relay needed
- The LR1121 board (Sprint 12) with separate sub-GHz and 2.4 GHz radios could dedicate one to detection and one to mesh relay

---

## References

1. Semtech SX1261/1262 Datasheet, Rev 1.2 (DS.SX1261-2.W.APP, June 2019)
2. Semtech AN1200.48 — "Introduction to Channel Activity Detection"
3. ExpressLRS source code: github.com/ExpressLRS/ExpressLRS
4. RadioLib SX126x API: jgromes.github.io/RadioLib/class_s_x126x.html
5. TBS Crossfire protocol documentation (community-sourced)
6. mLRS project: github.com/olliw42/mLRS
7. Garcia et al., "Implementation and Analysis of ExpressLRS Under Interference Using GNU Radio", GRCon 2025
8. Music et al., "Dense Deployment of LoRa Networks: Expectations and Limits of Channel Activity Detection", PMC 2021
9. CRFS drone detection technology: crfs.com/solutions/drone-detection
10. DHS Counter-UAS Technology Guide, February 2020
11. Drone Warfare counter-UAS RF detection guide: drone-warfare.com/counter-uas/rf-detection/
