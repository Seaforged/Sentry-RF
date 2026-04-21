# SENTRY-RF Known Issues & Unfinished Work Tracker
## As of April 21, 2026 — v2.0.0

This document tracks every identified issue, limitation, and unfinished item.
It is the single source of truth for what the firmware can and cannot do
today. Review this document before every sprint and before every release tag.

v2.0.0 closed many of the v1.x items below. Sections now distinguish:
- **RESOLVED** — fixed in a shipped release
- **OPEN** — still broken or incomplete; target version listed
- **KNOWN LIMITATIONS** — not bugs, deliberate design choices; listed so
  operators and reviewers can plan around them

---

## RESOLVED in v2.0.0 (Phases H–N + pre-release triage)

### [x] No boot self-test — RESOLVED in Phase K (v2.0.0)
**Fix:** `runSelfTest()` in `main.cpp` — 10 RSSI reads @ 915 MHz (radio-
health sentinel check), 10-frequency probe across 860–930 MHz
(antenna-coverage floor check), async GPS-fix timer with 120 s deadline,
OLED summary screen for 3 s, JSONL `selftest` event persisted to SD. Scan-
cycle watchdog logs `[WATCHDOG]` if any single cycle exceeds
`SCAN_WATCHDOG_MS` (5000 ms). Log-only — no task reset.

### [x] No operational modes (STANDARD / COVERT / HIGH_ALERT) — RESOLVED in Phase H (v2.0.0)
**Fix:** Multi-press BOOT button FSM in `displayTask`. Double-tap toggles
HIGH_ALERT (extends RSSI sweep gate from 8 s → 10 s so CAD gets scan
budget). Triple-tap toggles COVERT (fully deinits WiFi via
`esp_wifi_stop → esp_wifi_deinit`, stops BLE scan via `NimBLEScan::stop()`,
blanks OLED, suppresses buzzer + LED). Mode persists via `modeGet()/modeSet()`
guarded by `stateMutex`; changes are logged via `[MODE]` serial line and a
`mode_change` JSONL event.

### [x] GPS_MIN_CNO set to 6 (indoor value) — RESOLVED pre-v2.0.0
**Current value:** `GPS_MIN_CNO = 15` in `sentry_config.h`. Comment reads
`// dB-Hz minimum satellite signal strength. INDOOR TESTING: 6. FIELD/PRODUCTION: 15-20.`
The C/N0 uniformity-based spoofing detector is automatically suppressed
when `GPS_MIN_CNO < 15` to avoid indoor false positives.

### [x] No GNSS integrity improvements — RESOLVED pre-v2.0.0
**Fix:** `src/gnss_integrity.cpp` implements:
- C/N0 uniformity check (stddev across satellites) with configurable floor
- Satellite elevation filter via `MIN_ELEV_FOR_CNO = 20°` — low-elevation
  multipath-affected sats are excluded from the uniformity calc
- Position-jump detection (NAV-PVT consecutive comparison, >100 m step with
  tight hAcc flagged as spoofing indicator)
- RF-GNSS temporal correlation — a GNSS anomaly only contributes to
  candidate confirm score when an RF-side ADVISORY was active within
  `GNSS_RF_CORRELATION_WINDOW_MS` (30 s)

### [x] LR1121 CAD stub — RESOLVED pre-v2.0.0
**Fix:** Full LR1121 CAD pipeline in `cad_scanner.cpp` covers sub-GHz
(860–930 MHz, SF6–SF12, ELRS channel grid) and 2.4 GHz (2400–2480 MHz,
SF6–SF8, BW=812.5 kHz). Per-band `CadBandSummary` feeds the candidate
engine. 2.4 GHz is confirmer-only (never seeds a candidate by itself).

### [x] No ASTM F3411 full payload decode — RESOLVED in Phase J (v2.0.0)
**Fix:** Vendored `opendroneid-core-c` (Apache 2.0) at
`lib/opendroneid/` — 4 files, upstream commit `4b266c7`. WiFi beacons
with vendor-specific IE (OUI FA:0B:BC, type 0x0D) are decoded via
`odid_message_process_pack()` after the 5-byte skip (OUI + type + counter).
Populates `DecodedRID` struct on `SystemState.lastRID`, emits
`[RID] UAS-ID: ... Drone: lat,lon,alt ... Operator: ... Speed: ... Hdg: ...`,
renders on the new RID OLED screen, appends `rid_*` fields to JSONL.

