#include "rf_scanner.h"
#include "board_config.h"
#include <Arduino.h>

int scannerInit(SX1262& radio) {
    // FSK mode gives us per-frequency RSSI — LoRa mode can't tune arbitrarily
    int state = radio.beginFSK();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] FSK init failed: %d\n", state);
        return state;
    }

    // LNA boost improves sensitivity ~2 dB at cost of ~1 mA extra current
    state = radio.setRxBoostedGainMode(true);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] Rx boost failed: %d\n", state);
    }

    Serial.printf("[SCAN] FSK mode ready, %d bins, %.1f–%.1f MHz\n",
                  SCAN_BIN_COUNT, SCAN_FREQ_START, SCAN_FREQ_END);
    return RADIOLIB_ERR_NONE;
}

void scannerSweep(SX1262& radio, ScanResult& result) {
    unsigned long startTime = millis();
    result.peakRSSI = -200.0;
    result.peakFreq = SCAN_FREQ_START;

    for (int i = 0; i < SCAN_BIN_COUNT; i++) {
        float freq = SCAN_FREQ_START + (i * SCAN_FREQ_STEP);
        radio.setFrequency(freq);
        delayMicroseconds(SCAN_DWELL_US);

        result.rssi[i] = radio.getRSSI(false);

        if (result.rssi[i] > result.peakRSSI) {
            result.peakRSSI = result.rssi[i];
            result.peakFreq = freq;
        }
    }

    result.sweepTimeMs = millis() - startTime;
}

// Only print bins with real signals — dumping 700 noise-floor lines per sweep
// floods serial and buries actual detections once GPS data joins in Sprint 3
static const float CSV_NOISE_FLOOR = -110.0;

void scannerPrintCSV(const ScanResult& result) {
    bool found = false;
    for (int i = 0; i < SCAN_BIN_COUNT; i++) {
        if (result.rssi[i] > CSV_NOISE_FLOOR) {
            if (!found) {
                Serial.println("freq_mhz,rssi_dbm");
                found = true;
            }
            float freq = SCAN_FREQ_START + (i * SCAN_FREQ_STEP);
            Serial.printf("%.1f,%.1f\n", freq, result.rssi[i]);
        }
    }
    if (!found) {
        Serial.println("[SCAN] No signals above noise floor");
    }
}

void scannerPrintSummary(const ScanResult& result) {
    Serial.printf("[SCAN] Peak: %.1f MHz @ %.1f dBm  (%lu ms sweep)\n",
                  result.peakFreq, result.peakRSSI, result.sweepTimeMs);
}
