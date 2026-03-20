#include "alert_handler.h"
#include "board_config.h"
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

// Drive LED pattern based on threat level — called when no queue events pending
static void updateLED(ThreatLevel level) {
    static unsigned long lastToggle = 0;
    static bool ledState = false;
    unsigned long now = millis();

    switch (level) {
        case THREAT_CLEAR:
            digitalWrite(PIN_LED, LOW);
            break;
        case THREAT_ADVISORY:
            // 1 Hz blink — 500ms on/off
            if (now - lastToggle >= 500) {
                ledState = !ledState;
                digitalWrite(PIN_LED, ledState ? HIGH : LOW);
                lastToggle = now;
            }
            break;
        case THREAT_WARNING:
            // 4 Hz blink — 125ms on/off
            if (now - lastToggle >= 125) {
                ledState = !ledState;
                digitalWrite(PIN_LED, ledState ? HIGH : LOW);
                lastToggle = now;
            }
            break;
        case THREAT_CRITICAL:
            digitalWrite(PIN_LED, HIGH);
            break;
    }
}

void alertTask(void* param) {
    DetectionEvent event;
    ThreatLevel lastThreat = THREAT_CLEAR;

    for (;;) {
        // Wait up to 100ms for an event — allows LED updates between events
        if (xQueueReceive(detectionQueue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            lastThreat = (ThreatLevel)event.severity;

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

        updateLED(lastThreat);
    }
}
