#ifndef DETECTION_TYPES_H
#define DETECTION_TYPES_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"
#include "compass.h"

enum ThreatLevel : uint8_t {
    THREAT_CLEAR    = 0,
    THREAT_ADVISORY = 1,
    THREAT_WARNING  = 2,
    THREAT_CRITICAL = 3
};

// Shared state protected by stateMutex — copy under lock, process outside lock
static const uint8_t DET_SOURCE_WIFI = 2;

struct SystemState {
    ScanResult      spectrum;
    ScanResult24    spectrum24;
    GpsData         gps;
    IntegrityStatus integrity;
    ThreatLevel     threatLevel;
    bool            wifiScannerActive;
    CompassData     compass;
    uint8_t         batteryPercent;
    unsigned long   lastSweepMs;
    unsigned long   lastGpsMs;
    // Buzzer state (written by alert task, read by display task)
    bool            buzzerMuted;
    bool            buzzerAcknowledged;
    unsigned long   muteRemainingMs;
    // CAD results (written by loRaScanTask for logging)
    int             cadDiversity;
    int             cadConfirmed;
    int             cadTotalTaps;
    int             confidenceScore;
    // WiFi Remote ID (written by wifiScanTask)
    bool            remoteIdDetected;
    unsigned long   remoteIdLastMs;
    // WiFi per-channel activity for the Dashboard mini chart.
    // Count is frames-per-snapshot-window (currently 1 second).
    // wifiChannelSnapshotMs = 0 means "scanner not yet populated" — display
    // should show "WiFi scan..." text fallback in that case.
    uint8_t         wifiChannelCount[13];
    unsigned long   wifiChannelSnapshotMs;
};

// Detection event sources
static const uint8_t DET_SOURCE_RF   = 0;
static const uint8_t DET_SOURCE_GNSS = 1;

// Carried from sensor tasks to alert handler via detectionQueue
struct DetectionEvent {
    uint8_t       source;         // DET_SOURCE_RF or DET_SOURCE_GNSS
    uint8_t       severity;       // 0=info, 1=advisory, 2=warning, 3=critical
    float         frequency;      // MHz (RF events) or 0 (GNSS events)
    float         rssi;           // dBm (RF events) or C/N0 stddev (GNSS events)
    char          description[64];
    unsigned long timestamp;      // millis()
};

// Globals — created in main.cpp, used by all tasks
extern SystemState       systemState;
extern SemaphoreHandle_t stateMutex;
extern SemaphoreHandle_t serialMutex;
extern QueueHandle_t     detectionQueue;

#endif // DETECTION_TYPES_H
