#include "detection_engine.h"
#include "drone_signatures.h"
#include <Arduino.h>
#include <string.h>

// ── Tracked signal persistence ──────────────────────────────────────────────

static const int MAX_TRACKED = 8;
static const float TRACK_FREQ_TOLERANCE = 0.2;  // MHz — ±200 kHz
static const int PERSIST_THRESHOLD = 3;          // consecutive sweeps to confirm

struct TrackedSignal {
    float     frequency;
    FreqMatch match;
    int       consecutiveCount;
    unsigned long lastSeenMs;
    bool      active;
};

static TrackedSignal tracked[MAX_TRACKED];

// ── Threat state machine ────────────────────────────────────────────────────

static ThreatLevel currentThreat = THREAT_CLEAR;
static unsigned long lastThreatEventMs = 0;
static const unsigned long COOLDOWN_MS = 30000;  // decay one step per 30s of quiet

// ── Noise floor calculation ─────────────────────────────────────────────────

// Median of the RSSI array — robust to outlier peaks unlike mean or min
static float computeNoiseFloor(const float* rssi, int count) {
    // Partial selection: find the median without sorting the full array.
    // For 700 bins this simple approach is fast enough (~0.3ms).
    int below, equal;
    float candidate;

    // Test each unique-ish value as a median candidate
    for (int step = 0; step < count; step += 7) {
        candidate = rssi[step];
        below = 0;
        equal = 0;
        for (int j = 0; j < count; j++) {
            if (rssi[j] < candidate) below++;
            else if (rssi[j] == candidate) equal++;
        }
        if (below <= count / 2 && (below + equal) > count / 2) {
            return candidate;
        }
    }

    // Fallback: if sampling missed the exact median, use a safe default
    return -127.5;
}

// ── Peak extraction ─────────────────────────────────────────────────────────

static const float PEAK_THRESHOLD_DB = 12.0;  // above noise floor
static const int MAX_PEAKS = 8;

struct DetectedPeak {
    float frequency;
    float rssi;
    FreqMatch match;
};

static int extractPeaks(const ScanResult& scan, float noiseFloor, DetectedPeak* peaks) {
    float threshold = noiseFloor + PEAK_THRESHOLD_DB;
    int peakCount = 0;

    for (int i = 1; i < SCAN_BIN_COUNT - 1 && peakCount < MAX_PEAKS; i++) {
        // Local maximum above threshold
        if (scan.rssi[i] > threshold &&
            scan.rssi[i] >= scan.rssi[i - 1] &&
            scan.rssi[i] >= scan.rssi[i + 1]) {

            peaks[peakCount].frequency = SCAN_FREQ_START + (i * SCAN_FREQ_STEP);
            peaks[peakCount].rssi = scan.rssi[i];
            peaks[peakCount].match = matchFrequency(peaks[peakCount].frequency);
            peakCount++;
        }
    }

    return peakCount;
}

// ── Signal tracking ─────────────────────────────────────────────────────────

static void markAllUnseen() {
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (tracked[i].active) tracked[i].consecutiveCount--;
    }
}

static void updateTracking(const DetectedPeak* peaks, int peakCount) {
    unsigned long now = millis();

    // Try to match each peak to an existing tracked signal
    for (int p = 0; p < peakCount; p++) {
        bool matched = false;

        for (int t = 0; t < MAX_TRACKED; t++) {
            if (!tracked[t].active) continue;
            if (fabsf(peaks[p].frequency - tracked[t].frequency) < TRACK_FREQ_TOLERANCE) {
                tracked[t].consecutiveCount += 2;  // +2 to undo the -1 from markAllUnseen
                tracked[t].lastSeenMs = now;
                tracked[t].frequency = peaks[p].frequency;
                tracked[t].match = peaks[p].match;
                matched = true;
                break;
            }
        }

        if (!matched) {
            // Find an empty or expired slot
            for (int t = 0; t < MAX_TRACKED; t++) {
                if (!tracked[t].active || tracked[t].consecutiveCount <= 0) {
                    tracked[t].frequency = peaks[p].frequency;
                    tracked[t].match = peaks[p].match;
                    tracked[t].consecutiveCount = 1;
                    tracked[t].lastSeenMs = now;
                    tracked[t].active = true;
                    break;
                }
            }
        }
    }

    // Deactivate signals that have gone cold
    for (int t = 0; t < MAX_TRACKED; t++) {
        if (tracked[t].active && tracked[t].consecutiveCount <= 0) {
            tracked[t].active = false;
        }
    }
}

