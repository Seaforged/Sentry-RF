# SENTRY-RF Detection Architecture Rethink
## The Right Discriminator for FHSS Drone Detection

**Date:** April 1, 2026  
**Status:** Architecture proposal for review before implementation

---

## The Problem We Keep Solving Wrong

Across Sprints 1A through 2B-fix, we've iterated through dozens of threshold combinations trying to distinguish drone LoRa signals from ambient LoRa infrastructure on a busy bench. Every approach follows the same pattern:

1. Find a metric (hit count, persistence, rolling buffer, live taps)
2. Set a threshold that catches drones
3. Discover that ambient LoRa also exceeds the threshold
4. Add another filter or raise the threshold
5. Discovery that drones now fall below the threshold
6. Repeat

**This cycle will never end because we're measuring the wrong feature.**

All our metrics count *how many* detections occur. But both drones and infrastructure produce LoRa CAD hits — the *count* overlaps. What distinguishes them is *where* those hits occur across the frequency band.

---

## The Key Insight: Frequency Spread

A drone FHSS link (ELRS 200 Hz, 80 channels) produces CAD hits scattered across **dozens of distinct frequencies** within any 5-second window. A LoRaWAN gateway produces CAD hits on **2-3 fixed frequencies**. A Meshtastic node produces hits on **1 frequency**.

The discriminator isn't how many hits — it's **how many DISTINCT frequencies** produced hits within a time window.

| Source | Hits in 5s | Distinct frequencies | Spread pattern |
|--------|-----------|---------------------|----------------|
| ELRS drone (200 Hz, 80ch) | 3-8 | 3-8 different | Scattered across 902-928 MHz |
| LoRaWAN gateway (8ch, 1/15s) | 0-2 | 1-2 fixed | Clustered on fixed channels |
| Meshtastic node | 0-1 | 1 fixed | Single frequency |
| 3 ambient sources combined | 1-4 | 2-4 fixed | Few distinct, repeating |

**A drone with 3 CAD hits on 3 different frequencies is fundamentally different from infrastructure with 3 CAD hits on 1-2 frequencies, even though the hit count is identical.**

---

## Proposed Architecture: Frequency Diversity Scoring

Replace the current `recentHitCount` (total hits in 30s) with a **frequency diversity score** that counts distinct frequencies with CAD hits in a sliding window.

### Data Structure

```
FrequencySlot {
    float frequency;      // Center frequency of the hit
    uint8_t sf;           // Spreading factor
    unsigned long lastHitMs;  // When this frequency was last hit
    bool active;          // Is this slot occupied?
}

FrequencyDiversityTracker {
    FrequencySlot slots[32];  // Track up to 32 distinct frequencies
    int slotCount;            // Number of active slots
    
    void recordHit(float freq, uint8_t sf);  // Add/update a slot
    int countActiveFrequencies(unsigned long windowMs);  // Count distinct freqs hit within window
    void prune(unsigned long maxAge);  // Remove expired slots
}
```

### Recording Logic

