#include "gnss_integrity.h"
#include "sentry_config.h"
#include <Arduino.h>
#include <math.h>

static float cnoHistory[CNO_HISTORY_SIZE];
static int cnoHistoryHead = 0;
static bool cnoHistoryFull = false;

// CNO_STDDEV_SPOOF_THRESH, MIN_ELEV_FOR_CNO from sentry_config.h
static const uint8_t JAM_IND_THRESH = 50;  // 0-255 scale, >50 = likely interference

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

// Position jump: sudden >100 m teleport with a tight hAcc is a strong spoofing
// indicator. Real drift stays small; a spoofer flipping its simulated position
// produces a hard step while the receiver still reports high confidence.
// Static locals persist the previous valid fix across calls — no struct field
// needed since state only matters inside this module.
static void evaluatePositionJump(const GpsData& gps, IntegrityStatus& status) {
    if (gps.fixType < 3) return;

    float hAccM = gps.hAccMM / 1000.0f;
    if (hAccM >= GNSS_POSITION_JUMP_HACC_MAX_M) return;

    float lat = gps.latDeg7 / 1e7f;
    float lon = gps.lonDeg7 / 1e7f;

    static bool  havePrev = false;
    static float prevLat  = 0.0f;
    static float prevLon  = 0.0f;

    if (havePrev) {
        const float degToRad = (float)(M_PI / 180.0);
        float dLat = (lat - prevLat) * 111320.0f;                         // meters per degree
        float dLon = (lon - prevLon) * 111320.0f * cosf(lat * degToRad);
        float distM = sqrtf(dLat * dLat + dLon * dLon);
        if (distM > GNSS_POSITION_JUMP_THRESHOLD_M) {
            status.positionJumpDetected = true;
            Serial.printf("[GNSS] position jump %.0fm (hAcc=%.1fm) — SPOOFING INDICATOR\n",
                          distM, hAccM);
        }
    }

    prevLat  = lat;
    prevLon  = lon;
    havePrev = true;
}

static void evaluateSpoofing(const GpsData& gps, IntegrityStatus& status) {
    // M10 built-in spoofing detector in NAV-STATUS
    bool hwSpoof = (gps.spoofDetState >= 2);

    // C/N0 uniformity check: real satellites at different ranges/elevations have
    // varied signal strength. A spoofer broadcasts from one point so all signals
    // arrive at similar power — stdDev collapses below ~2 dB-Hz.
    status.cnoAnomalyDetected = (status.cnoStdDev < CNO_STDDEV_SPOOF_THRESH)
                                && (status.cnoStdDev >= 0.0);

    evaluatePositionJump(gps, status);

    status.spoofingDetected = hwSpoof || status.cnoAnomalyDetected
                              || status.positionJumpDetected;
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
    status.jammingDetected    = false;
    status.spoofingDetected   = false;
    status.cnoAnomalyDetected = false;
    status.positionJumpDetected = false;
    status.threatLevel        = 0;
    status.cnoStdDev          = 99.0;

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
