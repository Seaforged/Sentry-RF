# Plan: SENTRY-RF v1.6.0 — Implementation Sequence

**Derived from:** `SPEC.md` (approved 2026-04-10)
**Status:** Draft — awaiting ND review before Phase 3 (TASKS)
**Date:** 2026-04-10

---

## Overview

Four phases, each with explicit exit criteria. **Do not advance to the next phase until the current one's checkpoint is green.** If a phase blows its time budget, cut scope within the phase before spilling into the next one.

```
P1: Small Fixes    →  P2: Stabilization  →  P3: Signal + Soak  →  P4: Docs + Release
   (Sat AM)            (Sat PM)              (Sat PM + Sun AM)     (Sun PM)
     │                    │                        │                    │
     ▼                    ▼                        ▼                    ▼
  All 7 screens        Antenna check          All success           v1.6.0 live
  render clean         works on 3 boards      criteria 1-8 green    on GitHub
  SC 1-4 ✓             SC 5 ✓                                       SC 9-10 ✓
```

## Critical Path

**T1.1 → build → T1.3 → [block if blank] → T1.4 → T1.5 → T1.6**
**→ T2.1 (largest, serial testing on 3 boards) → T1.6 → T2.2 → T2.3 → T1.6**
**→ [Saturday signal testing: T3.1 → T3.2] → [Sun AM: T3.4 soak → T3.3 walk in parallel]**
**→ T4.1 → T4.2 → T4.3 → T4.4**

Longest serial dependency chain: T1.1 → T2.1 → T3.4 → T4.2 → T4.4. Everything else can be compressed around it.

---

## Phase P1 — Small Fixes (Saturday AM, budget 2 hours)

### Components
- `include/version.h` — bump
- `src/main.cpp` — banner print order, hardcoded version string audit
- `src/display.cpp` — visual-only verification (no code change expected)
- GPS + buzzer hardware bring-up

### Order of operations
1. **T1.1 Version audit** (15 min) — grep `src/ include/` for `v1.2.0-FIX4` and any stale `v1.5.x` references. Bump `version.h` to `1.6.0`. Build test.
2. **T1.6 Triple-target build** (5 min) — `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121`. Must pass before flashing.
3. **Flash LR1121 on COM14** (1 min) — `pio run -e t3s3_lr1121 --target upload --upload-port COM14`.
4. **T1.2 Boot banner check** (5 min) — `pio device monitor -b 115200 -p COM14`, watch for `SENTRY-RF v1.6.0` within ~2 seconds.
5. **T1.3 Visual spectrum verification** (10 min) — cycle through all 7 OLED screens with the BOOT button. Photograph each. Confirm spectrum bars are visible on Sub-GHz and 2.4 GHz screens. **This is the first hard checkpoint — if the bars are blank, STOP and debug before anything else.**
6. **T1.4 System screen collision check** (2 min) — included in the photo pass above. Should already be fixed from yesterday's edit, just confirming.
7. **T1.5 GPS + buzzer hardware bring-up** (30 min) — parallel path, hardware-dependent. Wire tonight per ND's plan; Saturday morning verify `[GPS] Fix:3` in serial (outdoors) and buzzer tone on boot.

### Parallelization
- T1.5 (GPS/buzzer wiring) is fully parallel to T1.1-T1.4 because it's hardware work, not code. ND can do it before opening the laptop Saturday morning.
- T1.3 and T1.4 happen in the same flash+observe session.

### Risks
- **R1 — Spectrum bars still blank on LR1121.** We never visually confirmed after yesterday's `startReceive()`-per-bin fix. If confirmed broken, this is the ONLY thing that matters until fixed. Cut P2 and P3 until resolved. Mitigation: the fix is already in the code, we just need eyes on hardware.
- **R2 — GPS fix takes longer than expected indoors.** Not a release blocker. If no fix in 60 seconds, take the board outside before declaring it broken.
- **R3 — Buzzer doesn't sound.** Check LEDC channel 1 isn't conflicting with anything else and the GPIO 16 wire is right. Known boilerplate issue.

### Exit checkpoint (all must be true)
- [ ] `grep -rE "v1\.(2|5)\.[0-9]" src/ include/` returns only `FW_VERSION = "1.6.0"` and any intentional historical comments
- [ ] `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121` — zero errors, zero new warnings
- [ ] LR1121 boot serial log shows `SENTRY-RF v1.6.0` within 3 seconds of power-on
- [ ] All 7 OLED screens render with no blanks, no text collisions
- [ ] GPS (if outdoors) or buzzer tone confirmed working
- [ ] Success Criteria 1-4 from SPEC.md can be marked green

