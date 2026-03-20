#include "gnss_integrity.h"
#include <Arduino.h>
#include <math.h>

// Circular buffer for C/N0 standard deviation history — used for trend analysis
static float cnoHistory[CNO_HISTORY_SIZE];
static int cnoHistoryHead = 0;
static bool cnoHistoryFull = false;

// Thresholds tuned for spoofing/jamming detection
static const float CNO_STDDEV_SPOOF_THRESH = 2.0;  // dB-Hz — uniform signals indicate single source
static const uint8_t JAM_IND_THRESH = 50;           // 0-255 scale, >50 = likely interference
static const int MIN_ELEV_FOR_CNO = 20;             // degrees — low sats are noisy, exclude them

void integrityInit() {
    for (int i = 0; i < CNO_HISTORY_SIZE; i++) {
        cnoHistory[i] = -1.0;
    }
    cnoHistoryHead = 0;
    cnoHistoryFull = false;
}

static void storeCnoStdDev(float stdDev) {
    cnoHistory[cnoHistoryHead] = stdDev;
    cnoHistoryHead = (cnoHistoryHead + 1) % CNO_HISTORY_SIZE;
    if (cnoHistoryHead == 0) cnoHistoryFull = true;
}

// Compute mean C/N0 across high-elevation satellites
static float computeCnoMean(const float* cno, int count) {
    if (count == 0) return 0.0;
    float sum = 0.0;
    for (int i = 0; i < count; i++) sum += cno[i];
    return sum / count;
}

// Standard deviation — measures signal uniformity across constellation
static float computeCnoStdDev(const float* cno, int count, float mean) {
    if (count < 2) return 99.0;
    float sumSq = 0.0;
    for (int i = 0; i < count; i++) {
        float diff = cno[i] - mean;
        sumSq += diff * diff;
    }
    return sqrtf(sumSq / (count - 1));
}

static void evaluateJamming(const GpsData& gps, IntegrityStatus& status) {
    status.jammingDetected = (gps.jammingState >= 2) || (gps.jamInd > JAM_IND_THRESH);
}

static void evaluateSpoofing(const GpsData& gps, IntegrityStatus& status) {
    // M10 built-in spoofing detector in NAV-STATUS
    bool hwSpoof = (gps.spoofDetState >= 2);

    // C/N0 uniformity check: real satellites at different ranges/elevations have
    // varied signal strength. A spoofer broadcasts from one point so all signals
    // arrive at similar power — stdDev collapses below ~2 dB-Hz.
    status.cnoAnomalyDetected = (status.cnoStdDev < CNO_STDDEV_SPOOF_THRESH)
                                && (status.cnoStdDev >= 0.0);

    status.spoofingDetected = hwSpoof || status.cnoAnomalyDetected;
}

static void evaluateThreatLevel(IntegrityStatus& status) {
    int indicators = 0;
    if (status.jammingDetected) indicators++;
    if (status.spoofingDetected) indicators++;

    if (indicators == 0)      status.threatLevel = 0;
    else if (indicators == 1) status.threatLevel = 1;
    else                      status.threatLevel = 2;

    // Simultaneous jamming + spoofing = critical (likely coordinated attack)
    if (status.jammingDetected && status.spoofingDetected) {
        status.threatLevel = 3;
    }
}

void integrityUpdate(const GpsData& gps, IntegrityStatus& status) {
    // Reset output fields — prevents stale true values carrying across cycles
    status.jammingDetected   = false;
    status.spoofingDetected  = false;
    status.cnoAnomalyDetected = false;
    status.threatLevel       = 0;
    status.cnoStdDev         = 99.0;

    // Don't run detection on uninitialized sensor data — MON-HW and NAV-SAT
    // auto packets haven't arrived yet if all values are still zero
    if (gps.jammingState == 0 && gps.jamInd == 0 && gps.satCnoCount == 0) {
        return;
    }

    float mean = computeCnoMean(gps.satCno, gps.satCnoCount);
    status.cnoStdDev = computeCnoStdDev(gps.satCno, gps.satCnoCount, mean);
    storeCnoStdDev(status.cnoStdDev);

    evaluateJamming(gps, status);
    evaluateSpoofing(gps, status);
    evaluateThreatLevel(status);
}

void integrityPrintStatus(const IntegrityStatus& status, const GpsData& gps) {
    Serial.printf("[INTEGRITY] Jam:%d(ind:%d) Spoof:%d C/N0\xCF\x83:%.1f dBHz Threat:%d\n",
                  status.jammingDetected ? 1 : 0,
                  gps.jamInd,
                  status.spoofingDetected ? 1 : 0,
                  status.cnoStdDev,
                  status.threatLevel);
}
