#ifndef BUZZER_MANAGER_H
#define BUZZER_MANAGER_H

#include <cstdint>

// ============================================================
// Non-blocking buzzer tone pattern player
// Uses ESP32 LEDC PWM — call buzzerUpdate() every loop iteration
// ============================================================

enum TonePatternType : uint8_t {
    TONE_NONE = 0,
    TONE_SELF_TEST,        // ascending 3-tone (boot only)
    TONE_RF_ADVISORY,      // 1 short high beep
    TONE_RF_WARNING,       // 3 rapid high beeps
    TONE_GNSS_WARNING,     // alternating siren
    TONE_REMOTEID_DETECT,  // 2-tone low
    TONE_CRITICAL,         // sustained 1800Hz
    TONE_ALL_CLEAR,        // descending 3-tone
};

void buzzerInit();
void buzzerPlayPattern(TonePatternType p);
void buzzerStop();
void buzzerUpdate();             // call every ~100ms from alert task
bool buzzerIsPlaying();
void buzzerSetMuted(bool muted);
bool buzzerIsMuted();
void buzzerSetVolume(uint8_t percent);  // 0-100

#endif // BUZZER_MANAGER_H
