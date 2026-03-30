#include "rf_scanner.h"
#include "board_config.h"
#include <Arduino.h>

// ── Sub-GHz scanner (SX1262 and LR1121) ────────────────────────────────────

#ifdef BOARD_T3S3_LR1121

int scannerInit(LR1121& radio) {
    // LR1121 uses GFSK, not FSK — different RadioLib method name, same result
    int state = radio.beginGFSK(4.8, 5.0, 234.3, 16, 1.8);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] GFSK init failed: %d\n", state);
        return state;
    }

    state = radio.setRxBoostedGainMode(true);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] Rx boost failed: %d\n", state);
    }

    Serial.printf("[SCAN] GFSK mode ready (LR1121), %d bins, %.1f–%.1f MHz\n",
                  SCAN_BIN_COUNT, SCAN_FREQ_START, SCAN_FREQ_END);
    return RADIOLIB_ERR_NONE;
}

void scannerSweep(LR1121& radio, ScanResult& result) {
    unsigned long startTime = millis();
    result.peakRSSI = -200.0;
    result.peakFreq = SCAN_FREQ_START;

    for (int i = 0; i < SCAN_BIN_COUNT; i++) {
        float freq = SCAN_FREQ_START + (i * SCAN_FREQ_STEP);
        radio.setFrequency(freq);
        delayMicroseconds(SCAN_DWELL_US);

        // LR11x0 getRSSI() takes no arguments (always instantaneous)
        result.rssi[i] = radio.getRSSI();

        if (result.rssi[i] > result.peakRSSI) {
            result.peakRSSI = result.rssi[i];
            result.peakFreq = freq;
        }
    }

    result.sweepTimeMs = millis() - startTime;
}

// 2.4 GHz sweep — LR1121 handles band switching transparently via setFrequency()
void scannerSweep24(LR1121& radio, ScanResult24& result) {
    unsigned long startTime = millis();
    result.peakRSSI = -200.0;
    result.peakFreq = SCAN_24_START;
    result.valid = true;

    for (int i = 0; i < SCAN_24_BIN_COUNT; i++) {
        float freq = SCAN_24_START + (i * SCAN_24_STEP);
        radio.setFrequency(freq);
        delayMicroseconds(SCAN_DWELL_US);

        result.rssi[i] = radio.getRSSI();

        if (result.rssi[i] > result.peakRSSI) {
            result.peakRSSI = result.rssi[i];
            result.peakFreq = freq;
        }
    }

    result.sweepTimeMs = millis() - startTime;
}

#else // SX1262 boards

int scannerInit(SX1262& radio) {
    // Explicitly reset the SX1262 via RESET pin for clean state after crash reboots
    pinMode(PIN_LORA_RST, OUTPUT);
    digitalWrite(PIN_LORA_RST, LOW);
    delay(10);
    digitalWrite(PIN_LORA_RST, HIGH);
    delay(20);

    // Init in FSK mode — proven to work, used for RSSI sweeps.
    // CAD scans switch to LoRa packet type via low-level SPI command.
    int state = radio.beginFSK(915.0, 4.8, 5.0, 234.3, 10, 16);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] FSK init failed: %d\n", state);
        return state;
    }

    state = radio.setFrequency(860.0);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] Frequency prime failed: %d\n", state);
        return state;
    }

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
        radio.startReceive();
        delayMicroseconds(SCAN_DWELL_US);

        result.rssi[i] = radio.getRSSI(false);

        if (result.rssi[i] > result.peakRSSI) {
            result.peakRSSI = result.rssi[i];
            result.peakFreq = freq;
        }
    }

    // Return to standby to cleanly exit RX mode
    radio.standby();

    result.sweepTimeMs = millis() - startTime;

    // Debug: check RSSI at key frequencies across the band
    Serial.printf("[SCAN-DBG] 860:%.0f 880:%.0f 900:%.0f 920:%.0f\n",
                  result.rssi[0], result.rssi[100], result.rssi[200], result.rssi[300]);
}

#endif // BOARD_T3S3_LR1121

// ── Print helpers (radio-independent) ───────────────────────────────────────

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
