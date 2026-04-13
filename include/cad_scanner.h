#ifndef CAD_SCANNER_H
#define CAD_SCANNER_H

#include <Arduino.h>
#include <RadioLib.h>
#include "board_config.h"
#include "rf_scanner.h"
#include "sentry_config.h"

// ── Tap-and-verify data structures ──────────────────────────────────────────

enum RfBand : uint8_t { BAND_SUB_GHZ = 0, BAND_2G4 = 1 };

struct CadTap {
    float    frequency;
    uint8_t  sf;              // 0 for FSK taps
    RfBand   band;            // BAND_SUB_GHZ or BAND_2G4
    uint8_t  consecutiveHits;
    uint8_t  missCount;
    unsigned long firstSeenMs;
    unsigned long firstConfirmedMs; // time consecutiveHits first reached TAP_CONFIRM_HITS (0 if never)
    unsigned long lastSeenMs;
    bool     active;
    bool     isFsk;           // true for FSK taps, false for LoRa CAD taps
    bool     isAmbient;       // true if matches ambient source from warmup
};

struct CadBandSummary {
    int confirmedCadCount;
    int confirmedFskCount;
    int strongPendingCad;
    int pendingTaps;
    int totalActiveTaps;
    int diversityCount;
    int persistentDiversityCount;
    int diversityVelocity;
    int sustainedCycles;
};

struct CadFskResult {
    CadBandSummary subGHz;
#ifdef BOARD_T3S3_LR1121
    CadBandSummary band24;
#endif
    // Aggregated totals for backward compat with main.cpp logging / detection_engine
    int confirmedCadCount;        // CAD taps with 3+ consecutive hits
    int confirmedFskCount;        // FSK taps with 3+ consecutive hits
    int strongPendingCad;         // CAD taps with exactly 2 consecutive hits
    int pendingTaps;              // active taps not yet confirmed
    int totalActiveTaps;          // all active taps (any hit count)
    int diversityCount;           // distinct non-ambient frequencies with CAD hits in window
    int persistentDiversityCount; // raw diversity when sustained >= threshold, else 0
    int diversityVelocity;        // diversity threshold crossings in last VELOCITY_WINDOW cycles
    int sustainedCycles;          // consecutive cycles with raw diversity >= PERSISTENCE_MIN_DIVERSITY
};

// ── Public API ──────────────────────────────────────────────────────────────

void cadScannerInit();
bool cadWarmupComplete();
void resetDiversityTracker();
bool cadHwFault();

// Run the full fishing pole scan cycle.
// Called from loRaScanTask every cycle. rssi may be nullptr if no RSSI data yet.
#ifdef BOARD_T3S3_LR1121
CadFskResult cadFskScan(LR1121_RSSI& radio, uint32_t cycleNum, const ScanResult* rssi = nullptr);
#else
CadFskResult cadFskScan(SX1262& radio, uint32_t cycleNum, const ScanResult* rssi = nullptr);
#endif

#endif // CAD_SCANNER_H
