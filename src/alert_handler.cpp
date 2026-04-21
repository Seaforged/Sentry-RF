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

// Issue 8: serialize access to the alert state fields above. displayTask
// enters here via alertAcknowledge()/alertToggleMute() while alertTask
// mutates the same fields on the queue drain path — previously racy.
// alertTask takes the mutex around its read/write blocks; the button-
// action entry points take it around their whole body. Initialized lazily
// in alertTask before the main loop so we never dereference nullptr from
// early boot events.
static SemaphoreHandle_t alertMutex = nullptr;

// Issue 8 F3: dropped-event counter. Surfaced via serial once per 10 s
// if non-zero so saturation is visible to the operator / analyst.
static volatile uint32_t alertQueueDrops   = 0;
static unsigned long     lastDropLogMs     = 0;
static const unsigned long DROP_LOG_INTERVAL_MS = 10000;

// ── LED pattern (non-LEDC, uses digitalWrite) ───────────────

static unsigned long ledLastToggleMs = 0;
static bool ledState = false;

static void updateLED(ThreatLevel level, bool acknowledged) {
    // Phase H: COVERT suppresses the LED entirely — zero emission.
    if (modeGet() == MODE_COVERT) {
        digitalWrite(PIN_LED, LOW);
        ledState = false;
        return;
    }

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
    // Issue 8: serialize against alertTask. Safe to guard with nullptr
    // check — if the mutex hasn't been created yet (pre-task boot), no
    // other task is running so the state write is race-free by default.
    if (alertMutex) xSemaphoreTake(alertMutex, portMAX_DELAY);

    if (_lastThreat <= THREAT_CLEAR) {
        if (alertMutex) xSemaphoreGive(alertMutex);
        return;
    }

    _isAcknowledged = true;
    _acknowledgedAt = _lastThreat;
    buzzerStop();
    ThreatLevel snapshot = _lastThreat;
    if (alertMutex) xSemaphoreGive(alertMutex);

    SERIAL_SAFE(Serial.printf("[ALERT] ACK by operator at %s level\n",
                              threatStr(snapshot)));
}

void alertToggleMute() {
    if (alertMutex) xSemaphoreTake(alertMutex, portMAX_DELAY);
    _isMuted = !_isMuted;
    buzzerSetMuted(_isMuted);
    bool nowMuted = _isMuted;
    if (nowMuted) _muteStartMs = millis();
    if (alertMutex) xSemaphoreGive(alertMutex);

    if (nowMuted) {
        SERIAL_SAFE(Serial.println("[ALERT] MUTED for 5 minutes"));
    } else {
        SERIAL_SAFE(Serial.println("[ALERT] UNMUTED"));
    }
}

bool alertIsMuted()          { return _isMuted; }
bool alertIsAcknowledged()   { return _isAcknowledged; }

void alertQueueDropInc()     { alertQueueDrops++; }

unsigned long alertMuteRemainingMs() {
    if (!_isMuted) return 0;
    unsigned long elapsed = millis() - _muteStartMs;
    return (elapsed < MUTE_DURATION_MS) ? (MUTE_DURATION_MS - elapsed) : 0;
}

// ── Alert task ──────────────────────────────────────────────

