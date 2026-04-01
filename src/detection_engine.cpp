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
static int recentHitCountThisCycle = 0;

// ── Band energy trending (902-928 MHz) ──────────────────────────────────
// Tracks average RSSI across US band to detect aggregate FHSS energy rise.
// FHSS drones spread energy across 80 channels — individual peaks come and
// go, but the band average rises measurably.

// BAND_ENERGY_HISTORY, BAND_ENERGY_THRESH_DB from sentry_config.h
static float bandEnergyHistory[BAND_ENERGY_HISTORY];
static int bandEnergyIdx = 0;
static int bandEnergySamples = 0;
static bool bandEnergyElevated = false;

static void updateBandEnergy(const float* rssi) {
    // Compute average RSSI across 902-928 MHz (bins 210-340)
    static const int US_START_BIN = 210;
    static const int US_END_BIN = 340;
    float sum = 0;
    for (int i = US_START_BIN; i < US_END_BIN; i++) {
        sum += rssi[i];
    }
    float currentAvg = sum / (US_END_BIN - US_START_BIN);

    // Compute rolling baseline from history
    float baseline = currentAvg;  // fallback if no history yet
    if (bandEnergySamples > 0) {
        float histSum = 0;
        int count = (bandEnergySamples < BAND_ENERGY_HISTORY) ? bandEnergySamples : BAND_ENERGY_HISTORY;
        for (int i = 0; i < count; i++) {
            histSum += bandEnergyHistory[i];
        }
        baseline = histSum / count;
    }

    // Store current value in circular buffer
    bandEnergyHistory[bandEnergyIdx] = currentAvg;
    bandEnergyIdx = (bandEnergyIdx + 1) % BAND_ENERGY_HISTORY;
    if (bandEnergySamples < BAND_ENERGY_HISTORY) bandEnergySamples++;

    // Flag elevated if current average exceeds rolling baseline by threshold
    bandEnergyElevated = (bandEnergySamples >= 3) && (currentAvg > baseline + BAND_ENERGY_THRESH_DB);
}

// ── Threat state machine ────────────────────────────────────────────────────

static ThreatLevel currentThreat = THREAT_CLEAR;
static unsigned long lastThreatEventMs = 0;
// COOLDOWN_MS from sentry_config.h
static int cleanCycleCount = 0;

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
        snprintf(event.description, sizeof(event.description),
                 "%s ch%d (dev %.0fkHz)",
                 sig.match.protocol->name, sig.match.channel, sig.match.deviationKHz);
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
    DetectionEvent event = {};
    event.source = DET_SOURCE_RF;
    event.severity = severity;
    event.frequency = freq;
    event.rssi = 0;
    event.timestamp = millis();
    snprintf(event.description, sizeof(event.description),
             "CAD LoRa SF%d/BW500 @ %.1f MHz", sf, freq);
    xQueueSend(detectionQueue, &event, 0);
}

static void emitFskEvent(float freq, uint8_t severity) {
    DetectionEvent event = {};
    event.source = DET_SOURCE_RF;
    event.severity = severity;
    event.frequency = freq;
    event.rssi = 0;
    event.timestamp = millis();
    snprintf(event.description, sizeof(event.description),
             "FSK 85k1 preamble @ %.1f MHz", freq);
    xQueueSend(detectionQueue, &event, 0);
}

