#include "detection_engine.h"
#include "drone_signatures.h"
#include "ambient_filter.h"
#include "cad_scanner.h"
#include "sentry_config.h"
#include <Arduino.h>
#include <string.h>

// ── Tracked signal persistence ──────────────────────────────────────────────
// Constants from sentry_config.h: MAX_TRACKED, TRACK_FREQ_TOLERANCE, PERSIST_THRESHOLD

struct TrackedSignal {
    float     frequency;
    FreqMatch match;
    int       consecutiveCount;
    unsigned long lastSeenMs;
    bool      active;
};

static TrackedSignal tracked[MAX_TRACKED];      // sub-GHz
static TrackedSignal tracked24[MAX_TRACKED];    // 2.4 GHz

// ── Protocol-level persistence (catches FHSS) ─────────────────────────────

// MAX_PROTO_TRACKED from sentry_config.h

struct ProtoTracker {
    const DroneProtocol* protocol;
    int   consecutiveCount;
    bool  seenThisSweep;
    bool  active;
    float minFreqSeen;
    float maxFreqSeen;
};

static ProtoTracker protoTracked[MAX_PROTO_TRACKED];

// ── CAD/FSK detection counts (per sweep cycle) ───────────────────────────

static int cadDetectionsThisCycle = 0;
static int fskDetectionsThisCycle = 0;
static int strongPendingCadThisCycle = 0;
static int totalActiveTapsThisCycle = 0;
static int diversityCountThisCycle = 0;
static int persistentDiversityThisCycle = 0;
static int diversityVelocityThisCycle = 0;
static int sustainedDiversityCyclesThisCycle = 0;

// ── Band energy trending (902-928 MHz) ──────────────────────────────────
// Tracks average RSSI across US band to detect aggregate FHSS energy rise.
// FHSS drones spread energy across 80 channels — individual peaks come and
// go, but the band average rises measurably.

// BAND_ENERGY_HISTORY, BAND_ENERGY_THRESH_DB from sentry_config.h

// US band 902-928 MHz
static float bandEnergyHistoryUS[BAND_ENERGY_HISTORY];
static int bandEnergyIdxUS = 0;
static int bandEnergySamplesUS = 0;
static bool bandEnergyElevatedUS = false;

// EU band 860-870 MHz
static float bandEnergyHistoryEU[BAND_ENERGY_HISTORY];
static int bandEnergyIdxEU = 0;
static int bandEnergySamplesEU = 0;
static bool bandEnergyElevatedEU = false;

// Combined flag
static bool bandEnergyElevated = false;

static void updateBandEnergyRegion(const float* rssi, int startBin, int endBin,
                                    float* history, int& idx, int& samples, bool& elevated) {
    float sum = 0;
    for (int i = startBin; i < endBin; i++) sum += rssi[i];
    float currentAvg = sum / (endBin - startBin);

    float baseline = currentAvg;
    if (samples > 0) {
        float histSum = 0;
        int count = (samples < BAND_ENERGY_HISTORY) ? samples : BAND_ENERGY_HISTORY;
        for (int i = 0; i < count; i++) histSum += history[i];
        baseline = histSum / count;
    }

    history[idx] = currentAvg;
    idx = (idx + 1) % BAND_ENERGY_HISTORY;
    if (samples < BAND_ENERGY_HISTORY) samples++;

    elevated = (samples >= 3) && (currentAvg > baseline + BAND_ENERGY_THRESH_DB);
}

static void updateBandEnergy(const float* rssi) {
    // US band: 902-928 MHz = bins 210-340
    updateBandEnergyRegion(rssi, 210, 340,
        bandEnergyHistoryUS, bandEnergyIdxUS, bandEnergySamplesUS, bandEnergyElevatedUS);
    // EU band: 863-870 MHz = bins 15-50 (EU ISM starts at 863 MHz)
    updateBandEnergyRegion(rssi, 15, 50,
        bandEnergyHistoryEU, bandEnergyIdxEU, bandEnergySamplesEU, bandEnergyElevatedEU);
    // Combined
    bandEnergyElevated = bandEnergyElevatedUS || bandEnergyElevatedEU;
}

// ── Threat state machine ────────────────────────────────────────────────────

