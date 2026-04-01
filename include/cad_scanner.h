#ifndef CAD_SCANNER_H
#define CAD_SCANNER_H

#include <Arduino.h>
#include <RadioLib.h>
#include "board_config.h"
#include "rf_scanner.h"
#include "sentry_config.h"

// ── Tap-and-verify data structures ──────────────────────────────────────────

struct CadTap {
    float    frequency;
    uint8_t  sf;              // 0 for FSK taps
    uint8_t  consecutiveHits;
    uint8_t  missCount;
    unsigned long firstSeenMs;
    unsigned long lastSeenMs;
    bool     active;
    bool     isFsk;           // true for FSK taps, false for LoRa CAD taps
    bool     isAmbient;       // true if matches ambient source from warmup
};

struct CadFskResult {
    int confirmedCadCount;    // CAD taps with 3+ consecutive hits
    int confirmedFskCount;    // FSK taps with 3+ consecutive hits
    int strongPendingCad;     // CAD taps with exactly 2 consecutive hits
    int pendingTaps;          // active taps not yet confirmed
    int totalActiveTaps;      // all active taps (any hit count)
    int recentHitCount;       // non-ambient CAD hits in last 30s (any freq/SF)
};

// ── Public API ──────────────────────────────────────────────────────────────

void cadScannerInit();
bool cadWarmupComplete();

// Run the full fishing pole scan cycle.
// Called from loRaScanTask every cycle. rssi may be nullptr if no RSSI data yet.
#ifdef BOARD_T3S3_LR1121
CadFskResult cadFskScan(LR1121& radio, uint32_t cycleNum, const ScanResult* rssi = nullptr);
#else
CadFskResult cadFskScan(SX1262& radio, uint32_t cycleNum, const ScanResult* rssi = nullptr);
#endif

#endif // CAD_SCANNER_H
