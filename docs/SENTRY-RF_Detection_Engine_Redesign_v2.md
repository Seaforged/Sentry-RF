# SENTRY-RF Detection Engine Redesign v2: Research & Specification Document

## Executive Summary

This document provides the technical foundation for redesigning SENTRY-RF's sub-GHz detection engine from an RSSI energy-based system to a **modulation-aware matched detection** architecture using a **scan-and-verify (fishing pole)** algorithm.

The core philosophy: cast as many lines as possible (scan every SF/BW combination drones use), note which lines get a bite (CAD hit), come back and verify (persistence check across multiple cycles), and only alert when the fish is confirmed real (persistent LoRa activity = drone, one-time hit = LoRaWAN or noise).

### Why This Redesign Is Necessary

The current RSSI-only detection engine cannot distinguish between LTE cell tower band-edge energy and drone control signals in the 869–886 MHz overlap zone. After 10+ iterations of statistical and behavioral filtering (ambient baseline, spectral width, frequency diversity, protocol novelty), none achieved zero false positives without also suppressing real drone detection. The fundamental problem: **RSSI energy detection has no knowledge of signal modulation — it treats all RF energy identically regardless of source.**

### The Solution

The SX1262 hardware contains a **correlation-based LoRa detector (CAD)** that identifies LoRa chirp spread spectrum modulation patterns. LTE OFDM, GSM, power line noise, and other non-LoRa interference will never trigger CAD because they use fundamentally different modulation. Similarly, the SX1262's FSK receive mode can detect FSK-modulated packets that match specific baud rate and deviation parameters. Together, these modulation-aware detectors provide high-confidence drone classification that energy detection cannot achieve.

### Key Design Principles

1. **Cast every line** — Scan ALL spreading factors drones use (SF6 through SF12 at BW500), not just the most common two. Three types of pilots exist: racers (high SF, fast rates), balanced/default (middle SF), and long-range (low rates, high SF). Miss any one group and you have a blind spot.

2. **Note the tap, verify the fish** — A single CAD hit is a "tap." It could be a drone, a LoRaWAN device, or a stray Meshtastic packet. Don't alert on one tap. Come back on the next cycle and check again. A drone transmitting 50–1000 packets per second will still be there. A LoRaWAN sensor that transmits once every 15 minutes will be gone.

3. **Persistence is the ultimate discriminator** — Drones are persistent (continuous C2 link). Everything else in the sub-GHz band is intermittent (LoRaWAN, Meshtastic, smart meters). If a CAD hit persists across 3+ scan cycles (~7–10 seconds), it's a drone. If it disappears, it wasn't.

4. **Adaptive priority** — When a tap is recorded, prioritize re-checking that frequency/SF on the next cycle. Don't wait for the rotation to come back around. This concentrates detection effort where signals were actually found, like pulling a fishing pole out of the water to inspect it closely.

---

## Part 1: Protocols in the 860–930 MHz Band

### 1.1 Drone Control Protocols (Detection Targets)

#### ExpressLRS (ELRS) 900 MHz

**The most common drone C2 protocol in the sub-GHz band.**

Frequency bands: 868 MHz (EU, 50 channels, 860–886 MHz) and 915 MHz (US/FCC, 80 channels, 902–928 MHz)

Pilot profiles and their typical settings:
- **Racing/freestyle pilots** (want responsiveness): 200–500 Hz packet rate → SF6/BW500
- **Balanced/set-and-forget pilots** (want reliability): 100–250 Hz → SF7/BW500 or SF8/BW500
- **Long-range pilots** (want maximum range): 25–50 Hz → SF9/BW500 to SF12/BW500

All ELRS 900 MHz modes on SX127x hardware:

