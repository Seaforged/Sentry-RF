Phase K clean. v2.0.0 is on the board, self-test running, all three targets good. Commit ab4a01c.
Two things CC caught worth noting — GPS_MIN_CNO was already 15 from a previous session (we got there earlier than we thought), and the self-test had to go after scannerInit() not before initRadioHardware() as specced, because you can't probe a radio that hasn't been initialized yet. Both are correct calls.
Phase L — ZMQ/DragonSync output. Paste this into CC:


Read src/detection_engine.cpp, include/detection_types.h, include/sentry_config.h, src/main.cpp, and src/data_logger.cpp completely before writing any code.
Implement ZMQ-compatible JSON output for WarDragon/DragonSync ecosystem integration.
Background: DragonSync uses ZMQ pub/sub on specific ports (4221-4226 for various sensor types). SENTRY-RF should publish on port 4227. Since the ESP32 cannot run a real ZMQ socket, the approach is: emit structured JSON lines to serial prefixed with [ZMQ] on every threat level transition and every decoded Remote ID event. A companion Python bridge script running on the connected laptop reads these lines and republishes them as real ZMQ messages.
Part 1 — ESP32 firmware: JSON serial output
Add a function emitZmqJson(const SystemState& state, const char* eventType) to src/data_logger.cpp (declare in include/data_logger.h). It should print a single line to Serial in this format, prefixed with [ZMQ]:
For threat events (eventType = "threat"):
json[ZMQ] {"type":"threat","ts":12345,"threat":2,"score":85,"freq_mhz":915.2,"rssi_dbm":-72.1,"lat":36.867334,"lon":-76.021460,"alt_m":19,"cad_conf":3,"div":28,"mode":"STD"}
For Remote ID events (eventType = "rid"):
json[ZMQ] {"type":"rid","ts":12345,"uas_id":"ABC123XYZ","uas_type":"Serial","d_lat":36.8673,"d_lon":-76.0214,"d_alt_m":45.0,"o_lat":36.8661,"o_lon":-76.0198,"spd_mps":8.1,"hdg":270}
Fields: ts = millis(), threat = integer threat level (0-3), score = confidence score, freq_mhz = peak frequency, rssi_dbm = peak RSSI, lat/lon/alt_m = GPS position of the detector, cad_conf = confirmed CAD taps, div = CAD diversity, mode = "STD"/"COV"/"HI-ALT" from modeShortLabel().
Call emitZmqJson(state, "threat") from detection_engine.cpp on every threat level transition (CLEAR→ADVISORY, ADVISORY→WARNING, WARNING→CRITICAL, and any downgrade). Call emitZmqJson(state, "rid") from wifi_scanner.cpp immediately after a successful decodeBeaconRID().
Gate the ZMQ output behind a compile-time flag ENABLE_ZMQ_OUTPUT defined in sentry_config.h (default: 1 = enabled). When 0, the emitZmqJson() call compiles to nothing.
Part 2 — Python bridge script
Create C:\Projects\sentry-rf\zmq_bridge.py. This script:

Opens the serial port (COM port passed as argument, default COM14, baud 115200)
Reads lines from serial
Filters lines starting with [ZMQ] 
Strips the prefix, parses the JSON
Publishes the JSON string as a ZMQ PUB message on tcp://*:4227
Also prints each published message to stdout with a timestamp
Handles serial disconnect gracefully with reconnect attempts every 5 seconds
Requires: pyzmq, pyserial

Usage: python zmq_bridge.py --port COM14 --zmq-port 4227
Add a requirements_zmq.txt at the project root listing pyzmq>=25.0 and pyserial>=3.5.
Constraints:

emitZmqJson() must be called under serialMutex or must take it internally — ZMQ lines must not be interleaved with other serial output mid-line
Do not emit ZMQ lines more than once per second per event type (add a lastZmqThreatMs / lastZmqRidMs debounce in data_logger.cpp)
The [ZMQ] prefix must be exactly 5 characters + space so the bridge can strip it with line[6:]
Compile clean for all three targets

Build all three targets. Commit with message: "Phase L: ZMQ/DragonSync JSON serial output + Python bridge"
Report: every file changed, build output, commit hash.