void alertTask(void* param) {
    DetectionEvent event;

    // Issue 8: create the alert-state mutex before any self-test beep or
    // queue drain. Public entry points alertAcknowledge / alertToggleMute
    // can arrive from displayTask on Core 0 as early as the first button
    // press, so we need the mutex alive before the main loop starts.
    if (alertMutex == nullptr) {
        alertMutex = xSemaphoreCreateMutex();
    }

    // Self-test beep on boot
    buzzerInit();
    buzzerPlayPattern(TONE_SELF_TEST);
    // Let the self-test pattern play out
    for (int i = 0; i < 10; i++) {
        buzzerUpdate();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    for (;;) {
        // Auto-unmute after timeout — take alertMutex to synchronize with
        // displayTask's alertToggleMute() writes.
        xSemaphoreTake(alertMutex, portMAX_DELAY);
        bool didAutoUnmute = false;
        if (_isMuted && (millis() - _muteStartMs >= MUTE_DURATION_MS)) {
            _isMuted = false;
            buzzerSetMuted(false);
            didAutoUnmute = true;
        }
        // Snapshot state for systemState mirror under the same lock
        bool snapMuted      = _isMuted;
        bool snapAck        = _isAcknowledged;
        unsigned long snapMuteLeft = alertMuteRemainingMs();
        xSemaphoreGive(alertMutex);

        if (didAutoUnmute) {
            SERIAL_SAFE(Serial.println("[ALERT] UNMUTED (timeout)"));
        }

        // Read system threat level and update buzzer state
        ThreatLevel systemThreat = THREAT_CLEAR;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            extern SystemState systemState;
            systemThreat = systemState.threatLevel;
            systemState.buzzerMuted        = snapMuted;
            systemState.buzzerAcknowledged = snapAck;
            systemState.muteRemainingMs    = snapMuteLeft;
            xSemaphoreGive(stateMutex);
        }

        // Issue 8 F3: drain the detection queue until empty each pass
        // instead of taking one event per loop. This keeps up with
        // bursty producers (RF/WiFi/BLE/GNSS/candidate engine/mode) on
        // the deepened 32-slot queue without starving.
        while (xQueueReceive(detectionQueue, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
            ThreatLevel newLevel = (ThreatLevel)event.severity;

            // Serial log — suppress (freq, rssi) suffix when fields are
            // not populated. CAD/FSK/FHSS events often have freq=0 (multi-
            // channel) and/or rssi=0 (not tracked in TrackedSignal yet).
            // Valid RSSI is always negative in dBm, so rssi < 0 means real.
            char suffix[40] = "";
            if (event.frequency > 0.0f && event.rssi < 0.0f) {
                snprintf(suffix, sizeof(suffix), " (%.1f MHz, %.1f dBm)",
                         event.frequency, event.rssi);
            } else if (event.frequency > 0.0f) {
                snprintf(suffix, sizeof(suffix), " (%.1f MHz)", event.frequency);
            } else if (event.rssi < 0.0f) {
                snprintf(suffix, sizeof(suffix), " (%.1f dBm)", event.rssi);
            }
            SERIAL_SAFE(Serial.printf("[ALERT] %s %s: %s%s\n",
                                      severityStr(event.severity),
                                      sourceStr(event.source),
                                      event.description,
                                      suffix));

            // Phase H: COVERT suppresses all audible output.
            bool covert = (modeGet() == MODE_COVERT);
            bool highAlert = (modeGet() == MODE_HIGH_ALERT);

            // Issue 8: all writes to _lastThreat / _isAcknowledged /
            // _lastEscalationMs serialized via alertMutex so concurrent
            // displayTask button-driven writes don't tear state.
            xSemaphoreTake(alertMutex, portMAX_DELAY);
            ThreatLevel prevThreat = _lastThreat;
            ThreatLevel newLevelLocal = (ThreatLevel)event.severity;
            bool escalated = false;
            bool deescalatedToClear = false;

            if (newLevelLocal > prevThreat) {
                _isAcknowledged = false;
                _lastEscalationMs = millis();
                _lastThreat = newLevelLocal;
                escalated = true;
            } else if (newLevelLocal == prevThreat && !_isAcknowledged) {
                // same-level-not-ack'd — escalation timestamp updated by
                // the actual buzzer branches below under alertMutex
            } else if (newLevelLocal < prevThreat) {
                if (newLevelLocal == THREAT_CLEAR) {
                    _isAcknowledged = false;
                    _lastThreat = newLevelLocal;
                    deescalatedToClear = true;
                } else {
                    _lastThreat = newLevelLocal;
                }
            }
            xSemaphoreGive(alertMutex);

            // Side effects (buzzer, serial) run outside the alertMutex so
            // they don't block other state writers. Buzzer patterns are
            // non-blocking; SERIAL_SAFE uses its own mutex.
            if (escalated) {
                if (!covert && newLevelLocal >= THREAT_WARNING) {
                    TonePatternType pat = selectPattern(event.source, newLevelLocal);
                    buzzerPlayPattern(pat);
                }
                SERIAL_SAFE(Serial.printf("[ALERT] ESCALATION: %s -> %s — buzzer: %s\n",
                                          threatStr(prevThreat),
                                          threatStr(newLevelLocal),
                                          (newLevelLocal >= THREAT_WARNING) ? "ON" : "silent"));
            } else if (newLevelLocal == prevThreat && !snapAck) {
                // SAME LEVEL, NOT ACK'd — COVERT suppresses all output
                // ; HIGH_ALERT fires immediately (no debounce); otherwise
                // wait for REMINDER_INTERVAL before a soft reminder.
                if (covert) {
                    // no-op
                } else if (highAlert && newLevelLocal >= THREAT_WARNING &&
                           !buzzerIsPlaying()) {
                    TonePatternType pat = selectPattern(event.source, newLevelLocal);
                    buzzerPlayPattern(pat);
                    xSemaphoreTake(alertMutex, portMAX_DELAY);
                    _lastEscalationMs = millis();
                    xSemaphoreGive(alertMutex);
                } else {
                    xSemaphoreTake(alertMutex, portMAX_DELAY);
                    bool timeToRemind =
                        (millis() - _lastEscalationMs) > REMINDER_INTERVAL;
                    if (timeToRemind) _lastEscalationMs = millis();
                    xSemaphoreGive(alertMutex);
                    if (timeToRemind && !buzzerIsPlaying() &&
                        newLevelLocal >= THREAT_WARNING) {
                        buzzerPlayPattern(TONE_RF_ADVISORY);
                    }
                }
            } else if (deescalatedToClear) {
                if (!covert) buzzerPlayPattern(TONE_ALL_CLEAR);
                SERIAL_SAFE(Serial.printf("[ALERT] De-escalation: %s -> CLEAR — buzzer: ALL_CLEAR\n",
                                          threatStr(prevThreat)));
            }
        }

        // Issue 8: log accumulated queue drops once per 10s if any.
        uint32_t now_ms = millis();
        if (now_ms - lastDropLogMs >= DROP_LOG_INTERVAL_MS) {
            uint32_t drops = alertQueueDrops;
            if (drops > 0) {
                alertQueueDrops = 0;
                SERIAL_SAFE(Serial.printf("[ALERT] Queue drops in last 10s: %u\n",
                                          (unsigned)drops));
            }
            lastDropLogMs = now_ms;
        }

        // Update buzzer state machine (non-blocking)
        buzzerUpdate();

        // LED tracks system threat level (from detection engine), not
        // individual events. Use the snap captured at the top of this
        // iteration so we don't contend on alertMutex here.
        updateLED(systemThreat, snapAck);

        // Yield before the next drain pass.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
