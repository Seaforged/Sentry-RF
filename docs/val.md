Paste this into CC:


Read-only audit pass — do not make any changes. This is a final pre-tag review of the current HEAD after the four Codex-identified blockers were fixed.
Read these files completely before writing anything: src/wifi_scanner.cpp, src/gnss_integrity.cpp, src/cad_scanner.cpp, src/detection_engine.cpp, src/main.cpp, src/alert_handler.cpp, src/data_logger.cpp, docs/SENTRY-RF_Known_Issues_Tracker.md, README.md.
Focus your review specifically on the four fixed areas plus any regressions introduced by the large sprint:
1. WiFi RID / NAN action-frame fix (was: every 0xD0 raised false ADVISORY)

Does the new path correctly gate isRemoteId on a successful NAN decode before emitting any event?
Can a crafted or malformed 0xD0 frame still trigger an alert without a valid decode?
Is there any new code path where a failed NAN decode falls through to the old IE-walker?

2. GNSS standalone alert / unified threat state (was: GNSS WARNING stuck, OLED showed RF CLEAR)

Does systemState.threatLevel now reflect max(RF, GNSS) correctly?
Does the GNSS path emit a clearing event when the anomaly resolves?
Is the GNSS event emitted under the correct mutex? Is there any race between the GNSS task writing threat state and the display task reading it?

3. Warmup probation table concurrency (was: pendingAmbientTaps unprotected shared state)

Is the new mutex actually protecting every read and write to pendingAmbientTaps?
Is the graduation loop (warmup complete → graduate taps) atomic with respect to corroboration writes from WiFi and GNSS tasks?
Can a corroboration mark still be lost if it arrives during graduation?

4. OFDM plateau confirmer (was: attached bwWide to all candidates, not just the best)

Does the plateau confirmer now select one candidate using the same policy as other confirmers?
Is there any remaining path that iterates all candidates and attaches evidence to multiple?

5. Regression check — ZMQ snapshot fix (additional fix CC applied)

Does the ZMQ emit now use a fresh snapshot taken after the FSM transition, not before?
Do [FSM] and [ZMQ] threat levels agree on every transition edge?

6. Quick scan for new issues introduced by the large sprint

Any new unprotected shared variable accesses introduced in this sprint
Any new Serial.printf without SERIAL_SAFE in the changed files
Any new heap allocation in a task loop
Any function that now exceeds reasonable complexity (look for anything obviously wrong, not a full style audit)

Output format: Numbered findings only. Same severity scale: CRITICAL / HIGH / MEDIUM / LOW / PASS. File and line number for each. If a fix is correct and complete, say PASS explicitly — don't leave it ambiguous. End with a go/no-go recommendation for v2.0.0 tagging.