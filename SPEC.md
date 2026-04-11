# Spec: SENTRY-RF v1.6.0 — LR1121 Stabilization Release

**Status:** APPROVED 2026-04-10 — proceeding to PLAN phase
**Date:** 2026-04-10
**Author:** ND + Claude Code (agent-skills:spec-driven-development)

---

## Objective

Ship **SENTRY-RF v1.6.0** as a public release by end of day Sunday 2026-04-12, focused on stabilizing the T3S3 LR1121 board for first real-signal validation. The weekend "tester" is ND himself — goals are: (1) confirm the LR1121 board boots cleanly, renders all screens, and does not crash under load, and (2) observe and understand how the AAD detection engine reacts to real ELRS signals on both sub-GHz (915 MHz) and 2.4 GHz using a newly-acquired RadioMaster transmitter. A future external tester will receive the same build Monday or later.

**This is a stabilization release, not a feature release.** Everything on the "not yet built" list in `.mex/ROUTER.md` stays on the roadmap.

### Why this matters

- The LR1121 build has never been soak-tested or exercised against a real controlled RF signal — only bench ambient
- The 2.4 GHz CAD phase (added this week) has never seen a real 2.4 GHz ELRS signal
- The AAD detection engine's behavior on real signals has been validated on SX1262 (842m suburban field test) but not on the LR1121
- v1.5.3 on LR1121 has known-visible bugs (version string, unconfirmed spectrum rendering) that a first-time viewer will notice immediately

## Tech Stack

No changes from v1.5.3. See `.mex/context/stack.md` for the full list:
- C++ Arduino framework, PlatformIO, ESP32-S3
- RadioLib v7.6.0 (pinned — no upgrades this weekend)
- SparkFun u-blox GNSS v3, Adafruit SSD1306 + GFX
- FreeRTOS dual-core, 5 tasks pinned across Core 0 / Core 1

## Commands

All three targets must compile clean:
```
pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121
```

LR1121-specific build / flash / monitor:
```
pio run -e t3s3_lr1121
pio run -e t3s3_lr1121 --target upload --upload-port COM14
pio device monitor -b 115200 -p COM14
```

Soak test (existing script, new application to LR1121):
```
cd C:\Projects\sentry-rf
python soak_test.py COM14 120   # 2-hour soak on LR1121
```

Signal response test (manual — RadioMaster TX + ND observation):
```
# Terminal 1:
python field_logger.py COM14

# Meanwhile: power up RadioMaster ELRS 915 MHz, observe serial + OLED
# Repeat for ELRS 2.4 GHz
```

## Project Structure

No structural changes. Existing layout stays:
- `include/` — board config, sentry config, task config, detection types
- `src/` — main, cad_scanner, rf_scanner, detection_engine, gps_manager, wifi_scanner, display, alert_handler, buzzer_manager
- `docs/` — gets a new `DETECTION_ENGINE_WALKTHROUGH.md` this weekend
- `test/lr1121_hello/` — existing hello sketch, left alone

See `.mex/context/architecture.md` for the FreeRTOS task layout.

## Code Style

No changes from v1.5.3. See `.mex/context/conventions.md`. Key reminders:
- No hardcoded pins — everything via `board_config.h`
- All three targets must compile clean before any commit
- Board-specific code wrapped in `#ifdef BOARD_T3S3 / BOARD_HELTEC_V3 / BOARD_T3S3_LR1121`
- LR1121 RSSI uses `LR1121_RSSI::getInstantRSSI()`, not bare `getRSSI()`

## Testing Strategy

Three test levels for this release:

### 1. Build verification (5 min, every commit)
`pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121` must succeed with zero errors. This is the baseline gate.

### 2. Boot and render verification (10 min, after every LR1121 flash)
Flash, power cycle, manually verify:
- Serial banner prints `SENTRY-RF v1.6.0` within 2 seconds
- OLED splash → dashboard transition within 60 seconds (warmup)
- All 7 screens cycle cleanly via short BOOT button press
- Spectrum screens (sub-GHz and 2.4 GHz) show visible bars, not blank
- System screen has no text/page-dot collision
- No error messages on OLED, no crashes in serial
- First `[CAD]` line shows `score=5` (baseline after warmup)

### 3. Signal response and soak (weekend, manual, ND-driven)
- **Saturday afternoon:** RadioMaster ELRS 915 MHz within ~1-5m of antenna → observe serial + OLED while the detection engine escalates from CLEAR through ADVISORY → WARNING → CRITICAL. Turn TX off, observe cooldown. Capture serial log for the walkthrough document.
- **Saturday evening:** Same exercise at ELRS 2.4 GHz. This is the first-ever real 2.4 GHz ELRS signal test on this codebase.
- **Sunday morning:** 2-hour LR1121 bench soak with no drones present — no crashes, no false positives above ADVISORY, memory stable (monitor heap via serial).
- **Sunday afternoon:** Multi-environment walk — bench → backyard → rural road → suburban — with the RadioMaster running. Confirm detection works end-to-end.

