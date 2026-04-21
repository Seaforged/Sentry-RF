#include "rf_scanner.h"
#include "board_config.h"
#include "sentry_config.h"
#include "detection_types.h"   // SERIAL_SAFE macro + serialMutex extern
#include <Arduino.h>
#include <algorithm>
#include <cmath>

// Monotonic sequence number for sweeps — incremented by every scannerSweep()
// and scannerSweep24() call. detectionEngineIngestSweep() uses this to reject
// duplicate sweeps (e.g. stale data reused between cycles).
static uint32_t sweepSeq = 0;

// ── Adaptive Noise Floor (Phase 1.1) ───────────────────────────────────────
// Dual-time-constant IIR on per-sweep median RSSI. -120 dBm at boot is a
// conservative starting point — the fast-attack branch pulls it down to the
// true environment median within 1-2 cycles, so detection is live from cycle 1.
static float adaptiveNoiseFloor = -120.0f;
static const float NF_ALPHA_FAST = 0.5f;   // median < floor  → drop quickly
static const float NF_ALPHA_SLOW = 0.05f;  // median >= floor → rise slowly

void computeAdaptiveNoiseFloor(const ScanResult& result) {
    // nth_element is O(n) vs O(n log n) sort — we only need the median, not
    // the sorted array. 350 floats on ESP32-S3 is ~50 µs, negligible vs sweep.
    float tmp[SCAN_BIN_COUNT];
    for (int i = 0; i < SCAN_BIN_COUNT; i++) tmp[i] = result.rssi[i];
    std::nth_element(tmp, tmp + SCAN_BIN_COUNT / 2, tmp + SCAN_BIN_COUNT);
    float median = tmp[SCAN_BIN_COUNT / 2];

    if (!std::isfinite(median)) return;
    if (!std::isfinite(adaptiveNoiseFloor)) adaptiveNoiseFloor = -120.0f;

    if (median < adaptiveNoiseFloor) {
        adaptiveNoiseFloor = NF_ALPHA_FAST * median + (1.0f - NF_ALPHA_FAST) * adaptiveNoiseFloor;
    } else {
        adaptiveNoiseFloor = NF_ALPHA_SLOW * median + (1.0f - NF_ALPHA_SLOW) * adaptiveNoiseFloor;
    }

    SERIAL_SAFE(Serial.printf("[NF] adaptive=%.1f dBm (median=%.1f)\n",
                              adaptiveNoiseFloor, median));
}

float getAdaptiveNoiseFloor() {
    return adaptiveNoiseFloor;
}

// Phase I: bandwidth discrimination primitive — spec in rf_scanner.h.
// Edge cases verified:
//   - peakIdx < 0 or >= numBins  → 0 (defensive; caller should avoid this)
//   - rssi[peakIdx] <= threshold → 0 (no elevated peak to count around)
//   - peakIdx == 0               → left loop bails immediately, right loop walks
//   - peakIdx == numBins - 1     → right loop bails immediately, left loop walks
//   - all bins elevated          → returns numBins
//   - no adjacent bins elevated  → returns 1 (just the peak)
int countElevatedAdjacentBins(const float* rssi, int numBins,
                              int peakIdx, float threshold) {
    if (peakIdx < 0 || peakIdx >= numBins) return 0;
    if (rssi[peakIdx] <= threshold) return 0;
    int count = 1;
    for (int i = peakIdx - 1; i >= 0          && rssi[i] > threshold; i--) count++;
    for (int i = peakIdx + 1; i <  numBins    && rssi[i] > threshold; i++) count++;
    return count;
}

// 10 test frequencies spread across 860-928 MHz for boot-time antenna check
static const float ANTENNA_TEST_FREQS[] = {
    860.0f, 867.5f, 875.0f, 882.5f, 890.0f,
    897.5f, 905.0f, 912.5f, 920.0f, 927.5f
};
static const int ANTENNA_TEST_COUNT = sizeof(ANTENNA_TEST_FREQS) / sizeof(ANTENNA_TEST_FREQS[0]);
static const int ANTENNA_TEST_DWELL_US = 5000;  // 5ms per bin — stable RSSI reads

// ── Sub-GHz scanner (SX1262 and LR1121) ────────────────────────────────────

#ifdef BOARD_T3S3_LR1121