| Packet Rate | Modulation | SF  | BW   | Sensitivity | CAD Time/ch | Pilot Type |
|-------------|-----------|-----|------|-------------|-------------|------------|
| 200 Hz      | LoRa      | SF6 | 500  | -108 dBm    | ~0.19 ms    | Racer      |
| 150 Hz      | LoRa      | SF7 | 500  | -112 dBm    | ~0.32 ms    | Balanced   |
| 100 Hz      | LoRa      | SF8 | 500  | -115 dBm    | ~0.58 ms    | Balanced   |
| 50 Hz       | LoRa      | SF9 | 500  | -120 dBm    | ~1.09 ms    | Long range |
| 25 Hz       | LoRa      | SF10| 500  | -123 dBm    | ~2.05 ms    | Long range |
| D50 Hz      | LoRa      | SF12| 500  | -128 dBm    | ~8.19 ms    | Ultra LR   |

On LR1121 hardware (newer):
| K1000       | FSK       | -   | -    | Custom      | N/A (FSK)   | Racer      |
| K500        | FSK       | -   | -    | Custom      | N/A (FSK)   | Racer      |

Frequency hopping: FHSS with pseudo-random sequence seeded by bind phrase. Hop rate equals packet rate (200 Hz = 200 hops/sec = 5 ms per channel). Channel spacing ~325 kHz (US) / ~520 kHz (EU). TX power configurable 10 mW to 1 W.

**CRITICAL: We must scan ALL six LoRa SF values to cover all pilot types. Scanning only SF6 and SF7 misses every long-range pilot.**

#### TBS Crossfire

Frequency band: 868 MHz (EU) and 915 MHz (US), 100 channels, 260 kHz spacing

| Mode   | Modulation | Parameters        | Detection Method |
|--------|-----------|-------------------|-----------------|
| 50 Hz  | LoRa      | SF6 BW500         | CAD at SF6/BW500 |
| 4 Hz   | LoRa      | ~SF12 BW500       | CAD at SF12/BW500|
| 150 Hz | FSK       | 85.1 kbaud GFSK   | FSK preamble detection |

TX power up to 2 W (33 dBm). Encrypted (only hobby RC protocol with encryption).

#### mLRS

Frequency band: 868/915 MHz and 433 MHz. All LoRa modulation.

| Mode  | SF   | BW  | Detection Method |
|-------|------|-----|-----------------|
| 50 Hz | SF6  | 500 | CAD at SF6/BW500 |
| 31 Hz | SF7  | 500 | CAD at SF7/BW500 |
| 19 Hz | SF11 | 500 | CAD at SF11/BW500 |

#### FrSky R9 System

Frequency band: 868/915 MHz. FSK only, no LoRa. Baud rate ~50 kbaud (proprietary). CAD will NOT detect. Primary detection via RSSI energy in the 902–928 MHz US band (no cell tower overlap) or FSK listen at estimated baud rate.

#### ELRS on LR1121 (K modes)

FSK at high packet rates (K1000, K500). Custom FSK parameters. CAD will NOT detect. FSK listen at the appropriate baud rate is needed. These are newer and less common but represent the bleeding edge.

### 1.2 Non-Drone LoRa Signals (Rejected by Persistence, Not by CAD)

**Key insight: CAD alone cannot distinguish a Meshtastic packet from an ELRS packet if they use the same SF/BW. But PERSISTENCE can.** Drones transmit 50–1000 packets per second continuously. Everything below transmits occasionally and briefly.

#### LoRaWAN (IoT sensors, smart meters)
- SF7–SF12 at **BW125** (not BW500) — CAD at BW500 won't trigger on BW125
- US uplink channels: 903.9, 904.1, 904.3, 904.5, 904.7, 904.9, 905.1, 905.3 MHz
- EU uplink channels: 868.1, 868.3, 868.5 MHz
- **Transmission pattern:** Single packet, then silent for seconds to minutes. Duty cycle 0.1–1%.
- **Persistence filter eliminates:** Hit on cycle 1, gone on cycle 2 and 3 → cleared from tap list

