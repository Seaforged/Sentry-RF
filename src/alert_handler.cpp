#include "alert_handler.h"
#include "buzzer_manager.h"
#include "board_config.h"
#include "sentry_config.h"
#include <Arduino.h>

// ============================================================
// Alert Handler — drives LED + buzzer based on detection events
// ============================================================

static const char* sourceStr(uint8_t source) {
    if (source == DET_SOURCE_RF)   return "RF";
    if (source == DET_SOURCE_WIFI) return "WIFI";
    return "GNSS";
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

static const char* threatStr(ThreatLevel t) {
    switch (t) {
        case THREAT_CLEAR:    return "CLEAR";
        case THREAT_ADVISORY: return "ADVISORY";
        case THREAT_WARNING:  return "WARNING";
        case THREAT_CRITICAL: return "CRITICAL";
        default:              return "?";
    }
}

// ── State ───────────────────────────────────────────────────

static ThreatLevel   _lastThreat       = THREAT_CLEAR;
static ThreatLevel   _acknowledgedAt   = THREAT_CLEAR;
static bool          _isAcknowledged   = false;
static bool          _isMuted          = false;
static unsigned long _muteStartMs      = 0;
static unsigned long _lastEscalationMs = 0;
// MUTE_DURATION_MS, REMINDER_INTERVAL from sentry_config.h

// ── LED pattern (non-LEDC, uses digitalWrite) ───────────────

static unsigned long ledLastToggleMs = 0;
static bool ledState = false;

static void updateLED(ThreatLevel level, bool acknowledged) {
    if (acknowledged || level == THREAT_CLEAR) {
        digitalWrite(PIN_LED, LOW);
        ledState = false;
        return;
    }

    unsigned long now = millis();

    if (level == THREAT_CRITICAL) {
        digitalWrite(PIN_LED, HIGH);
        ledState = true;
    } else if (level == THREAT_WARNING) {
        // Fast blink: 200ms on / 200ms off
        if (now - ledLastToggleMs >= 200) {
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? HIGH : LOW);
            ledLastToggleMs = now;
        }
    } else if (level == THREAT_ADVISORY) {
        // Slow blink: 500ms on / 500ms off
        if (now - ledLastToggleMs >= 500) {
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? HIGH : LOW);
            ledLastToggleMs = now;
        }
    }
}

// ── Select tone pattern based on source and severity ────────

static TonePatternType selectPattern(uint8_t source, ThreatLevel level) {
    if (level == THREAT_CRITICAL) return TONE_CRITICAL;

    if (source == DET_SOURCE_RF) {
        return (level >= THREAT_WARNING) ? TONE_RF_WARNING : TONE_RF_ADVISORY;
    }
    if (source == DET_SOURCE_WIFI) {
        return TONE_REMOTEID_DETECT;
    }
    // GNSS
    return TONE_GNSS_WARNING;
}

// ── Public API (called from display task) ───────────────────

void alertAcknowledge() {
    if (_lastThreat <= THREAT_CLEAR) return;

    _isAcknowledged = true;
    _acknowledgedAt = _lastThreat;
    buzzerStop();

    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        Serial.printf("[ALERT] ACK by operator at %s level\n", threatStr(_lastThreat));
        xSemaphoreGive(serialMutex);
    }
}

void alertToggleMute() {
    _isMuted = !_isMuted;
    buzzerSetMuted(_isMuted);

    if (_isMuted) {
        _muteStartMs = millis();
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            Serial.println("[ALERT] MUTED for 5 minutes");
            xSemaphoreGive(serialMutex);
        }
    } else {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            Serial.println("[ALERT] UNMUTED");
            xSemaphoreGive(serialMutex);
        }
    }
}

bool alertIsMuted()          { return _isMuted; }
bool alertIsAcknowledged()   { return _isAcknowledged; }

unsigned long alertMuteRemainingMs() {
    if (!_isMuted) return 0;
    unsigned long elapsed = millis() - _muteStartMs;
    return (elapsed < MUTE_DURATION_MS) ? (MUTE_DURATION_MS - elapsed) : 0;
}

// ── Alert task ──────────────────────────────────────────────

void alertTask(void* param) {
    DetectionEvent event;

    // Self-test beep on boot
    buzzerInit();
    buzzerPlayPattern(TONE_SELF_TEST);
    // Let the self-test pattern play out
    for (int i = 0; i < 10; i++) {
        buzzerUpdate();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    for (;;) {
        // Auto-unmute after timeout
        if (_isMuted && (millis() - _muteStartMs >= MUTE_DURATION_MS)) {
            _isMuted = false;
            buzzerSetMuted(false);
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.println("[ALERT] UNMUTED (timeout)");
                xSemaphoreGive(serialMutex);
            }
        }

        // Read system threat level and update buzzer state
        ThreatLevel systemThreat = THREAT_CLEAR;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            extern SystemState systemState;
            systemThreat = systemState.threatLevel;
            systemState.buzzerMuted = _isMuted;
            systemState.buzzerAcknowledged = _isAcknowledged;
            systemState.muteRemainingMs = alertMuteRemainingMs();
            xSemaphoreGive(stateMutex);
        }

        // Process detection queue
        if (xQueueReceive(detectionQueue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            ThreatLevel newLevel = (ThreatLevel)event.severity;

            // Serial log
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.printf("[ALERT] %s %s: %s (%.1f MHz, %.1f dBm)\n",
                              severityStr(event.severity),
                              sourceStr(event.source),
                              event.description,
                              event.frequency,
                              event.rssi);
                xSemaphoreGive(serialMutex);
            }

            if (newLevel > _lastThreat) {
                // ── ESCALATION ──
                _isAcknowledged = false;
                _lastEscalationMs = millis();

                if (newLevel >= THREAT_WARNING) {
                    TonePatternType pat = selectPattern(event.source, newLevel);
                    buzzerPlayPattern(pat);
                }

                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    Serial.printf("[ALERT] ESCALATION: %s -> %s — buzzer: %s\n",
                                  threatStr(_lastThreat), threatStr(newLevel),
                                  (newLevel >= THREAT_WARNING) ? "ON" : "silent");
                    xSemaphoreGive(serialMutex);
                }

                _lastThreat = newLevel;

            } else if (newLevel == _lastThreat && !_isAcknowledged) {
                // ── SAME LEVEL, NOT ACK'd — reminder beep after 30s ──
                if (millis() - _lastEscalationMs > REMINDER_INTERVAL && !buzzerIsPlaying()) {
                    if (newLevel >= THREAT_WARNING) {
                        buzzerPlayPattern(TONE_RF_ADVISORY);  // soft reminder
                    }
                    _lastEscalationMs = millis();
                }

            } else if (newLevel < _lastThreat) {
                // ── DE-ESCALATION ──
                if (newLevel == THREAT_CLEAR) {
                    buzzerPlayPattern(TONE_ALL_CLEAR);
                    _isAcknowledged = false;

                    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        Serial.printf("[ALERT] De-escalation: %s -> CLEAR — buzzer: ALL_CLEAR\n",
                                      threatStr(_lastThreat));
                        xSemaphoreGive(serialMutex);
                    }
                }
                _lastThreat = newLevel;
            }
        }

        // Update buzzer state machine (non-blocking)
        buzzerUpdate();

        // LED tracks system threat level (from detection engine), not individual events
        updateLED(systemThreat, _isAcknowledged);
    }
}