// ── Threat assessment ───────────────────────────────────────────────────────

static int countPersistentDrone() {
    int count = 0;
    for (int t = 0; t < MAX_TRACKED; t++) {
        if (tracked[t].active &&
            tracked[t].consecutiveCount >= PERSIST_THRESHOLD &&
            tracked[t].match.protocol != nullptr) {
            count++;
        }
    }
    return count;
}

static void emitEvent(const TrackedSignal& sig, uint8_t severity) {
    DetectionEvent event = {};
    event.source = DET_SOURCE_RF;
    event.severity = severity;
    event.frequency = sig.frequency;
    event.rssi = 0;
    event.timestamp = millis();

    if (sig.match.protocol) {
        snprintf(event.description, sizeof(event.description),
                 "%s ch%d (dev %.0fkHz)",
                 sig.match.protocol->name, sig.match.channel, sig.match.deviationKHz);
    } else {
        snprintf(event.description, sizeof(event.description),
                 "Unknown signal %.1f MHz", sig.frequency);
    }

    xQueueSend(detectionQueue, &event, 0);
}

static ThreatLevel assessThreat(const IntegrityStatus& integrity) {
    int persistentDrones = countPersistentDrone();
    bool gnssAnomaly = integrity.jammingDetected || integrity.spoofingDetected ||
                       integrity.cnoAnomalyDetected;

    ThreatLevel desired = THREAT_CLEAR;

    if (persistentDrones >= 1) {
        desired = THREAT_ADVISORY;
    }
    if (persistentDrones >= 1 && gnssAnomaly) {
        desired = THREAT_WARNING;
    }
    if (persistentDrones >= 2 || (persistentDrones >= 1 &&
        (integrity.jammingDetected || integrity.spoofingDetected))) {
        desired = THREAT_CRITICAL;
    }

    unsigned long now = millis();

    // Hysteresis: only increase by one step per sweep cycle
    if (desired > currentThreat) {
        desired = (ThreatLevel)(currentThreat + 1);
        lastThreatEventMs = now;
    }

    // Cooldown: decay one step every 30s with no triggering events
    if (desired <= currentThreat && currentThreat > THREAT_CLEAR) {
        if (now - lastThreatEventMs > COOLDOWN_MS) {
            desired = (ThreatLevel)(currentThreat - 1);
            lastThreatEventMs = now;
        } else {
            desired = currentThreat;
        }
    }

    // Emit events on threat level change
    if (desired != currentThreat) {
        for (int t = 0; t < MAX_TRACKED; t++) {
            if (tracked[t].active && tracked[t].consecutiveCount >= PERSIST_THRESHOLD) {
                emitEvent(tracked[t], desired);
            }
        }
    }

    currentThreat = desired;
    return currentThreat;
}

// ── Public API ──────────────────────────────────────────────────────────────

void detectionEngineInit() {
    memset(tracked, 0, sizeof(tracked));
    currentThreat = THREAT_CLEAR;
    lastThreatEventMs = 0;
}

ThreatLevel detectionEngineUpdate(const ScanResult& scan, const GpsData& gps,
                                  const IntegrityStatus& integrity) {
    float noiseFloor = computeNoiseFloor(scan.rssi, SCAN_BIN_COUNT);

    DetectedPeak peaks[MAX_PEAKS];
    int peakCount = extractPeaks(scan, noiseFloor, peaks);

    markAllUnseen();
    updateTracking(peaks, peakCount);

    return assessThreat(integrity);
}
