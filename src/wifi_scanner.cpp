#include "wifi_scanner.h"
#include "board_config.h"
#include "sentry_config.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

// Phase J: ASTM F3411 Remote ID payload decode via opendroneid-core-c.
// opendroneid.h is a C header with its own extern "C" wrapping. We only call
// odid_message_process_pack(); the rest of the library (printf helpers, NAN
// frame builder, etc.) is compiled but stripped by the linker or disabled via
// -DODID_DISABLE_PRINTF in platformio.ini.
extern "C" {
#include "opendroneid.h"
}

// ── Packet capture queue ────────────────────────────────────────────────────

static const int MAX_PAYLOAD = 256;

struct CapturedFrame {
    uint8_t  srcMAC[6];
    int8_t   rssi;
    uint8_t  channel;
    uint16_t frameType;
    uint16_t length;
    uint16_t payloadLen;          // actual bytes copied into payload[]
    uint8_t  payload[MAX_PAYLOAD]; // raw frame body after MAC header
};

static QueueHandle_t wifiPacketQueue = nullptr;
static const int PACKET_QUEUE_DEPTH = 20;

// Task handle — set by main.cpp at task creation, declared extern in
// wifi_scanner.h so displayTask can vTaskResume() us on exit from COVERT.
TaskHandle_t hWiFiTask = nullptr;

// Per-channel frame counters for Dashboard mini chart. Incremented from the
// ISR callback on every captured management frame; snapshotted into
// SystemState.wifiChannelCount by wifiScanTask once per second, then reset.
// Non-atomic races are acceptable — losing 1-2 counts per second on a bar
// chart is invisible. 32-bit counters, 13 channels indexed 0..12.
static volatile uint32_t g_wifiFrameCounts[13] = {0};
static unsigned long g_wifiLastSnapshotMs = 0;
static const unsigned long WIFI_SNAPSHOT_INTERVAL_MS = 1000;

// ── Known drone MAC OUI prefixes ────────────────────────────────────────────

static const DroneOUI DRONE_OUIS[] = {
    { {0x60, 0x60, 0x1F}, "DJI" },
    { {0x34, 0xD2, 0x62}, "Autel" },
    { {0x90, 0x03, 0xB7}, "Parrot" },
    { {0xA0, 0x14, 0x3D}, "DJI" },
    { {0x48, 0x21, 0x0B}, "DJI" },
};
static const int DRONE_OUI_COUNT = sizeof(DRONE_OUIS) / sizeof(DRONE_OUIS[0]);

// Remote ID vendor-specific IE OUI (ASTM F3411)
static const uint8_t REMOTE_ID_OUI[] = { 0xFA, 0x0B, 0xBC };

// ── Promiscuous callback (ISR context — keep minimal) ───────────────────────

static void IRAM_ATTR wifiPromiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    if (wifiPacketQueue == nullptr) return;

    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;  // too short for a management frame

    // Count every captured management frame for the Dashboard channel
    // activity chart. Channel 1-13 -> index 0-12.
    int ch = pkt->rx_ctrl.channel;
    if (ch >= 1 && ch <= 13) {
        g_wifiFrameCounts[ch - 1]++;
    }

    CapturedFrame frame;
    // Source MAC is at offset 10 in the 802.11 header
    memcpy(frame.srcMAC, pkt->payload + 10, 6);
    frame.rssi = pkt->rx_ctrl.rssi;
    frame.channel = pkt->rx_ctrl.channel;
    frame.frameType = (pkt->payload[0] & 0xFC);  // frame type + subtype
    frame.length = pkt->rx_ctrl.sig_len;

    // Copy frame body after the 24-byte MAC header for IE parsing
    uint16_t bodyLen = (pkt->rx_ctrl.sig_len > 24) ? pkt->rx_ctrl.sig_len - 24 : 0;
    if (bodyLen > MAX_PAYLOAD) bodyLen = MAX_PAYLOAD;
    frame.payloadLen = bodyLen;
    if (bodyLen > 0) {
        memcpy(frame.payload, pkt->payload + 24, bodyLen);
    }

    xQueueSendFromISR(wifiPacketQueue, &frame, nullptr);
}

// ── Public API ──────────────────────────────────────────────────────────────