static ThreatLevel assessThreat(const IntegrityStatus& integrity) {
    // ── CAD-as-discriminator threat assessment ─────────────────────────
    //
    // Philosophy: RSSI detects presence. CAD discriminates modulation.
    // One CAD hit has near-zero false alarm rate against non-LoRa signals.
    // So CAD + RSSI corroboration = definitive drone identification.
    //
    // HIGH:   CAD confirmed (3+ hits alone) OR any CAD activity + persistent RSSI
    // MEDIUM: Persistent RSSI in US band, OR band energy elevated, OR strong CAD
    // LOW:    RSSI in EU overlap band without CAD

    // RSSI persistence — per-frequency and per-protocol
    int freqSubGHz  = countPersistentDrone();
    int freqUS      = countPersistentDroneUS();
    int freq24GHz   = countPersistentDrone24();
    int protoSubGHz = countPersistentProtocol(false);
    int proto24GHz  = countPersistentProtocol(true);
    int persistent24GHz = (proto24GHz > freq24GHz) ? proto24GHz : freq24GHz;

    bool gnssAnomaly = integrity.jammingDetected || integrity.spoofingDetected ||
                       integrity.cnoAnomalyDetected;

    // RSSI persistence in the US band (no cell tower overlap)
    int protoUS = countPersistentProtocolUS();
    bool rssiPersistentUS = (freqUS >= 1) || (protoUS >= 1);

    // Rolling 30-second non-ambient CAD hit count — aggregates hits across ALL
    // frequencies/SFs. Catches FHSS pattern where no single frequency accumulates
    // consecutive hits but the aggregate rate is clearly above ambient.
    int recentHits = recentHitCountThisCycle;

    // HIGH: RSSI persistence + 3+ recent CAD hits in 30s (two independent sensors),
    // OR 2+ confirmed non-ambient taps (consecutive-hit path).
    // recentHits alone is NOT sufficient — LoRa-rich environments produce 10+ ambient
    // hits in 30s even with the warmup filter.
    bool confirmedCad = (cadDetectionsThisCycle > 0) || (fskDetectionsThisCycle > 0);
    // recentHits alone NOT sufficient for CRITICAL — LoRa-rich environments
    // produce 6+ ambient hits in 30s. Require RSSI corroboration.
    bool highConfidence = (rssiPersistentUS && recentHits >= 3)
                          || (confirmedCad && cadDetectionsThisCycle >= 2);

    // MEDIUM: RSSI persistence + any CAD evidence, OR persistent RSSI in US band alone,
    //         OR band energy elevated.
    // MEDIUM: RSSI persistence in US band, band energy elevated.
    bool mediumConfidence = (rssiPersistentUS && recentHits >= 1)
                            || rssiPersistentUS || bandEnergyElevated;

    ThreatLevel desired = THREAT_CLEAR;

    // HIGH: CAD discriminated → CRITICAL
    if (highConfidence) {
        desired = THREAT_CRITICAL;
    }
    // MEDIUM: persistent RSSI in US band or 2.4 GHz → WARNING
    else if (mediumConfidence || persistent24GHz >= 1) {
        desired = THREAT_WARNING;
        if (gnssAnomaly) desired = THREAT_CRITICAL;
    }
    // LOW: RSSI-only in EU overlap band → ADVISORY max
    else if (freqSubGHz >= 1 || protoSubGHz >= 1) {
        desired = THREAT_ADVISORY;
        if (gnssAnomaly) desired = THREAT_WARNING;
    }

    // GNSS anomaly alone → WARNING
    if (desired < THREAT_WARNING && gnssAnomaly) {
        desired = THREAT_WARNING;
    }

    // Warmup guard — both RSSI and CAD warmups must complete
    if ((!ambientFilterReady() || !cadWarmupComplete()) && desired > THREAT_ADVISORY) {
        desired = THREAT_ADVISORY;
    }

    unsigned long now = millis();

    // Hysteresis: increase by one step per sweep cycle
    if (desired > currentThreat) {
        desired = (ThreatLevel)(currentThreat + 1);
        lastThreatEventMs = now;
    }

    // Cooldown: decay one step every COOLDOWN_MS
    if (desired <= currentThreat && currentThreat > THREAT_CLEAR) {
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
    // Rapid-clear checks CAD silence only — RSSI persistence decays slowly
    // (3 sweep cycles × 3 CAD cycles each = 9+ cycles) and would prevent
    // rapid-clear from ever firing in the useful timeframe.
    // Check that no escalation-feeding signals exist. Pending taps (1 hit) are
    // always churning on a LoRa-rich bench and don't block rapid-clear.
    bool allClean = (cadDetectionsThisCycle == 0)
                    && (strongPendingCadThisCycle == 0);

    if (allClean) {
        cleanCycleCount++;
        if (cleanCycleCount >= RAPID_CLEAR_CLEAN_CYCLES && currentThreat > THREAT_CLEAR) {
            Serial.printf("[RAPID-CLEAR] %d clean cycles — forcing CLEAR\n", cleanCycleCount);
            desired = THREAT_CLEAR;
            cleanCycleCount = 0;
            lastThreatEventMs = now;
        }
    } else {
        cleanCycleCount = 0;
    }

    // Emit events on change
    if (desired != currentThreat) {
        // CAD/FSK events
        if (highConfidence) {
            if (cadDetectionsThisCycle > 0)
                emitCadEvent(0, 6, desired);  // freq=0 since multiple channels
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
    recentHitCountThisCycle = 0;
    memset(bandEnergyHistory, 0, sizeof(bandEnergyHistory));
    bandEnergyIdx = 0;
    bandEnergySamples = 0;
    bandEnergyElevated = false;
    currentThreat = THREAT_CLEAR;
    lastThreatEventMs = 0;
    ambientFilterInit();
}

void detectionEngineSetCadFsk(int cadCount, int fskCount, int strongPendingCad, int activeTaps, int recentHits) {
    cadDetectionsThisCycle = cadCount;
    fskDetectionsThisCycle = fskCount;
    strongPendingCadThisCycle = strongPendingCad;
    totalActiveTapsThisCycle = activeTaps;
    recentHitCountThisCycle = recentHits;
}

ThreatLevel detectionEngineUpdate(const ScanResult& scan, const GpsData& gps,
                                  const IntegrityStatus& integrity,
                                  const ScanResult24* scan24) {
    ambientFilterUpdate(scan);

    // Band energy trending — detect aggregate FHSS energy in 902-928 MHz
    updateBandEnergy(scan.rssi);

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
