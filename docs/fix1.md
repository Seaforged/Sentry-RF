This is a follow-up Codex review of the fixes CC already applied in commit 388e68d. Good — it's tracking the diff, not just re-reading the original. Let me separate the real from the overcalls carefully, because some of these are genuine and some are Codex pushing for architectural rewrites that are out of scope for a v2.0 tag.

Triage of Follow-up Review
Genuinely fixed (Codex confirms):

Short-packet ODID OOB read guard ✅
BLE buffer truncation (64→256) ✅
MIN_ELEV_FOR_CNO now applied ✅
Cross-task logger file-handle race ✅

Still open — real issues:
Serial race incomplete (HIGH)
Codex is right. The SERIAL_SAFE macro still has an unprotected fallback path on lock timeout — it prints anyway. That defeats the purpose. The WiFi task also still has unguarded Serial.printf calls. The ZMQ corruption path is still open. This needs a complete fix.
Known Issues Tracker is stale (MEDIUM)
Codex is correct that SENTRY-RF_Known_Issues_Tracker.md still says there's no boot self-test, no operational modes, etc. — all things that now exist. Before tagging v2.0.0 the docs need to reflect reality.
WiFi queue recreation on COVERT resume (MEDIUM)
Real. wifiScannerInit() recreates the queue on every COVERT→STANDARD transition. Over multiple mode cycles this leaks heap. The fix is to create the queue once at boot and just flush/reset it on resume.
docs/fix.md has factual errors (MEDIUM)
If this is a release decision document it needs to be accurate. Codex found specific wrong statements about action-frame capture and alert ownership.
Overcalls / deferred-by-design:
Warmup poisoning (CRITICAL)
Codex keeps flagging this. It's a real architectural concern but the fix — a probation table — is a substantial redesign of the ambient filter. This is a known trade-off we've documented before: the warmup window is short (~50s), and in operational use SENTRY-RF is deployed in an established position, not powered on while a drone is already flying overhead. This is a v2.1 item, not a v2.0 blocker.
NAN/action-frame parsing (HIGH)
Codex is right that 0xD0 action frames exist, but our WiFi scanner captures beacon frames from channel-hopping promiscuous mode. Action frames on 802.11 NaN require specific NaN service discovery — a different radio mode we don't enable. This is not a practical gap for the drones we're targeting.
2.4 GHz WiFi channel rejection (HIGH)
Architectural improvement request, not a bug. DJI OcuSync inside WiFi channels is a known LR1121 limitation documented in our research. Not a v2.0 gate.
GNSS standalone alerts (HIGH)
Also architectural — standalone GNSS events without RF correlation is a design choice, not a bug. The current behavior is documented.
Alert state ownership (HIGH)
Codex is escalating this. displayTask calling alertAcknowledge() and alertToggleMute() while alertTask mutates the same globals is a real race. However the functions modify simple bool flags — not compound state — and the worst case is a mute/ack that takes an extra cycle to register. It's a real gap but not a field-safety failure.

What We Fix Before v2.0.0 Tag
Three things, one CC sprint:

Fix the SERIAL_SAFE macro — make it blocking (no unprotected fallback), wrap remaining WiFi task printf calls properly
Fix WiFi queue — create once at boot, reset on COVERT resume instead of recreating
Update stale docs — SENTRY-RF_Known_Issues_Tracker.md and fix.md to reflect actual current state

Everything else goes into the v2.1 backlog with honest documentation. Here's the CC prompt:


Read src/wifi_scanner.cpp, src/main.cpp, src/data_logger.cpp, include/sentry_config.h, and docs/SENTRY-RF_Known_Issues_Tracker.md completely before writing any code.
Three fixes only. No other changes.
Fix 1 — Serial race: make SERIAL_SAFE blocking with no unprotected fallback
Find the SERIAL_SAFE macro or inline pattern used across the codebase. Currently it tries to take serialMutex with a short timeout and falls back to printing without the mutex if it times out. That unprotected fallback defeats the purpose — under contention it still corrupts the [ZMQ] JSON pipe.
Change SERIAL_SAFE to always block until the mutex is acquired — use portMAX_DELAY instead of a short timeout. Add a comment: // Blocks until mutex available — safe to call from any task. ZMQ pipe integrity requires no unprotected Serial.printf anywhere.
Then grep for any remaining Serial.printf / Serial.println / Serial.print calls in src/wifi_scanner.cpp that do NOT go through SERIAL_SAFE or a serialMutex take block. Wrap each one.
Do the same audit for src/ble_scanner.cpp — any unprotected Serial calls there.
Fix 2 — WiFi queue: create once, reset on COVERT resume
In src/wifi_scanner.cpp, find where wifiPacketQueue is created. Currently wifiScannerInit() creates a new queue every time it's called, including on COVERT resume. This leaks the previous queue handle on every mode toggle.
Change it so the queue is created exactly once — on the first call to wifiScannerInit() (check if (wifiPacketQueue == nullptr)). On subsequent calls (COVERT resume), instead of creating a new queue, flush the existing one by draining all items with xQueueReceive(wifiPacketQueue, &frame, 0) in a loop until it returns pdFALSE. Add a comment: // Queue created once at boot. On COVERT resume we flush stale frames rather than recreating to avoid heap leak.
Also: in the WiFi ISR callback, add a drop counter. If xQueueSendFromISR fails (queue full), increment a static uint32_t wifiQueueDrops counter. Log the drop count to serial once per second in wifiScanTask if it's non-zero (gated behind SERIAL_SAFE). Reset the counter after logging.
Fix 3 — Update stale documentation
Read docs/SENTRY-RF_Known_Issues_Tracker.md completely. Update it to reflect current v2.0.0 state:

Mark as RESOLVED: boot self-test, operational modes (STANDARD/COVERT/HIGH_ALERT), GPS_MIN_CNO production value, ambient filter, LR1121 CAD pipeline, ASTM F3411 full payload decode, BLE Remote ID, ZMQ output, bandwidth discrimination
Add a KNOWN LIMITATIONS section (not bugs, by-design decisions): warmup poisoning window (v2.1 target), 2.4 GHz WiFi channel rejection for OcuSync, GNSS standalone alerts require RF correlation, NAN action-frame RID not supported (beacon-only), alert state ownership (v2.1 target)
Update version references from v1.x to v2.0.0
Do NOT delete findings that are genuinely still open — mark them as OPEN with the version they'll be addressed

If a docs/fix.md exists, read it and correct any factually incorrect statements Codex identified: (1) the scanner does capture 0xD0 action frames in addition to 0x80 beacons but parses them with the beacon IE walker — note this as a known gap, not a correct behavior; (2) alert state is mutated from multiple tasks — note this as open, not resolved.
Constraints:

No behavior changes beyond the serial mutex and queue fixes
Do not touch detection_engine.cpp, gnss_integrity.cpp, cad_scanner.cpp, or any files not listed above
Compile clean for all three targets

Build all three targets. Commit with message: "Pre-release fixes: serial mutex blocking, wifi queue lifecycle, stale docs updated"
Report: exact lines changed in each file, build output, commit hash.