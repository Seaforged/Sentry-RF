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

// Phase I: bandwidth classification for RSSI peaks. Bin spacing is 200 kHz on
// the sub-GHz sweep, so the width ranges translate as:
//   BW_NARROW  1-3 bins     ~0-600 kHz    ELRS, Crossfire, FrSky
//   BW_MEDIUM  4-9 bins     ~600 kHz-1.8 MHz   unknown/other
//   BW_WIDE    10+ bins     ~1.8 MHz+     DJI OcuSync OFDM
// Populated per-peak by the classifier; the strongest-peak class is mirrored
// into SystemState for logging/display.
enum BandwidthClass : uint8_t {
    BW_NARROW = 0,
    BW_MEDIUM = 1,
    BW_WIDE   = 2
};

// Shared state protected by stateMutex — copy under lock, process outside lock
static const uint8_t DET_SOURCE_WIFI = 2;

// Phase J: ASTM F3411 Remote ID decoded payload. Populated by wifiScanTask
// after a successful opendroneid-core-c decode of a beacon's vendor-specific
// IE (OUI FA:0B:BC, OUI_type 0x0D). Consumers (display, JSONL logger) should
// treat `valid=false` or `millis() - lastUpdateMs > 10000` as "no data".
struct DecodedRID {
    bool          valid;
    char          uasID[21];       // drone serial / UAS ID (null-terminated)
    char          uasIDType[16];   // "Serial", "CAA", "UTM", "Specific"
    float         droneLat;        // drone latitude degrees
    float         droneLon;        // drone longitude degrees
    float         droneAltM;       // drone altitude metres (WGS84 geo)
    float         operatorLat;     // operator/pilot latitude degrees
    float         operatorLon;     // operator/pilot longitude degrees
    float         speedMps;        // horizontal speed m/s
    uint16_t      headingDeg;      // heading 0-359
    unsigned long lastUpdateMs;    // millis() when last successfully decoded
};

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
    // Phase G: candidate engine diagnostics — mirrored from ThreatDecision
    // each cycle so display and logger can show what the candidate engine
    // is actually doing without reaching into detection_engine internals.
    int             fastScore;
    int             confirmScore;
    float           anchorFreq;       // MHz (0.0 when no candidate)
    uint8_t         bandMask;         // bit0=sub-GHz, bit1=2.4 GHz
    bool            hasCandidate;
    int             candidateCount;
    // WiFi Remote ID (written by wifiScanTask)
    bool            remoteIdDetected;
    unsigned long   remoteIdLastMs;
    // WiFi per-channel activity for the Dashboard mini chart.
    // Count is frames-per-snapshot-window (currently 1 second).
    // wifiChannelSnapshotMs = 0 means "scanner not yet populated" — display
    // should show "WiFi scan..." text fallback in that case.
    uint8_t         wifiChannelCount[13];
    unsigned long   wifiChannelSnapshotMs;
    // Phase I: bandwidth classification of the strongest sub-GHz peak, updated
    // by detectionEngineIngestSweep(). peakAdjBins is the raw run length;
    // peakBwClass is the bucketed BandwidthClass. Zero when no peak survived.
    uint8_t         peakBwClass;
    uint8_t         peakAdjBins;
    // Phase J: most-recently-decoded ASTM F3411 Remote ID payload. Fields stay
    // populated until a newer decode overwrites them; use lastUpdateMs for
    // freshness checks (display/logger treat >10s as stale).
    DecodedRID      lastRID;
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