**If exit checkpoint fails:** debug in-place. Do not start P2 with P1 in a red state.

---

## Phase P2 — Stabilization (Saturday PM, budget 4 hours, hard cut at 6)

### Components
- `src/rf_scanner.cpp` — add `scannerAntennaCheck()` function, wire into all 3 board paths
- `src/main.cpp` — call antenna check after `scannerInit()`, display result on OLED, halt scan task on failure
- `src/main.cpp` — error code lookup table + human-readable display
- `src/main.cpp` — ensure version banner prints before any init errors

### Order of operations
1. **T2.3 Boot banner print order cleanup** (30 min) — this is small and unlocks everything else because tester-facing errors will now carry the version number. Do it first.
2. **T2.1 Boot self-test — SX1262 T3S3 first** (1-2 hrs)
   - Write `scannerAntennaCheck()` in `rf_scanner.cpp`. Sweeps 10 frequencies across 860-928 MHz, returns `true` if any reads > `ANTENNA_CHECK_THRESHOLD_DBM` (default -125), otherwise `false`.
   - Add `ANTENNA_CHECK_THRESHOLD_DBM = -125.0f` constant to `sentry_config.h`.
   - Call it at end of `scannerInit()`. On failure, display `ANTENNA CHECK` on OLED and halt scan task via infinite `delay(1000)` loop (matches existing error-halt pattern).
   - **Test on SX1262 T3S3 (COM9) first:** flash, boot normally with antenna → confirm "no ANTENNA CHECK on screen, scan task runs". Then remove antenna, power cycle → confirm `ANTENNA CHECK` appears on OLED and scan task halts.
3. **T2.1 Boot self-test — Heltec V3** (30 min)
   - Same code path, different board. Flash Heltec V3 (COM13), repeat both antenna tests.
4. **T2.1 Boot self-test — LR1121** (30 min)
   - Flash LR1121 (COM14), repeat both antenna tests. This is the target board for the weekend.
5. **T1.6 Triple-target build gate** (5 min) — after T2.1 is green on all 3 boards.
6. **T2.2 Human-readable error messages** (1-2 hrs)
   - Table-driven: map known RadioLib error codes to short operator-friendly strings. At minimum cover: `-2` (no chip), `-12` (invalid freq), `-102` (invalid freq deviation), `-707` (chip not found), plus a default "RADIO FAIL: %d" for unknown codes.
   - Table lives in a new small file `src/error_messages.cpp` / `include/error_messages.h` to keep `main.cpp` clean.
   - Use in the existing OLED error-halt paths in `main.cpp` setup().
7. **T1.6 Triple-target build gate** (5 min) — final build pass.

### Parallelization
- T2.2 and T2.3 are independent of T2.1. If T2.1 blows its time budget, T2.2/T2.3 can still ship without it.
- The three-board flash sequence in T2.1 is strictly serial (one board plugged in at a time).

### Risks
- **R4 — Boot self-test breaks SX1262 or Heltec boot.** Both were field-validated at v1.5.3. Mitigation: test SX1262 first because we know exactly what "working" looks like there (30-min soak + suburban field test). If SX1262 regresses, fix before moving to Heltec or LR1121.
- **R5 — `ANTENNA_CHECK_THRESHOLD_DBM = -125.0f` is wrong for some board.** The SX1262 noise floor is ~-110 dBm, LR1121 is ~-127 dBm. A universal threshold may not work. Mitigation: if a board reports "antenna connected" when it's not, per-board threshold via `board_config.h` — adds 15 minutes.
- **R6 — T2.1 eats the time budget.** Hard cut at 6 hours total for P2. If T2.1 isn't done by then, revert the T2.1 commit, document "ALWAYS CONNECT ANTENNA FIRST" prominently in the release notes, and proceed to P3.
- **R7 — Error code table grows beyond what we've actually seen.** Don't map codes we haven't hit in real logs. Mitigation: only include codes from SPEC section with a TODO comment for adding more later.

### Exit checkpoint (all must be true)
- [ ] `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121` — zero errors
- [ ] Each of three boards: removing the antenna → `ANTENNA CHECK` on OLED, scan task halted
- [ ] Each of three boards: antenna connected → normal boot, scan task runs
- [ ] Inducing a known radio init failure (e.g., swap SPI pins in a branch, or unplug DIO1 on Heltec) shows a human-readable error on OLED, not a raw error number
- [ ] Success Criterion 5 from SPEC.md marked green
- [ ] Git commit tagged `v1.6.0-rc1` as the pre-signal-test baseline

**If exit checkpoint fails:** debug specific failing items. Do not let partial stabilization leak into P3 where the variables multiply.

