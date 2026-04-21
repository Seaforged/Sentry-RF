Read include/sentry_config.h, src/gnss_integrity.cpp, src/main.cpp, and include/board_config.h completely before writing any code.
Then make two changes:
Change 1 — GPS_MIN_CNO production value:
In include/sentry_config.h, find GPS_MIN_CNO and change its value from 6 to 15. Update the comment to read:
// dB-Hz minimum satellite signal strength. INDOOR TESTING: 6. FIELD/PRODUCTION: 15-20.
Also find the C/N0 uniformity check in src/gnss_integrity.cpp. Gate it behind a check: if GPS_MIN_CNO < 15, skip the uniformity calculation entirely and return without flagging. Add a comment: // Uniformity check suppressed when GPS_MIN_CNO < 15 — indoor attenuated signals cluster naturally and produce false positives.
Change 2 — Boot self-test:
In src/main.cpp, after displayBootSplash() and before initRadioHardware(), add a runSelfTest() function call. Implement runSelfTest() as a static function in main.cpp:

Radio health: Set frequency to 915.0 MHz, enter RX mode, read RSSI 10 times with 10ms spacing via getInstantRSSI(). If all 10 reads return -127.5 dBm exactly (the error/no-signal sentinel), set radioHealthy = false. This is a warning only — do NOT halt. Print [SELFTEST] Radio: FAIL or [SELFTEST] Radio: OK.
Antenna quality: Read RSSI at 10 frequencies spread across 860–930 MHz (use 860, 867, 874, 881, 888, 895, 902, 909, 916, 930 MHz). If ALL 10 readings are below ANTENNA_FLOOR_DBM (-130.0 dBm, already in sentry_config.h), set antennaWarning = true. Print [SELFTEST] Antenna: WARN (no signal detected) or [SELFTEST] Antenna: OK. This is a warning only — do NOT halt.
GPS check: Start a non-blocking timer. Set a flag gpsFixPending = true. In gpsReadTask, when the first valid 3D fix is received, clear this flag. If GPS_FIX_TIMEOUT_MS (120000ms, already in sentry_config.h) elapses without a fix, log [SELFTEST] GPS: NO FIX after 120s to serial. This check runs asynchronously — do not block setup() waiting for GPS.
OLED summary screen: After radio and antenna checks complete, display a 3-second boot summary screen:
SENTRY-RF v2.0
Radio:  OK / FAIL
Antenna: OK / WARN
GPS:    Acquiring...
Use the existing OLED display handle. After 3 seconds, proceed to normal boot.
Scan cycle watchdog: In loRaScanTask, at the top of each cycle, record cycleStartMs = millis(). At the end of each cycle, if millis() - cycleStartMs > SCAN_WATCHDOG_MS (5000ms, already in sentry_config.h), print [WATCHDOG] Scan cycle exceeded 5000ms — cycle:{N} duration:{D}ms to serial. Do NOT reset — just log it. Resetting mid-operation would corrupt the radio state.
Self-test log to SD: In loggerInit() or immediately after, write a single JSONL line: {"event":"selftest","radio":"OK|FAIL","antenna":"OK|WARN","fw":"2.0.0","boot":N} where N is the boot count.

Constraints:

runSelfTest() must complete within 500ms (radio + antenna checks only — GPS is async)
Do not call radio.begin() inside self-test — use the already-initialized radio object
The watchdog is log-only, no task reset
All three targets must compile clean

Build all three targets. Commit with message: "Phase K: GPS_MIN_CNO=15, boot self-test, scan watchdog"
Report: every file changed, build output, commit hash.