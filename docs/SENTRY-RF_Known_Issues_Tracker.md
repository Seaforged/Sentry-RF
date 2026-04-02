# SENTRY-RF Known Issues & Unfinished Work Tracker
## As of April 1, 2026 — Post-Sprint 2C

This document tracks every identified issue, limitation, and unfinished item. Nothing gets forgotten. Check items off as they're resolved. Reference this before every sprint.

---

## CRITICAL — Must Fix Before Field Deployment

### [ ] GPS_MIN_CNO is set to 6 (indoor testing value)
**Impact:** Weak attenuated signals degrade fix quality and integrity monitoring.  
**Fix:** Raise to 15-20 in `sentry_config.h` before any field or production deployment.  
**Also:** C/N0 uniformity check should be gated behind `GPS_MIN_CNO >= 15` — produces false positives at CNO=6.  
**Sprint:** 6A in the phased plan.

### [ ] FSK Phase 3 disabled (#if 0)
**Impact:** Crossfire 150 Hz mode and FrSky R9 drones are invisible to primary detection. Only detected via RSSI energy (low confidence, slow).  
**Root cause:** RadioLib internal state corruption after FSK→LoRa mode transition via SPI opcode 0x8A. RadioLib caches modem params (bitrate, deviation, Rx BW) that become stale after raw SPI packet type switch. Subsequent LoRa `scanChannel()` calls use corrupted internal state, inflating CAD false hits.  
**Possible fixes:**
1. After Phase 3, explicitly re-set all LoRa params (SF, BW, CR, syncWord, preamble) via RadioLib API before Phase 1 of the next cycle
2. Restructure so Phase 3 runs AFTER `switchToFSK()` — FSK→FSK transition avoids the corruption. Phase 3 scans, then switchToFSK restores RSSI sweep params
3. Investigate RadioLib source to find which cached variables are causing the issue — may be a one-line fix
4. Use `radio.beginFSK()` for Phase 3 instead of raw SPI — but this was previously proven to cause -707 errors. May work if done carefully with proper standby/reset sequence  
**Code location:** `src/cad_scanner.cpp` lines ~440-490 (wrapped in `#if 0`)  
**Test:** JJ 'b' command (band sweep) or 'n' (Crossfire) on COM6.

### [ ] LED alert system still disabled
**Impact:** No visual alert for the operator. The device is serial-output-only for threat indication.  
**Root cause:** Originally disabled because ambient ISM traffic caused false CRITICAL. Now that the detection engine is redesigned, LED should be re-evaluated.  
**Fix:** Re-enable LED in `alert_handler.cpp` and test with current detection thresholds.  
**Dependency:** Should be validated during or after field testing when thresholds are finalized.

---

## HIGH — Significant Limitations

### [ ] Bench environment produces 4-7 distinct ambient LoRa frequencies in 3-5 seconds
**Impact:** Frequency diversity thresholds (WARNING=5, CRITICAL=8) are set conservatively to avoid bench false positives. This may make ELRS detection slower than necessary in clean field environments.  
**Fix:** Field test will determine if thresholds can be lowered. If field baseline diversity is 0-2, drop to WARNING=3, CRITICAL=5.  
**Data needed:** Field baseline diversity measurements at multiple locations.

### [ ] Long-running bench degradation
**Impact:** After 60-90+ seconds of runtime on a LoRa-rich bench, ambient state accumulates enough to cause false WARNING/CRITICAL. Post-warmup reset only fires once.  
**Root cause:** Continuous ambient LoRa sources produce new non-ambient CAD taps faster than the auto-learn (60s) can absorb them.  
**Possible fix:** Periodic state reset every N minutes, or continuous diversity baseline recalculation.  
**Field relevance:** May not exist outdoors. Needs field validation.

### [ ] ELRS detection time varies significantly (2s to 48s across sprints)
**Impact:** The operator can't predict how quickly the system will alert.  
**Root cause:** Detection speed depends on which confidence path triggers first — diversity (fast but threshold-dependent), RSSI persistence (reliable but slow ~9-12s minimum), or confirmed CAD taps (definitive but statistically rare).  
**Status:** Current clean-boot WARNING time is driven by diversity threshold. If diversity < DIVERSITY_WARNING, falls back to RSSI persistence path (~30s). Field testing will determine realistic expectations.

### [ ] RSSI sweep noise floor varies between LoRa and FSK mode
**Impact:** RSSI thresholds tuned in FSK mode may not apply correctly after mode switches.  
**Status:** Currently the RSSI sweep always runs in FSK mode (correct). But if Phase 3 FSK detection is enabled, the mode switch sequence changes the radio state between sweeps.

### [ ] bandEnergyElevated removed from mediumConfidence
**Impact:** The band energy trending feature (rolling average of 902-928 MHz RSSI) is computed but no longer feeds into threat escalation. It was causing false WARNING on bench.  
**Fix:** Re-evaluate in the field. If field noise floor is stable, band energy could be a useful FHSS indicator at a higher threshold.  
**Code:** `detection_engine.cpp` — `bandEnergyElevated` is computed but not used in `assessThreat()`.

---

## MEDIUM — Feature Gaps