---

## Phase P3 — Signal Validation + Soak (Saturday evening → Sunday AM, budget 6-8 hours including soak wait time)

### Components
- RadioMaster ELRS TX (arriving Saturday)
- `field_logger.py` (existing) — capture serial logs from COM14
- `soak_test.py` (existing) — 2-hour soak, verify script parses LR1121 serial format

### Order of operations
1. **T3.1 Sub-GHz 915 MHz ELRS response test** (1-2 hrs)
   - Flash `v1.6.0-rc1` to LR1121. Power on, wait for warmup complete.
   - In one terminal: `python field_logger.py COM14` to capture serial.
   - In another terminal or on a separate computer: power up RadioMaster at ELRS 915 MHz, place ~1-5 meters from the SENTRY-RF antenna.
   - **Observe and capture:** time-to-ADVISORY, time-to-WARNING, time-to-CRITICAL, peak `score`, peak `persDiv`, peak `sustainedCycles`, peak `conf`. Also capture the OLED screen photos at each escalation.
   - Turn off TX, observe cooldown. Capture time-to-CLEAR.
   - Save log file as `docs/v1.6.0-signal-test-915.log` (or similar).
   - **Compare against SX1262 baseline:** ADVISORY 2.7s / WARNING 6.4s / CRITICAL 11.2s. Document any difference as "LR1121 is X% faster/slower" without judgment — it's a first measurement, not a regression gate.
2. **T3.2 2.4 GHz ELRS response test (FIRST EVER)** (1-2 hrs)
   - Repeat T3.1 but with RadioMaster at ELRS 2.4 GHz.
   - **Pass criterion:** *visible* detection in the serial log. CAD taps increment, or `div` rises, or `persDiv` activates. We are NOT holding 2.4 GHz to sub-GHz timing.
   - If there is no detection at all: capture raw scan output, screenshot the 2.4 GHz spectrum screen (should show TX energy even without CAD), and log it as a known limitation. **Do not block the release on this** — document instead.
   - Save log file as `docs/v1.6.0-signal-test-2g4.log`.
3. **T3.4 LR1121 bench soak** (2 hours wait time)
   - RadioMaster off, no drones present, bench ambient only.
   - `python soak_test.py COM14 120`.
   - Confirm the script parses the LR1121 serial format correctly (Resolved Question 4). If not, patch the script (should be a one-line fix).
   - **Pass criteria (from SPEC):** zero cycles above WARNING, zero crashes, heap stable within ±5%.
4. **T3.3 Multi-environment walk** (1-2 hrs, runs in parallel with T3.4 soak)
   - During the soak, ND walks the board through bench → backyard → rural → suburban with the RadioMaster running. **But** — the soak is supposed to be drone-free, so T3.3 must happen with a DIFFERENT board, OR before/after the soak, not during. **Correction: T3.3 runs BEFORE the soak, not in parallel.** See "Parallelization" below.

### Parallelization
- **Correction to initial instinct:** T3.3 (walk with RadioMaster on) and T3.4 (soak with no signal) CANNOT overlap on the same board. Revised sequence: T3.1 → T3.2 → T3.3 → T3.4. The 2-hour soak is sequential at the end.
- T4.1 walkthrough doc DRAFTING can begin during the T3.4 soak wait — the T3.1/T3.2 logs are already in hand. This is the main parallelization win.

### Risks
- **R8 — RadioMaster doesn't arrive Saturday.** Shipment delay. Mitigation: shift T3.1/T3.2 to Sunday, compress P4. If it doesn't arrive by Sunday noon, the release slips a day — do not ship without real-signal validation.
- **R9 — 2.4 GHz CAD doesn't respond to real ELRS 2.4.** The most likely bug in the whole weekend. Mitigation: per SPEC resolved question 1, this is NOT a release blocker. Document the actual behavior, ship anyway, fix in v1.6.1.
- **R10 — Soak finds a memory leak.** Scariest risk. Mitigation: start the soak early Sunday morning so we have all day Sunday to debug if it fails. If the leak is in a task we already understand (e.g., WiFi promiscuous buffer), fix it. If it's in something new (LR1121 driver), cut it to a known-stable subset for the release and flag it.
- **R11 — Soak script doesn't parse LR1121 logs correctly.** Easy fix — the serial format is identical. One-line patch likely sufficient.
- **R12 — The SENTRY-RF detection engine reacts in a way that DOESN'T match the mental model from the code.** This is actually a DESIRED outcome — it's why we're doing the walkthrough doc. Any "huh, that's unexpected" moments are high-value captures for the walkthrough.