### [x] No BLE Remote ID — RESOLVED in Phase M (v2.0.0)
**Fix:** NimBLE-Arduino 2.x scanner in `ble_scanner.cpp`. Passive scan at
10% duty (50 ms window / 500 ms interval) coexists with WiFi promiscuous
mode via ESP-IDF coexistence scheduler. ASTM F3411 BLE service UUID 0xFFFA
matched; payload decoded with the same `odid_message_process_pack()` path
as WiFi (after the 1-byte counter skip). Writes to the same `lastRID`
struct so the OLED RID screen shows BLE-sourced decodes without display
changes. Stops cleanly on COVERT entry.

### [x] ZMQ / DragonSync output — RESOLVED in Phase L (v2.0.0)
**Fix:** `emitZmqJson()` in `data_logger.cpp` prints `[ZMQ] {...}\n` lines
to Serial on every threat transition and every decoded RID. Debounced to
≤1 Hz per event type. Companion Python bridge at `tools/zmq_bridge.py`
strips the prefix and republishes as real ZMQ PUB messages on
`tcp://*:4227` (DragonSync convention).

### [x] Bandwidth discrimination (DJI OcuSync target) — RESOLVED in Phase I (v2.0.0)
**Fix:** `countElevatedAdjacentBins()` in `rf_scanner.cpp`. Per-peak
classification into `BW_NARROW` / `BW_MEDIUM` / `BW_WIDE` based on 200 kHz
bin run lengths. `bwWide` evidence slot on `DetectionCandidate` attaches
as a confirmer. **Caveat**: the existing `narrowWidth > 6` filter in
`extractPeaks()` still rejects flat-top OFDM before classification, so
full OcuSync detection requires the follow-up work tracked in OPEN below.

### [x] Enhanced data logging for field analysis — RESOLVED in Phase L+N (v2.0.0)
**Fix:** JSONL rows include threat, score, CAD diversity/confirmed/taps,
peak freq/RSSI/bandwidth class, GPS position/fix/SVs, jam/spoof
indicators, C/N0 stddev, and (when present) decoded RID fields. Plus
one-shot `selftest` and `mode_change` events. Logger access is serialized
via a dedicated `loggerMutex` so concurrent writes from `loRaScanTask`
and `displayTask` don't interleave.

### [x] No field test analysis tooling — RESOLVED in Phase N (v2.0.0)
**Fix:** `tools/field_analyzer.py` consumes JSONL logs (single file or
directory of files), produces console summary + self-contained HTML
report (base64 PNGs) + CSV detection-event table. Handles malformed lines
gracefully. Requirements pinned in `tools/requirements_analysis.txt`.

---

## OPEN — carried forward from v1.x

### [ ] FSK_DETECT_THRESHOLD_DBM bench vs field split
**Impact:** -50 dBm is bench-safe but conservative for field. -70 dBm is
field-appropriate but can trip earlier on a noisy bench from ambient ISM
energy.
**Current value:** -70 dBm (field-oriented).
**Plan:** Change to -70 dBm before field deployment, or add runtime
BENCH/FIELD mode switch. **Target:** v2.1.

### [ ] ELRS detection time varies significantly (2–48 s)
**Impact:** Operator can't predict alert latency.
**Current timing (v2.0.0 bench):** ADVISORY 2.7 s, WARNING 6.4 s,
CRITICAL 11.2 s.
**Status:** Field testing required to validate timing with real drone
signals at various ranges.

### [ ] RSSI sweep noise floor varies between LoRa and FSK modes
**Impact:** RSSI thresholds tuned in FSK mode may not apply after mode
switches.
**Status:** RSSI sweep always runs in FSK — no active issue today, but
documented because a future mode-switch path could reintroduce it.

### [ ] Ambient confirmed-tap WARNING breach (v1.6.0 finding, carried forward)
**Impact:** No-drone baseline can spike to `score=44` on a single cycle-
burst of ambient activity even with `PERSISTENCE_MIN_CONSECUTIVE=5`.
Separate failure pathway from the diversity-sustained escalation closed in
v1.5.0/v1.5.3.
**Evidence:** April 11, 2026 Codex review flagged cycle 156 of a baseline
log where `persDiv` jumped 0→3 while `sustainedCycles` stayed at 2.
**Root cause hypothesis:** `persDiv` has an update path (possibly via
confirmed-tap or velocity calculation) that bypasses the `sustainedCycles`
gate.
**Regression criterion:** 30+ minute soak with no drone present must show
zero cycles at or above WARNING (score < 24).
**Status:** Tracked for v2.1. Audit `persDiv` update paths in
`detection_engine.cpp`, find the bypass path, add a `conf`-contribution
gate that also requires `sustainedCycles >= N`.