**Test recording:** All serial sessions captured with `field_logger.py` for reference in the walkthrough doc and release notes.

## Boundaries

### Always
- Run `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121` before every commit
- Connect the antenna before powering on any board (non-negotiable; RF damage)
- Use the mex workflow — read `.mex/ROUTER.md` at the start of each session, update on GROW
- Commit after every working increment, not at the end of a sprint
- Tag known-good states as we go (e.g., `v1.6.0-rc1` before the soak test)

### Ask first
- Anything that touches `sentry_config.h` constants (AAD gates, scoring weights, thresholds) — these are field-calibrated and we don't tune them without data
- Adding a new `#ifdef` guard (risk of board-specific drift)
- Changing the FreeRTOS task layout (priorities, cores, stack sizes)
- Modifying the Detection Engine scoring math

### Never
- Add BLE Remote ID, 2.4 GHz protocol classification, operational modes, or any other "not yet built" item from ROUTER
- Upgrade RadioLib mid-weekend
- Push to origin without an explicit "push it" from ND
- Use `--no-verify` or `--force` on git operations
- Remove failing tests or soak data to make things look clean

## Success Criteria

v1.6.0 ships if and only if **all** of these are true by Sunday evening:

1. **Clean build:** `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121` — zero errors, zero warnings introduced this release
2. **Version sanity:** `grep -r "v1.2.0-FIX4\|v1.5.3" src/ include/` returns nothing (after the fix); `version.h` reads `1.6.0`; boot banner prints `SENTRY-RF v1.6.0`
3. **Boot behavior on LR1121:** Serial banner within 2 seconds of USB plug-in on a fresh flash, no template placeholders, no error dumps
4. **All 7 OLED screens render** on the LR1121 with no visible text collisions or blank chart areas. Verified by ND's eyes, captured in photos for the release notes
5. **Boot self-test catches a missing antenna** (Tier 2 work item below) — remove the SMA antenna, power on, confirm OLED shows `ANTENNA CHECK` or equivalent, confirm scan task does not start
6. **Real signal response on sub-GHz** — RadioMaster ELRS 915 MHz at close range drives the LR1121 from CLEAR → CRITICAL within 15 seconds. Cooldown returns to CLEAR within 15 seconds after TX off
7. **Real signal response on 2.4 GHz** — RadioMaster ELRS 2.4 GHz at close range produces visible detection in the serial log (CAD taps increment, or persDiv rises). **No target timing number** — document actual observed timing for future calibration. The 2.4 GHz CAD phase has never seen a real ELRS 2.4 signal; this is a first-light test, not a benchmark.
8. **2-hour soak on the LR1121** — zero cycles above WARNING with no drone present, zero crashes, heap stable within ±5% of start
9. **`docs/DETECTION_ENGINE_WALKTHROUGH.md` exists** — annotated serial log from the real-signal tests, explaining what each `[CAD]` field means and how the AAD gates fire in practice
10. **v1.6.0 tagged, release notes written, pushed to origin** — public GitHub release with the walkthrough doc and field test summary linked

If any of 1-9 fail, do NOT do 10. Ship when green, not when the calendar says so.

## Work Items (prioritized)

### TIER 1 — Small, low-risk (Saturday morning, before RadioMaster arrives)
- [ ] **T1.1 Version audit** — grep for `v1.2.0-FIX4` and stale v1.5.x references in `src/` and `include/`, remove. Bump `version.h` to `1.6.0`. (~15 min)
- [ ] **T1.2 Serial boot banner sanity** — flash LR1121, confirm banner prints cleanly within 2s of USB plug-in. Fix if not. (~15 min)
- [ ] **T1.3 Visual verification of spectrum bar rendering** — power on LR1121, cycle to Sub-GHz and 2.4 GHz spectrum screens, confirm bars are visible (not blank). Photograph for release notes. (~15 min)
- [ ] **T1.4 System screen collision check** — cycle to Screen 5 on the LR1121, confirm "Buz: ... Cmp: ..." line does not overlap the page dots. (~5 min)
- [ ] **T1.5 GPS + buzzer bring-up** — with the newly-wired GPS + buzzer, confirm `[GPS] Fix:3` in serial within 60s outdoors, and buzzer self-test tone on boot. (~30 min)
- [ ] **T1.6 All-three build gate** — `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121`. (5 min)