### Exit checkpoint (all must be true)
- [ ] `docs/v1.6.0-signal-test-915.log` exists with full ADVISORY→CRITICAL escalation and cooldown
- [ ] `docs/v1.6.0-signal-test-2g4.log` exists with whatever happened (even if nothing)
- [ ] Sub-GHz metrics captured: time-to-ADVISORY, -WARNING, -CRITICAL, peak score, peak persDiv, cooldown time
- [ ] OLED screen photos during peak CRITICAL captured
- [ ] Multi-environment walk shows detection holds in at least one non-bench environment
- [ ] 2-hour LR1121 soak: zero WARNING+ false positives, zero crashes, heap stable
- [ ] Success Criteria 6, 7, 8 from SPEC.md marked green

**If exit checkpoint fails:**
- T3.1 fail (sub-GHz no detection): release blocker. Debug.
- T3.2 fail (2.4 GHz no detection): document as known limitation, continue.
- T3.4 fail (soak finds issue): debug or cut scope. Release blocker if not resolved.

---

## Phase P4 — Docs + Release (Sunday PM, budget 4 hours)

### Components
- `docs/DETECTION_ENGINE_WALKTHROUGH.md` — new
- `CHANGELOG.md` or `docs/RELEASE_NOTES_v1.6.0.md` — new
- `.mex/ROUTER.md` — update Current Project State
- Git tag, GitHub release

