#ifndef RF_SCANNER_H
#define RF_SCANNER_H

#include <RadioLib.h>

// Sub-GHz sweep parameters (860–930 MHz)
// 200 kHz step keeps sweep under ~2.2s with per-bin startReceive() overhead.
// Still fine for drone signals that occupy 250+ kHz channels.
static const float SCAN_FREQ_START = 860.0;
static const float SCAN_FREQ_END   = 930.0;
static const float SCAN_FREQ_STEP  = 0.2;    // MHz (200 kHz)
static const int   SCAN_BIN_COUNT  = 350;
static const int   SCAN_DWELL_US   = 1000;   // microseconds per bin

struct ScanResult {
    float rssi[SCAN_BIN_COUNT];
    float peakFreq;
    float peakRSSI;
    unsigned long sweepTimeMs;
};

// 2.4 GHz sweep parameters — only populated on LR1121 boards
static const int   SCAN_24_BIN_COUNT = 100;
static const float SCAN_24_START     = 2400.0;
static const float SCAN_24_END       = 2500.0;
static const float SCAN_24_STEP      = 1.0;

struct ScanResult24 {
    float rssi[SCAN_24_BIN_COUNT];
    float peakFreq;
    float peakRSSI;
    unsigned long sweepTimeMs;
    bool valid;   // false on SX1262 boards
};

// Sub-GHz scanner — works on both SX1262 and LR1121
#ifdef BOARD_T3S3_LR1121

// Expose getRssiInst() which is protected in LR11x0 base class.
// RadioLib v7.6 only has getRSSI() (packet RSSI, useless for scanning).
// Instantaneous RSSI requires the protected getRssiInst() SPI command.
class LR1121_RSSI : public LR1121 {
public:
    using LR1121::LR1121;
    float getInstantRSSI() {
        float rssi = 0;
        getRssiInst(&rssi);
        return rssi;
    }
};

int scannerInit(LR1121_RSSI& radio);
void scannerSweep(LR1121_RSSI& radio, ScanResult& result);
void scannerSweep24(LR1121_RSSI& radio, ScanResult24& result);
bool scannerAntennaCheck(LR1121_RSSI& radio);
#else
int scannerInit(SX1262& radio);
void scannerSweep(SX1262& radio, ScanResult& result);
bool scannerAntennaCheck(SX1262& radio);
#endif

// Print helpers — radio-independent
void scannerPrintCSV(const ScanResult& result);
void scannerPrintSummary(const ScanResult& result);

#endif // RF_SCANNER_H