int scannerInit(LR1121_RSSI& radio) {
    // RF switch table — LR1121 needs DIO5/DIO6 for antenna RX/TX switching
    static const uint32_t rfswitch_dio_pins[] = {
        RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
        RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
    };
    static const Module::RfSwitchMode_t rfswitch_table[] = {
        { LR11x0::MODE_STBY,  { LOW,  LOW  } },
        { LR11x0::MODE_RX,    { HIGH, LOW  } },
        { LR11x0::MODE_TX,    { LOW,  HIGH } },
        { LR11x0::MODE_TX_HP, { LOW,  HIGH } },
        { LR11x0::MODE_TX_HF, { LOW,  LOW  } },
        { LR11x0::MODE_GNSS,  { LOW,  LOW  } },
        { LR11x0::MODE_WIFI,  { LOW,  LOW  } },
        END_OF_MODE_TABLE,
    };
    radio.setRfSwitchTable(rfswitch_dio_pins, rfswitch_table);

    // LR1121::beginGFSK(freq, br, freqDev, rxBw, power, preambleLength, tcxoVoltage)
    // 7-arg signature — frequency first, TCXO voltage last (SPI init happens inside)
    // Max freq deviation 200 kHz (SX1262 allows 234.3)
    //
    // INTENTIONAL DIVERGENCE from SX1262 (which uses dev=5.0, rxBw=234.3):
    // - LR1121 freqDev=50.0: LR11x0 has a different GFSK noise floor and
    //   demod behavior; 50 kHz deviation centers the RX filter for better
    //   RSSI stability vs. SX126x's narrow 5 kHz config. Both are valid
    //   for pure RSSI sweep (we don't decode GFSK packets here).
    // - LR1121 rxBw=156.2: integrates ~33% less adjacent noise than the
    //   SX1262's 234.3 kHz bin — intentional asymmetry, affects only the
    //   absolute RSSI baseline, not relative peak detection.
    int state = radio.beginGFSK(915.0, 4.8, 50.0, 156.2, 10, 16, LR1121_TCXO_VOLTAGE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] GFSK init failed: %d\n", state);
        return state;
    }

    state = radio.setFrequency(SCAN_FREQ_START);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] Frequency prime failed: %d\n", state);
    }

    state = radio.setRxBoostedGainMode(true);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[SCAN] Rx boost failed: %d\n", state);
    }

    Serial.printf("[SCAN] GFSK mode ready (LR1121), %d bins, %.1f-%.1f MHz\n",
                  SCAN_BIN_COUNT, SCAN_FREQ_START, SCAN_FREQ_END);
    return RADIOLIB_ERR_NONE;
}

void scannerSweep(LR1121_RSSI& radio, ScanResult& result) {
    unsigned long startTime = millis();
    result.peakRSSI = -200.0;
    result.peakFreq = SCAN_FREQ_START;

    for (int i = 0; i < SCAN_BIN_COUNT; i++) {
        float freq = SCAN_FREQ_START + (i * SCAN_FREQ_STEP);
        radio.setFrequency(freq);
        radio.startReceive();       // must re-enter RX after each setFrequency
        delayMicroseconds(SCAN_DWELL_US);

        result.rssi[i] = radio.getInstantRSSI();

        if (result.rssi[i] > result.peakRSSI) {
            result.peakRSSI = result.rssi[i];
            result.peakFreq = freq;
        }
    }

    result.sweepTimeMs = millis() - startTime;
    result.seq = ++sweepSeq;
    result.valid = true;
    computeAdaptiveNoiseFloor(result);
}