### Order of operations
1. **T4.1 DETECTION_ENGINE_WALKTHROUGH.md** (2-3 hrs) — can start during T3.4 soak.
   - Open with a section: "What are you looking at?" — introduces the `[CAD]` line format field by field (`cycle`, `conf`, `taps`, `div`, `persDiv`, `vel`, `sustainedCycles`, `score`).
   - Next section: "Ambient warmup — what the first 50 seconds do" — show warmup log, explain the `[WARMUP]` message, what gets tagged as ambient and why.
   - Next section: "Detection from CLEAR to CRITICAL" — annotated walkthrough of the T3.1 log. Each CAD line gets a one-line commentary ("score jumps from 5 to 12 because conf went from 0 to 1 — one confirmed CAD tap at half weight"). Show the AAD persistence gate firing, show the diversity gate at `consecutiveHits >= 2`.
   - Next section: "Cooldown" — what happens when the signal disappears.
   - Next section: "2.4 GHz observations" — honest description of what the 2.4 GHz test showed (or didn't).
   - Next section: "When to worry" — what a real crash or leak would look like in serial.
   - Wrap with pointers to source files for curious readers.
2. **T4.2 v1.6.0 release notes** (1 hr)
   - Headline: "First LR1121 stabilization release"
   - Sections: What's fixed (version audit, spectrum rendering confirmation, System screen), What's added (boot self-test, human-readable errors, dual-band CAD on LR1121), What's tested (T3.1 metrics, T3.4 soak result, multi-environment walk), What's known-limited (2.4 GHz timing, SD card), What's next (point at `.mex/ROUTER.md` roadmap).
   - Link to `docs/DETECTION_ENGINE_WALKTHROUGH.md`.
   - Link to the signal test log files.
3. **T4.3 Update `.mex/ROUTER.md`** (15 min)
   - Bump version to `v1.6.0`
   - Move `v1.6.0` items from "Not yet built" to "Working"
   - Update "Known issues" — remove version-string issue, remove "GPS flood" issue if finally confirmed fixed, add any new limitations from T3
4. **T4.4 Tag + push + GitHub release** (30 min)
   - `git tag -a v1.6.0 -m "v1.6.0: LR1121 stabilization + dual-band CAD first light"`
   - `git push origin main`
   - `git push origin v1.6.0`
   - Create GitHub release via web UI with the release notes body (no `gh` CLI in this environment)
   - Attach any relevant log files or screenshots
5. **Session wrap** (15 min)
   - Commit any final doc fixes
   - Mark SPEC.md as "COMPLETE"
   - Update MEMORY.md if any session-worthy lessons came out

### Parallelization
- T4.1 drafting begins during T3.4 soak wait (~2 hours of parallel work).
- T4.2 can begin as soon as T4.1 is substantially done.
- T4.3 is a 15-minute surgical edit — do it last thing before T4.4.

### Risks
- **R13 — T4.1 walkthrough takes longer than expected.** Mitigation: hard timebox at 3 hours. If not done, ship v1.6.0 with a stub walkthrough and a note "expanded walkthrough coming in v1.6.1".
- **R14 — Release notes overstate what was tested.** Mitigation: every claim in the release notes must trace to a specific log file, photo, or commit. If we can't back it up, don't claim it.
- **R15 — Tag/push mistake (wrong branch, wrong message).** Mitigation: dry-run the tag first; review the release body before clicking publish on GitHub.

### Exit checkpoint (all must be true)
- [ ] `docs/DETECTION_ENGINE_WALKTHROUGH.md` exists and is reviewable by a stranger
- [ ] v1.6.0 release notes published on GitHub
- [ ] All 10 SPEC success criteria reviewed and marked green in a final checklist
- [ ] `.mex/ROUTER.md` reflects v1.6.0 state
- [ ] Tag `v1.6.0` on origin
- [ ] GitHub release live with notes, walkthrough link, and artifacts

**If exit checkpoint fails:** don't publish the release until SC 1-8 are all green. SC 9 (walkthrough) and SC 10 (release notes + tag) can be done in a follow-up commit if we're out of time.

---

## Global risks (apply across all phases)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **RadioMaster delivery delay** | Medium | High (blocks P3) | Start P1/P2 Saturday regardless. If RM arrives Sunday, compress P3 and P4 into one sprint. Slip to Monday only as absolute last resort. |
| **LR1121 silicon bug we haven't seen yet** | Low | Very high | Soak time in P3 is the primary detector. If found, triage: is it known (documented limitation) or new (release blocker)? |
| **Context-switching fatigue** | Medium | Medium | Finish each phase's exit checkpoint cleanly. Do not leave P2 with "everything builds but I haven't tested the antenna check" hanging into P3. |
| **Scope creep — "while I'm in there"** | High | High | Every "this would be a nice improvement" idea gets written into a v1.7.0 bucket in `docs/`. Do not implement in v1.6.0. |
| **The weekend running out** | Guaranteed | Low if above are managed | Time budgets per phase are hard caps. Cutting scope at phase boundaries is normal and expected. |

## Verification checkpoints summary

| Checkpoint | When | Criteria |
|---|---|---|
| **CP1** | End of P1 (Sat noon) | SC 1-4 green, all screens render, version correct |
| **CP2** | End of P2 (Sat ~5pm) | SC 5 green, antenna check works on 3 boards, `v1.6.0-rc1` tagged |
| **CP3** | End of P3 (Sun noon) | SC 6-8 green, signal test logs captured, soak complete |
| **CP4** | End of P4 (Sun evening) | SC 9-10 green, release live |

## Parallelization opportunities

1. **T1.5 hardware wiring** (GPS/buzzer) happens Friday night before any coding — removes from Saturday budget entirely.
2. **T4.1 walkthrough drafting** during T3.4 soak wait — saves ~2 hours.
3. **SX1262 and Heltec V3 flash-and-test** during T2.1 can be done sequentially but the COMPILATION is shared (one `pio run` builds all three). Avoid rebuilding between board flashes.
4. **RadioMaster unboxing + bind + config** can happen Saturday morning as a background task while T1/T2 code work proceeds.

## What's NOT in this plan (explicit)

- No library upgrades
- No new features from ROUTER's "Not yet built" list
- No refactoring "while we're here"
- No external-tester quickstart doc (ND is the weekend tester)
- No SD card work
- No GPIO 10/21 investigation (still waiting on LilyGo)
- No attempt at 2.4 GHz protocol classification even if a clean detection is observed — that's v1.7.0

---

## Review questions for ND

1. **Time budget realism:** the total budget is ~16 hours of focused work across Saturday + Sunday. Does that match your actual weekend availability? If not, which phase should we cut first?
2. **P3 correction:** I initially wrote T3.3 and T3.4 as parallel, then corrected to sequential because the same board can't be in "walk with signal" and "bench soak without signal" at the same time. Confirm that's right — unless you're using two boards (e.g., LR1121 soak on bench + SX1262 walk in field), in which case parallel is fine again.
4. **Tag strategy:** I'm proposing `v1.6.0-rc1` at the P2 exit as a safety checkpoint so we can diff signal test behavior against a known-good baseline. Worth doing, or overkill?
5. **Where should signal test logs live?** Proposing `docs/` so they're versioned with the release. Alternative: `logs/` (gitignored, kept local). If the logs contain GPS coordinates from real-world testing, we probably don't want them on GitHub.
6. **Release timing:** does "Sunday evening" mean Sunday before sleep, or Sunday ~6pm so we have buffer? I assumed the latter. If you want to ship by a specific hour, tell me.

---

**Next step:** review this plan, answer the 5 questions above, approve or request changes. Once approved, I'll advance to Phase 3 (TASKS) and break each work item into single-session tasks with explicit acceptance criteria and verification steps. Or — if you want to just start executing P1 directly from this plan without the TASKS phase, that's a valid skill deviation and I'll flag it explicitly.