#### Meshtastic
- Common presets: LongFast = SF11/BW250, MediumSlow = SF11/BW500, ShortFast = SF7/BW250
- **Risk case:** MediumSlow (SF11/BW500) would match our CAD scan at SF11/BW500
- **Persistence filter eliminates:** Meshtastic transmits a few packets per minute, not hundreds per second. A tap at SF11/BW500 that doesn't persist across 3 consecutive cycles = not a drone.
- **Acceptable edge case:** If a Meshtastic device happens to transmit during 3 consecutive scan cycles (very unlikely at typical message rates), it would briefly trigger ADVISORY. This is acceptable — a Meshtastic device producing LoRa energy at drone-typical cadence in a threat environment is worth noting anyway.

#### Reticulum
- Typically SF7–SF10 at BW125 or BW250 — BW mismatch rejects most configurations
- Low duty cycle — persistence filter handles the rest

### 1.3 Interference Sources (Rejected by CAD — Not LoRa Modulation)

#### LTE Cell Towers (Band 5, Band 8)
- US Band 5 downlink: 869–894 MHz (25 MHz wide)
- EU Band 8 downlink: 925–960 MHz
- **Modulation:** OFDM — CAD will ALWAYS return "channel clear." This is the definitive fix for the cell tower problem.

#### Power Lines / Distribution Stations
- Broadband electromagnetic interference, no modulation structure
- Spectral width classifier (existing) handles broadband. CAD returns "clear."

#### Other ISM Devices (garage doors, weather stations)
- Typically OOK or simple FSK at low baud rates
- CAD returns "clear." FSK detector at 85.1 kbaud won't match low-baud OOK.

---

## Part 2: SX1262 CAD Technical Specification

### 2.1 How CAD Works

Channel Activity Detection performs a hardware correlation on received samples, looking for LoRa chirp preamble patterns. On the SX1262, it can detect the full packet, not just the preamble.

1. Radio configured with target frequency, SF, and BW
2. Radio enters CAD mode
3. Radio listens for approximately one symbol period
4. Hardware correlator compares received samples against ideal LoRa chirp waveform
5. CadDone interrupt fires, CadDetected flag indicates presence/absence of LoRa signal

**Analogy:** RSSI is like checking if there's noise in a room. CAD is like checking if someone in the room is speaking Spanish. The room might be full of noise (cell tower energy), but CAD only fires if it hears the specific language (LoRa chirps) it's listening for.

### 2.2 CAD Timing Per SF (at BW500)

| SF  | Symbol Time (ms) | CAD Time (ms) | Channels/second | What It Catches |
|-----|-------------------|---------------|-----------------|-----------------|
| SF6 | 0.128            | ~0.19         | ~5200           | ELRS 200Hz, CRSF 50Hz, mLRS 50Hz |
| SF7 | 0.256            | ~0.32         | ~3100           | ELRS 150Hz, mLRS 31Hz |
| SF8 | 0.512            | ~0.58         | ~1700           | ELRS 100Hz |
| SF9 | 1.024            | ~1.09         | ~920            | ELRS 50Hz |
| SF10| 2.048            | ~2.05         | ~490            | ELRS 25Hz |
| SF11| 4.096            | ~4.19         | ~240            | mLRS 19Hz (also catches Meshtastic MediumSlow) |
| SF12| 8.192            | ~8.19         | ~120            | ELRS D50 redundant, Crossfire 4Hz ultra LR |

### 2.3 Time Budget Analysis

Available time per scan cycle after RSSI sweep: ~100 ms is comfortable without impacting sweep cadence.

Proposed scan allocation per cycle:

| Phase | SF  | Channels | Time/ch | Total Time | Covers |
|-------|-----|----------|---------|------------|--------|
| Fast scan | SF6 | 20 | 0.19 ms | 3.8 ms | Racers, most common |
| Fast scan | SF7 | 15 | 0.32 ms | 4.8 ms | Balanced pilots |
| Medium scan | SF8 | 10 | 0.58 ms | 5.8 ms | Balanced/conservative |
| Medium scan | SF9 | 8 | 1.09 ms | 8.7 ms | Long range |
| Slow scan | SF10 | 4 | 2.05 ms | 8.2 ms | Extreme long range |
| Slow scan | SF11 | 2 | 4.19 ms | 8.4 ms | mLRS 19Hz |
| Slow scan | SF12 | 2 | 8.19 ms | 16.4 ms | Ultra LR, CRSF 4Hz |
| FSK listen | - | 4 | 3.0 ms | 12.0 ms | Crossfire 150Hz FSK |
| **Total** | | **65** | | **~68 ms** | **All pilot types** |

68 ms per cycle is ~2.7% of a 2.5-second sweep cycle. Well within budget.

With rotation: 65 channels per cycle, rotating across the full channel plan. Full coverage of all 80 US channels × all 6 SF values = 480 combinations. At 65 checks per cycle, full coverage every ~7 cycles (~18 seconds). A drone hopping at even 25 Hz (slowest rate) transmits 450 packets in 18 seconds — extremely high probability of being caught during at least one scan.

### 2.4 RadioLib CAD API

```cpp
// Configure LoRa mode for CAD
radio.begin(freq, BW, SF, CR, syncWord, outputPower, preambleLength);

// Reconfigure on the fly (faster than full begin()):
radio.setSpreadingFactor(6);
radio.setBandwidth(500.0);
radio.setFrequency(915.0);

// Blocking CAD (simplest, used for initial implementation):
int16_t result = radio.scanChannel();
if (result == RADIOLIB_LORA_DETECTED) {
    // LoRa chirp pattern detected at this freq/SF/BW
} else if (result == RADIOLIB_CHANNEL_FREE) {
    // No LoRa signal (or non-LoRa interference — cell towers end up here)
}
```

### 2.5 CAD Detection Performance

From Semtech AN1200.48 and academic literature:
- At SNR > 0 dB: Pd > 99%, Pfa < 1%
- At SNR = -5 dB: Pd > 95%, Pfa < 1%
- At SNR = -10 dB: Pd drops to ~80%, Pfa < 1%
- Pfa for non-LoRa signals (LTE, FSK, noise): effectively 0%

---

## Part 3: The Fishing Pole Algorithm (Scan-and-Verify)

### 3.1 Concept

Imagine 12 fishing poles in a lake. Each pole is tuned for a different fish (SF/BW combination). You continuously walk along checking each pole for a bite. When you feel a tap on pole 3 and pole 7, you note it but keep checking all 12. On your next pass, you go back to pole 3 and 7 first. If pole 3 still has tension — there's a fish. Pull it out, inspect it, confirm it's real. If pole 7 has nothing — false hit. Clear it and move on.

### 3.2 Data Structures

```cpp
// A "tap" — one CAD detection on one frequency/SF
struct CadTap {
    float frequency;
    uint8_t sf;
    uint8_t consecutiveHits;    // How many cycles in a row this tap was seen
    uint8_t missCount;          // How many cycles since last hit
    unsigned long firstSeenMs;
    unsigned long lastSeenMs;
    bool active;
};

static const int MAX_TAPS = 32;  // Track up to 32 simultaneous taps
static CadTap tapList[MAX_TAPS];
```

### 3.3 Algorithm Per Scan Cycle

```
PHASE 1 — PRIORITY RE-CHECK (verify existing taps)
  For each active tap in tapList:
    Run CAD at that tap's frequency and SF
    If detected: tap.consecutiveHits++, tap.missCount = 0
    If not detected: tap.missCount++
    If tap.consecutiveHits >= 3: → CONFIRMED DRONE → HIGH CONFIDENCE
    If tap.missCount >= 3: → deactivate tap (was LoRaWAN/transient)

PHASE 2 — BROAD SCAN (cast all lines)
  For each SF in [6, 7, 8, 9, 10, 11, 12]:
    For each channel in the current rotation group for this SF:
      Run CAD
      If detected AND not already in tapList:
        Add to tapList with consecutiveHits=1, missCount=0

PHASE 3 — FSK SCAN
  For each Crossfire FSK channel in the current rotation:
    Switch to FSK mode, listen for 3 ms
    If RSSI > -80 dBm: → FSK tap (same tap-and-verify logic)

PHASE 4 — RESTORE
  Switch back to FSK scan mode for next RSSI sweep
```

