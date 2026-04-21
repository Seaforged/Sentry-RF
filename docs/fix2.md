Paste this into CC:


Decisions made. Proceed with all 12 fixes. CC's preferred options on the two open decisions: Issue 5 = Option A (full NAN decode, extend CapturedFrame with MAC header), Issue 8 = Option B+C (alertMutex + deeper queue + batch drain). Implement everything in one commit.
Read every file cited in the findings before touching anything: src/cad_scanner.cpp, include/cad_scanner.h, src/wifi_scanner.cpp, src/detection_engine.cpp, src/gps_manager.cpp, src/gnss_integrity.cpp, src/alert_handler.cpp, src/buzzer_manager.cpp, src/main.cpp, src/data_logger.cpp, src/rf_scanner.cpp, include/detection_types.h, include/sentry_config.h, include/alert_handler.h, README.md, docs/SENTRY-RF_Known_Issues_Tracker.md.
Implement in this order to avoid dependency issues. Build after every group of changes and fix before continuing.

GROUP 1 — Small, high-confidence fixes (Issues 2, 3, 4, 9, 12)
Issue 2 — Invalid RID → WARNING demotion:
In src/wifi_scanner.cpp, find the path where isRemoteId is true. Change the logic so:

event.severity = THREAT_WARNING and systemState.remoteIdDetected = true are ONLY set when decoded && rid.valid is true
When isRemoteId is true but decoded is false: set event.severity = THREAT_ADVISORY, description = "RID beacon undecoded OUI:%02X:%02X:%02X", do NOT set systemState.remoteIdDetected = true
When decoded && rid.valid is true: existing behavior (WARNING + remoteIdDetected = true + ZMQ emit)

Issue 3 — ZMQ race fix:
In src/data_logger.cpp, find emitZmqJson(). Replace the manual xSemaphoreTake(serialMutex, pdMS_TO_TICKS(20)) + locked-flag + fallback pattern with SERIAL_SAFE(...) wrapping both Serial.printf branches. Delete the locked variable entirely. The function should fail-safe by skipping the emit if it cannot take the mutex within the SERIAL_SAFE timeout — never print without holding it.
Issue 4 — WiFi frame loss: ISR filter + batch drain:
In src/wifi_scanner.cpp:

In the promiscuous callback (ISR context), before xQueueSendFromISR, check the frame subtype. Only enqueue frame types 0x80 (beacon) and 0xD0 (action frame). Reject all others with an early return. Comment: // Only beacons and action frames can carry Remote ID — reject all other management subtypes before enqueue.
In wifiScanTask, replace the single xQueueReceive per loop iteration with a batch drain: while (xQueueReceive(wifiPacketQueue, &frame, pdMS_TO_TICKS(5)) == pdTRUE) { /* process frame */ } — drain until empty each pass, then yield with vTaskDelay(pdMS_TO_TICKS(10)).

Issue 9 — Candidate region-wide merge:
In src/detection_engine.cpp, find the candidate association logic for fhssSub. Delete the clause that matches any frequency within the entire 860–870 MHz or 902–928 MHz region when fhssSub evidence is live. Keep only: (a) direct anchor ±CAND_ASSOC_SUB_MHZ and (b) within the seen-frequency envelope (minFreqSeen–maxFreqSeen) ±CAND_ASSOC_SUB_MHZ. Add comment: // Region-wide association removed — fuses unrelated emitters in dense ISM environments. Envelope-based association is sufficient as the drone hops.
Issue 12 — Logger drops:
In src/data_logger.cpp, find loggerLock() or the mutex-take timeout. Change the timeout from pdMS_TO_TICKS(50) to portMAX_DELAY. Add a static uint32_t loggerDropsBefore = 0 counter — if the existing drop counter is non-zero and increased since last check, log [LOG] Dropped N rows via SERIAL_SAFE. (If there is no existing drop counter, add one.)
Build all three targets after Group 1. Fix any errors before continuing.

GROUP 2 — GNSS fixes (Issue 7)
Three stacked changes:

