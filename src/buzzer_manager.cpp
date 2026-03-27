#include "buzzer_manager.h"
#include "board_config.h"
#include <Arduino.h>

// ============================================================
// Tone step: {frequency_hz, duration_ms}. {0,0} = end sentinel.
// ============================================================

struct ToneStep {
    uint16_t freqHz;
    uint16_t durationMs;
};

static const ToneStep PAT_SELF_TEST[] = {
    {1000, 100}, {1500, 100}, {2000, 100}, {0, 0}
};
static const ToneStep PAT_RF_ADVISORY[] = {
    {2000, 150}, {0, 0}
};
static const ToneStep PAT_RF_WARNING[] = {
    {2000, 100}, {0, 80}, {2000, 100}, {0, 80}, {2000, 100}, {0, 0}
};
static const ToneStep PAT_GNSS_WARNING[] = {
    {1500, 150}, {800, 150}, {1500, 150}, {800, 150}, {0, 0}
};
static const ToneStep PAT_REMOTEID_DETECT[] = {
    {1000, 120}, {1200, 120}, {0, 0}
};
static const ToneStep PAT_CRITICAL[] = {
    {1800, 5000}, {0, 0}
};
static const ToneStep PAT_ALL_CLEAR[] = {
    {1500, 80}, {1000, 80}, {600, 80}, {0, 0}
};

// ── State ───────────────────────────────────────────────────

static const ToneStep* _pattern    = nullptr;
static uint8_t         _stepIndex  = 0;
static unsigned long   _stepStartMs = 0;
static bool            _playing    = false;
static bool            _muted      = false;
static uint8_t         _volume     = 50;  // 0-100%

// ── Helpers ─────────────────────────────────────────────────

static const ToneStep* getPattern(TonePatternType p) {
    switch (p) {
        case TONE_SELF_TEST:       return PAT_SELF_TEST;
        case TONE_RF_ADVISORY:     return PAT_RF_ADVISORY;
        case TONE_RF_WARNING:      return PAT_RF_WARNING;
        case TONE_GNSS_WARNING:    return PAT_GNSS_WARNING;
        case TONE_REMOTEID_DETECT: return PAT_REMOTEID_DETECT;
        case TONE_CRITICAL:        return PAT_CRITICAL;
        case TONE_ALL_CLEAR:       return PAT_ALL_CLEAR;
        default:                   return nullptr;
    }
}

static void setTone(uint16_t freqHz) {
    if (!HAS_BUZZER) return;

    if (freqHz == 0 || _muted) {
        ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
    } else {
        ledcWriteTone(BUZZER_LEDC_CHANNEL, freqHz);
        // Set duty cycle for volume (8-bit resolution: 0-255, 128 = 50%)
        uint8_t duty = map(_volume, 0, 100, 0, 128);
        ledcWrite(BUZZER_LEDC_CHANNEL, duty);
    }
}

// ── Public API ──────────────────────────────────────────────

void buzzerInit() {
    if (!HAS_BUZZER) return;

    // ESP32 Arduino v2.x LEDC API
    ledcSetup(BUZZER_LEDC_CHANNEL, 2000, BUZZER_LEDC_RESOLUTION);
    ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CHANNEL);
    ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);  // start silent
}

void buzzerPlayPattern(TonePatternType p) {
    if (!HAS_BUZZER || _muted) return;

    const ToneStep* pat = getPattern(p);
    if (!pat) return;

    _pattern = pat;
    _stepIndex = 0;
    _stepStartMs = millis();
    _playing = true;

    // Start first step immediately
    setTone(_pattern[0].freqHz);
}

void buzzerStop() {
    setTone(0);
    _playing = false;
    _pattern = nullptr;
    _stepIndex = 0;
}

void buzzerUpdate() {
    if (!_playing || !_pattern) return;
    if (_muted) { buzzerStop(); return; }

    const ToneStep& step = _pattern[_stepIndex];

    // Check if current step duration elapsed
    if (millis() - _stepStartMs >= step.durationMs) {
        _stepIndex++;

        // Check for end sentinel
        if (_pattern[_stepIndex].freqHz == 0 && _pattern[_stepIndex].durationMs == 0) {
            buzzerStop();
            return;
        }

        // Start next step
        _stepStartMs = millis();
        setTone(_pattern[_stepIndex].freqHz);
    }
}

bool buzzerIsPlaying() {
    return _playing;
}

void buzzerSetMuted(bool muted) {
    _muted = muted;
    if (_muted) buzzerStop();
}

bool buzzerIsMuted() {
    return _muted;
}

void buzzerSetVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    _volume = percent;
}
