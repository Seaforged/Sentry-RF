#include "alert_handler.h"
#include <Arduino.h>

static const char* sourceStr(uint8_t source) {
    return (source == DET_SOURCE_RF) ? "RF" : "GNSS";
}

static const char* severityStr(uint8_t severity) {
    switch (severity) {
        case 0:  return "INFO";
        case 1:  return "ADVISORY";
        case 2:  return "WARNING";
        case 3:  return "CRITICAL";
        default: return "?";
    }
}

void alertTask(void* param) {
    DetectionEvent event;

    for (;;) {
        // Block until an event arrives — no CPU burn while idle
        if (xQueueReceive(detectionQueue, &event, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.printf("[ALERT] %s %s: %s (%.1f MHz, %.1f dBm)\n",
                              severityStr(event.severity),
                              sourceStr(event.source),
                              event.description,
                              event.frequency,
                              event.rssi);
                xSemaphoreGive(serialMutex);
            }
        }
    }
}