### [ ] No buzzer alert system
**Impact:** No audible alert for the operator. In the field, visual-only alerts on a tiny OLED are inadequate.  
**Status:** Sprint prompt written (`SENTRY-RF_BUZZER_ALERT_SPRINT_PROMPT.md`). Design complete (edge-triggered, per-source tone patterns, cooldown suppression, ACK/mute gestures). Not implemented.  
**Sprint:** Phase 3B in the phased plan.

### [ ] No boot self-test
**Impact:** No way to verify hardware health at power-on. A broken antenna or dead GPS gives no warning.  
**Status:** Designed (radio health, antenna quality, GPS fix timeout, scan cycle watchdog). Not implemented.  
**Sprint:** Phase 4A in the phased plan.

### [ ] No operational modes (STANDARD/COVERT/HIGH-ALERT)
**Impact:** No way to suppress WiFi emissions for COVERT operation or optimize for fastest detection in HIGH-ALERT.  
**Status:** Designed. Not implemented.  
**Sprint:** Phase 4B in the phased plan.

### [ ] LR1121 CAD stub still returns {0,0,0,0,0,0}
**Impact:** Dual-band board has no CAD detection capability — running blind on its strongest feature.  
**Status:** Hardware not yet in hand. Sprint 5A-5C in the phased plan.  
**Hardware prerequisites:** DIO9 GPIO 36, TCXO voltage verification, antenna connectors.

### [ ] No GNSS integrity improvements (C/N0 uniformity, position jump, jam+fix-loss correlation)
**Impact:** GNSS spoofing/jamming detection is basic — uses u-blox built-in indicators only, no host-side algorithms.  
**Status:** Designed in Sprint 3A of the phased plan. Not implemented.

### [ ] No enhanced data logging for field analysis
**Impact:** Current logging doesn't capture per-cycle CAD state, diversity counts, or threat transitions with enough detail for post-test analysis.  
**Status:** Sprint 6B in the phased plan. Needed for field test.

### [ ] No field test analysis tooling
**Impact:** No automated way to compute Pd, Pfa, time-to-alert from log files.  
**Status:** Sprint 6C in the phased plan. Python script needed.

---

## LOW — Nice to Have / Future

### [ ] Power management / battery life optimization
**Sprint:** Phase 7.1

### [ ] OTA firmware updates
**Sprint:** Phase 7.2

### [ ] Multi-device mesh (ESP-NOW)
**Sprint:** Phase 7.3

### [ ] Multi-constellation GNSS consistency
**Sprint:** Phase 7.4

### [ ] Compass integration (FlyFishRC M10QMC)
**Requires:** GPIO 10/21 resistor removal on T3S3  
**Sprint:** Phase 7.5

### [ ] Confidence scoring system (weighted per-detection-source)
**Sprint:** Phase 6D — requires field data for weight calibration

### [ ] DJI OFDM detection via LR1121 2.4 GHz RSSI
**Sprint:** Phase 5C — requires LR1121 hardware

---

## Testing Methodology Issues

### [ ] Bench test scripts don't properly reset the device
**Impact:** Tests inherit stale state from previous runs, producing misleading results.  
**Root cause:** T3S3 native USB DTR/RTS reset is unreliable. Serial port open doesn't always trigger a reboot.  
**Fix:** Test scripts must use DTR/RTS toggle AND verify boot message ("SENTRY-RF v1.x.x") before starting measurements. Alternatively, use the RST button or power cycle.  
**Lesson learned:** Every test failure from Sprint 2B through 2C was traced to stale state, not code bugs.

### [ ] No automated regression test for detection timing
**Impact:** Each sprint potentially regresses WARNING/CRITICAL timing without automated detection.  
**Fix:** Create a Python test that boots SENTRY, waits for warmup, starts JJ, measures time-to-WARNING and time-to-CRITICAL, and fails if they exceed thresholds.

---

## Architecture Decisions Documented

### Frequency diversity is the correct FHSS discriminator
- Hit COUNT doesn't separate drone from ambient (both produce similar counts)
- Hit SPREAD (distinct frequencies) does separate them — drones hit many frequencies, infrastructure hits few
- 3-second window prevents ambient accumulation while capturing FHSS pattern
- Bench validation limited (ambient diversity 4-7 overlaps with ELRS diversity 2-5)
- Field validation needed to confirm separation in clean environments

### CAD detects full LoRa packet, not just preamble
- Confirmed by Semtech AN1200.48 and AN1200.85
- Detection window is ~5ms (full packet airtime), not ~1ms (preamble only)
- This 5x increase in detection window was incorporated in Sprint 2A-fix

### Persistence (consecutive hits) is the infrastructure discriminator
- Drones transmit continuously via FHSS — short-lived per-frequency
- Infrastructure transmits intermittently on fixed frequencies — long-lived per-frequency
- 3 consecutive hits = confirmed (drone FHSS expires in ~2s per frequency)
- 10+ second fixed-frequency tap = auto-learn as ambient

### Never call radio.begin() or radio.beginFSK() mid-operation
- Causes -707 CHIP_NOT_FOUND (full chip reset + SPI re-probe fails)
- Use SPI opcode 0x8A for packet type switching
- RadioLib internal state becomes stale after raw SPI switches — known issue, causes FSK Phase 3 corruption

---

*Last updated: April 1, 2026 — post-Sprint 2C*
*Review this document before every sprint.*
