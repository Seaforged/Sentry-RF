Triage — What Matters Before v2.0.0
First, calibrate the Codex findings. Codex is doing its job as an adversarial reviewer — it's finding real issues, some real gaps, and also some overcalls. Let me go through the conflicts and verdicts:
Genuine CRITICAL/HIGH that we must fix before tagging:
C2 / Codex confirmed — Serial races with ZMQ path (CC: HIGH)
Both reviewers agree. Unprotected Serial.printf in cad_scanner.cpp, detection_engine.cpp, rf_scanner.cpp can corrupt the [ZMQ] JSON pipe mid-line. This is real. Fix before tagging.
D1 — Dead wifi_dashboard.cpp with SoftAP (CC: HIGH)
dashboardInit() is never called, but it exists with a WiFi.softAP() call. If anyone ever calls it without hooking COVERT mode, it silently breaks COVERT. Delete the file. It's 112 lines of unreachable code. Fix before tagging.
M2/M3 — BleFrame 64B truncates BLE 5, CapturedFrame 256B thin margin (CC: MEDIUM)
Both buffer sizes are too small. Codex escalates M2 to HIGH correctly — a real BLE 5 extended advertisement drone gets silently missed. Fix before tagging: bump BleFrame to 256, CapturedFrame to 320.
C1 — MIN_ELEV_FOR_CNO defined but never applied (CC: HIGH)
Real. The C/N0 uniformity check doesn't filter by elevation, making it unreliable. Four lines to fix.
Codex-specific findings — calibrated:
Codex CRITICAL 1 — Warmup "poisonable" by drone transmitting during boot
This is a real architectural concern but not a v2.0 blocker. The warmup window is 20-50 seconds. In a real operational scenario a drone appearing at boot is an edge case, and the fix (probation table) is a substantial redesign. Flag for v2.1, not a v2.0 gate.
Codex CRITICAL 2 — odid_message_process_pack() OOB read on short packet
This is real and worth fixing — add a minimum-length guard before calling the library. It's 3-4 lines of validation. Fix before tagging.
Codex HIGH — NAN/action frame parsing mismatch
**Correction (April 21, 2026, after re-review):** Prior text here was wrong
on two counts. (1) The WiFi promiscuous capture *does* pass 0xD0 action
frames through — the filter is `WIFI_PROMIS_FILTER_MASK_MGMT` which admits
every 802.11 management subtype. (2) `findRemoteIdIE()` uses different
offsets for beacon (12 bytes — timestamp/interval/cap) vs action (1 byte —
category), so it walks both, but it parses action frames *as if* they were
vendor-IE-carrying beacons. NAN Service Discovery Frames use a NAN header
after the category byte, not a vendor IE at that position, so NAN-based
Remote ID broadcasts will not correctly decode. This is an OPEN gap, not
correct behavior — tracked in `SENTRY-RF_Known_Issues_Tracker.md` under
KNOWN LIMITATIONS. In practice all consumer drones in the FAA RID
compliance program broadcast on beacons; NAN-only emitters are rare. A
proper decoder using `odid_wifi_receive_message_pack_nan_action_frame()`
needs preserving the 24-byte MAC header in `CapturedFrame` (currently
discarded), so it's a structural change planned for v2.1.
Codex HIGH — Alert state races
**Correction (April 21, 2026, after re-review):** Prior text here
understated the race. `_lastThreat`, `_isAcknowledged`, and `_isMuted` are
*not* single-writer. `displayTask` calls `alertAcknowledge()` (writes
`_isAcknowledged` + `_acknowledgedAt`) and `alertToggleMute()` (writes
`_isMuted` + `_muteStartMs`) from Core 0. `alertTask` mutates the same
globals concurrently (also Core 0 — same core, but preemptively
scheduled). Worst case is a mute/ack taking an extra cycle to register or
a stale read during the write window. Not a field-safety failure — the
state machine self-corrects on the next alertTask iteration — but this is
an OPEN finding, not resolved. Tracked in the Known Issues Tracker for
v2.1 (add an `alertMutex` or route ack/mute through `detectionQueue`).
Codex HIGH — Logger file handles written from multiple tasks
CC didn't catch this. If loggerWrite() is called from multiple task contexts without a lock, JSONL rows could interleave. Need to verify whether this is actually true before fixing.
Codex — 2.4 GHz WiFi channel rejection, GNSS standalone alerts, ambient table sizing
These are architectural improvement suggestions, not bugs. Defer to v2.1.

Decision: Fix These Before v2.0.0 Tag
#FindingSourceFix SizeF1Serial races — wrap unprotected callsC2Medium (~40 sites)F2Delete wifi_dashboard dead codeD1Small (delete 2 files)F3BleFrame buffer 64→256M2TrivialF4CapturedFrame buffer 256→320M3TrivialF5MIN_ELEV_FOR_CNO applied to C/N0 filterC1Small (~4 lines)F6Minimum-length guard before opendroneidCodex CRIT 2Small (~8 lines)F7Verify logger multi-task accessCodex HIGHInvestigate first