### [~] Alert state ownership race — MITIGATED (Issue 8 pre-release fix)
**Impact:** `displayTask` calls `alertAcknowledge()` and `alertToggleMute()`
while `alertTask` mutates the same `_lastThreat` / `_isAcknowledged` /
`_isMuted` globals. Worst case was a mute/ack taking an extra cycle to
register.
**Mitigation:** Added `alertMutex` covering all three public entry points
AND the alertTask queue drain. Deepened `detectionQueue` 10 → 32 slots.
Changed producer sends from `xQueueSend(..., 0)` to `xQueueSend(..., 5ms)`
with an `alertQueueDropInc()` counter that logs once per 10 s on non-zero
drops. Source-single-writer refactor deferred to **v2.1** as a clean
architecture move — current fix eliminates the torn-state risk.
**Resolved for v2.0.0 tag (mitigated):** April 21, 2026.

### [!] Warmup poisoning — OPEN, CRITICAL (fixed pre-tag in probation-table rewrite)
**Impact (pre-fix):** A drone transmitting on any RF path during the
20-50 s ambient-learning window was permanently tagged as infrastructure
and remained invisible for the rest of the boot session.
**Fix:** Issue 1 probation table. Taps observed during warmup enter a
`pendingAmbient[]` holding area; at warmup completion only un-corroborated
entries graduate to true ambient. Corroboration markers: FHSS diversity on
a matching freq (marks that freq only), successful WiFi RID decode
(blanket-marks all pending as drone-correlated), rising GNSS integrity
threat (blanket-marks all pending). Also deleted the
`firstSeenMs < AMBIENT_WARMUP_MS` retag path that was re-learning
discarded taps after warmup ended. Boot summary: `[WARMUP] Complete.
Graduated N/M pending taps as ambient (K discarded — corroboration
detected).`
**Resolved:** April 21, 2026 — v2.0.0 pre-release.

### [!] NaN action-frame Remote ID decode — CLOSED (Issue 5, Option A)
**Impact (pre-fix):** `findRemoteIdIE()` walked 0xD0 action frames as if
they were beacon IE lists; NAN Service Discovery Frames carrying ASTM
F3411 RID were not decoded.
**Fix:** Added 24-byte MAC-header field to `CapturedFrame`, populated in
the promiscuous ISR. Added `decodeNanActionRID()` which reconstructs the
full 802.11 management frame and passes it to
`odid_wifi_receive_message_pack_nan_action_frame()`. Dispatch by frame
subtype: 0x80 → beacon IE walker, 0xD0 → NAN decoder. NAN decodes emit
`[NAN-RID]` serial line; otherwise identical to beacon RID.
**Resolved:** April 21, 2026 — v2.0.0 pre-release.

### [~] 2.4 GHz OFDM plateau detector — MITIGATED (Issue 6)
**Impact (pre-fix):** The WiFi-channel reject in `extractPeaks24()`
dropped DJI OcuSync / Autel C2 flat-top OFDM signals that share WiFi
channels. Those drones were invisible to the RF detection path on LR1121.
**Mitigation:** Added `findLongestElevatedRun()` primitive that scans the
full 100-bin 2.4 GHz sweep for contiguous elevated runs, independent of
the peak-finder (bypasses `isWiFiChannel()`). Attached `bwWide` evidence
to the best active sub-GHz candidate when a run of ≥ `OFDM_PLATEAU_MIN_BINS`
persists for `OFDM_PLATEAU_PERSIST_CYCLES` sweeps. Same "confirmer, never
seeds" policy as proto24/cad24.
**Remaining limitation:** Still requires a correlated sub-GHz candidate
to fire — a pure 2.4 GHz-only video emitter won't escalate on its own.
Full standalone 2.4 GHz candidate tracking is v2.1+ work.
**Mitigated:** April 21, 2026 — v2.0.0 pre-release.

---

## KNOWN LIMITATIONS (by design — not bugs)