When a non-ambient CAD hit occurs at frequency F:
1. Check if F is already in the tracker (within ±200 kHz)
2. If yes: update `lastHitMs` (same frequency hit again — doesn't increase diversity)
3. If no: add a new slot (new frequency — increases diversity)

### Threat Assessment

```
diversityCount = tracker.countActiveFrequencies(5000);  // Distinct freqs in last 5s

// FHSS signature: hits on 3+ distinct frequencies in 5 seconds
// Infrastructure: hits on 1-2 frequencies (gateway channels repeat)
if (diversityCount >= 4) → CRITICAL (definitive FHSS pattern)
if (diversityCount >= 2) → WARNING (possible FHSS, strong evidence)
if (diversityCount >= 1) → ADVISORY (single CAD hit, monitoring)
if (diversityCount == 0) → use RSSI-only path (existing logic)
```

### Why This Works Against Ambient LoRa

- **LoRaWAN gateway on 8 channels, transmitting 1/15s per channel:** In any 5-second window, produces 0-2 CAD hits on 0-2 frequencies. `diversityCount` stays at 0-2. Never reaches 3.
- **Meshtastic node on 1 channel:** Produces hits on 1 frequency. `diversityCount` = 1 max. ADVISORY only.
- **3 ambient sources combined:** Produces hits on 3-4 distinct frequencies, BUT those frequencies are the same every time. Over 5 seconds, `diversityCount` = 3-4. This COULD trigger WARNING.

**The ambient protection:** The ambient warmup filter already tags known ambient frequencies. The diversity tracker should **exclude ambient-tagged frequencies** from the diversity count. If 3 of 4 distinct frequencies are ambient-tagged, `diversityCount` = 1, not 4.

- **ELRS drone (200 Hz, 80 channels):** CAD hits scattered across many frequencies. Even with 0.3% per-channel catch rate, in 5 seconds (5 cycles × 121 channels = 605 CAD checks), expected distinct frequency hits = ~1.8 per cycle. Over 5 cycles: `diversityCount` = 5-10 distinct frequencies. Easily clears the threshold.

### The 5-Second Window

Why 5 seconds, not 30? Because FHSS is a *fast* phenomenon. A drone hops to a new channel every 5-7ms. Within 5 seconds, it has visited every channel dozens of times. Infrastructure signals don't gain diversity over time — a gateway on 8 channels has 8 distinct frequencies whether you measure over 5 seconds or 5 minutes. The short window prevents ambient accumulation while still capturing the FHSS spread pattern.

---

## What Changes vs Current Architecture

| Component | Current | Proposed |
|-----------|---------|----------|
| Primary discriminator | recentHitCount (total hits in 30s) | diversityCount (distinct frequencies in 5s) |
| mediumConfidence | rssiPersistentUS AND recentHits OR bandEnergy | diversityCount >= 2 |
| highConfidence | rssiPersistentUS AND recentHits >= 3 OR confirmed taps | diversityCount >= 4 |
| RSSI role | Required for WARNING/CRITICAL corroboration | Optional — diversity alone is sufficient for WARNING |
| Ambient protection | Ambient taps excluded from hit count | Ambient frequencies excluded from diversity count |
| Time window | 30 seconds (ambient accumulates) | 5 seconds (ambient doesn't accumulate) |
| False alarm on LoRa-rich bench | Likely after 60s+ (ambient hits accumulate) | Unlikely (ambient diversity stays low after warmup) |

### What Stays the Same

- CAD fishing-pole scan engine (cadFskScan, tap tracking, persistence)
- RSSI sweep and peak extraction
- Ambient warmup filter and auto-learning
- WiFi Remote ID scanner
- GNSS integrity monitoring
- Rapid-clear path
- Centralized sentry_config.h
- Radio mode switching (SPI opcode 0x8A)

### What Gets Replaced

- `recentHitTimestamps[]` ring buffer → `FrequencyDiversityTracker`
- `recentHitCount` field in CadFskResult → `diversityCount` field
- `assessThreat()` confidence logic → diversity-based thresholds
- Standalone `rssiPersistentUS` as WARNING trigger → diversity handles WARNING independently

---

## RSSI's New Role

RSSI persistence stops being a WARNING gate and becomes a **CRITICAL accelerator**:

- diversityCount >= 2 alone → WARNING (CAD-only, fast)
- diversityCount >= 4 alone → CRITICAL (strong FHSS evidence)
- diversityCount >= 2 AND rssiPersistentUS → CRITICAL (two sensors agree, faster)
- rssiPersistentUS alone → ADVISORY (energy detected but unconfirmed modulation)

This means WARNING can be achieved with CAD alone (no RSSI sweep needed), which is the fast path. RSSI adds confidence for faster CRITICAL escalation but isn't required.

---

## Expected Performance

**WARNING time:** The diversity tracker sees its first distinct-frequency hit within 1-2 CAD cycles (~1-2s). The second distinct frequency hit within the next 1-2 cycles. `diversityCount >= 2` at ~2-4 seconds.

**CRITICAL time:** `diversityCount >= 4` requires 4 distinct frequencies in 5 seconds. At ~1.8 hits per cycle across distinct frequencies, this is reached in ~3-4 cycles = ~3-4 seconds for diversity-only CRITICAL. With RSSI corroboration, possibly faster.

**Baseline:** Ambient LoRa with warmup filter produces 0-2 non-ambient distinct frequencies in any 5-second window. The 5-second window prevents accumulation. ADVISORY max.

**Rapid-clear:** When drone leaves, diversity count drops to 0 within 5 seconds (the window). Rapid-clear fires within 4 clean cycles after that. Total: ~9 seconds from drone departure to CLEAR.

---

## Risk Assessment

**Risk 1: Ambient produces 3+ distinct non-ambient frequencies in 5 seconds.**
Mitigation: Increase threshold to 4 for WARNING. Or extend the warmup to catch more ambient frequencies. This is testable on the bench.

**Risk 2: Drone at long range produces only 1-2 distinct frequency hits in 5 seconds.**
Mitigation: Keep the RSSI persistence path as a backup — diversityCount >= 1 AND rssiPersistentUS → WARNING. The diversity path is the fast path; RSSI is the long-range backup.

**Risk 3: Multiple drones on different protocols look like one high-diversity source.**
This is actually fine — multiple drones IS a higher threat and should escalate faster.

---

## Implementation Plan

This is a single focused sprint. The changes are in 3 files:

1. **cad_scanner.cpp:** Replace `recentHitTimestamps[]` with `FrequencyDiversityTracker`. Record non-ambient CAD hits as frequency slots. Expose `diversityCount` in CadFskResult.

2. **detection_engine.cpp:** Rewrite `assessThreat()` confidence logic to use `diversityCount` instead of `recentHitCount`. Remove the tangled web of conditions that accumulated across 6 sprints.

3. **sentry_config.h:** Add diversity thresholds: `DIVERSITY_WINDOW_MS = 5000`, `DIVERSITY_WARNING = 2`, `DIVERSITY_CRITICAL = 4`.

**Estimated effort:** 1 sprint (CC session), ~30 minutes of coding + testing.

**What this eliminates:** The entire mediumConfidence/highConfidence condition maze that's been the source of every regression. Instead of 6+ boolean conditions ORed together, the threat assessment becomes a simple numeric comparison on a single metric that directly measures the FHSS signature.

---

*The fishing pole catches fish. The problem was never the fishing pole — it was that we were counting fish instead of counting how many different spots in the lake had fish. FHSS means "frequency hopping" — the hopping IS the signature.*