void wifiScannerInit() {
    wifiPacketQueue = xQueueCreate(PACKET_QUEUE_DEPTH, sizeof(CapturedFrame));

    // Use ESP-IDF WiFi API directly for promiscuous mode
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();

    // Only capture management frames (beacons, probes, action frames)
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(wifiPromiscuousCallback);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    Serial.println("[WIFI] Promiscuous scanner active — channel hopping");
}

void wifiScannerStop() {
    esp_wifi_set_promiscuous(false);
    Serial.println("[WIFI] Promiscuous scanner stopped");
}

// Full teardown — used by COVERT mode to eliminate WiFi RF emissions.
// Order matters: promiscuous off → stop driver → deinit. Calling
// esp_wifi_deinit() while the driver is still running returns
// ESP_ERR_WIFI_NOT_STOPPED on ESP-IDF 4.x.
void wifiScannerDeinit() {
    esp_wifi_set_promiscuous(false);
    esp_err_t stopErr = esp_wifi_stop();
    esp_err_t deinitErr = esp_wifi_deinit();
    Serial.printf("[WIFI] Deinit — stop:0x%x deinit:0x%x\n",
                  (unsigned)stopErr, (unsigned)deinitErr);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static bool matchOUI(const uint8_t* mac, const uint8_t* oui) {
    return (mac[0] == oui[0] && mac[1] == oui[1] && mac[2] == oui[2]);
}

static const char* identifyDroneMAC(const uint8_t* mac) {
    for (int i = 0; i < DRONE_OUI_COUNT; i++) {
        if (matchOUI(mac, DRONE_OUIS[i].oui)) return DRONE_OUIS[i].name;
    }
    return nullptr;
}

// Parse 802.11 Information Elements for ASTM F3411 Remote ID OUI (FA:0B:BC).
// Beacon body: 8 bytes timestamp + 2 bytes interval + 2 bytes capability = 12 fixed,
// then IEs start at offset 12 within the body payload.
//
// Phase J: extended to optionally return the offset and length of the IE's
// *data* bytes (after element_id + length). Caller uses this to feed the
// message pack (minus oui[3] + oui_type[1] + message_counter[1]) to
// odid_message_process_pack(). Pass nullptrs if only presence is needed.
static bool findRemoteIdIE(const CapturedFrame& frame,
                           uint16_t* outDataOffset, uint16_t* outDataLen) {
    // Only beacons (0x80) and action frames (0xD0) can carry Remote ID
    if (frame.frameType != 0x80 && frame.frameType != 0xD0) return false;

    // IE offset: 12 for beacons (after fixed fields), 1 for action frames (after category)
    uint16_t ieOffset = (frame.frameType == 0x80) ? 12 : 1;
    if (frame.payloadLen <= ieOffset) return false;

    uint16_t pos = ieOffset;
    while (pos + 2 <= frame.payloadLen) {
        uint8_t eid = frame.payload[pos];
        uint8_t elen = frame.payload[pos + 1];

        if (pos + 2 + elen > frame.payloadLen) break;  // truncated IE

        // Vendor-specific IE: element ID 0xDD, OUI in first 3 bytes of data
        if (eid == 0xDD && elen >= 3) {
            if (frame.payload[pos + 2] == REMOTE_ID_OUI[0] &&
                frame.payload[pos + 3] == REMOTE_ID_OUI[1] &&
                frame.payload[pos + 4] == REMOTE_ID_OUI[2]) {
                if (outDataOffset) *outDataOffset = pos + 2;  // after eid+length
                if (outDataLen)    *outDataLen    = elen;
                return true;
            }
        }

        pos += 2 + elen;
    }
    return false;
}

static bool hasRemoteIdIE(const CapturedFrame& frame) {
    return findRemoteIdIE(frame, nullptr, nullptr);
}

// Phase J: Convert ODID_idtype_t enum to the 4 canonical short strings from
// astm.md (Serial / CAA / UTM / Specific). ODID_IDTYPE_NONE and any future
// additions map to "Unknown" so we never write garbage into the log.
static const char* idTypeToString(uint8_t t) {
    switch (t) {
        case ODID_IDTYPE_SERIAL_NUMBER:         return "Serial";
        case ODID_IDTYPE_CAA_REGISTRATION_ID:   return "CAA";
        case ODID_IDTYPE_UTM_ASSIGNED_UUID:     return "UTM";
        case ODID_IDTYPE_SPECIFIC_SESSION_ID:   return "Specific";
        default:                                return "Unknown";
    }
}

// Phase J: Decode an ASTM F3411 beacon IE into a DecodedRID. Returns true on
// a clean decode where the BasicID and Location fields are populated. The IE
// layout inside the vendor-specific IE data (after eid+length) is:
//   [0..2]  OUI = FA:0B:BC                    (already matched)
//   [3]     OUI type = 0x0D                   (ASTM F3411 fixed value)
//   [4]     message counter                   (varies, not needed)
//   [5..]   ODID_MessagePack_encoded          (fed to odid_message_process_pack)
// ieDataOffset/ieDataLen come from findRemoteIdIE(); ieDataLen includes the
// OUI + OUI_type + counter prefix, so we advance 5 bytes and pass the rest.
static bool decodeBeaconRID(const CapturedFrame& frame,
                            uint16_t ieDataOffset, uint16_t ieDataLen,
                            DecodedRID& out) {
    memset(&out, 0, sizeof(out));

    // Require the OUI_type byte = 0x0D (ASTM F3411 beacon). Anything else in
    // this IE slot is a different vendor extension sharing the OUI and won't
    // decode.
    if (ieDataLen < 5) return false;
    uint8_t ouiType = frame.payload[ieDataOffset + 3];
    if (ouiType != 0x0D) return false;

    const uint8_t* pack = frame.payload + ieDataOffset + 5;
    uint16_t       packLen = ieDataLen - 5;

    ODID_UAS_Data uas;
    odid_initUasData(&uas);
    int processed = odid_message_process_pack(&uas, pack, packLen);
    if (processed < 0) return false;

    // BasicID[0] is usually present in a beacon pack; guard on BasicIDValid.
    if (uas.BasicIDValid[0]) {
        strncpy(out.uasID, uas.BasicID[0].UASID, sizeof(out.uasID) - 1);
        out.uasID[sizeof(out.uasID) - 1] = '\0';
        const char* t = idTypeToString((uint8_t)uas.BasicID[0].IDType);
        strncpy(out.uasIDType, t, sizeof(out.uasIDType) - 1);
        out.uasIDType[sizeof(out.uasIDType) - 1] = '\0';
    }

    if (uas.LocationValid) {
        out.droneLat   = (float)uas.Location.Latitude;
        out.droneLon   = (float)uas.Location.Longitude;
        out.droneAltM  = uas.Location.AltitudeGeo;  // WGS84-HAE; -1000 = unknown
        out.speedMps   = uas.Location.SpeedHorizontal;
        // Direction is 0..360 (361 = unknown); clamp to 0..359 as uint16_t.
        float dir = uas.Location.Direction;
        out.headingDeg = (dir >= 0.0f && dir < 360.0f) ? (uint16_t)dir : 0;
    }

    if (uas.SystemValid) {
        out.operatorLat = (float)uas.System.OperatorLatitude;
        out.operatorLon = (float)uas.System.OperatorLongitude;
    }

    out.valid        = uas.BasicIDValid[0] || uas.LocationValid;
    out.lastUpdateMs = millis();
    return out.valid;
}

// ── WiFi scan task ──────────────────────────────────────────────────────────

void wifiScanTask(void* param) {
    CapturedFrame frame;
    uint8_t currentChannel = 1;
    unsigned long lastHopMs = 0;
    static const unsigned long HOP_INTERVAL_MS = 100;

    for (;;) {
        // Phase H: mode gating. On entering COVERT we tear down WiFi and
        // self-suspend. On resume (from non-COVERT mode), we re-init.
        if (modeGet() == MODE_COVERT) {
            wifiScannerDeinit();
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                systemState.wifiScannerActive = false;
                xSemaphoreGive(stateMutex);
            }
            Serial.println("[WIFI] COVERT — suspending task");
            vTaskSuspend(NULL);
            // --- Resumed by displayTask on exit from COVERT ---
            Serial.println("[WIFI] Resumed — reinitializing");
            wifiScannerInit();
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                systemState.wifiScannerActive = true;
                xSemaphoreGive(stateMutex);
            }
            lastHopMs = millis();
            currentChannel = 1;
            continue;
        }

        // Channel hop every 100ms across channels 1-13
        if (millis() - lastHopMs > HOP_INTERVAL_MS) {
            currentChannel = (currentChannel % 13) + 1;
            esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
            lastHopMs = millis();
        }

        // Snapshot per-channel frame counts into SystemState once per second
        // so the Dashboard mini chart has current activity data. Reset the
        // ISR-side counters after the snapshot to start a fresh window.
        unsigned long now = millis();
        if (now - g_wifiLastSnapshotMs >= WIFI_SNAPSHOT_INTERVAL_MS) {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (int i = 0; i < 13; i++) {
                    uint32_t c = g_wifiFrameCounts[i];
                    g_wifiFrameCounts[i] = 0;  // race window: single-frame losses acceptable
                    systemState.wifiChannelCount[i] = (c > 255) ? 255 : (uint8_t)c;
                }
                systemState.wifiChannelSnapshotMs = now;
                xSemaphoreGive(stateMutex);
            }
            g_wifiLastSnapshotMs = now;
        }

        // Process captured frames
        if (xQueueReceive(wifiPacketQueue, &frame, pdMS_TO_TICKS(10)) == pdTRUE) {
            const char* droneName = identifyDroneMAC(frame.srcMAC);
            uint16_t ieDataOffset = 0, ieDataLen = 0;
            bool isRemoteId = findRemoteIdIE(frame, &ieDataOffset, &ieDataLen);

            if (droneName != nullptr || isRemoteId) {
                DetectionEvent event = {};
                event.source = DET_SOURCE_WIFI;
                event.severity = isRemoteId ? THREAT_WARNING : THREAT_ADVISORY;
                event.frequency = 2412.0 + ((frame.channel - 1) * 5.0);
                event.rssi = frame.rssi;
                event.timestamp = millis();

                if (isRemoteId) {
                    // Phase J: attempt full payload decode. If it fails (truncated
                    // frame, wrong OUI type, garbled pack) we still raise the
                    // WARNING — presence of the IE alone is a valid detection.
                    DecodedRID rid;
                    bool decoded = decodeBeaconRID(frame, ieDataOffset, ieDataLen, rid);

                    // Signal to detection engine via shared state
                    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        systemState.remoteIdDetected = true;
                        systemState.remoteIdLastMs = millis();
                        if (decoded) systemState.lastRID = rid;
                        xSemaphoreGive(stateMutex);
                    }

                    if (decoded) {
                        Serial.printf("[RID] UAS-ID: %s Drone: %.6f,%.6f,%.1fm "
                                      "Operator: %.6f,%.6f Speed: %.1fm/s Hdg: %udeg\n",
                                      rid.uasID,
                                      rid.droneLat, rid.droneLon, rid.droneAltM,
                                      rid.operatorLat, rid.operatorLon,
                                      rid.speedMps, rid.headingDeg);
                        snprintf(event.description, sizeof(event.description),
                                 "RID %s @%.4f,%.4f",
                                 rid.uasID, rid.droneLat, rid.droneLon);
                    } else {
                        Serial.printf("[WIFI] RID beacon from %02X:%02X:%02X:%02X:%02X:%02X ch%d RSSI:%d (undecoded)\n",
                                      frame.srcMAC[0], frame.srcMAC[1], frame.srcMAC[2],
                                      frame.srcMAC[3], frame.srcMAC[4], frame.srcMAC[5],
                                      frame.channel, frame.rssi);
                        snprintf(event.description, sizeof(event.description),
                                 "RemoteID %02X:%02X:%02X:%02X:%02X:%02X ch%d",
                                 frame.srcMAC[0], frame.srcMAC[1], frame.srcMAC[2],
                                 frame.srcMAC[3], frame.srcMAC[4], frame.srcMAC[5],
                                 frame.channel);
                    }
                } else {
                    snprintf(event.description, sizeof(event.description),
                             "%s WiFi %02X:%02X:%02X:%02X:%02X:%02X ch%d",
                             droneName,
                             frame.srcMAC[0], frame.srcMAC[1], frame.srcMAC[2],
                             frame.srcMAC[3], frame.srcMAC[4], frame.srcMAC[5],
                             frame.channel);
                }

                xQueueSend(detectionQueue, &event, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
