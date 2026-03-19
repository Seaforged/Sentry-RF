#ifndef RF_SCANNER_H
#define RF_SCANNER_H

#include <RadioLib.h>

// Sweep parameters
static const float SCAN_FREQ_START = 860.0;  // MHz
static const float SCAN_FREQ_END   = 930.0;  // MHz
static const float SCAN_FREQ_STEP  = 0.1;    // MHz (100 kHz)
static const int   SCAN_BIN_COUNT  = 700;
static const int   SCAN_DWELL_US   = 500;    // microseconds per bin

// Holds results from one complete sweep
struct ScanResult {
    float rssi[SCAN_BIN_COUNT];
    float peakFreq;
    float peakRSSI;
    unsigned long sweepTimeMs;
};

// One-time FSK mode setup — call after radio hardware init
int scannerInit(SX1262& radio);

// Run a single 860–930 MHz sweep, populate result struct
void scannerSweep(SX1262& radio, ScanResult& result);

// Print full CSV dump to serial (one line per bin)
void scannerPrintCSV(const ScanResult& result);

// Print single summary line with peak info
void scannerPrintSummary(const ScanResult& result);

#endif // RF_SCANNER_H