### GNSS alerts fire standalone; correlation still gates score boost
Rising GNSS threat levels (jam / spoof / position-jump / C/N0 uniformity)
emit a standalone DetectionEvent that sounds the buzzer directly (v2.0
Issue 7 fix). The candidate engine's confirm-score GNSS contribution
still requires a correlated RF candidate active within the last 30 s —
this is deliberate, to prevent urban driving from boosting drone-score
in the candidate engine. But jam/spoof DO alert on their own path via
the new `DET_SOURCE_GNSS` queue events.

### OFDM plateau detector is confirmer-only
Issue 6 added a contiguous-elevated-bins scanner that bypasses the WiFi-
channel reject so OcuSync / Autel C2 flat-top OFDM can contribute
`bwWide` evidence. But it still only attaches to an already-existing
sub-GHz candidate — a pure 2.4 GHz-only video emitter won't escalate
unless the drone's sub-GHz control link has already been seen. Standalone
2.4 GHz candidate tracking is v2.1+ work.

### SD card init on T3S3 hardware
Known hardware issue — SD init fails on the specific T3S3 boards in the
bench. Documented in `.mex/ROUTER.md`. On failure, all logger calls become
no-ops; JSONL events are still visible on serial. Not a firmware bug.

### Compass not reliably detected on all hardware
QMC5883L on Wire1 occasionally fails the chip-ID check (`ID=0x00`
observed). Boot continues without compass; bearing display shows `--`.
Not on critical path for RF detection.

---

## LOW — deferred future work

### [ ] Power management / battery life optimization — v2.x
### [ ] OTA firmware updates — v2.x
### [ ] Multi-device mesh (ESP-NOW) — v2.x
### [ ] Multi-constellation GNSS consistency — v2.x

---

## Testing Methodology Issues

### [ ] Bench test scripts — stale state on DTR/RTS reset
**Impact:** T3S3 native USB DTR/RTS reset is unreliable. Serial port open
doesn't always trigger a reboot.
**Workaround:** Test scripts must verify boot banner (`SENTRY-RF v2.0.0`)
before starting measurements. Alternatively, physical RST button or
power cycle.

### [ ] No automated regression test for detection timing
**Impact:** Each release potentially regresses WARNING/CRITICAL timing.
**Plan:** v2.1 — Python harness that boots SENTRY, waits for warmup,
starts a signal source, measures time-to-WARNING and time-to-CRITICAL,
fails if beyond thresholds.

---

## Architecture decisions documented

### Frequency diversity is the correct FHSS discriminator
- Hit count doesn't separate drone from ambient (both produce similar counts)
- Hit spread (distinct frequencies) does separate them — drones hit many
  frequencies, infrastructure hits few
- 3-second window prevents ambient accumulation while capturing FHSS
  pattern

### CAD detects full LoRa packet, not just preamble
- Confirmed by Semtech AN1200.48 and AN1200.85
- Detection window is ~5 ms (full packet airtime), not ~1 ms (preamble)

### Persistence (consecutive hits) is the infrastructure discriminator
- Drones transmit continuously via FHSS — short-lived per-frequency
- Infrastructure transmits intermittently on fixed frequencies — long-lived
- 3 consecutive hits = confirmed tap
- 10+ s fixed-frequency tap = auto-learn as ambient

### Never call radio.begin() or beginFSK() mid-operation
- Causes -707 CHIP_NOT_FOUND (full chip reset + SPI re-probe fails)
- Use SPI opcode 0x8A for packet type switching
- Phase H COVERT mode explicitly avoids radio.begin() calls during mode
  transitions for this reason

### 2.4 GHz is confirmer-only (LR1121)
- The candidate engine never seeds a new candidate from 2.4 GHz evidence
  alone (see Phase E commit). Always requires a paired sub-GHz anchor.
- Rationale: WiFi/Bluetooth/microwave oven energy floods the 2.4 GHz band;
  using it as a primary detector would false-positive constantly.

### Serial output is globally serialized (pre-v2.0.0 triage)
- The `SERIAL_SAFE(body)` macro in `detection_types.h` blocks on
  `serialMutex` with `portMAX_DELAY` for every write. Required for the
  Phase L `[ZMQ]` JSON frame format — even a one-byte interleave from a
  concurrent print corrupts the pipe to the DragonSync bridge.

---

*Last updated: April 21, 2026 — v2.0.0 tag candidate after pre-release
fix sweep (serial mutex blocking, WiFi queue lifecycle, documentation
reconciliation). Review this document before every sprint and release.*
