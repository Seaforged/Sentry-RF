---
name: decisions
description: Key architectural and technical decisions with reasoning. Load when making design choices or understanding why something is built a certain way.
triggers:
  - "why do we"
  - "why is it"
  - "decision"
  - "alternative"
  - "we chose"
edges:
  - target: context/architecture.md
    condition: when a decision relates to system structure
  - target: context/stack.md
    condition: when a decision relates to technology choice
last_updated: 2026-04-10
---

# Decisions

## Decision Log

### Frequency diversity (not hit count) is the FHSS discriminator
**Date:** 2026-03 (Sprint 2B)
**Status:** Active
**Decision:** Drone FHSS is discriminated from ambient LoRa by counting DISTINCT frequencies hit in a 3-second window, not total CAD hit count.
**Reasoning:** Hit count doesn't separate drone from ambient — both produce similar counts. Hit SPREAD does: drones hit many frequencies, infrastructure hits few.
**Alternatives considered:** Hit count threshold (rejected — LoRaWAN gateways produce similar hit counts as drones). RSSI-only detection (rejected — doesn't distinguish LoRa from wideband ISM noise).
**Consequences:** 3-second sliding window prevents ambient accumulation. Requires tracking distinct frequencies via `MAX_DIVERSITY_SLOTS=32` ring buffer. Bench ambient diversity of 4-7 overlaps with drone diversity of 2-5 — this is why the persistence gate exists.

### AAD persistence gate raised from 3 to 5 cycles
**Date:** 2026-04-06
**Status:** Active
**Decision:** `PERSISTENCE_MIN_CONSECUTIVE = 5` — diversity only counts toward scoring when raw diversity stays ≥3 for 5 consecutive scan cycles (~12.5s).
**Reasoning:** The 28-minute dual-device soak test at v1.5.2 showed T3S3 hitting score=80 (CRITICAL) on 2 cycles. Ambient LoRa sustained div≥3 for 4 consecutive cycles, barely passing the old 3-cycle gate. Once persDiv jumped to 5, scoring cascaded to CRITICAL. Max observed ambient `sustainedCycles` was 4.
**Alternatives considered:** Halving scoring weights (rejected — reduces real drone detection). Raising diversity threshold from 3 to 5 (rejected — real drones may not always produce 5 distinct frequencies in 3s on edge reception).
**Consequences:** Adds ~5s to worst-case detection time on the diversity path. Real drone detection through confirmed CAD taps (half-weight pathway) is unaffected — field test showed CRITICAL in 11.2s via the tap pathway, not diversity.

### `consecutiveHits >= 2` gate on diversity recording
**Date:** 2026-04-10
**Status:** Active
**Decision:** A CAD tap only contributes to the FrequencyDiversityTracker if its `consecutiveHits >= 2`. Single "nibbles" don't count — only confirmed "bites" do.
**Reasoning:** On the LR1121 dual-band bench, single ambient hits across 32 frequencies were inflating diversity to 32 post-warmup, even though confirmed taps counted 0. The persistence gate held, but the raw diversity metric was misleading and could have passed if enough single-hits lined up.
**Alternatives considered:** Rely solely on the persistence gate (rejected — masks a real problem in the diversity tracker). Filter per-frequency hit counts (rejected — already done via CadTap struct, just needs to be read before recording).
**Consequences:** Post-warmup bench baseline is now `div=0` on LR1121 (was 32). Real drone FHSS hopping still registers because the second hit on any frequency qualifies that frequency for diversity. Adds one scan cycle of latency to diversity-driven detection, but the dominant detection path is confirmed taps.

### LR1121 requires the `LR1121_RSSI` subclass for instantaneous RSSI
**Date:** 2026-04-10
**Status:** Active
**Decision:** Use a C++ subclass `LR1121_RSSI : public LR1121` that exposes the protected `getRssiInst()` method via a public `getInstantRSSI()` wrapper.
**Reasoning:** RadioLib v7.6's `LR11x0::getRSSI()` returns the last PACKET RSSI, which is 0 if no packet has been received. Later RadioLib versions added a `getRSSI(bool packet, bool skipReceive)` overload but v7.6 doesn't have it. The protected `getRssiInst()` method sends the correct `CMD_GET_RSSI_INST` SPI command for real-time RSSI during sweeps.
**Alternatives considered:** Upgrade RadioLib (deferred — would require retesting all radio-dependent code on all three boards). Manually send the SPI command (rejected — brittle, bypasses RadioLib's SPI state machine).
**Consequences:** `rf_scanner.h` defines `LR1121_RSSI`. `main.cpp` declares `radio` as this subclass when `BOARD_T3S3_LR1121` is defined. When RadioLib is upgraded, this wrapper can be removed and `getRSSI(false)` called directly.

### Per-bin `startReceive()` on LR1121 spectrum sweeps
**Date:** 2026-04-10
**Status:** Active
**Decision:** The LR1121 spectrum sweep calls `radio.startReceive()` before every RSSI read in the loop.
**Reasoning:** Observed behaviour: on LR1121, `setFrequency()` puts the radio back to standby. RSSI reads after the first bin return -127.5 dBm (noise floor register) because the radio isn't in receive mode. The SX1262 version had the same pattern for the same reason.
**Alternatives considered:** Call `startReceive()` once before the loop (rejected — doesn't work on LR1121). Use continuous RX mode with frequency steps (deferred — RadioLib API doesn't expose this cleanly).
**Consequences:** Sweep time increases from ~720ms to ~3970ms on LR1121 (vs SX1262's 2400ms). Still within the 6-second cycle budget. Investigating batch RSSI read in Phase F1.

### `LR1121::beginGFSK` uses the 7-argument signature, frequency first
**Date:** 2026-04-10
**Status:** Active
**Decision:** Call `radio.beginGFSK(915.0, 4.8, 50.0, 156.2, 10, 16, 3.0)` — the LR1121/LR1120 derived class overload, not the 5-arg base class overload.
**Reasoning:** The base `LR11x0::beginGFSK(br, freqDev, rxBw, preambleLength, tcxoVoltage)` takes no frequency. Without calling `setFrequency()` first, the internal `freqMHz` is 0 and `workaroundGFSK()` fails with -12 INVALID_FREQUENCY. The derived `LR1120::beginGFSK(freq, br, freqDev, rxBw, power, preambleLength, tcxoVoltage)` takes frequency as the first argument and sets it inside the method. Max frequency deviation is 200 kHz (SX1262 allows 234.3 — this broke a copy-paste port).
**Alternatives considered:** Call `setFrequency()` before the 5-arg `beginGFSK` (rejected — RadioLib internal state ordering issue). Use LoRa mode only and skip GFSK sweeps (rejected — CAD needs LoRa but RSSI sweeps need GFSK).
**Consequences:** `beginGFSK` signature differs between SX1262 and LR1121 code paths. Documented in `rf_scanner.cpp` and enforced by the `BOARD_T3S3_LR1121` guard.

### GPS serial output rate-limited to 5 seconds
**Date:** 2026-04-06
**Status:** Active
**Decision:** `gpsReadTask` prints `[GPS]` and `[INTEGRITY]` lines once every 5 seconds, not every 10ms task iteration.
**Reasoning:** The 1-hour dual-device soak test dropped serial capture at 28 minutes because the Python reader couldn't keep up with ~100 Hz GPS prints (~161K GPS lines in 28 minutes). The UART drain itself still runs every 10ms for buffer health; only the printing is throttled.
**Alternatives considered:** Raise baud rate (rejected — doesn't fix the reader bottleneck). Buffer on device and dump on demand (rejected — too much complexity for a diagnostic).
**Consequences:** Log files are 13x smaller (2.16 MB vs 28 MB over 30 minutes). Soak test reliability restored. Operator-visible GPS status on OLED still updates in real time because that reads from `systemState` directly.