### TIER 2 — Stabilization (Saturday afternoon)
- [ ] **T2.1 Boot self-test: antenna check (ALL 3 boards)** — at the end of `scannerInit()`, sweep 10 test frequencies across 860-928 MHz and read RSSI. If all ≤ threshold (config constant, default -125 dBm), display `ANTENNA CHECK` on OLED and halt the scan task. If any > threshold, proceed normally. Implement once in `rf_scanner.cpp`, wire into all three board targets (SX1262 T3S3, Heltec V3, LR1121). Test SX1262 first (known-working baseline from the 30-min soak), then Heltec, then LR1121. Document the threshold as a config constant. (~3-6 hrs)
- [ ] **T2.2 Human-readable error messages on init failure** — replace `RADIO HW FAIL: -12` with a table lookup that maps common RadioLib error codes to operator-friendly text. (~1-2 hrs)
- [ ] **T2.3 Boot banner print order cleanup** — ensure `SENTRY-RF v1.6.0` prints BEFORE any init errors, so tester sees the version even on a failed boot. (~30 min)

### TIER 3 — Real-signal validation (Saturday afternoon → Sunday evening)
- [ ] **T3.1 Sub-GHz 915 MHz ELRS response test** — flash LR1121, power up RadioMaster ELRS 915, capture serial log with `field_logger.py`. Observe: time-to-ADVISORY, time-to-WARNING, time-to-CRITICAL, peak score, peak `persDiv`, cooldown-to-CLEAR. Compare against SX1262 baseline (6.4s WARNING / 11.2s CRITICAL). (~1-2 hrs)
- [ ] **T3.2 2.4 GHz ELRS response test (FIRST EVER)** — flash LR1121, power up RadioMaster ELRS 2.4 GHz, capture serial log. Observe: does the 2.4 GHz CAD phase produce hits? Does `div` increase? Does `persDiv` activate? Pass criterion is "detection is visible in serial log," tighter timing metrics are nice-to-have. (~1-2 hrs)
- [ ] **T3.3 Multi-environment walk** — with RadioMaster running, walk bench → backyard → rural → suburban, observe detection stability. No specific range goal (that's a post-stabilization exercise). (~1-2 hrs)
- [ ] **T3.4 2-hour LR1121 bench soak** — no drones present. Zero escalations above ADVISORY target. Use existing `soak_test.py COM14 120`. (~2 hrs, mostly waiting)

### TIER 4 — Tester enablement + release (Sunday afternoon / evening)
- [ ] **T4.1 `docs/DETECTION_ENGINE_WALKTHROUGH.md`** — annotated walkthrough of a real sub-GHz ELRS session. Show the serial log, explain each `[CAD]` field (cycle, conf, taps, div, persDiv, vel, sustainedCycles, score), map the AAD gates to observed behavior, explain why score stays at 5 until sustainedCycles hits 5, show the ambient warmup, show the diversity gate firing. Uses logs captured in T3.1/T3.2. (~2-3 hrs)
- [ ] **T4.2 Release notes for v1.6.0** — public-facing. Headline: first LR1121 stabilization release. Include: what was fixed, what was added (dual-band CAD, boot self-test), what's known-limited (2.4 GHz timing not yet calibrated), what's next on the roadmap. Link to the walkthrough doc. Include signal-response metrics from T3.1. (~1 hr)
- [ ] **T4.3 Update `.mex/ROUTER.md` Current Project State** — mark v1.6.0 as shipped, move completed items out of "not yet built", update "Known issues". (~15 min)
- [ ] **T4.4 Tag `v1.6.0`, create GitHub release, push to origin** — only if all Success Criteria are met. (~30 min)

### TIER 5 — Explicitly cut from this release (stays on roadmap)
- SD card fix (hardware limitation)
- BLE Remote ID scanning (feature, roadmap Phase D1)
- 2.4 GHz protocol classification (feature, roadmap Phase C1)
- Per-band diversity tracking (refactor, roadmap Phase C2)
- Dual-band correlation engine (feature, roadmap Phase D2)
- Operational modes COVERT / HIGH_ALERT (feature, roadmap Phase E1)
- Tester-oriented quickstart doc (ND is the weekend tester, defer until external tester confirmed)

## Resolved Questions (2026-04-10)

1. **2.4 GHz timing target:** No target number. Document actual observed timing; future release will calibrate against it.
2. **Boot self-test scope:** All three boards (SX1262 T3S3, Heltec V3, LR1121). Test order: SX1262 → Heltec → LR1121 to build on a known-good baseline.
3. **Walkthrough doc location:** `docs/DETECTION_ENGINE_WALKTHROUGH.md` in the SENTRY-RF repo, linked from the v1.6.0 release notes.
4. **Soak script compatibility:** Serial format is identical to SX1262, so `soak_test.py` should work as-is. Verify during T3.4; if it fails, it's a script bug not a firmware bug.

## Verification (before proceeding to PLAN phase)

- [ ] This spec covers all six core areas (Objective, Stack, Commands, Structure, Code Style, Testing Strategy, Boundaries)
- [ ] ND has reviewed and approved the spec
- [ ] Success criteria are specific and testable
- [ ] Boundaries (Always / Ask First / Never) are defined
- [ ] The spec is saved to `SPEC.md` in the repository root
- [ ] Open questions have proposed answers or are marked for resolution