bool scannerAntennaCheck(LR1121_RSSI& radio) {
    float peakRssi = -200.0f;
    float minRssi = 0.0f;
    float peakFreq = 0.0f;
    bool firstSample = true;

    for (int i = 0; i < ANTENNA_TEST_COUNT; i++) {
        if (radio.setFrequency(ANTENNA_TEST_FREQS[i]) != RADIOLIB_ERR_NONE) continue;
        radio.startReceive();  // LR1121 drops to standby after setFrequency
        delayMicroseconds(ANTENNA_TEST_DWELL_US);
        float rssi = radio.getInstantRSSI();
        if (firstSample) { minRssi = rssi; firstSample = false; }
        if (rssi > peakRssi) { peakRssi = rssi; peakFreq = ANTENNA_TEST_FREQS[i]; }
        if (rssi < minRssi) { minRssi = rssi; }
    }

    float variance = peakRssi - minRssi;
    bool absPass = (peakRssi > ANTENNA_CHECK_THRESHOLD_DBM);
    bool varPass = (variance > ANTENNA_CHECK_VARIANCE_DB);
    bool connected = absPass && varPass;

    Serial.printf("[ANTENNA] Peak %.1f dBm @ %.1f MHz, var %.1f dB (thresh %.1f dBm / %.1f dB) — %s\n",
                  peakRssi, peakFreq, variance,
                  ANTENNA_CHECK_THRESHOLD_DBM, ANTENNA_CHECK_VARIANCE_DB,
                  connected ? "OK" : "FAIL");
    return connected;
}

// 2.4 GHz sweep — LR1121 handles band switching transparently via setFrequency()
void scannerSweep24(LR1121_RSSI& radio, ScanResult24& result) {
    unsigned long startTime = millis();
    result.peakRSSI = -200.0;
    result.peakFreq = SCAN_24_START;
    result.valid = true;

    for (int i = 0; i < SCAN_24_BIN_COUNT; i++) {
        float freq = SCAN_24_START + (i * SCAN_24_STEP);
        radio.setFrequency(freq);
        radio.startReceive();
        delayMicroseconds(SCAN_DWELL_US);

        result.rssi[i] = radio.getInstantRSSI();

        if (result.rssi[i] > result.peakRSSI) {
            result.peakRSSI = result.rssi[i];
            result.peakFreq = freq;
        }
    }

    result.sweepTimeMs = millis() - startTime;
    result.seq = ++sweepSeq;
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
    //
    // INTENTIONAL DIVERGENCE from LR1121 (which uses dev=50.0, rxBw=156.2):
    // - SX1262 freqDev=5.0: narrower deviation matches SX126x's 25 kHz
    //   minimum Gaussian filter response. Field-proven in v1.5.3 at 842m.
    // - SX1262 rxBw=234.3: wider RX bandwidth compensates for the narrower
    //   deviation, keeping RSSI reads stable across the sweep.
    // Do NOT "harmonize" these values to match the LR1121 — both boards
    // have independently tuned baselines that work on their own hardware.
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
    result.seq = ++sweepSeq;
    result.valid = true;
    computeAdaptiveNoiseFloor(result);

    // Debug: check RSSI at key frequencies across the band
    SERIAL_SAFE(Serial.printf("[SCAN-DBG] 860:%.0f 880:%.0f 900:%.0f 920:%.0f\n",
                              result.rssi[0], result.rssi[100],
                              result.rssi[200], result.rssi[300]));
}

bool scannerAntennaCheck(SX1262& radio) {
    float peakRssi = -200.0f;
    float minRssi = 0.0f;
    float peakFreq = 0.0f;
    bool firstSample = true;

    for (int i = 0; i < ANTENNA_TEST_COUNT; i++) {
        if (radio.setFrequency(ANTENNA_TEST_FREQS[i]) != RADIOLIB_ERR_NONE) continue;
        radio.startReceive();
        delayMicroseconds(ANTENNA_TEST_DWELL_US);
        float rssi = radio.getRSSI(false);
        if (firstSample) { minRssi = rssi; firstSample = false; }
        if (rssi > peakRssi) { peakRssi = rssi; peakFreq = ANTENNA_TEST_FREQS[i]; }
        if (rssi < minRssi) { minRssi = rssi; }
    }
    radio.standby();

    float variance = peakRssi - minRssi;
    bool absPass = (peakRssi > ANTENNA_CHECK_THRESHOLD_DBM);
    bool varPass = (variance > ANTENNA_CHECK_VARIANCE_DB);
    bool connected = absPass && varPass;

    Serial.printf("[ANTENNA] Peak %.1f dBm @ %.1f MHz, var %.1f dB (thresh %.1f dBm / %.1f dB) — %s\n",
                  peakRssi, peakFreq, variance,
                  ANTENNA_CHECK_THRESHOLD_DBM, ANTENNA_CHECK_VARIANCE_DB,
                  connected ? "OK" : "FAIL");
    return connected;
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
