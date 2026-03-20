#ifndef DETECTION_TYPES_H
#define DETECTION_TYPES_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"

// Shared state protected by stateMutex — copy under lock, process outside lock
struct SystemState {
    ScanResult      spectrum;
    GpsData         gps;
    IntegrityStatus integrity;
    unsigned long   lastSweepMs;
    unsigned long   lastGpsMs;
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
extern SemaphoreHandle_t stateMutex;
extern SemaphoreHandle_t serialMutex;
extern QueueHandle_t     detectionQueue;

#endif // DETECTION_TYPES_H
