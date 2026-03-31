#ifndef CAD_SCANNER_H
#define CAD_SCANNER_H

#include <Arduino.h>
#include <RadioLib.h>
#include "board_config.h"

// ── Tap-and-verify data structures ──────────────────────────────────────────

static const int MAX_TAPS = 32;
static const int TAP_CONFIRM_HITS = 3;   // consecutive hits to confirm as drone
static const int TAP_EXPIRE_MISSES = 3;  // consecutive misses to deactivate
static const float TAP_FREQ_TOL = 0.2f;  // MHz — ±200 kHz to match an existing tap

struct CadTap {
    float    frequency;
    uint8_t  sf;              // 0 for FSK taps
    uint8_t  consecutiveHits;
    uint8_t  missCount;
    unsigned long firstSeenMs;
    unsigned long lastSeenMs;
    bool     active;
    bool     isFsk;           // true for FSK taps, false for LoRa CAD taps
};

struct CadFskResult {
    int confirmedCadCount;    // CAD taps with 3+ consecutive hits
    int confirmedFskCount;    // FSK taps with 3+ consecutive hits
    int strongPendingCad;     // CAD taps with exactly 2 consecutive hits
    int pendingTaps;          // active taps not yet confirmed
};

// ── Channel scan parameters ─────────────────────────────────────────────────

// Channels scanned per cycle at each SF (rotating across full channel plan)
// SF6 dominates: ELRS 200Hz and Crossfire 50Hz are the most common drone modes.
// Total: 66 channels, ~33ms per cycle.
static const int CAD_CH_SF6  = 40;
static const int CAD_CH_SF7  = 15;
static const int CAD_CH_SF8  = 5;
static const int CAD_CH_SF9  = 3;
static const int CAD_CH_SF10 = 1;
static const int CAD_CH_SF11 = 1;
static const int CAD_CH_SF12 = 1;
static const int FSK_CH      = 4;

// Total ELRS channel counts
static const int ELRS_915_CHANNELS = 80;   // US: 902-928 MHz, 325 kHz spacing
static const int ELRS_868_CHANNELS = 50;   // EU: 860-886 MHz, 520 kHz spacing
static const int CRSF_CHANNELS     = 100;  // 260 kHz spacing

// ── Public API ──────────────────────────────────────────────────────────────

void cadScannerInit();

// Run the full fishing pole scan cycle.
// Called from loRaScanTask after RSSI sweep completes.
// cycleNum increments each call for channel rotation.
#ifdef BOARD_T3S3_LR1121
CadFskResult cadFskScan(LR1121& radio, uint32_t cycleNum);
#else
CadFskResult cadFskScan(SX1262& radio, uint32_t cycleNum);
#endif

#endif // CAD_SCANNER_H
