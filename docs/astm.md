Read the following files completely before writing any code: src/wifi_scanner.cpp, include/wifi_scanner.h, include/detection_types.h, include/sentry_config.h, src/display.cpp, src/data_logger.cpp, docs/SENTRY-RF_UPDATED_ROADMAP.md.
Then implement full ASTM F3411 Remote ID payload decoding using the opendroneid-core-c library.
Step 1 — Vendor the library:
Clone or download only the following files from https://github.com/opendroneid/opendroneid-core-c into a new lib/opendroneid/ directory in the project:

libopendroneid/opendroneid.h
libopendroneid/opendroneid.c
libopendroneid/wifi.h
libopendroneid/wifi.c

Add lib/opendroneid/ as a library source in platformio.ini for all three targets.
Step 2 — Add a decoded RID struct to SystemState:
In include/detection_types.h, add a new struct DecodedRID and a field DecodedRID lastRID to SystemState:
cppstruct DecodedRID {
    bool     valid;              // true if a successfully decoded message was received
    char     uasID[21];         // drone serial number / UAS ID (null-terminated)
    char     uasIDType[16];     // "Serial", "CAA", "UTM", "Specific"
    float    droneLat;          // drone latitude degrees
    float    droneLon;          // drone longitude degrees
    float    droneAltM;         // drone altitude MSL metres
    float    operatorLat;       // operator/pilot latitude degrees
    float    operatorLon;       // operator/pilot longitude degrees
    float    speedMps;          // horizontal speed m/s
    uint16_t headingDeg;        // heading 0-359
    unsigned long lastUpdateMs; // millis() when last decoded
};
Step 3 — Decode in wifi_scanner.cpp:
In wifiScanTask, after hasRemoteIdIE() returns true and the raw IE payload is available, call the opendroneid-core-c parser:

Call odid_wifi_receive_message_pack_nan_action_frame() or the appropriate function for standard beacon frames to decode the ASTM F3411 message pack
If decode succeeds, extract fields from the resulting ODID_UAS_Data struct into DecodedRID
Write the populated DecodedRID into systemState.lastRID under stateMutex
Log the decoded fields to serial: [RID] UAS-ID: {id} Drone: {lat},{lon},{alt}m Operator: {lat},{lon} Speed: {spd}m/s Hdg: {hdg}deg

Step 4 — Add a new OLED screen:
In src/display.cpp, add a new screen (call it SCREEN_RID) that displays:

Line 1: RID label + UAS ID (truncated to fit if needed)
Line 2: Drone lat/lon (e.g. D: 36.8673 -76.0214)
Line 3: Drone alt + speed + heading (e.g. Alt:45m 8m/s 270d)
Line 4: Operator lat/lon (e.g. P: 36.8661 -76.0198)
If lastRID.valid is false or lastUpdateMs is more than 10 seconds old, show RID: No data instead

Add SCREEN_RID to the screen rotation. It should only exist on boards that have a WiFi scanner (HAS_WIFI_SCANNER or equivalent guard).
Step 5 — Log decoded fields to JSONL:
In data_logger.cpp, include the decoded RID fields in the existing JSONL log line when lastRID.valid is true. Add fields: rid_id, rid_dlat, rid_dlon, rid_dalt, rid_olat, rid_olon.
Constraints:

Use ODID_AUTH_MAX_PAGES 1 when including opendroneid.h to minimize RAM footprint
The decode must happen in the wifiScanTask context (Core 0), not in the ISR
Do not modify the existing threat escalation behavior — a decoded RID is still a WARNING event regardless of whether fields decoded correctly
If opendroneid-core-c download fails or the repo structure differs, report what you found and ask before continuing

Acceptance criteria:

lib/opendroneid/ exists with the four source files
platformio.ini builds all three targets with the new library
Serial output shows [RID] lines with decoded fields when a compliant drone is nearby
New OLED screen cycles into rotation and shows decoded data
JSONL log includes RID fields when valid
Compile clean for all three targets: pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121

Build all three targets. Report:

Exact opendroneid-core-c files vendored and their source commit/version
Every file modified and what changed
Build output for all three targets including flash size delta
Commit with message: "Phase J: ASTM F3411 full payload decode (opendroneid-core-c)"