static ThreatLevel currentThreat = THREAT_CLEAR;
static unsigned long lastThreatEventMs = 0;
static uint32_t lastDetectionMs = 0;
// COOLDOWN_MS from sentry_config.h
static int cleanCycleCount = 0;
static unsigned long clearSinceMs = 0;

// Captured at the top of detectionEngineUpdate() so assessThreat() can gate
// RSSI-derived confirm score components on whether the sweep is fresh.
static bool freshRssiThisCycle = false;
static int lastFastScore = 0;
static int lastConfirmScore = 0;

// ── Noise floor calculation ─────────────────────────────────────────────────

static float computeNoiseFloor(const float* rssi, int count) {
    int below, equal;
    float candidate;

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
    return -127.5;
}

// ── Peak extraction (strongest-peak selection) ──────────────────────────────

// PEAK_THRESHOLD_DB, PEAK_ABS_FLOOR_DBM, MAX_PEAKS from sentry_config.h

struct DetectedPeak {
    float frequency;
    float rssi;
    FreqMatch match;
};

static int extractPeaks(const ScanResult& scan, float noiseFloor, DetectedPeak* peaks) {
    float relThresh = noiseFloor + PEAK_THRESHOLD_DB;
    float threshold = (relThresh > PEAK_ABS_FLOOR_DBM) ? relThresh : PEAK_ABS_FLOOR_DBM;
    int peakCount = 0;

    for (int i = 1; i < SCAN_BIN_COUNT - 1; i++) {
        if (scan.rssi[i] > threshold &&
            scan.rssi[i] >= scan.rssi[i - 1] &&
            scan.rssi[i] >= scan.rssi[i + 1]) {

            if (ambientFilterIsAmbient(i)) continue;

            // Spectral width classifier: skip broadband (>1.2 MHz) energy
            float peakRSSI = scan.rssi[i];
            int narrowWidth = 1;
            for (int j = i - 1; j >= 0 && (peakRSSI - scan.rssi[j]) < 6.0f; j--)
                narrowWidth++;
            for (int j = i + 1; j < SCAN_BIN_COUNT && (peakRSSI - scan.rssi[j]) < 6.0f; j++)
                narrowWidth++;
            if (narrowWidth > 6) continue;

            float freq = SCAN_FREQ_START + (i * SCAN_FREQ_STEP);

            if (peakCount < MAX_PEAKS) {
                peaks[peakCount].frequency = freq;
                peaks[peakCount].rssi = peakRSSI;
                peaks[peakCount].match = matchFrequency(freq);
                peakCount++;
            } else {
                int weakest = 0;
                for (int p = 1; p < MAX_PEAKS; p++) {
                    if (peaks[p].rssi < peaks[weakest].rssi) weakest = p;
                }
                if (peakRSSI > peaks[weakest].rssi) {
                    peaks[weakest].frequency = freq;
                    peaks[weakest].rssi = peakRSSI;
                    peaks[weakest].match = matchFrequency(freq);
                }
            }
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

    for (int p = 0; p < peakCount; p++) {
        bool matched = false;

        for (int t = 0; t < MAX_TRACKED; t++) {
            if (!tracked[t].active) continue;
            if (fabsf(peaks[p].frequency - tracked[t].frequency) < TRACK_FREQ_TOLERANCE) {
                tracked[t].consecutiveCount += 2;
                tracked[t].lastSeenMs = now;
                tracked[t].frequency = peaks[p].frequency;
                tracked[t].match = peaks[p].match;
                matched = true;
                break;
            }
        }

        if (!matched) {
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

    for (int t = 0; t < MAX_TRACKED; t++) {
        if (tracked[t].active && tracked[t].consecutiveCount <= 0) {
            tracked[t].active = false;
        }
    }
}

// ── Protocol-level tracking ─────────────────────────────────────────────────

static void markAllProtoUnseen() {
    for (int i = 0; i < MAX_PROTO_TRACKED; i++) {
        if (protoTracked[i].active) {
            protoTracked[i].consecutiveCount--;
            protoTracked[i].seenThisSweep = false;
        }
    }
}

static void markProtoSeen(const DroneProtocol* proto, float freq) {
    if (proto == nullptr) return;

    for (int i = 0; i < MAX_PROTO_TRACKED; i++) {
        if (protoTracked[i].active && protoTracked[i].protocol == proto) {
            if (freq < protoTracked[i].minFreqSeen) protoTracked[i].minFreqSeen = freq;
            if (freq > protoTracked[i].maxFreqSeen) protoTracked[i].maxFreqSeen = freq;
            if (!protoTracked[i].seenThisSweep) {
                protoTracked[i].consecutiveCount += 2;
                protoTracked[i].seenThisSweep = true;
            }
            return;
        }
    }

    for (int i = 0; i < MAX_PROTO_TRACKED; i++) {
        if (!protoTracked[i].active || protoTracked[i].consecutiveCount <= 0) {
            protoTracked[i].protocol = proto;
            protoTracked[i].consecutiveCount = 1;
            protoTracked[i].seenThisSweep = true;
            protoTracked[i].active = true;
            protoTracked[i].minFreqSeen = freq;
            protoTracked[i].maxFreqSeen = freq;
            return;
        }
    }
}

static void cleanupProtoTracking() {
    for (int i = 0; i < MAX_PROTO_TRACKED; i++) {
        if (protoTracked[i].active && protoTracked[i].consecutiveCount <= 0) {
            protoTracked[i].active = false;
            protoTracked[i].minFreqSeen = 0;
            protoTracked[i].maxFreqSeen = 0;
        }
    }
}

static void updateProtoTracking(const DetectedPeak* peaks, int peakCount) {
    markAllProtoUnseen();
    for (int p = 0; p < peakCount; p++) {
        markProtoSeen(peaks[p].match.protocol, peaks[p].frequency);
    }
    cleanupProtoTracking();
}

static int countPersistentProtocol(bool want24GHz) {
    int count = 0;
    for (int i = 0; i < MAX_PROTO_TRACKED; i++) {
        if (protoTracked[i].active &&
            protoTracked[i].consecutiveCount >= PERSIST_THRESHOLD &&
            protoTracked[i].protocol != nullptr &&
            protoTracked[i].protocol->is24GHz == want24GHz) {
            count++;
        }
    }
    return count;
}

// Count protocols seen exclusively in the 902-928 MHz US band (no EU overlap)
static int countPersistentProtocolUS() {
    int count = 0;
    for (int i = 0; i < MAX_PROTO_TRACKED; i++) {
        if (protoTracked[i].active &&
            protoTracked[i].consecutiveCount >= PERSIST_THRESHOLD &&
            protoTracked[i].protocol != nullptr &&
            !protoTracked[i].protocol->is24GHz &&
            protoTracked[i].minFreqSeen >= 902.0f) {
            count++;
        }
    }
    return count;
}

// ── 2.4 GHz peak extraction ────────────────────────────────────────────────

static int extractPeaks24(const ScanResult24& scan, float noiseFloor, DetectedPeak* peaks) {
    float threshold = noiseFloor + PEAK_THRESHOLD_DB;
    int peakCount = 0;

    for (int i = 1; i < SCAN_24_BIN_COUNT - 1 && peakCount < MAX_PEAKS; i++) {
        if (scan.rssi[i] > threshold &&
            scan.rssi[i] >= scan.rssi[i - 1] &&
            scan.rssi[i] >= scan.rssi[i + 1]) {

            float freq = SCAN_24_START + (i * SCAN_24_STEP);
            if (isWiFiChannel(freq)) continue;

            peaks[peakCount].frequency = freq;
            peaks[peakCount].rssi = scan.rssi[i];
            peaks[peakCount].match = matchFrequency24(freq);
            peakCount++;
        }
    }

    return peakCount;
}

// ── Threat assessment (confidence-based with CAD/FSK) ───────────────────────

// Frequency band classification for RSSI-only detections
static bool isInUSBand(float freq) { return freq >= 902.0f && freq <= 928.0f; }

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

// Count persistent drones in the US non-overlap band only (MEDIUM confidence)
static int countPersistentDroneUS() {
    int count = 0;
    for (int t = 0; t < MAX_TRACKED; t++) {
        if (tracked[t].active &&
            tracked[t].consecutiveCount >= PERSIST_THRESHOLD &&
            tracked[t].match.protocol != nullptr &&
            isInUSBand(tracked[t].frequency)) {
            count++;
        }
    }
    return count;
}

static int countPersistentDrone24() {
    int count = 0;
    for (int t = 0; t < MAX_TRACKED; t++) {
        if (tracked24[t].active &&
            tracked24[t].consecutiveCount >= PERSIST_THRESHOLD &&
            tracked24[t].match.protocol != nullptr) {
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
        // "(dev %.0fkHz)" removed: FreqMatch.deviationKHz is actually the
        // channel-center OFFSET in kHz (how close the signal is to the
        // nearest channel center), not the modulation frequency deviation
        // that operators might expect. For channel-aligned signals it
        // reads as "0kHz" which looked like broken data. Protocol + channel
        // alone is the actionable info; the real frequency is shown in
        // the alert_handler suffix.
        snprintf(event.description, sizeof(event.description),
                 "%s ch%d",
                 sig.match.protocol->name, sig.match.channel);
    } else {
        snprintf(event.description, sizeof(event.description),
                 "Unknown signal %.1f MHz", sig.frequency);
    }

    xQueueSend(detectionQueue, &event, 0);
}

static void emitProtoEvent(const ProtoTracker& pt, uint8_t severity) {
    DetectionEvent event = {};
    event.source = DET_SOURCE_RF;
    event.severity = severity;
    event.frequency = 0;
    event.rssi = 0;
    event.timestamp = millis();
    snprintf(event.description, sizeof(event.description),
             "%s (FHSS protocol)", pt.protocol->name);
    xQueueSend(detectionQueue, &event, 0);
}

static void emitCadEvent(float freq, uint8_t sf, uint8_t severity) {
    // CAD detection is multi-channel — no single representative frequency
    // at the event-emit site, so freq is typically 0. Description omits
    // the "@ X MHz" suffix to avoid printing "@ 0.0 MHz" in alert logs.
    DetectionEvent event = {};
    event.source = DET_SOURCE_RF;
    event.severity = severity;
    event.frequency = freq;
    event.rssi = 0;
    event.timestamp = millis();
    snprintf(event.description, sizeof(event.description),
             "CAD LoRa SF%d/BW500", sf);
    xQueueSend(detectionQueue, &event, 0);
}

static void emitFskEvent(float freq, uint8_t severity) {
    // FSK detection is also multi-channel — see emitCadEvent note above.
    DetectionEvent event = {};
    event.source = DET_SOURCE_RF;
    event.severity = severity;
    event.frequency = freq;
    event.rssi = 0;
    event.timestamp = millis();
    snprintf(event.description, sizeof(event.description),
             "FSK 85k1 preamble");
    xQueueSend(detectionQueue, &event, 0);
}

static int lastScore = 0;  // exposed for serial output

static ThreatLevel assessThreat(const IntegrityStatus& integrity) {
    // ── Two-layer scoring (v1.8.0) ───────────────────────────────────
    // FAST score: CAD-only evidence. Updates every cycle. Drives ADVISORY
    // immediately from cycle 1 — no warmup gate needed.
    //
    // CAD/FSK weights scale per-tap (10/tap CAD, 15/tap FSK) so a single
    // infrastructure tap contributes 10 points (ADVISORY only) while 4+
    // confirmed drone taps reach the 40-point WARNING floor.
    int fastScore = 0;
    int cadFastPts = cadDetectionsThisCycle * 10;
    if (cadFastPts > 40) cadFastPts = 40;
    int fskFastPts = fskDetectionsThisCycle * 15;
    if (fskFastPts > 30) fskFastPts = 30;
    fastScore += cadFastPts;
    fastScore += fskFastPts;
    if (persistentDiversityThisCycle > 0)    fastScore += WEIGHT_FAST_PERSISTENT_DIV;
    if (diversityVelocityThisCycle >= DIVERSITY_VELOCITY_FHSS_MIN)
                                             fastScore += WEIGHT_FAST_FHSS_VELOCITY;

    // CONFIRM score: RSSI + protocol + RID + GNSS. Independent corroboration
    // gates promotion to WARNING/CRITICAL. RSSI-derived components only fire
    // on fresh sweeps — avoids stale-data contributions on non-sweep cycles.
    int confirmScore = 0;
    if (freshRssiThisCycle) {
        int freqUS = countPersistentDroneUS();
        int protoUS = countPersistentProtocolUS();
        int freqSubGHz = countPersistentDrone();
        int protoSubGHz = countPersistentProtocol(false);
        int proto24GHz = countPersistentProtocol(true);
        int freq24GHz = countPersistentDrone24();
        bool anyPeak = (freqUS + freqSubGHz + freq24GHz) > 0;
        bool anyProto = (protoUS + protoSubGHz + proto24GHz) > 0;
        if (anyPeak)  confirmScore += WEIGHT_CONFIRM_PEAK;
        if (anyProto) confirmScore += WEIGHT_CONFIRM_PROTO;
        if (bandEnergyElevated) confirmScore += WEIGHT_CONFIRM_BAND_ENERGY;
    }

    // WiFi Remote ID — independent evidence, not RSSI-gated
    bool ridDetected = false;
    unsigned long ridLastMs = 0;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5))) {
        ridDetected = systemState.remoteIdDetected;
        ridLastMs = systemState.remoteIdLastMs;
        xSemaphoreGive(stateMutex);
    }
    if (ridDetected && (millis() - ridLastMs) < 10000) {
        confirmScore += WEIGHT_CONFIRM_REMOTE_ID;
    }

    // GNSS anomaly — cross-domain independent evidence
    bool gnssAnomaly = integrity.jammingDetected || integrity.spoofingDetected ||
                       integrity.cnoAnomalyDetected;
    if (gnssAnomaly) confirmScore += WEIGHT_CONFIRM_GNSS_TEMPORAL;

    lastFastScore = fastScore;
    lastConfirmScore = confirmScore;
    lastScore = fastScore + confirmScore;  // legacy display field

    // ── Policy: fast produces ADVISORY, confirm promotes to WARNING/CRITICAL
    ThreatLevel desired = THREAT_CLEAR;
    if (fastScore >= FAST_WARNING_THRESH && confirmScore >= CONFIRM_CRITICAL_THRESH) {
        desired = THREAT_CRITICAL;
    } else if (fastScore >= FAST_WARNING_THRESH && confirmScore >= CONFIRM_WARNING_THRESH) {
        desired = THREAT_WARNING;
    } else if (fastScore >= FAST_ADVISORY_THRESH) {
        desired = THREAT_ADVISORY;
    } else if (confirmScore >= CONFIRM_ADVISORY_THRESH) {
        // RID-only path: Remote ID detection alone is enough for ADVISORY
        desired = THREAT_ADVISORY;
    }

    if (desired >= THREAT_ADVISORY) lastDetectionMs = millis();

    // Post-warmup housekeeping: clear persistence counters once the filters
    // finish learning. The two-score system is safe from cycle 1, so no
    // artificial warmup cap on threat level — the confirm path IS the gate.
    static bool postWarmupReset = false;
    bool warmupActive = !ambientFilterReady() || !cadWarmupComplete();
    if (warmupActive) {
        postWarmupReset = false;
    } else if (!postWarmupReset) {
        postWarmupReset = true;
        for (int t = 0; t < MAX_TRACKED; t++) { tracked[t].active = false; }
        for (int t = 0; t < MAX_TRACKED; t++) { tracked24[t].active = false; }
        for (int i = 0; i < MAX_PROTO_TRACKED; i++) { protoTracked[i].active = false; }
        memset(bandEnergyHistoryUS, 0, sizeof(bandEnergyHistoryUS));
        memset(bandEnergyHistoryEU, 0, sizeof(bandEnergyHistoryEU));
        bandEnergyIdxUS = 0; bandEnergySamplesUS = 0; bandEnergyElevatedUS = false;
        bandEnergyIdxEU = 0; bandEnergySamplesEU = 0; bandEnergyElevatedEU = false;
        bandEnergyElevated = false;
        cleanCycleCount = 0;
        clearSinceMs = 0;
        currentThreat = THREAT_CLEAR;
        desired = THREAT_CLEAR;
        lastThreatEventMs = millis();
    }

    unsigned long now = millis();

    // Escalation: jump directly to the level evidence supports (fast up).
    // This replaces the old one-step-per-cycle limiter which delayed
    // legitimate detections by N cycles during escalation.
    if (desired > currentThreat) {
        currentThreat = desired;
        lastThreatEventMs = now;
    } else if (desired == currentThreat && currentThreat > THREAT_CLEAR) {
        // Steady state at evidence-supported level — reset cooldown timer
        // so we don't decay while evidence is still present.
        lastThreatEventMs = now;
    }

    // Cooldown: decay one step every COOLDOWN_MS when evidence weakens
    if (desired < currentThreat && currentThreat > THREAT_CLEAR) {
        if (now - lastThreatEventMs > COOLDOWN_MS) {
            desired = (ThreatLevel)(currentThreat - 1);
            lastThreatEventMs = now;
        } else {
            desired = currentThreat;
        }
    }

    // Rapid-clear: if ALL detection sources are silent for N consecutive cycles,
    // skip cooldown decay and jump directly to CLEAR. Gives operator fast
    // feedback that the threat has left.
    // Rapid-clear: explicit check that ALL detection sources are silent.
    // Score-based check was inconsistent (EU RSSI at 5 pts passed, US at 10 didn't).
    bool allClean = (diversityCountThisCycle == 0)
                    && (cadDetectionsThisCycle == 0)
                    && (fskDetectionsThisCycle == 0)
                    && !bandEnergyElevated;

    if (allClean) {
        cleanCycleCount++;
        if (cleanCycleCount >= RAPID_CLEAR_CLEAN_CYCLES && currentThreat >= THREAT_WARNING) {
            Serial.printf("[RAPID-CLEAR] %d clean cycles — forcing CLEAR\n", cleanCycleCount);
            desired = THREAT_CLEAR;
            cleanCycleCount = 0;
            lastThreatEventMs = now;
        }
    } else {
        cleanCycleCount = 0;
    }

    // Sustained-CLEAR diversity reset: if CLEAR for 60 continuous seconds,
    // reset the diversity tracker to prevent slow drift on LoRa-rich bench.
    if (desired == THREAT_CLEAR && currentThreat == THREAT_CLEAR) {
        if (clearSinceMs == 0) clearSinceMs = now;
        else if ((now - clearSinceMs) > 60000) {
            resetDiversityTracker();
            clearSinceMs = now;
        }
    } else {
        clearSinceMs = 0;
    }

    // Emit events on change
    if (desired != currentThreat) {
        // CAD/FSK events (emit when threat at WARNING+)
        if (desired >= THREAT_WARNING) {
            if (cadDetectionsThisCycle > 0)
                emitCadEvent(0, 6, desired);
            if (fskDetectionsThisCycle > 0)
                emitFskEvent(0, desired);
        }
        for (int t = 0; t < MAX_TRACKED; t++) {
            if (tracked[t].active && tracked[t].consecutiveCount >= PERSIST_THRESHOLD) {
                emitEvent(tracked[t], desired);
            }
        }
        for (int i = 0; i < MAX_PROTO_TRACKED; i++) {
            if (protoTracked[i].active &&
                protoTracked[i].consecutiveCount >= PERSIST_THRESHOLD &&
                protoTracked[i].protocol != nullptr) {
                emitProtoEvent(protoTracked[i], desired);
            }
        }
    }

    currentThreat = desired;
    return currentThreat;
}

// ── Public API ──────────────────────────────────────────────────────────────

void detectionEngineInit() {
    memset(tracked, 0, sizeof(tracked));
    memset(tracked24, 0, sizeof(tracked24));
    memset(protoTracked, 0, sizeof(protoTracked));
    cadDetectionsThisCycle = 0;
    fskDetectionsThisCycle = 0;
    strongPendingCadThisCycle = 0;
    totalActiveTapsThisCycle = 0;
    diversityCountThisCycle = 0;
    persistentDiversityThisCycle = 0;
    diversityVelocityThisCycle = 0;
    sustainedDiversityCyclesThisCycle = 0;
    memset(bandEnergyHistoryUS, 0, sizeof(bandEnergyHistoryUS));
    memset(bandEnergyHistoryEU, 0, sizeof(bandEnergyHistoryEU));
    bandEnergyIdxUS = 0; bandEnergySamplesUS = 0; bandEnergyElevatedUS = false;
    bandEnergyIdxEU = 0; bandEnergySamplesEU = 0; bandEnergyElevatedEU = false;
    bandEnergyElevated = false;
    clearSinceMs = 0;
    currentThreat = THREAT_CLEAR;
    lastThreatEventMs = 0;
    ambientFilterInit();
}

int detectionEngineGetScore() { return (lastScore > 100) ? 100 : lastScore; }
int detectionEngineGetFastScore() { return lastFastScore; }
int detectionEngineGetConfirmScore() { return lastConfirmScore; }

uint32_t getLastDetectionMs() { return lastDetectionMs; }

void detectionEngineSetCadFsk(int cadCount, int fskCount, int strongPendingCad,
                              int activeTaps, int diversityCount,
                              int persistentDiversity, int diversityVelocity,
                              int sustainedCycles) {
    cadDetectionsThisCycle = cadCount;
    fskDetectionsThisCycle = fskCount;
    strongPendingCadThisCycle = strongPendingCad;
    totalActiveTapsThisCycle = activeTaps;
    diversityCountThisCycle = diversityCount;
    persistentDiversityThisCycle = persistentDiversity;
    diversityVelocityThisCycle = diversityVelocity;
    sustainedDiversityCyclesThisCycle = sustainedCycles;
}

ThreatLevel detectionEngineUpdate(const ScanResult& scan, const GpsData& gps,
                                  const IntegrityStatus& integrity,
                                  const ScanResult24* scan24,
                                  bool freshRssi) {
    freshRssiThisCycle = freshRssi;
    if (freshRssi) {
        ambientFilterUpdate(scan);
        updateBandEnergy(scan.rssi);
    }

    // Sub-GHz RSSI pipeline
    float noiseFloor = computeNoiseFloor(scan.rssi, SCAN_BIN_COUNT);
    DetectedPeak peaks[MAX_PEAKS];
    int peakCount = extractPeaks(scan, noiseFloor, peaks);

    markAllUnseen();
    updateTracking(peaks, peakCount);
    updateProtoTracking(peaks, peakCount);

    // 2.4 GHz pipeline (LR1121 only)
    if (scan24 != nullptr && scan24->valid) {
        float nf24 = computeNoiseFloor(scan24->rssi, SCAN_24_BIN_COUNT);
        DetectedPeak peaks24[MAX_PEAKS];
        int peakCount24 = extractPeaks24(*scan24, nf24, peaks24);

        for (int i = 0; i < MAX_TRACKED; i++) {
            if (tracked24[i].active) tracked24[i].consecutiveCount--;
        }
        unsigned long now = millis();
        for (int p = 0; p < peakCount24; p++) {
            bool matched = false;
            for (int t = 0; t < MAX_TRACKED; t++) {
                if (!tracked24[t].active) continue;
                if (fabsf(peaks24[p].frequency - tracked24[t].frequency) < TRACK_FREQ_TOLERANCE) {
                    tracked24[t].consecutiveCount += 2;
                    tracked24[t].lastSeenMs = now;
                    tracked24[t].frequency = peaks24[p].frequency;
                    tracked24[t].match = peaks24[p].match;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                for (int t = 0; t < MAX_TRACKED; t++) {
                    if (!tracked24[t].active || tracked24[t].consecutiveCount <= 0) {
                        tracked24[t].frequency = peaks24[p].frequency;
                        tracked24[t].match = peaks24[p].match;
                        tracked24[t].consecutiveCount = 1;
                        tracked24[t].lastSeenMs = now;
                        tracked24[t].active = true;
                        break;
                    }
                }
            }
        }
        for (int t = 0; t < MAX_TRACKED; t++) {
            if (tracked24[t].active && tracked24[t].consecutiveCount <= 0)
                tracked24[t].active = false;
        }

        for (int p = 0; p < peakCount24; p++) {
            markProtoSeen(peaks24[p].match.protocol, peaks24[p].frequency);
        }
        cleanupProtoTracking();
    }

    return assessThreat(integrity);
}
