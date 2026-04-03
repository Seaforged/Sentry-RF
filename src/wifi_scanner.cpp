#include "wifi_scanner.h"
#include "board_config.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

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
static bool hasRemoteIdIE(const CapturedFrame& frame) {
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
                return true;
            }
        }

        pos += 2 + elen;
    }
    return false;
}

// ── WiFi scan task ──────────────────────────────────────────────────────────

void wifiScanTask(void* param) {
    CapturedFrame frame;
    uint8_t currentChannel = 1;
    unsigned long lastHopMs = 0;
    static const unsigned long HOP_INTERVAL_MS = 100;

    for (;;) {
        // Channel hop every 100ms across channels 1-13
        if (millis() - lastHopMs > HOP_INTERVAL_MS) {
            currentChannel = (currentChannel % 13) + 1;
            esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
            lastHopMs = millis();
        }

        // Process captured frames
        if (xQueueReceive(wifiPacketQueue, &frame, pdMS_TO_TICKS(10)) == pdTRUE) {
            const char* droneName = identifyDroneMAC(frame.srcMAC);
            bool isRemoteId = hasRemoteIdIE(frame);

            if (droneName != nullptr || isRemoteId) {
                DetectionEvent event = {};
                event.source = DET_SOURCE_WIFI;
                event.severity = isRemoteId ? THREAT_WARNING : THREAT_ADVISORY;
                event.frequency = 2412.0 + ((frame.channel - 1) * 5.0);
                event.rssi = frame.rssi;
                event.timestamp = millis();

                if (isRemoteId) {
                    // Signal to detection engine via shared state
                    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        systemState.remoteIdDetected = true;
                        systemState.remoteIdLastMs = millis();
                        xSemaphoreGive(stateMutex);
                    }
                    Serial.printf("[WIFI] RID beacon from %02X:%02X:%02X:%02X:%02X:%02X ch%d RSSI:%d\n",
                                  frame.srcMAC[0], frame.srcMAC[1], frame.srcMAC[2],
                                  frame.srcMAC[3], frame.srcMAC[4], frame.srcMAC[5],
                                  frame.channel, frame.rssi);
                    snprintf(event.description, sizeof(event.description),
                             "RemoteID %02X:%02X:%02X:%02X:%02X:%02X ch%d",
                             frame.srcMAC[0], frame.srcMAC[1], frame.srcMAC[2],
                             frame.srcMAC[3], frame.srcMAC[4], frame.srcMAC[5],
                             frame.channel);
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