### 3.4 Why This Handles Every Signal Type

| Signal Source | CAD Hit? | Persists? | Result |
|--------------|----------|-----------|--------|
| ELRS drone (any SF) | YES | YES (continuous TX) | Tap → verify → CONFIRMED → CRITICAL |
| Crossfire LoRa (50Hz) | YES | YES (continuous TX) | Tap → verify → CONFIRMED → CRITICAL |
| Crossfire FSK (150Hz) | No (wrong modulation) | YES via FSK detect | FSK tap → verify → CONFIRMED → CRITICAL |
| FrSky R9 (FSK) | No | Detected via RSSI in US band | RSSI persistence → MEDIUM → WARNING |
| LoRaWAN sensor | YES (if BW500, rare) | NO (single packet) | Tap → miss → miss → cleared |
| Meshtastic | YES (if SF11/BW500) | NO (few packets/min) | Tap → miss → miss → cleared |
| LTE cell tower | NO (OFDM ≠ LoRa) | N/A | Never enters tap list |
| Power lines | NO (broadband noise) | N/A | Never enters tap list |
| WiFi router | NO (wrong band) | N/A | Not in sub-GHz scan |
| Unknown drone protocol | No | Detected via RSSI | RSSI anomaly → ADVISORY |

### 3.5 Adaptive Priority — Concentrate Effort Where Fish Are Biting

When a tap has 1-2 consecutive hits (promising but not yet confirmed), it gets **priority re-check** at the start of the next cycle — before the broad scan. This means:

- If a drone just appeared, it gets re-checked within one cycle (~2.5 seconds) instead of waiting for the rotation to come back to that channel/SF
- Re-checks are fast (single CAD operation, <1 ms for SF6) so they don't significantly impact the broad scan budget
- Once confirmed (3+ hits), the tap transitions to the detection engine as HIGH CONFIDENCE and gets emitted as a detection event. The tap stays in the list for continued monitoring but no longer needs priority re-check.

### 3.6 LoRaWAN Rejection In Detail

LoRaWAN presents two cases:

**Case 1: LoRaWAN at BW125 (99% of deployments)** — CAD at BW500 physically cannot detect BW125 signals. The chirp bandwidth mismatch causes correlation failure. These never enter the tap list at all. Zero additional filtering needed.

**Case 2: LoRaWAN at BW500 (rare, some DR4 configurations)** — CAD would detect these. But LoRaWAN duty cycle is 0.1–1%. A device transmitting one packet every 30 seconds produces a tap that appears once and then is absent for the next 12+ scan cycles. The persistence requirement (3 consecutive hits) eliminates this instantly. Even the chattiest LoRaWAN device (1% duty cycle at 1 Hz) would only be present for 10 ms out of every 1000 ms — the probability of hitting 3 consecutive 2.5-second scan cycles is effectively zero.

---

## Part 4: Full Detection Architecture

### 4.1 Detection Layers

**Layer 1 — LoRa CAD Scan with Tap-and-Verify (PRIMARY)**
- Scans ALL six SF values (6, 7, 8, 9, 10, 11, 12) at BW500
- Covers every ELRS packet rate, Crossfire LoRa, mLRS, and any LoRa-based drone protocol
- Uses the fishing pole algorithm: tap → verify → confirm
- 3 consecutive CAD hits at the same frequency/SF = CONFIRMED DRONE
- **CONFIRMED = HIGH CONFIDENCE → escalate to CRITICAL**
- Inherently rejects: LTE (not LoRa), power lines (not LoRa), WiFi (wrong band)
- Persistence rejects: LoRaWAN (not persistent), Meshtastic (not persistent)
- ~68 ms per scan cycle across 65 channels

