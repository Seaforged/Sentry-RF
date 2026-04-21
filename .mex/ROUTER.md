---
name: router
description: Session bootstrap and navigation hub. Read at the start of every session before any task. Contains project state, routing table, and behavioural contract.
edges:
  - target: context/architecture.md
    condition: when working on system design, integrations, or understanding how components connect
  - target: context/stack.md
    condition: when working with specific technologies, libraries, or making tech decisions
  - target: context/conventions.md
    condition: when writing new code, reviewing code, or unsure about project patterns
  - target: context/decisions.md
    condition: when making architectural choices or understanding why something is built a certain way
  - target: context/setup.md
    condition: when setting up the dev environment or running the project for the first time
  - target: patterns/INDEX.md
    condition: when starting a task — check the pattern index for a matching pattern file
last_updated: 2026-04-10
---

# Session Bootstrap

If you haven't already read `AGENTS.md`, read it now — it contains the project identity, non-negotiables, and commands.

Then read this file fully before doing anything else in this session.

## Current Project State

**Version:** v1.9.0 — Phase H shipped: operational modes (STANDARD/COVERT/HIGH_ALERT) verified on LR1121 hardware 2026-04-21.

**Working:**
- Sub-GHz CAD detection on all three boards (t3s3, heltec_v3, t3s3_lr1121) with SF6-SF12 sweep across 860-930 MHz
- LR1121 dual-band CAD — adds 2.4 GHz scanning (2400-2480 MHz, SF6-SF8, BW800)
- AAD persistence gate at 5 cycles + consecutiveHits ≥ 2 diversity recording gate
- 30-minute bench soak: 0.00% false positive rate on SX1262
- Suburban field test: 842m WARNING, 1022m ADVISORY at 10 mW ELRS
- ELRS detection: ADVISORY 2.7s, WARNING 6.4s, CRITICAL 11.2s
- WiFi Remote ID scanner (ASTM F3411), GNSS integrity (u-blox M10 UBX), LED + buzzer alerts
- 6-screen OLED UI (7 on LR1121 with 2.4 GHz spectrum), rotated by BOOT button
- GPS serial output rate-limited to 5s to prevent serial buffer overflow
- Operational modes (STANDARD / COVERT / HIGH_ALERT) — double-tap BOOT toggles HIGH_ALERT, triple-tap toggles COVERT. COVERT fully deinits WiFi (esp_wifi_stop→deinit), blanks OLED, suppresses buzzer+LED. HIGH_ALERT extends RSSI sweep gate from 8s to 10s so CAD gets scan budget. Multi-press FSM in displayTask with 800ms quiet-window disambiguation.

**Not yet built:**
- BLE Remote ID scanning (ESP32-S3 BLE advertising channel sweep)
- 2.4 GHz protocol classification (ELRS_2G4 vs GHOST_2G4 vs TRACER_2G4)
- Per-band diversity tracking (currently pooled across sub-GHz + 2.4 GHz)
- Dual-band correlation engine (paired signal detection from D-TECT-R research)
- Boot self-test with antenna quality check

**Known issues:**
- GPIO 10/21 status unknown on LR1121 V1.2 — awaiting LilyGo response
- SD card init fails on T3S3 hardware — extensive testing couldn't get it to register; field logs go to serial only
- `radio.getRSSI(false)` overload not available in RadioLib v7.6 for LR11x0 — use the `LR1121_RSSI` subclass (defined in `rf_scanner.h`) which exposes protected `getRssiInst()`
- LR1121 `setFrequency()` drops radio to standby — must call `startReceive()` before every RSSI read in the sweep loop
- Version string may still show `v1.2.0-FIX4` somewhere in `main.cpp` — needs audit in next session

## Routing Table

Load the relevant file based on the current task. Always load `context/architecture.md` first if not already in context this session.

| Task type | Load |
|-----------|------|
| Understanding how the system works | `context/architecture.md` |
| Working with a specific technology | `context/stack.md` |
| Writing or reviewing code | `context/conventions.md` |
| Making a design decision | `context/decisions.md` |
| Setting up or running the project | `context/setup.md` |
| Any specific task | Check `patterns/INDEX.md` for a matching pattern |

## Behavioural Contract

For every task, follow this loop:

1. **CONTEXT** — Load the relevant context file(s) from the routing table above. Check `patterns/INDEX.md` for a matching pattern. If one exists, follow it. Narrate what you load: "Loading architecture context..."
2. **BUILD** — Do the work. If a pattern exists, follow its Steps. If you are about to deviate from an established pattern, say so before writing any code — state the deviation and why.
3. **VERIFY** — Load `context/conventions.md` and run the Verify Checklist item by item. State each item and whether the output passes. Do not summarise — enumerate explicitly.
4. **DEBUG** — If verification fails or something breaks, check `patterns/INDEX.md` for a debug pattern. Follow it. Fix the issue and re-run VERIFY.
5. **GROW** — After completing the task:
   - If no pattern exists for this task type, create one in `patterns/` using the format in `patterns/README.md`. Add it to `patterns/INDEX.md`. Flag it: "Created `patterns/<name>.md` from this session."
   - If a pattern exists but you deviated from it or discovered a new gotcha, update it with what you learned.
   - If any `context/` file is now out of date because of this work, update it surgically — do not rewrite entire files.
   - Update the "Current Project State" section above if the work was significant.
