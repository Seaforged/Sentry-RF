#include "gnss_integrity.h"
#include "sentry_config.h"
#include "detection_types.h"
#include "alert_handler.h"   // Issue 8: alertQueueDropInc()
#include "cad_scanner.h"     // Issue 1: warmup corroboration
#include <Arduino.h>
#include <math.h>
#include <string.h>

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

// Standard deviation - measures signal uniformity across constellation
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
// Static locals persist the previous valid fix across calls - no struct field
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
            SERIAL_SAFE(Serial.printf("[GNSS] position jump %.0fm (hAcc=%.1fm) - SPOOFING INDICATOR\n",
                                      distM, hAccM));
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
    // arrive at similar power - stdDev collapses below ~2 dB-Hz.
    //
    // Uniformity check suppressed when GPS_MIN_CNO < 15 - indoor attenuated
    // signals cluster naturally and produce false positives.
    if (GPS_MIN_CNO < 15) {
        status.cnoAnomalyDetected = false;
    } else {
        status.cnoAnomalyDetected = (status.cnoStdDev < CNO_STDDEV_SPOOF_THRESH)
                                    && (status.cnoStdDev >= 0.0);
    }

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

// Track the last emitted GNSS threat so we publish on transitions, not every
// sample. Rising transitions drive new alerts; falling transitions and clear
// events let alertTask retire standalone GNSS state instead of leaving it
// stuck until some unrelated RF event arrives.
static uint8_t lastEmittedThreatLevel = 0;

static void emitGnssDetectionEvent(uint8_t level, const IntegrityStatus& status) {
    if (detectionQueue == nullptr) return;
    DetectionEvent event = {};
    event.source    = DET_SOURCE_GNSS;
    event.severity  = level;
    event.frequency = 0.0f;                 // GNSS events have no RF freq
    event.rssi      = (float)status.cnoStdDev;
    event.timestamp = millis();
    // Description lists the specific indicator(s) that fired. Multiple
    // indicators can coexist (e.g. jam + spoof). CLEAR uses an explicit tag
    // so the alert task can log and retire GNSS-only state coherently.
    const char* tag =
        (level == THREAT_CLEAR)   ? "clear"
      : status.jammingDetected    ? "jam"
      : status.positionJumpDetected ? "position-jump"
      : status.cnoAnomalyDetected ? "C/N0 uniformity"
      : status.spoofingDetected   ? "spoof"
      : "integrity";
    snprintf(event.description, sizeof(event.description),
             "GNSS: %s (level %u)", tag, (unsigned)level);
    if (xQueueSend(detectionQueue, &event, pdMS_TO_TICKS(5)) != pdTRUE) {
        alertQueueDropInc();
    }
}

void integrityUpdate(const GpsData& gps, IntegrityStatus& status) {
    // Reset output fields - prevents stale true values carrying across cycles
    status.jammingDetected    = false;
    status.spoofingDetected   = false;
    status.cnoAnomalyDetected = false;
    status.positionJumpDetected = false;
    status.threatLevel        = 0;
    status.cnoStdDev          = 99.0;

    // Don't run detection on uninitialized sensor data - MON-HW and NAV-SAT
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

    // Publish standalone GNSS events on every level transition so alertTask
    // can both raise and retire GNSS-only alert state. The candidate engine
    // still attaches GNSS evidence for score boosting when RF is correlated
    // (see gnssBoost comment in detection_engine.cpp) - this queue path is
    // additive, not a replacement.
    if (status.threatLevel != lastEmittedThreatLevel) {
        emitGnssDetectionEvent(status.threatLevel, status);
        // Issue 1: GNSS anomaly during CAD warmup disqualifies all pending
        // ambient taps - a jam/spoof event at boot is almost certainly
        // correlated with the arriving drone's RC signal.
        if (status.threatLevel > lastEmittedThreatLevel &&
            cadWarmupInProgress()) {
            markPendingAmbientCorroboration(0.0f);
        }
    }
    lastEmittedThreatLevel = status.threatLevel;
}

void integrityPrintStatus(const IntegrityStatus& status, const GpsData& gps) {
    Serial.printf("[INTEGRITY] Jam:%d(ind:%d) Spoof:%d C/N0\xCF\x83:%.1f dBHz Threat:%d\n",
                  status.jammingDetected ? 1 : 0,
                  gps.jamInd,
                  status.spoofingDetected ? 1 : 0,
                  status.cnoStdDev,
                  status.threatLevel);
}