**Layer 2 — FSK Packet Detection with Tap-and-Verify**
- Listens on rotating Crossfire FSK channels at 85.1 kbaud
- Also listens at ~50 kbaud for FrSky R9 (lower confidence — proprietary params)
- Same tap-and-verify logic as CAD: 3 consecutive FSK hits = confirmed
- **CONFIRMED = HIGH CONFIDENCE → escalate to CRITICAL**
- Inherently rejects: LTE (not FSK at 85.1k), LoRa (different modulation), noise
- ~12 ms per scan cycle across 4 channels

**Layer 3 — RSSI Energy Sweep (spectrum awareness + unknown protocols)**
- Existing 860–930 MHz sweep with strongest-peak selection (8 strongest peaks kept)
- Provides spectrum visualization on OLED display
- Catches energy from unknown/custom protocols that don't match any CAD or FSK signature
- **RSSI in 902–928 MHz (no cell tower overlap) with protocol match + persistence = MEDIUM CONFIDENCE → WARNING**
- **RSSI in 869–886 MHz (cell tower overlap) without CAD confirmation = LOW CONFIDENCE → ADVISORY max**
- Ambient baseline lock and spectral width classifier still active for RSSI layer

**Layer 4 — WiFi Remote ID (independent, packet-level)**
- ASTM F3411 vendor-specific IE parsing in promiscuous mode
- Runs continuously on Core 0, unaffected by sub-GHz changes
- **WiFi RID detection = HIGH CONFIDENCE → escalates independently**

**Layer 5 — GNSS Integrity Monitor (independent sensor)**
- u-blox M10 jamming/spoofing indicators (UBX-MON-RF, NAV-STATUS, NAV-SAT)
- Runs continuously on Core 0
- **GNSS anomaly correlated with any RF detection → escalates one tier**

### 4.2 Threat Escalation Rules

| Detection | Confidence | Max Threat (alone) | With Corroboration |
|-----------|-----------|-------------------|-------------------|
| CAD tap-and-verify confirmed (3+ consecutive) | HIGH | CRITICAL | — |
| FSK tap-and-verify confirmed (3+ consecutive) | HIGH | CRITICAL | — |
| WiFi Remote ID beacon | HIGH | CRITICAL | — |
| RSSI peak at 902–928 MHz + protocol match + persistence | MEDIUM | WARNING | CRITICAL with CAD/WiFi/GNSS |
| RSSI peak at 869–886 MHz without CAD confirmation | LOW | ADVISORY | WARNING with WiFi/GNSS |
| GNSS jamming/spoofing alone | MEDIUM | WARNING | CRITICAL with any RF detection |
| Single CAD tap (not yet verified) | PENDING | ADVISORY | Triggers priority re-check |

### 4.3 FreeRTOS Task Integration

```
LoRaScanTask cycle on Core 1 (~2.6 seconds total):

  1. RSSI sweep 860–930 MHz                           [~2400 ms]
     └─ strongest-peak selection, ambient baseline, spectral width

  2. Switch SX1262 from FSK to LoRa mode              [~2 ms]

  3. PRIORITY RE-CHECK active taps                     [~0.2-8 ms per tap]
     └─ Re-CAD each active tap at its frequency/SF
     └─ Update consecutiveHits or missCount

  4. BROAD CAD SCAN (all SF values, rotating channels) [~68 ms]
     └─ SF6:  20 channels    (~3.8 ms)
     └─ SF7:  15 channels    (~4.8 ms)
     └─ SF8:  10 channels    (~5.8 ms)
     └─ SF9:   8 channels    (~8.7 ms)
     └─ SF10:  4 channels    (~8.2 ms)
     └─ SF11:  2 channels    (~8.4 ms)
     └─ SF12:  2 channels    (~16.4 ms)
     └─ New taps added to tapList

  5. FSK SCAN (Crossfire channels, rotating)           [~12 ms]
     └─ 4 channels at 85.1 kbaud, 3ms dwell each

  6. Switch back to FSK scan mode                      [~2 ms]

  7. Feed results to detection engine                  [<1 ms]
     └─ cadDetections = count of CONFIRMED taps (3+ hits)
     └─ fskDetections = count of CONFIRMED FSK taps
     └─ RSSI peaks already processed in step 1

  Total added time: ~85 ms (~3.4% of sweep cycle)
```

