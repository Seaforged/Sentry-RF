# SENTRY-RF Known Issues & Unfinished Work Tracker
## As of April 5, 2026 — Post-Sprint 5/5 + Code Review

This document tracks every identified issue, limitation, and unfinished item. Nothing gets forgotten. Check items off as they're resolved. Reference this before every sprint.

---

## CRITICAL — Must Fix Before Field Deployment

### [x] GPS_MIN_CNO is set to 6 (indoor testing value) — RESOLVED
**Impact:** Weak attenuated signals degrade fix quality and integrity monitoring.  
**Fix:** Raised to 15 in `sentry_config.h`.  
**Resolved:** Pre-field-test (Sprint 6A). Current value: `GPS_MIN_CNO = 15`.

### [x] FSK Phase 3 disabled (#if 0) — RESOLVED
**Impact:** Crossfire 150 Hz mode and FrSky R9 drones were invisible to primary detection.  
**Fix:** Sprint 1/5 (commit `f4361ea`) moved Phase 3 after `switchToFSK()`. FSK→FSK transition avoids the RadioLib state corruption. Phase 3 now runs after the LoRa→FSK switch, scans Crossfire channels, then restores RSSI sweep params.  
**Resolved:** April 2, 2026.

### [x] Boot banner version string was hardcoded — RESOLVED
**Impact:** Serial output showed "v1.2.0-FIX4" instead of the actual firmware version from `version.h`.  
**Fix:** Replaced hardcoded string in `main.cpp` setup() with `Serial.printf` using `FW_NAME` and `FW_VERSION`.  
**Resolved:** April 5, 2026.

### [ ] LED alert system still disabled
**Impact:** No visual alert for the operator. The device is serial-output-only for threat indication.  
**Root cause:** Originally disabled because ambient ISM traffic caused false CRITICAL. Now that the detection engine is redesigned, LED should be re-evaluated.  
**Fix:** Re-enable LED in `alert_handler.cpp` and test with current detection thresholds.  
**Dependency:** Should be validated during or after field testing when thresholds are finalized.

---

## HIGH — Significant Limitations

### [ ] FSK_DETECT_THRESHOLD_DBM requires manual toggle between bench and field
**Impact:** -50 dBm is bench-safe but too conservative for field use. -70 dBm is field-appropriate but causes immediate CRITICAL on bench from ambient ISM energy (-60 to -70 dBm on Crossfire frequencies).  
**Current value:** -50 dBm (bench-safe, set in commit `8160c8b`).  
**Fix:** Change to -70 dBm in `sentry_config.h` before field deployment. Long-term: add runtime mode switch (BENCH/FIELD) or auto-detect based on ambient energy level.

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

### [x] bandEnergyElevated removed from mediumConfidence — RESOLVED
**Impact:** Band energy trending was computed but not used in threat escalation.  
**Fix:** Sprint 3/5 (commit `be1b007`) re-integrated band energy into confidence scoring at 8 dB threshold with `WEIGHT_BAND_ENERGY = 5`. Also added EU band (863-870 MHz) tracking.  
**Resolved:** April 3, 2026.

---

## MEDIUM — Feature Gaps

### [x] No buzzer alert system — RESOLVED
**Impact:** No audible alert for the operator.  
**Fix:** Implemented in `buzzer_manager.cpp` and `alert_handler.cpp`. 7 tone patterns (self-test, RF advisory/warning, GNSS warning, Remote ID, critical, all-clear). Non-blocking LEDC PWM. ACK (1s hold) and mute (3s hold) via BOOT button. Auto-unmute after 5 minutes.  
**Resolved:** Pre-v1.4.0.

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

### [x] Confidence scoring system (weighted per-detection-source) — RESOLVED
**Fix:** Sprint 5/5 (commit `c98b1d7`) replaced boolean threat logic with weighted scoring. Weights in `sentry_config.h`. Thresholds: ADVISORY=8, WARNING=24, CRITICAL=40.  
**Resolved:** April 3, 2026.

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

*Last updated: April 5, 2026 — post-Sprint 5/5 + code review*
*Review this document before every sprint.*