In src/gps_manager.cpp, find the satellite C/N0 aggregation loop. Change the filter from cno > 0 to cno >= GPS_MIN_CNO so weak-SNR satellites are excluded from the uniformity calculation.
In src/gnss_integrity.cpp or wherever GNSS threat level is assessed, add standalone GNSS DetectionEvent emission. When status.threatLevel transitions to THREAT_ADVISORY or higher (compare against previous value), emit a DetectionEvent to detectionQueue with source = DET_SOURCE_GNSS, severity = status.threatLevel, description = "GNSS: {jam|spoof|position jump}". Use xQueueSend(detectionQueue, &event, pdMS_TO_TICKS(5)). This allows the alert system to sound for jamming/spoofing even when no RF candidate is active.
In src/detection_engine.cpp, find the GNSS evidence attachment gate (rfRecentlyActive && activeCount == 1). Rename the concept to "gnssBoost" in the comments — GNSS still attaches to candidates for score boosting when RF is correlated, but the standalone DetectionEvent path (step 2) handles the case where RF is silent. No code change needed here beyond the comment update — the standalone event path is additive.

Build all three targets after Group 2. Fix before continuing.

GROUP 3 — Alert state (Issue 8, Option B+C)

In src/alert_handler.cpp and include/alert_handler.h, add a static SemaphoreHandle_t alertMutex = nullptr. Initialize it in alertInit() with xSemaphoreCreateMutex(). Wrap the body of alertAcknowledge() and alertToggleMute() with xSemaphoreTake(alertMutex, portMAX_DELAY) / xSemaphoreGive(alertMutex). Inside alertTask's queue drain loop, take alertMutex before reading/writing _lastThreat, _isAcknowledged, _isMuted. Release after each event is processed.
In src/main.cpp, find where detectionQueue is created. Change depth from 10 to 32. Comment: // Deepened from 10 to 32 — 7 producer paths (RF, WiFi, BLE, GNSS×2, candidate engine, mode changes) under burst conditions.
In src/alertTask loop, change xQueueReceive to drain until empty each cycle: while (xQueueReceive(detectionQueue, &event, pdMS_TO_TICKS(10)) == pdTRUE) { processEvent(event); } then vTaskDelay(pdMS_TO_TICKS(20)). Add a drop counter: at all 7 xQueueSend(detectionQueue, &event, 0) call sites, change timeout from 0 to pdMS_TO_TICKS(5) so brief stalls don't drop events. If queue is genuinely full, increment a static uint32_t alertQueueDrops counter and log via SERIAL_SAFE once per 10 seconds.

Build all three targets after Group 3. Fix before continuing.

GROUP 4 — Self-test improvement (Issue 10)
In src/main.cpp, runSelfTest(), replace the -127.5 dBm sentinel check with:

Check API return codes: if (radio.setFrequency(915.0) != RADIOLIB_ERR_NONE) { radioHealthy = false; } — if setFrequency fails, the radio is dead.
RSSI variance check: read RSSI at 5 different frequencies (860, 880, 900, 915, 930 MHz). Compute the range (max - min). If range < 3.0 dBm, the radio is returning a stuck/clamped value — set radioHealthy = false. A live radio in any RF environment will show at least 3 dBm of variation across 70 MHz.
Keep the existing 10-read loop but replace the > -127.4f || < -127.6f check with the variance check above.

Build all three targets after Group 4. Fix before continuing.

GROUP 5 — Warmup probation table (Issue 1, CRITICAL)
This is the most structural change. Read src/cad_scanner.cpp completely — especially the ambient learning paths — before writing any code.
Replace the warmup ambient learning behavior:

Add a pendingAmbient table alongside the existing ambientTaps array. Use a fixed-size array of PendingAmbientTap structs (max 64 entries):
cppstruct PendingAmbientTap {
    float freqMHz;
    uint8_t sf;
    unsigned long firstSeenMs;
    bool hasCorroboration; // true if FHSS diversity, RID, or GNSS has fired on this freq
};

During warmup (cycle count < AMBIENT_CAD_WARMUP_CYCLES): instead of calling recordAmbientTap() directly, add the tap to pendingAmbient if not already present. Do NOT mark it ambient yet.
After warmup completes: graduate taps from pendingAmbient to ambientTaps ONLY if hasCorroboration == false. Any pending tap where FHSS diversity accumulated, or where a WiFi RID or GNSS anomaly fired within CAND_ASSOC_SUB_MHZ of the frequency during the warmup window, is NOT graduated — it's discarded from pending and treated as a real detection candidate.
Wire the corroboration flag: when the detection engine sees FHSS diversity > FHSS_DIVERSITY_MIN on a frequency during warmup, call markPendingCorroboration(freqMHz). When systemState.remoteIdDetected becomes true during warmup, call markPendingCorroboration(0) (marks all pending taps as corroborated — any drone nearby during warmup should disqualify all pending ambient). When a GNSS anomaly fires during warmup, same: markPendingCorroboration(0).
Delete the firstSeenMs < AMBIENT_WARMUP_MS retag path entirely.
Add a boot log line after graduation: [WARMUP] Complete. Graduated {N}/{M} pending taps as ambient ({K} discarded — corroboration detected).