### 4.4 What Gets Removed from Current Codebase

- Protocol novelty detection (AmbientProtocol, ambientProtos, ambientProtosLocked, recordAmbientProto, isNovelProtocol, isNovelFrequency)
- Frequency diversity tracking (uniqueBins, MIN_UNIQUE_BINS)
- Second-pass baseline
- isNearBaselineBin

### 4.5 What Gets Kept

- **Strongest-peak selection in extractPeaks** — correct regardless of detection method
- **Ambient baseline lock** — still useful for RSSI layer stable interference filtering
- **Spectral width classifier** — still useful for broadband RSSI interference
- **Warmup cap at ADVISORY** — first 10 sweeps while ambient baseline populates
- **Per-frequency persistence tracker** — feeds MEDIUM confidence RSSI detections
- **Per-protocol persistence tracker** — feeds MEDIUM confidence RSSI detections
- **WiFi Remote ID scanner** — unchanged, independent layer
- **GNSS integrity monitor** — unchanged, independent layer

---

## Part 5: Implementation Notes for Claude Code

### 5.1 New Files

```
include/cad_scanner.h    — CadTap struct, CadFskResult struct, function declarations
src/cad_scanner.cpp      — Fishing pole algorithm implementation
```

### 5.2 Modified Files

```
src/detection_engine.cpp — Remove novelty system, add confidence-based assessThreat()
include/detection_engine.h — Add detectionEngineSetCadFsk() declaration
src/main.cpp             — Call cadFskScan() after RSSI sweep in loRaScanTask
```

### 5.3 Key Implementation Details

**Mode switching:** The SX1262 switches between FSK (RSSI sweep) and LoRa (CAD scan) each cycle. Mode switch takes ~2 ms. When doing CAD at multiple SF values, use `radio.setSpreadingFactor()` between groups rather than full `radio.begin()` — it's faster because it doesn't reinitialize the full radio.

**Channel rotation:** Each SF has its own rotation counter. SF6 scans 20 of 80 US channels per cycle (full coverage in 4 cycles). SF12 scans 2 of 80 per cycle (full coverage in 40 cycles, but SF12 drones transmit slowly so this is acceptable). EU 868 channels are included in the rotation for each SF.

**Tap list management:**
- New tap: frequency rounded to nearest 100 kHz, SF recorded, consecutiveHits=1
- Match existing tap: frequency within ±200 kHz AND same SF → increment consecutiveHits
- Miss: an active tap not re-detected this cycle → increment missCount
- Confirm: consecutiveHits >= 3 → mark as confirmed, emit HIGH CONFIDENCE to detection engine
- Expire: missCount >= 3 → deactivate tap, free the slot
- Priority: confirmed taps get re-checked every cycle. Unconfirmed taps with 1-2 hits get re-checked before broad scan.

**FSK detection:** After CAD scans, switch to FSK mode at 85.1 kbaud. Set frequency to Crossfire channel. Start receive. Wait 3 ms. Read RSSI. If > -80 dBm, count as FSK tap. Same tap-and-verify logic applies. The 3 ms dwell at 85.1 kbaud captures ~255 bits — enough for preamble detection via energy.

### 5.4 Testing with JUH-MAK-IN JAMMER

Expected test results:

| JAMMER Mode | Detection Path | Expected Taps | Expected Threat | Notes |
|-------------|---------------|---------------|-----------------|-------|
| ELRS (e) | CAD SF6/BW500 | Taps accumulate → confirmed in ~7s | CRITICAL | LoRa FHSS at 200Hz, CAD catches hops |
| CW (c) | RSSI only (not LoRa) | Zero CAD taps | WARNING | CW in US 915MHz band, RSSI persistence → MEDIUM |
| Band Sweep (b) | May trigger CAD | Variable | ADVISORY-WARNING | Depends on sweep timing vs CAD |
| RID (r) | WiFi scanner | WiFi RID alerts | CRITICAL | Independent of CAD system |
| Mixed FP (m) | CAD + RSSI | Some taps | WARNING | LoRaWAN component is transient |
| Combined (x) | CAD + WiFi | Taps + RID alerts | CRITICAL | Both paths active |
| Drone Swarm (w) | WiFi scanner | RID alerts | CRITICAL | WiFi RID primary, swarm detection |
| Baseline (q) | Nothing | Zero taps, zero FSK | CLEAR/ADVISORY | Cell towers never trigger CAD |
| Crossfire FSK (n) | FSK detection | FSK taps → confirmed | CRITICAL | 85.1 kbaud GFSK |

### 5.5 TCXO Considerations

When switching between FSK and LoRa modes, the TCXO voltage setting must be maintained. On Heltec V3 boards, `radio.setTCXO(1.8)` must be called before `radio.begin()` in BOTH FSK and LoRa modes. The T3S3 does not require TCXO voltage setting. The cad_scanner.cpp must handle this per-board difference using `#ifdef BOARD_HELTEC_V3`.

---

## Part 6: Distributed Sensor Network (One Per Soldier)

### 6.1 Concept

When every soldier carries a SENTRY-RF unit, individual detection limitations are compensated by network effects. A drone that one unit catches intermittently via CAD becomes a confirmed detection when three units in the same area all report taps at overlapping times.

### 6.2 Network Benefits

- **Direction finding:** Two units with different RSSI/CAD readings → bearing estimate
- **Triangulation:** Three+ units → position estimate of drone and operator
- **Redundancy:** If one unit fails or is jammed, others continue
- **Faster confirmation:** A tap on unit A + a tap on unit B at the same frequency/SF in the same time window = network-confirmed detection even before either unit's local persistence check reaches 3 hits
- **Coverage multiplication:** 10 units each scanning 65 channels/cycle = 650 channel-checks per cycle across the squad. Near-certain detection of any active drone within range of any unit.

### 6.3 Implementation Path

- Detection events are already structured (DetectionEvent struct) — add network serialization
- LoRa mesh relay of detection events between units (Meshtastic-style)
- **Conflict:** The SX1262 is shared between drone scanning and mesh communication. Solution: time-division — dedicate a short slot each cycle for mesh TX/RX, or use the LR1121 board's second radio for mesh.
- Network-level threat fusion: central display unit (squad leader) aggregates detections from all units

---

## Part 7: References

1. Semtech SX1261/1262 Datasheet, Rev 1.2 (DS.SX1261-2.W.APP, June 2019)
2. Semtech AN1200.48 — "Introduction to Channel Activity Detection"
3. ExpressLRS source code and documentation: github.com/ExpressLRS/ExpressLRS, expresslrs.org
4. RadioLib SX126x API: jgromes.github.io/RadioLib/class_s_x126x.html
5. TBS Crossfire protocol documentation (community-sourced)
6. mLRS project: github.com/olliw42/mLRS
7. Garcia et al., "Implementation and Analysis of ExpressLRS Under Interference Using GNU Radio", GRCon 2025
8. Music et al., "Dense Deployment of LoRa Networks: Expectations and Limits of Channel Activity Detection", PMC 2021
9. CRFS drone detection technology: crfs.com/solutions/drone-detection
10. DHS Counter-UAS Technology Guide, February 2020
11. Drone Warfare counter-UAS RF detection guide: drone-warfare.com/counter-uas/rf-detection/
12. Semtech LoRa FAQ: semtech.com/design-support/faq/faq-lora
13. Oscar Liang ExpressLRS guide: oscarliang.com/expresslrs/
