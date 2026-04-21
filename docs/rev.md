Full system audit of SENTRY-RF v2.0.0 before release tagging. Read every source file completely before writing anything. This is a read-only review — do not make any changes. Produce a written report only.
Files to read (read ALL of them):
platformio.ini, include/board_config.h, include/sentry_config.h, include/version.h, include/detection_types.h, include/detection_engine.h, include/rf_scanner.h, include/cad_scanner.h, include/gnss_integrity.h, include/gps_manager.h, include/wifi_scanner.h, include/ble_scanner.h, include/data_logger.h, include/display.h, include/alert_handler.h, include/buzzer_manager.h, include/task_config.h, include/drone_signatures.h, include/ambient_filter.h, include/error_messages.h, src/main.cpp, src/detection_engine.cpp, src/rf_scanner.cpp, src/cad_scanner.cpp, src/gnss_integrity.cpp, src/gps_manager.cpp, src/wifi_scanner.cpp, src/ble_scanner.cpp, src/data_logger.cpp, src/display.cpp, src/alert_handler.cpp, src/buzzer_manager.cpp, src/drone_signatures.cpp, src/ambient_filter.cpp, src/compass.cpp, lib/opendroneid/opendroneid.h, tools/zmq_bridge.py, tools/field_analyzer.py
Audit dimensions — report on each one:
1. Correctness

Any logic errors, wrong comparisons, off-by-one errors, incorrect calculations
Any function that returns or uses uninitialized data
Any race conditions between tasks (check every shared variable and whether it's properly protected by stateMutex or serialMutex)
Any place where the wrong radio API is called for a given board target (SX1262 vs LR1121 specific calls)
Any ASTM F3411 / opendroneid decode path that could silently produce wrong results

2. Memory

Stack usage: are any tasks at risk of overflow? Cross-reference task stack sizes in task_config.h against what each task actually uses (look for large local arrays, deep call chains)
Heap: any dynamic allocation (new, malloc, std::string, std::vector) that happens in a task loop rather than at init time — these fragment the heap over time
Any buffer that could overflow (fixed-size char arrays written with snprintf/sprintf — verify the size math)
The CapturedFrame payload[256] vs maximum WiFi beacon size — is 256 enough?
The BleFrame queue depth 4 — could it overflow under burst BLE advertising conditions?

3. Dead code and bloat

Any #ifdef blocks that can never be true given current board definitions
Any functions declared in headers but never called
Any constants defined but never referenced
Any commented-out code blocks that should be removed
Any duplicate logic between files that could be consolidated

4. Performance

Any busy-wait loops that should use vTaskDelay
Any operation in a task loop that's more expensive than it needs to be (e.g. sorting, nth_element, large memcpy every cycle)
The RSSI sweep median computation — what's the actual cost per cycle?
Any Serial.printf calls not protected by serialMutex that could cause garbled output
BLE scan duty cycle: is 10% (50ms/500ms) actually being honored, or is there a code path that runs it wider?

5. Correctness of Phase implementations

Phase H (operational modes): does COVERT mode actually suppress ALL emissions? Is there any code path that could still emit WiFi or BLE in COVERT?
Phase I (bandwidth discrimination): is countElevatedAdjacentBins() called with the correct threshold value? Is the result actually used to influence detection scoring?
Phase J (ASTM F3411 decode): is the 5-byte skip (OUI + type + counter) correct per the ASTM spec? Could a truncated frame crash the decoder?
Phase K (self-test): does the async GPS check actually clear the flag when a fix is received, or is there a timing issue?
Phase L (ZMQ output): is the 1 Hz debounce working correctly? Is there any path where [ZMQ] could be emitted without the serialMutex held?
Phase M (BLE RID): is the NimBLE callback safe to call xQueueSend from? What happens if the BleFrame queue is full?

6. Missing items or gaps

Anything specified in the roadmap docs that was never implemented
Any error condition that is logged but never handled
Any hardware-specific path that works on one board but silently fails on another
The Heltec V4 — is it referenced anywhere in the code or fully excluded?

7. Code quality

Any magic numbers that should be named constants
Any function longer than 100 lines that should be split
Inconsistent naming conventions
Any header that doesn't have include guards

Output format:
Produce a numbered findings list. Each finding: severity (CRITICAL / HIGH / MEDIUM / LOW / INFO), file and line number, description, and recommended fix. Group by dimension. End with a summary table: total findings by severity. Be specific — vague findings like "consider refactoring" are not useful. If something is correct and well-implemented, say so briefly. Do not suggest changes that would alter system behavior — flag them but note they require human decision.