Build all three targets after Group 5. Fix before continuing.

GROUP 6 — NAN action-frame RID (Issue 5, Option A)

In include/wifi_scanner.h and src/wifi_scanner.cpp, extend CapturedFrame to include a 24-byte MAC header field:
cppstruct CapturedFrame {
    uint8_t  macHeader[24]; // 802.11 MAC header (Phase M-A: needed for NAN decode)
    uint8_t  frameType;
    uint8_t  srcMAC[6];
    int8_t   rssi;
    uint8_t  channel;
    uint16_t payloadLen;
    uint8_t  payload[320];  // bumped from 256 in earlier fix
};

In the promiscuous ISR callback, populate macHeader by copying the first 24 bytes of the raw frame before the payload offset.
For 0xD0 action frames: instead of calling findRemoteIdIE(), call odid_wifi_receive_message_pack_nan_action_frame() from the opendroneid library, passing the full raw frame (macHeader + payload reconstructed). If it returns a valid ODID_UAS_Data, populate DecodedRID from it using the same field extraction as decodeBeaconRID().
For 0x80 beacon frames: existing path unchanged.
Update the [WIFI] log line for NAN decodes to read [NAN-RID] instead of [RID] so operators can distinguish the transport.

Build all three targets after Group 6. Fix before continuing.

GROUP 7 — Plateau detector for 2.4 GHz WiFi-channel blind spot (Issue 6)

In src/rf_scanner.cpp, add int countContiguousElevatedBins24(const float* rssi, int numBins, float threshold) — scans the full 2.4 GHz sweep array for the longest run of consecutive bins above threshold. Returns the run length. O(N), zero allocation.
In src/detection_engine.cpp, after the existing 2.4 GHz peak extraction (which rejects WiFi-channel bins), add a parallel plateau check: call countContiguousElevatedBins24() with threshold = adaptiveNoiseFloor24 + OFDM_PLATEAU_THRESHOLD_DB (add this constant to sentry_config.h, default 8.0f). If the longest run >= OFDM_PLATEAU_MIN_BINS (add to config, default 10) for OFDM_PLATEAU_PERSIST_CYCLES (add to config, default 3) consecutive sweeps, emit a [OFDM-PLATEAU] serial log and attach bwWide evidence to any active 2.4 GHz candidate. Keep the isWiFiChannel() reject in the existing peak-finder unchanged — the plateau detector runs independently.
Add a persistence counter: static int plateauConsecutive = 0. Increment when run >= threshold, reset to 0 when it drops below. Only act when plateauConsecutive >= OFDM_PLATEAU_PERSIST_CYCLES.

Build all three targets after Group 7.

GROUP 8 — Docs (Issue 11)

In README.md: update the GNSS section to say "GNSS jamming and spoofing anomaly detection with RF correlation — standalone GNSS alerts planned for v2.1". Remove any claim that standalone jamming/spoofing alerts the operator independently.
In docs/SENTRY-RF_Known_Issues_Tracker.md: move warmup poisoning from KNOWN LIMITATIONS to OPEN with severity CRITICAL. Update the FSK threshold entry if it still says -50 (should be -70). Add entries for: NaN action-frame RID (OPEN, v2.1), 2.4 GHz OcuSync WiFi-channel blind spot (OPEN, v2.1 — plateau detector is mitigation not full fix), alert state ownership (OPEN, mitigated with alertMutex in this sprint).


Final steps:
After all groups build clean:

Run pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121 one final time to confirm all three clean together
Flash t3s3_lr1121 to COM14 and capture 60 seconds of serial to verify: boot self-test passes, warmup completes with graduation log line, no [ZMQ] lines at idle, no panics
Commit all changes: git add -A && git commit -m "v2.0.0 pre-release audit fixes: warmup probation, GNSS standalone alerts, NAN RID, alert mutex, plateau detector, serial races, candidate isolation"
Report: every file modified, total lines changed, build output for all three targets, commit hash, and a summary table of all 12 findings with FIXED / MITIGATED / DEFERRED status