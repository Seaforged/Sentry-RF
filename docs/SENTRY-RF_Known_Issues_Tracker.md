# SENTRY-RF Known Issues & Unfinished Work Tracker
## As of April 6, 2026 — v1.5.2

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

### [x] Bench false WARNING/CRITICAL from ambient LoRa — RESOLVED
**Impact:** Ambient LoRaWAN/Meshtastic infrastructure on the bench produced 3-5 distinct CAD frequency hits in the diversity window, triggering false WARNING (score=37-45) and CRITICAL (score=100) with no drone present.  
**Fix:** AAD Sprint 1 — sustained-diversity persistence gate (commits `fb8e6a2` through `99d9639`). Diversity only counts toward scoring when raw diversity stays >= 3 for 3 consecutive scan cycles (~7.5s). LoRaWAN can't sustain this (max sustainedCycles=1 on bench); FHSS drones sustain it for 9+ cycles continuously.  
**Bench validation:** Baseline holds CLEAR with sustainedCycles max 1, persDiv=0, score=5. ELRS FHSS detection reaches CRITICAL in 6.6s with persDiv=32, score=100.  
**Resolved:** April 5, 2026 (v1.5.0).

### [x] Soak test false positives from ambient CAD tap confirmation — RESOLVED
**Impact:** 30-minute soak test (no drone) showed 7.77% false positive rate. Ambient LoRaWAN gateways accumulated confirmed CAD taps (conf=3, score=45+) before being tagged as ambient. Fast-detect bonus triggered on raw diversity (div>=5) instead of persistent diversity.  
**Fix:** Three changes: (1) Fast-detect now requires persistent diversity, not raw (commit `755edc9`). (2) Ambient auto-learn reduced from 60s to 15s — gateways tagged faster. (3) Confirmed tap weight halved when persistentDiversity==0 — ambient conf=1 scores 7 pts instead of 15, keeping total below WARNING (commit `913ecd8`).  
**Validation:** 15-minute soak test: 0.00% false positive rate, 391 cycles, max score=22, zero cycles above WARNING. 4/4 detection modes (ELRS, band sweep, CW, mixed FP) still reach CRITICAL with score=100.  
**Resolved:** April 6, 2026 (v1.5.2).

### [x] LED alert system still disabled — RESOLVED
**Impact:** No visual alert for the operator.  
**Fix:** Re-enabled in commit `0114cf0` with threat-level blink patterns: CLEAR=off, ADVISORY=slow blink (500ms), WARNING=fast blink (200ms), CRITICAL=solid on. Non-blocking millis() timing. Safe to enable now that AAD persistence gate eliminates false alarms.  
**Resolved:** April 6, 2026 (v1.5.1).

---

## HIGH — Significant Limitations

### [ ] FSK_DETECT_THRESHOLD_DBM requires manual toggle between bench and field
**Impact:** -50 dBm is bench-safe but too conservative for field use. -70 dBm is field-appropriate but causes immediate CRITICAL on bench from ambient ISM energy (-60 to -70 dBm on Crossfire frequencies).  
**Current value:** -50 dBm (bench-safe, set in commit `8160c8b`).  
**Fix:** Change to -70 dBm in `sentry_config.h` before field deployment. Long-term: add runtime mode switch (BENCH/FIELD) or auto-detect based on ambient energy level.

### [x] Bench environment produces 4-7 distinct ambient LoRa frequencies in 3-5 seconds — MITIGATED
**Impact:** Ambient LoRa diversity overlapped with drone FHSS diversity thresholds, causing false escalation.  
**Mitigation:** AAD sustained-diversity persistence gate (v1.5.0). Raw diversity still reaches 3-5 from ambient, but `persDiv` stays at 0 because ambient sources don't sustain high diversity across 3 consecutive scan cycles. Drone FHSS sustains it for 9+ cycles. The raw `div` metric is now informational only — scoring uses `persDiv`.  
**Remaining risk:** Dense urban environments with many simultaneous LoRaWAN gateways could theoretically sustain div>=3 for 3+ cycles. Not yet tested in urban deployment. AAD Sprint 2 (continuous ambient catalog) would further mitigate this.  
**Mitigated:** April 5, 2026.

### [x] Long-running bench degradation — MITIGATED
**Impact:** After 60-90+ seconds on a LoRa-rich bench, ambient state accumulated causing false escalation.  
**Mitigation:** AAD sustained-diversity persistence gate (v1.5.0) prevents ambient diversity from triggering escalation. Tap prune on sustained-diversity drop (commit `66b5501`) aggressively clears confirmed taps when a drone departs, preventing stale state from accumulating across detection cycles. Cooldown reduced from 15s to 5s per level for faster return to CLEAR.  
**Remaining risk:** Not tested beyond ~2 hours continuous runtime. Very long deployments may still accumulate edge-case state.  
**Mitigated:** April 6, 2026.

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

*Last updated: April 6, 2026 — v1.5.2 (zero false positives, all detection modes validated)*
*Review this document before every sprint.*
