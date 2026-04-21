Read src/wifi_scanner.cpp, include/wifi_scanner.h, include/detection_types.h, include/sentry_config.h, src/main.cpp, and include/board_config.h completely before writing any code.
Implement BLE Remote ID scanning for ASTM F3411 on the ESP32-S3's built-in Bluetooth radio.
Background: ASTM F3411-22a mandates drones broadcast Remote ID on either WiFi NaN or Bluetooth Legacy Advertising. Some drones use BLE only. The ESP32-S3 has BLE 5.0 built in. The service UUID for ASTM F3411 BLE is 0xFFFA. BLE advertising packets containing this UUID in their Service Data AD type carry the same ODID message pack we already know how to decode via opendroneid-core-c.
Important hardware constraint: On ESP32-S3, WiFi and BLE share the same 2.4 GHz radio hardware. They can coexist via ESP-IDF's coexistence mode, but WiFi promiscuous mode and BLE scanning cannot both be fully active simultaneously — the coexistence scheduler time-slices them. The approach: run BLE scanning at reduced duty cycle (scan window 50ms, scan interval 500ms — 10% duty) to minimize impact on WiFi Remote ID capture.
Implementation:

Add #define HAS_BLE_RID 1 to include/board_config.h for all three board targets (all use ESP32-S3 which has BLE).
Create include/ble_scanner.h and src/ble_scanner.cpp with:

void bleScannerInit() — initializes BLE, sets scan parameters (window 50ms / interval 500ms), registers the advertisement callback
void bleScannerStart() / void bleScannerStop() — start/stop scanning
void bleScanTask(void* param) — FreeRTOS task that runs the BLE scan loop, checks mode (stops scanning in COVERT mode, resumes on return to STANDARD/HIGH_ALERT)
The advertisement callback: when an AD type 0x16 (Service Data) with UUID 0xFFFA is found, extract the service data payload and call odid_message_process_pack() to decode it into ODID_UAS_Data, then populate a DecodedRID struct and write it to systemState.lastRID under stateMutex — same struct as Phase J WiFi RID. Emit [BLE-RID] UAS-ID: {id} Drone: {lat},{lon} RSSI:{rssi}dBm to serial. Emit emitZmqJson(snap, "rid") after the mutex release.
Use the Arduino BLE library (NimBLEDevice is preferred if available in the project's lib_deps; otherwise use BLEDevice from ESP32 Arduino core). Check which is already present in platformio.ini before choosing.


In src/main.cpp:

Add TaskHandle_t hBLETask = nullptr
In setup(), after wifiScannerInit(), call bleScannerInit() and spawn bleScanTask on Core 0 with 4096 stack, priority 1
In COVERT mode handling: bleScannerStop() when entering COVERT, bleScannerStart() when leaving (same pattern as WiFi suspend/resume)


Add BLE RID detection to threat escalation: in the BLE advertisement callback, after a successful decode, also send a DetectionEvent with source = DET_SOURCE_WIFI (reuse the WiFi RID source — BLE RID is the same ASTM standard, same threat level), threat = THREAT_WARNING, description "BLE-RID {uasID}".
The OLED screenRID display (Phase J) already shows systemState.lastRID — BLE decodes will automatically appear there since they write the same struct. No display changes needed.

Constraints:

BLE scan callback runs in BLE stack context — must not call Serial.printf directly. Use a FreeRTOS queue (depth 4) to pass decoded structs to bleScanTask for processing and serial output, same pattern as WiFi's wifiPacketQueue
Do not call esp_wifi_deinit() or any WiFi stack function from the BLE callback
In COVERT mode, BLE must be fully stopped — bleScannerStop() must call BLEDevice::getScan()->stop()
Gate all BLE code behind #ifdef HAS_BLE_RID so it compiles out cleanly if the flag is removed
Compile clean for all three targets

Build all three targets. Commit with message: "Phase M: BLE Remote ID scanning (ASTM F3411 BLE advertising)"
Report: every file changed, build output, commit hash.