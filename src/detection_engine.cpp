#include "detection_engine.h"
#include "drone_signatures.h"
#include "ambient_filter.h"
#include "cad_scanner.h"
#include "rf_scanner.h"
#include "sentry_config.h"
#include "data_logger.h"       // Phase L: emitZmqJson on threat transitions
#include "alert_handler.h"     // Issue 8: alertQueueDropInc()
#include <Arduino.h>
#include <math.h>
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

// Candidate FSM cooldown timer. Independent of any legacy state (which is
// gone as of Phase G) so transitions are driven strictly by the candidate
// engine path.
static uint32_t candidateThreatEventMs = 0;
static uint32_t lastDetectionMs = 0;

// Last wall time (millis) a candidate crossed the ADVISORY fast-score
// threshold. Used by the GNSS temporal correlation gate (Phase 3.3) — GNSS
// anomaly evidence only contributes to confirm score if RF detection was
// active recently. 0 means "never", treated as not-active regardless of age.
static uint32_t lastRfDetectionMs = 0;
// COOLDOWN_MS from sentry_config.h
static unsigned long clearSinceMs = 0;

// Rapid-clear timer: tracks how long the candidate engine has reported
// CLEAR continuously. When it exceeds RAPID_CLEAR_CLEAN_MS, forces
// currentThreat to CLEAR without waiting for the COOLDOWN_MS one-step
// decay. Ported from legacy assessThreat() in Phase G.1b — Phase G
// dropped this by mistake when assessThreat() was deleted.
static uint32_t rapidClearSinceMs = 0;

static int lastFastScore = 0;
static int lastConfirmScore = 0;
// Phase G: cached last ThreatDecision fields for main.cpp/display.
static float   lastAnchorFreq     = 0.0f;
static uint8_t lastBandMask       = 0;
static bool    lastHasCandidate   = false;
static int     lastCandidateCount = 0;

// ── Candidate engine state (introduced Phase C, cutover Phase D) ─────────────
// As of Phase D the candidate engine drives currentThreat and the FSM. The
// pool, cached summaries, and per-evidence helpers below are the core state.

static DetectionCandidate candidatePool[MAX_CANDIDATES] = {};

// Cached sub-GHz CadBandSummary — populated by detectionEngineIngestCadBandSummary()
// each cycle from main.cpp, consumed by evaluateCandidateEngine().
static CadBandSummary lastSubGHzSummary = {};
static bool lastSubGHzSummaryValid = false;

// Phase C.2: cached 2.4 GHz CadBandSummary — populated by
// detectionEngineIngestCad24BandSummary() on LR1121 builds. The candidate
// engine uses this strictly as a confirmer for existing sub-GHz candidates.
static CadBandSummary lastBand24Summary = {};
static bool lastBand24SummaryValid = false;

static void resetCandidate(DetectionCandidate& c) {
    memset(&c, 0, sizeof(c));
    c.state = CAND_EMPTY;
    c.active = false;
    c.protoHint = nullptr;  // critical — never keep a stale protocol pointer
}

static bool evidenceLive(const EvidenceTerm& e, uint32_t nowMs) {
    return e.ready && e.score > 0 && (nowMs - e.lastSeenMs) <= e.ttlMs;
}

static uint8_t evidenceScore(const EvidenceTerm& e, uint32_t nowMs) {
    return evidenceLive(e, nowMs) ? e.score : (uint8_t)0;
}

// Phase E: `ready` is REQUIRED, no default. Each call site must compute the
// per-evidence readiness gate from Part 7 of the spec. `ready=false` means
// the evidence won't contribute to scoring even though it was observed.
static void refreshEvidence(EvidenceTerm& e, uint8_t score, uint32_t ttlMs,
                            uint32_t nowMs, bool ready) {
    e.score = score;
    e.lastSeenMs = nowMs;
    e.ttlMs = ttlMs;
    e.ready = ready;
}

// Phase E helper: does the candidate already have any live FAST-side
// (CAD/FSK/FHSS/cluster) evidence? Used as one of the three ways to satisfy
// the sweepSub readiness gate (see Part 7).
static bool candHasLiveFastEvidence(const DetectionCandidate& c, uint32_t nowMs);
static uint8_t candidateLocalDiversityCount(const DetectionCandidate& c);
static uint8_t candidateLocalFastConfirmedCadCount(const DetectionCandidate& c, uint32_t nowMs);
static bool candidateHasValidatedClusterEvidence(const DetectionCandidate& c, uint32_t nowMs);
static uint8_t candidateCorroboratorCount(const DetectionCandidate& c, uint32_t nowMs);
static bool candidateHasProbationReleaseEvidence(const DetectionCandidate& c, uint32_t nowMs);
static bool candidateNeedsInfraProbation(const DetectionCandidate& c, uint32_t nowMs);
static bool candidateCanAccept24Confirmer(const DetectionCandidate& c, uint32_t nowMs);

static bool allEvidenceDead(const DetectionCandidate& c, uint32_t nowMs) {
    return !evidenceLive(c.cadConfirmed, nowMs) &&
           !evidenceLive(c.cadPending, nowMs) &&
           !evidenceLive(c.fskConfirmed, nowMs) &&
           !evidenceLive(c.fhssSub, nowMs) &&
           !evidenceLive(c.fhssCluster, nowMs) &&
           !evidenceLive(c.sweepSub, nowMs) &&
           !evidenceLive(c.protoSub, nowMs) &&
           !evidenceLive(c.bandEnergy, nowMs) &&
           !evidenceLive(c.cad24, nowMs) &&
           !evidenceLive(c.proto24, nowMs) &&
           !evidenceLive(c.bwWide, nowMs) &&
           !evidenceLive(c.rid, nowMs) &&
           !evidenceLive(c.gnss, nowMs);
}

// Phase E: helper for sweepSub readiness gate. Sweep evidence is allowed to
// score if the candidate already has any live fast-side evidence (CAD/FSK/
// FHSS/cluster) — the rationale is "we already have radio-side proof that
// this candidate is real, so a corroborating sweep peak is trustworthy even
// if the ambient filter hasn't fully learned the bench yet."
static bool candHasLiveFastEvidence(const DetectionCandidate& c, uint32_t nowMs) {
    return evidenceLive(c.cadConfirmed, nowMs) ||
           evidenceLive(c.cadPending,   nowMs) ||
           evidenceLive(c.fskConfirmed, nowMs) ||
           evidenceLive(c.fhssSub,      nowMs) ||
           evidenceLive(c.fhssCluster,  nowMs);
}

static float candidateFreqSpanMhz(const DetectionCandidate& c) {
    float span = c.maxFreqSeen - c.minFreqSeen;
    return (span > 0.0f) ? span : 0.0f;
}

// Convert the candidate's own observed anchor spread into a conservative
// diversity-slot count. This keeps fhssCluster candidate-local instead of
// letting a fixed emitter borrow unrelated bench diversity from the global
// sub-GHz summary.
static uint8_t candidateLocalDiversityCount(const DetectionCandidate& c) {
    float span = candidateFreqSpanMhz(c);
    if (span <= 0.0f) return 1;
    uint16_t slots = (uint16_t)floorf(span / CAND_ASSOC_SUB_MHZ) + 1;
    return (slots > 255) ? (uint8_t)255 : (uint8_t)slots;
}

// Derive the candidate-local fast-confirmed CAD count from the evidence
// already attached to this candidate. cadConfirmed contributes 10 points per
// tap, cadPending contributes 5 points per strong-pending tap.
static uint8_t candidateLocalFastConfirmedCadCount(const DetectionCandidate& c, uint32_t nowMs) {
    uint8_t confirmed = evidenceScore(c.cadConfirmed, nowMs) / FAST_SCORE_CAD_PER_TAP;
    uint8_t pending = evidenceScore(c.cadPending, nowMs) / (FAST_SCORE_CAD_PER_TAP / 2);
    uint16_t total = (uint16_t)confirmed + (uint16_t)pending;
    return (total > 255) ? (uint8_t)255 : (uint8_t)total;
}

// fhssCluster is candidate-local in Phase E.1a: the diversity and fast-
// confirmed CAD used by this gate must come from the candidate's own anchor
// neighborhood, not the aggregate sub-GHz bench summary.
static bool candidateHasValidatedClusterEvidence(const DetectionCandidate& c, uint32_t nowMs) {
    return evidenceLive(c.fhssCluster, nowMs) &&
           candidateLocalDiversityCount(c) >= 8 &&
           candidateLocalFastConfirmedCadCount(c, nowMs) >= 1;
}

static uint8_t candidateCorroboratorCount(const DetectionCandidate& c, uint32_t nowMs) {
    uint8_t count = 0;
    if (candidateHasValidatedClusterEvidence(c, nowMs)) count++;
    if (evidenceLive(c.protoSub, nowMs)) count++;
    if (evidenceLive(c.bandEnergy, nowMs)) count++;
    if (evidenceLive(c.sweepSub, nowMs)) count++;
    if (evidenceLive(c.cad24, nowMs)) count++;
    return count;
}

static bool candidateHasProbationReleaseEvidence(const DetectionCandidate& c, uint32_t nowMs) {
    if (evidenceLive(c.rid, nowMs) || evidenceLive(c.gnss, nowMs)) {
        return true;
    }
    return candidateCorroboratorCount(c, nowMs) >= 2;
}

static bool candidateNeedsInfraProbation(const DetectionCandidate& c, uint32_t nowMs) {
    if (!c.active || !(c.bandMask & 0x01)) return false;
    if ((nowMs - c.firstSeenMs) >= CAND_INFRA_PROBATION_MS) return false;
    if (candidateHasProbationReleaseEvidence(c, nowMs)) return false;
    return candidateFreqSpanMhz(c) <= CAND_INFRA_NARROW_SPAN_MHZ;
}

static bool candidateCanAccept24Confirmer(const DetectionCandidate& c, uint32_t nowMs) {
    // sweepSub intentionally excluded here: a post-warmup sweep peak alone is
    // too easy to earn on persistent bench infrastructure. Cross-band attach
    // requires stronger corroboration.
    return candidateHasValidatedClusterEvidence(c, nowMs) ||
           evidenceLive(c.protoSub, nowMs) ||
           evidenceLive(c.bandEnergy, nowMs);
}

static void ageOutCandidates(DetectionCandidate* pool, uint8_t count, uint32_t nowMs) {
    for (uint8_t i = 0; i < count; i++) {
        DetectionCandidate& c = pool[i];
        if (!c.active) continue;
        if (allEvidenceDead(c, nowMs) && (nowMs - c.lastSeenMs) > CAND_AGE_OUT_MS) {
            resetCandidate(c);
        }
    }
}

static DetectionCandidate* findCandidateForBandFreq(DetectionCandidate* pool, uint8_t count,
                                                    float freq, uint8_t bandMask,
                                                    uint32_t nowMs) {
    for (uint8_t i = 0; i < count; i++) {
        DetectionCandidate& c = pool[i];
        if (!c.active) continue;
        if ((c.bandMask & bandMask) == 0) continue;
        if ((nowMs - c.lastSeenMs) > CAND_ASSOC_SUB_TTL_MS) continue;

        // (a) direct anchor match
        if (fabsf(freq - c.anchorFreq) <= CAND_ASSOC_SUB_MHZ) {
            return &c;
        }
        // (b) falls within the expanded seen-frequency envelope
        if (freq >= (c.minFreqSeen - CAND_ASSOC_SUB_MHZ) &&
            freq <= (c.maxFreqSeen + CAND_ASSOC_SUB_MHZ)) {
            return &c;
        }
        // Issue 9: Region-wide association removed — fuses unrelated emitters
        // in dense ISM environments. Envelope-based association is sufficient
        // as the drone hops; each new hop expands minFreqSeen/maxFreqSeen so
        // legitimate FHSS grows the envelope organically while unrelated
        // gateways elsewhere in the band don't get merged in.
    }
    return nullptr;
}

static DetectionCandidate* allocCandidate(DetectionCandidate* pool, uint8_t count) {
    // Prefer an empty slot
    for (uint8_t i = 0; i < count; i++) {
        if (pool[i].state == CAND_EMPTY) return &pool[i];
    }
    // Otherwise evict the oldest non-CONFIRMED inactive-or-active candidate
    int oldestIdx = -1;
    uint32_t oldestLastSeen = UINT32_MAX;
    for (uint8_t i = 0; i < count; i++) {
        if (pool[i].state == CAND_CONFIRMED) continue;
        if (pool[i].lastSeenMs < oldestLastSeen) {
            oldestLastSeen = pool[i].lastSeenMs;
            oldestIdx = i;
        }
    }
    if (oldestIdx >= 0) {
        resetCandidate(pool[oldestIdx]);
        return &pool[oldestIdx];
    }
    // All slots are CAND_CONFIRMED — refuse to evict
    return nullptr;
}

// Phase D: pick the highest-fast-score active sub-GHz candidate to attach
// broad-spectrum sweep evidence to. The sweep is band-wide, so the strongest
// sub-GHz candidate is the natural target. Returns nullptr if none.
static DetectionCandidate* findCandidateForSweep(DetectionCandidate* pool, uint8_t count,
                                                 uint32_t nowMs);

static uint8_t computeFastScore(const DetectionCandidate& c, uint32_t nowMs) {
    uint16_t cadRaw = (uint16_t)evidenceScore(c.cadConfirmed, nowMs) +
                      (uint16_t)evidenceScore(c.cadPending, nowMs);
    if (cadRaw > FAST_SCORE_CAD_CAP) cadRaw = FAST_SCORE_CAD_CAP;

    uint16_t fskRaw = (uint16_t)evidenceScore(c.fskConfirmed, nowMs);
    if (fskRaw > FAST_SCORE_FSK_CAP) fskRaw = FAST_SCORE_FSK_CAP;

    uint16_t divBonus = evidenceLive(c.fhssSub, nowMs) ? FAST_SCORE_DIVERSITY_BONUS : 0;

    // Phase E: cluster bonus closes the LR1121 fast-FHSS gap. Only fires when
    // the cluster evidence term is live (anchor + spread + fast-confirmed),
    // adding FAST_SCORE_FHSS_CLUSTER points so 1 confirmed sub-GHz tap (10) +
    // diversity bonus (20) + cluster (15) = 45 → crosses WARNING fast threshold.
    uint16_t clusterBonus = candidateHasValidatedClusterEvidence(c, nowMs)
                                ? (uint16_t)evidenceScore(c.fhssCluster, nowMs)
                                : (uint16_t)0;

    uint16_t total = cadRaw + fskRaw + divBonus + clusterBonus;
    return (total > 255) ? (uint8_t)255 : (uint8_t)total;
}

static uint8_t computeConfirmScore(const DetectionCandidate& c, uint32_t nowMs) {
    uint16_t total = (uint16_t)evidenceScore(c.sweepSub,   nowMs) +
                     (uint16_t)evidenceScore(c.protoSub,   nowMs) +
                     (uint16_t)evidenceScore(c.bandEnergy, nowMs) +  // Phase E
                     (uint16_t)evidenceScore(c.cad24,      nowMs) +
                     (uint16_t)evidenceScore(c.proto24,    nowMs) +
                     (uint16_t)evidenceScore(c.bwWide,     nowMs) +  // Phase I
                     (uint16_t)evidenceScore(c.rid,        nowMs) +
                     (uint16_t)evidenceScore(c.gnss,       nowMs);
    return (total > 255) ? (uint8_t)255 : (uint8_t)total;
}

static DetectionCandidate* findCandidateForSweep(DetectionCandidate* pool, uint8_t count,
                                                 uint32_t nowMs) {
    DetectionCandidate* best = nullptr;
    uint8_t bestFast = 0;
    for (uint8_t i = 0; i < count; i++) {
        DetectionCandidate& c = pool[i];
        if (!c.active || !(c.bandMask & 0x01)) continue;
        if ((nowMs - c.lastSeenMs) > CAND_ASSOC_SUB_TTL_MS) continue;
        uint8_t f = computeFastScore(c, nowMs);
        if (best == nullptr || f > bestFast) {
            best = &c;
            bestFast = f;
        }
    }
    return best;
}

static ThreatDecision chooseBestCandidate(DetectionCandidate* pool, uint8_t count,
                                          uint32_t nowMs) {
    ageOutCandidates(pool, count, nowMs);

    ThreatDecision best = {};
    best.level          = THREAT_CLEAR;
    best.committedLevel = THREAT_CLEAR;
    best.fastScore      = 0;
    best.confirmScore   = 0;
    best.anchorFreq     = 0.0f;
    best.bandMask       = 0;
    best.hasCandidate   = false;

    for (uint8_t i = 0; i < count; i++) {
        DetectionCandidate& c = pool[i];
        if (!c.active) continue;

        uint8_t fast = computeFastScore(c, nowMs);
        uint8_t confirm = computeConfirmScore(c, nowMs);
        bool infraProbation = candidateNeedsInfraProbation(c, nowMs);

        ThreatLevel level = THREAT_CLEAR;
        if (fast >= POLICY_CRITICAL_FAST && confirm >= POLICY_CRITICAL_CONFIRM) {
            level = THREAT_CRITICAL;
        } else if (fast >= POLICY_WARNING_FAST && confirm >= POLICY_WARNING_CONFIRM) {
            level = THREAT_WARNING;
        } else if (fast >= POLICY_ADVISORY_FAST) {
            level = THREAT_ADVISORY;
        } else if (evidenceLive(c.rid, nowMs) && c.rid.score >= POLICY_RID_ONLY_ADVISORY) {
            // RID-only path: no fast evidence required
            level = THREAT_ADVISORY;
        }

        // Bridge the post-warmup ambient auto-learn gap: let infrastructure-
        // like sub-GHz candidates accumulate evidence locally, but keep them
        // off the operator-facing FSM until they either pick up stronger
        // corroboration or age past the probation window.
        if (infraProbation && level >= THREAT_ADVISORY) {
            level = THREAT_CLEAR;
        }

        // Promote to CAND_CONFIRMED on first WARNING+ crossing
        if (level >= THREAT_WARNING && c.state != CAND_CONFIRMED) {
            c.state = CAND_CONFIRMED;
        }

        // Pick highest level; break ties by highest fast+confirm
        bool better = false;
        if (level > best.level) {
            better = true;
        } else if (level == best.level && (uint16_t)(fast + confirm) >
                                          (uint16_t)(best.fastScore + best.confirmScore)) {
            better = true;
        }
        if (better) {
            best.level = level;
            best.fastScore = fast;
            best.confirmScore = confirm;
            best.anchorFreq = c.anchorFreq;
            best.bandMask = c.bandMask;
            best.hasCandidate = true;
        }
    }

    return best;
}

// Public: main.cpp caches the full sub-GHz CadBandSummary here once per cycle
// so the candidate engine can read the anchor + full counts. Phase G: this
// is now the sole CAD ingest path — the legacy aggregate-int shim is gone.
void detectionEngineIngestCadBandSummary(const CadBandSummary& subGHz) {
    lastSubGHzSummary = subGHz;
    lastSubGHzSummaryValid = true;
}

// 2.4 GHz CadBandSummary ingest — LR1121 only, confirmer-only.
void detectionEngineIngestCad24BandSummary(const CadBandSummary& band24) {
    lastBand24Summary = band24;
    lastBand24SummaryValid = true;
}

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
    float          frequency;
    float          rssi;
    FreqMatch      match;
    int            adjacentBinCount;  // Phase I: contiguous bins > threshold
    BandwidthClass bwClass;           // Phase I: bucketed width (NARROW/MED/WIDE)
    int            peakBin;           // Phase I: bin index within the sweep
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

            // Phase I: bandwidth classification. Uses the same `threshold`
            // already computed above (max(noiseFloor + PEAK_THRESHOLD_DB,
            // PEAK_ABS_FLOOR_DBM)) so the adjacency run matches the gate
            // that let the peak through in the first place.
            int adjBins = countElevatedAdjacentBins(
                scan.rssi, SCAN_BIN_COUNT, i, threshold);
            BandwidthClass bw = (adjBins >= BW_WIDE_BIN_THRESHOLD)   ? BW_WIDE
                              : (adjBins >= BW_MEDIUM_BIN_THRESHOLD) ? BW_MEDIUM
                              : BW_NARROW;

            if (peakCount < MAX_PEAKS) {
                peaks[peakCount].frequency        = freq;
                peaks[peakCount].rssi             = peakRSSI;
                peaks[peakCount].match            = matchFrequency(freq);
                peaks[peakCount].adjacentBinCount = adjBins;
                peaks[peakCount].bwClass          = bw;
                peaks[peakCount].peakBin          = i;
                peakCount++;
            } else {
                int weakest = 0;
                for (int p = 1; p < MAX_PEAKS; p++) {
                    if (peaks[p].rssi < peaks[weakest].rssi) weakest = p;
                }
                if (peakRSSI > peaks[weakest].rssi) {
                    peaks[weakest].frequency        = freq;
                    peaks[weakest].rssi             = peakRSSI;
                    peaks[weakest].match            = matchFrequency(freq);
                    peaks[weakest].adjacentBinCount = adjBins;
                    peaks[weakest].bwClass          = bw;
                    peaks[weakest].peakBin          = i;
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

            int adjBins24 = countElevatedAdjacentBins(
                scan.rssi, SCAN_24_BIN_COUNT, i, threshold);
            BandwidthClass bw24 = (adjBins24 >= BW_WIDE_BIN_THRESHOLD)   ? BW_WIDE
                                : (adjBins24 >= BW_MEDIUM_BIN_THRESHOLD) ? BW_MEDIUM
                                : BW_NARROW;
            peaks[peakCount].frequency        = freq;
            peaks[peakCount].rssi             = scan.rssi[i];
            peaks[peakCount].match            = matchFrequency24(freq);
            peaks[peakCount].adjacentBinCount = adjBins24;
            peaks[peakCount].bwClass          = bw24;
            peaks[peakCount].peakBin          = i;
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

    if (xQueueSend(detectionQueue, &event, pdMS_TO_TICKS(5)) != pdTRUE) {
        alertQueueDropInc();
    }
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
    if (xQueueSend(detectionQueue, &event, pdMS_TO_TICKS(5)) != pdTRUE) {
        alertQueueDropInc();
    }
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
    if (xQueueSend(detectionQueue, &event, pdMS_TO_TICKS(5)) != pdTRUE) {
        alertQueueDropInc();
    }
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
    if (xQueueSend(detectionQueue, &event, pdMS_TO_TICKS(5)) != pdTRUE) {
        alertQueueDropInc();
    }
}

static int lastScore = 0;  // exposed for serial output

// Threat level name helper for FSM transition logging.
static const char* threatName(ThreatLevel t) {
    switch (t) {
        case THREAT_CLEAR:    return "CLEAR";
        case THREAT_ADVISORY: return "ADVISORY";
        case THREAT_WARNING:  return "WARNING";
        case THREAT_CRITICAL: return "CRITICAL";
        default:              return "?";
    }
}

// Single point of FSM transition emission. No-op if prev == next.
// Centralized here so downstream effects (log, buzzer, LED, display)
// trigger exactly once per true state change — never on steady-state
// reassessment cycles.
static void emitThreatTransition(ThreatLevel prev, ThreatLevel next, uint32_t nowMs) {
    if (prev == next) return;
    SERIAL_SAFE(Serial.printf("[FSM] %s -> %s at %lums\n",
                              threatName(prev), threatName(next),
                              (unsigned long)nowMs));
}

// Phase D Stabilization: emit a DetectionEvent into the alert pipeline on
// every candidate FSM transition. Replaces the legacy per-source emit block
// in assessThreat() that was firing on legacy desired transitions and could
// silently lag behind candidate state.
static void emitCandidateThreatEvent(ThreatLevel level, float anchorFreq) {
    DetectionEvent event = {};
    event.source    = DET_SOURCE_RF;
    event.severity  = (uint8_t)level;
    event.frequency = anchorFreq;
    event.rssi      = 0;
    event.timestamp = millis();
    if (level == THREAT_CLEAR) {
        snprintf(event.description, sizeof(event.description),
                 "Candidate clear");
    } else if (anchorFreq > 0.0f) {
        snprintf(event.description, sizeof(event.description),
                 "Candidate RF track %.1f MHz", anchorFreq);
    } else {
        snprintf(event.description, sizeof(event.description),
                 "Candidate RF track");
    }
    if (xQueueSend(detectionQueue, &event, pdMS_TO_TICKS(5)) != pdTRUE) {
        alertQueueDropInc();
    }
}

// Phase G: the legacy assessThreat() scorer was removed here. It had been
// kept in Phase D as a comparison-only shadow for [CAND-DELTA] regression
// logging, but the candidate engine has been the sole decision path since
// commit aaaa6e9. With Phase G the shadow path, its `legacyThreat` static,
// its `cleanSinceMs`/`lastThreatEventMs` timers, and the associated
// [CAND-DELTA] emit in detectionEngineAssess() are all gone. See the
// Phase G section of the Detection Engine v2 spec for rationale.

// ── Public API ──────────────────────────────────────────────────────────────

void detectionEngineInit() {
    memset(tracked, 0, sizeof(tracked));
    memset(tracked24, 0, sizeof(tracked24));
    memset(protoTracked, 0, sizeof(protoTracked));
    memset(bandEnergyHistoryUS, 0, sizeof(bandEnergyHistoryUS));
    memset(bandEnergyHistoryEU, 0, sizeof(bandEnergyHistoryEU));
    bandEnergyIdxUS = 0; bandEnergySamplesUS = 0; bandEnergyElevatedUS = false;
    bandEnergyIdxEU = 0; bandEnergySamplesEU = 0; bandEnergyElevatedEU = false;
    bandEnergyElevated = false;
    clearSinceMs = 0;
    rapidClearSinceMs = 0;
    currentThreat = THREAT_CLEAR;
    candidateThreatEventMs = 0;
    ambientFilterInit();
}

int detectionEngineGetScore() { return (lastScore > 100) ? 100 : lastScore; }
int detectionEngineGetFastScore() { return lastFastScore; }
int detectionEngineGetConfirmScore() { return lastConfirmScore; }

float   detectionEngineGetAnchorFreq()     { return lastAnchorFreq; }
uint8_t detectionEngineGetBandMask()       { return lastBandMask; }
bool    detectionEngineHasCandidate()      { return lastHasCandidate; }
int     detectionEngineGetCandidateCount() { return lastCandidateCount; }

uint32_t getLastDetectionMs() { return lastDetectionMs; }

// Sprint 1 (v3 Tier 1) — see detection_engine.h. Envelope-based match with a
// hard span cap to reject candidates whose envelope has stretched across
// ambient. DetectionCandidate has no per-evidence frequency list, so we use
// minFreqSeen/maxFreqSeen — but only when the seen-range is narrow enough to
// represent a coherent drone signal (≤5 MHz, per ND refinement after the
// envelope-only run zeroed out cross-band attach). A candidate that has
// already grown a wider envelope is either (a) far enough along that the
// FHSS-spread tracker is firing on its own, or (b) polluted by ambient and
// shouldn't be the basis for suppressing graduation.
constexpr float SPRINT1_MAX_ENVELOPE_SPAN_MHZ = 5.0f;
bool detectionEngineHasActiveCandidateNearFreq(float freqMHz, float toleranceMHz) {
    for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
        const DetectionCandidate& c = candidatePool[i];
        if (!c.active) continue;
        const float span = c.maxFreqSeen - c.minFreqSeen;
        if (span > SPRINT1_MAX_ENVELOPE_SPAN_MHZ) continue;
        if (freqMHz >= c.minFreqSeen - toleranceMHz &&
            freqMHz <= c.maxFreqSeen + toleranceMHz) {
            return true;
        }
    }
    return false;
}

// ── Refactor 3: split ingest/assess ────────────────────────────────────────
// Duplicate sweep rejection: if the same ScanResult is fed to
// detectionEngineIngestSweep() twice in a row (same seq), reject the second
// call so tracking counters don't double-count the same observation.
static uint32_t lastProcessedSweepSeq = 0;
static uint32_t lastProcessed24Seq = 0;

void detectionEngineIngestSweep(const ScanResult& scan, const ScanResult24* scan24) {
    // Reject duplicate sweeps — safety net. scannerSweep() increments seq on
    // every call, so consecutive calls never share a seq; this guards against
    // stale ScanResult reuse from the caller.
    if (scan.valid && scan.seq == lastProcessedSweepSeq) {
        return;
    }
    lastProcessedSweepSeq = scan.seq;

    ambientFilterUpdate(scan);
    updateBandEnergy(scan.rssi);

    // Sub-GHz RSSI pipeline
    // Use IIR adaptive noise floor (dual time constant, rf_scanner.cpp)
    // instead of per-sweep median — adapts across cycles, not just within
    // the current sweep. Fixes LR1121 warmup false-floor behavior.
    float noiseFloor = getAdaptiveNoiseFloor();
    DetectedPeak peaks[MAX_PEAKS];
    int peakCount = extractPeaks(scan, noiseFloor, peaks);

    markAllUnseen();
    updateTracking(peaks, peakCount);
    updateProtoTracking(peaks, peakCount);

    // Phase I: mirror the strongest-peak bandwidth class into SystemState so
    // the JSONL logger can include it. Defaults to NARROW/0 when no peak
    // survived extraction — which is honest (nothing elevated to classify).
    {
        uint8_t bwOut = BW_NARROW;
        uint8_t binsOut = 0;
        float   bestRssi = -200.0f;
        for (int p = 0; p < peakCount; p++) {
            if (peaks[p].rssi > bestRssi) {
                bestRssi = peaks[p].rssi;
                bwOut = (uint8_t)peaks[p].bwClass;
                binsOut = (uint8_t)((peaks[p].adjacentBinCount > 255)
                                    ? 255 : peaks[p].adjacentBinCount);
            }
        }
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            systemState.peakBwClass = bwOut;
            systemState.peakAdjBins = binsOut;
            xSemaphoreGive(stateMutex);
        }
    }

    // Phase I: log wide-band signals and attach confirmer evidence. The
    // existing extractPeaks() spectral-width filter rejects anything with
    // narrowWidth > 6 bins within 6 dB of peak, so BW_WIDE here reflects an
    // asymmetric "sharp peak + long shoulder" shape — the few wide-ish
    // emissions that slip past. True flat-top OcuSync OFDM won't survive the
    // filter to reach this point; see docs/dji.md follow-up for a separate
    // wide-band pipeline if that's needed.
    {
        const uint32_t nowMs = (uint32_t)millis();
        for (int p = 0; p < peakCount; p++) {
            if (peaks[p].bwClass != BW_WIDE) continue;
            float approxMHz = peaks[p].adjacentBinCount * SCAN_FREQ_STEP;
            SERIAL_SAFE(Serial.printf("[BW] Wide-band signal detected: %.1f MHz, %d bins elevated (~%.1f MHz)\n",
                                      peaks[p].frequency,
                                      peaks[p].adjacentBinCount, approxMHz));
            // Attach to any active sub-GHz candidate whose anchor overlaps
            // the peak's center frequency (±CAND_ASSOC_SUB_MHZ). Same
            // association window as the sweep/proto confirmers.
            for (uint8_t c = 0; c < MAX_CANDIDATES; c++) {
                DetectionCandidate& cand = candidatePool[c];
                if (!cand.active || !(cand.bandMask & 0x01)) continue;
                if (fabsf(peaks[p].frequency - cand.anchorFreq)
                    > CAND_ASSOC_SUB_MHZ) continue;
                refreshEvidence(cand.bwWide,
                                (uint8_t)WEIGHT_CONFIRM_BW_WIDE,
                                TTL_BW_WIDE_MS, nowMs, /*ready=*/true);
            }
        }
    }

    // 2.4 GHz pipeline (LR1121 only)
    if (scan24 != nullptr && scan24->valid && scan24->seq != lastProcessed24Seq) {
        lastProcessed24Seq = scan24->seq;

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

        // Issue 6: OFDM plateau detector. Runs in parallel with the
        // peak-finder, does NOT share its WiFi-channel reject. Scans the
        // full 100-bin 2.4 GHz sweep for the longest run of bins at-or-
        // above threshold; when the run meets/exceeds OFDM_PLATEAU_MIN_BINS
        // for OFDM_PLATEAU_PERSIST_CYCLES consecutive sweeps, attaches
        // bwWide evidence to the best active sub-GHz candidate (same
        // confirmer-only association rule as proto24/cad24 — never seeds
        // a new candidate).
        static int plateauConsecutive = 0;
        float plateauThreshold = nf24 + OFDM_PLATEAU_THRESHOLD_DB;
        int plateauLen = findLongestElevatedRun(scan24->rssi,
                                                SCAN_24_BIN_COUNT,
                                                plateauThreshold);
        if (plateauLen >= OFDM_PLATEAU_MIN_BINS) {
            plateauConsecutive++;
        } else {
            plateauConsecutive = 0;
        }
        if (plateauConsecutive == OFDM_PLATEAU_PERSIST_CYCLES) {
            // Only log on the edge — don't spam every sweep while the
            // plateau persists. Attach evidence each cycle though, so TTL
            // refreshes naturally.
            SERIAL_SAFE(Serial.printf("[OFDM-PLATEAU] 2.4 GHz run=%d bins @ NF+%.0fdB (persist=%d sweeps)\n",
                                      plateauLen,
                                      (double)OFDM_PLATEAU_THRESHOLD_DB,
                                      OFDM_PLATEAU_PERSIST_CYCLES));
        }
        if (plateauConsecutive >= OFDM_PLATEAU_PERSIST_CYCLES) {
            // Attach bwWide to one lone/dominant sub-GHz candidate — same
            // confirmer-only association rule used by cad24/proto24. A
            // single wide 2.4 GHz plateau must not boost every candidate in
            // a cluttered ISM environment.
            uint32_t nowMs2 = (uint32_t)millis();
            int liveSubGHzCount = 0;
            DetectionCandidate* bestSubGHz = nullptr;
            uint8_t bestFast = 0;
            uint8_t secondFast = 0;
            for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
                DetectionCandidate& cand = candidatePool[i];
                if (!cand.active || !(cand.bandMask & 0x01)) continue;
                if ((nowMs2 - cand.lastSeenMs) > CAND_ASSOC_24_TTL_MS) continue;
                liveSubGHzCount++;
                uint8_t f = computeFastScore(cand, nowMs2);
                if (f > bestFast) {
                    secondFast = bestFast;
                    bestFast = f;
                    bestSubGHz = &cand;
                } else if (f > secondFast) {
                    secondFast = f;
                }
            }
            bool canAttachPlateau = (liveSubGHzCount == 1) ||
                                    (liveSubGHzCount >= 2 &&
                                     (uint16_t)bestFast >= (uint16_t)secondFast + 15);
            if (canAttachPlateau && bestSubGHz &&
                candidateCanAccept24Confirmer(*bestSubGHz, nowMs2)) {
                refreshEvidence(bestSubGHz->bwWide,
                                (uint8_t)WEIGHT_CONFIRM_BW_WIDE,
                                TTL_BW_WIDE_MS, nowMs2, /*ready=*/true);
                bestSubGHz->bandMask |= 0x02;
            }
        }
    }

    // ── Phase D.2 + Phase E: attach sweep evidence to best active sub-GHz
    // candidate, with per-evidence readiness gates from spec Part 7.
    //
    // sweepSub:   ready if ambientFilterReady() OR peak matches a known proto
    //             family OR the candidate already has live fast evidence
    // protoSub:   ready when match.protocol != nullptr (implied by protoMatch)
    // bandEnergy: ready only if ambientFilterReady() — band-energy baseline
    //             isn't trustworthy until the ambient filter has learned
    // proto24:    confirmer-only, attached using the same lone/dominant
    //             sub-GHz candidate rule used for cad24
    {
        const uint32_t nowMs = (uint32_t)millis();
        DetectionCandidate* sweepCand =
            findCandidateForSweep(candidatePool, MAX_CANDIDATES, nowMs);
        if (sweepCand) {
            bool peakCorroborates =
                (countPersistentDroneUS() + countPersistentDrone()) > 0;
            // protoMatch implies match.protocol != nullptr (countPersistent*
            // only counts taps with a non-null protocol pointer), so this is
            // also the protoSub readiness gate.
            bool protoMatch =
                (countPersistentProtocolUS() + countPersistentProtocol(false)) > 0;

            if (peakCorroborates) {
                // sweepSub readiness: ambient filter ready, OR the peak comes
                // with a known protocol (peak/proto are tracked together so
                // protoMatch implies a known family), OR the candidate
                // already has independent fast-side evidence.
                bool sweepReady = ambientFilterReady() || protoMatch ||
                                  candHasLiveFastEvidence(*sweepCand, nowMs);
                uint8_t newScore = evidenceLive(sweepCand->sweepSub, nowMs)
                                       ? (uint8_t)(WEIGHT_CONFIRM_PEAK * 2)
                                       : (uint8_t)WEIGHT_CONFIRM_PEAK;
                refreshEvidence(sweepCand->sweepSub, newScore,
                                TTL_SWEEP_SUB_MS, nowMs, /*ready=*/sweepReady);
            }
            if (protoMatch) {
                // protoSub readiness: protoMatch above already requires a
                // non-null protocol pointer, so always ready when refreshed.
                refreshEvidence(sweepCand->protoSub,
                                (uint8_t)WEIGHT_CONFIRM_PROTO,
                                TTL_PROTO_SUB_MS, nowMs, /*ready=*/true);
            }
            // Phase E bandEnergy: candidate-confirm only, gated on ambient
            // filter readiness. The bandEnergyElevated flag is computed in
            // updateBandEnergy() from the same sweep we just ingested.
            if (bandEnergyElevated) {
                bool beReady = ambientFilterReady();
                refreshEvidence(sweepCand->bandEnergy,
                                (uint8_t)WEIGHT_CONFIRM_BAND_ENERGY,
                                TTL_SWEEP_SUB_MS, nowMs, /*ready=*/beReady);
            }
        }

        // Phase E: proto24 attachment — confirmer-only, same lone/dominant
        // sub-GHz candidate rule used for cad24. Independent of sweepCand
        // because it iterates the full pool and applies the dominance check.
        int proto24Count = countPersistentProtocol(true);
        if (proto24Count > 0) {
            int liveSubGHzCount = 0;
            DetectionCandidate* bestSubGHz = nullptr;
            uint8_t bestFast = 0;
            uint8_t secondFast = 0;
            for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
                DetectionCandidate& ccc = candidatePool[i];
                if (!ccc.active || !(ccc.bandMask & 0x01)) continue;
                if ((nowMs - ccc.lastSeenMs) > CAND_ASSOC_24_TTL_MS) continue;
                liveSubGHzCount++;
                uint8_t f = computeFastScore(ccc, nowMs);
                if (f > bestFast) {
                    secondFast = bestFast;
                    bestFast = f;
                    bestSubGHz = &ccc;
                } else if (f > secondFast) {
                    secondFast = f;
                }
            }
            bool canAttachProto24 = (liveSubGHzCount == 1) ||
                                    (liveSubGHzCount >= 2 &&
                                     (uint16_t)bestFast >= (uint16_t)secondFast + 15);
            if (canAttachProto24 && bestSubGHz &&
                candidateCanAccept24Confirmer(*bestSubGHz, nowMs)) {
                refreshEvidence(bestSubGHz->proto24,
                                (uint8_t)WEIGHT_CONFIRM_PROTO,
                                TTL_PROTO24_MS, nowMs, /*ready=*/true);
                bestSubGHz->bandMask |= 0x02;
                SERIAL_SAFE(Serial.printf("[CAND] 2.4GHz proto confirmed — attached to anchor=%.1fMHz\n",
                                          bestSubGHz->anchorFreq));
            }
        }
    }
}

// ── Candidate engine evaluation ─────────────────────────────────────────────
// Runs from detectionEngineAssess() on every cycle. Reads the cached sub-GHz
// (and 2.4 GHz on LR1121) CadBandSummary, refreshes evidence on the matching
// candidate per Part 7 readiness gates, attaches cross-domain (RID/GNSS)
// evidence, and returns a ThreatDecision that drives the FSM (Phase D
// cutover, no longer shadow mode).
//
// Phase E: per-evidence readiness gates from spec Part 7 are enforced via the
// explicit `ready` parameter on every refreshEvidence call. The old global
// warmup-cap on candidate seeding has been removed — pre-warmup safety now
// comes from per-evidence readiness (sweepSub/bandEnergy gated on
// ambientFilterReady) rather than a phase-wide branch on s.warmupReady.
static ThreatDecision evaluateCandidateEngine(const GpsData& gps,
                                              const IntegrityStatus& integrity,
                                              uint32_t nowMs) {
    // ── 1. Seed / refresh a sub-GHz candidate from the cached band summary ──
    // Phase E: NO warmup-phase branch on candidate creation. The non-ambient
    // anchor requirement is already enforced by chooseAnchor() in cad_scanner
    // (which filters tap.isAmbient). The 2.4 GHz "never seeds" rule is
    // preserved — only the cached sub-GHz summary is checked here.
    if (lastSubGHzSummaryValid && lastSubGHzSummary.anchor.valid) {
        const CadBandSummary& s = lastSubGHzSummary;
        const float anchorFreq  = s.anchor.frequency;

        bool haveCad  = (s.confirmedCadCount > 0) || (s.strongPendingCad > 0);
        bool haveFsk  = (s.confirmedFskCount > 0);
        // Phase E.1a: haveFhss MUST stay strict because fhssSub being live
        // adds FAST_SCORE_DIVERSITY_BONUS=20 to fast score (the score=1
        // passed to refreshEvidence is just an evidenceLive sentinel — the
        // real scoring contribution is the bonus in computeFastScore()).
        // Loosening to (diversityCount > 0) or (diversityCount >= 2) caused
        // false ADVISORY/WARNING on cold-boot benches where 2-6 unique
        // sub-GHz channels persist as ambient. Keeping the time-gated
        // persistentDiversityCount path matches the original Phase E
        // acceptance behavior. fhssCluster (div>=8 + fastConf>=1) provides
        // the cycle-1 capable path for fast-FHSS; fhssSub stays strict.
        bool haveFhss = (s.persistentDiversityCount > 0) ||
                        (s.diversityVelocity >= DIVERSITY_VELOCITY_FHSS_MIN);

        // Phase E: simple seed gate. Confirmed CAD, confirmed FSK, or any
        // strong-pending CAD all permit seeding. No pre/post-warmup branch.
        bool seedAllowed = (s.confirmedCadCount > 0) ||
                           (s.confirmedFskCount > 0) ||
                           (s.strongPendingCad > 0);

        DetectionCandidate* c = findCandidateForBandFreq(
            candidatePool, MAX_CANDIDATES, anchorFreq, 0x01 /* sub-GHz */, nowMs);

        if (!c && seedAllowed) {
            c = allocCandidate(candidatePool, MAX_CANDIDATES);
            if (c) {
                resetCandidate(*c);
                c->active      = true;
                // Confirmed CAD/FSK → TRACKING; pending-only → SEEDING
                // (still requires confirmed evidence to promote later)
                c->state       = ((s.confirmedCadCount > 0) || (s.confirmedFskCount > 0))
                                     ? CAND_TRACKING : CAND_SEEDING;
                c->bandMask    = 0x01;
                c->anchorFreq  = anchorFreq;
                c->minFreqSeen = anchorFreq;
                c->maxFreqSeen = anchorFreq;
                c->firstSeenMs = nowMs;
            }
        } else if (c) {
            // Existing candidate found — expand envelope; gently pull anchor.
            if (anchorFreq < c->minFreqSeen) c->minFreqSeen = anchorFreq;
            if (anchorFreq > c->maxFreqSeen) c->maxFreqSeen = anchorFreq;
            c->anchorFreq = (c->anchorFreq * 0.7f) + (anchorFreq * 0.3f);
        }

        if (c) {
            c->lastSeenMs = nowMs;

            // ── Per-evidence readiness gates per spec Part 7 ────────────
            // cadConfirmed / cadPending / fskConfirmed / fhssSub / fhssCluster
            // are all "always ready" — radio-side observations are trustworthy
            // from cycle 1.

            if (haveCad) {
                uint16_t cadPts = (uint16_t)s.confirmedCadCount * FAST_SCORE_CAD_PER_TAP;
                if (cadPts > FAST_SCORE_CAD_CAP) cadPts = FAST_SCORE_CAD_CAP;
                refreshEvidence(c->cadConfirmed, (uint8_t)cadPts,
                                TTL_CAD_CONFIRMED_MS, nowMs, /*ready=*/true);

                uint16_t pendPts = (uint16_t)s.strongPendingCad * (FAST_SCORE_CAD_PER_TAP / 2);
                if (pendPts > FAST_SCORE_CAD_CAP) pendPts = FAST_SCORE_CAD_CAP;
                refreshEvidence(c->cadPending, (uint8_t)pendPts,
                                TTL_CAD_PENDING_MS, nowMs, /*ready=*/true);

                if (s.confirmedCadCount > 0 && c->state == CAND_SEEDING) {
                    c->state = CAND_TRACKING;
                }
            }

            if (haveFsk) {
                uint16_t fskPts = (uint16_t)s.confirmedFskCount * FAST_SCORE_FSK_PER_TAP;
                if (fskPts > FAST_SCORE_FSK_CAP) fskPts = FAST_SCORE_FSK_CAP;
                refreshEvidence(c->fskConfirmed, (uint8_t)fskPts,
                                TTL_FSK_CONFIRMED_MS, nowMs, /*ready=*/true);
                if (c->state == CAND_SEEDING) c->state = CAND_TRACKING;
            }

            // FHSS may refresh — never seeds (Phase C.2 Fix 1).
            if (haveFhss) {
                refreshEvidence(c->fhssSub, 1, TTL_FHSS_SUB_MS, nowMs, /*ready=*/true);
            }

            // Phase E.1a cluster evidence — candidate-local, not bench-global.
            // A fixed emitter must not be allowed to borrow unrelated
            // sub-GHz bench diversity from lastSubGHzSummary. Derive the gate
            // from this candidate's own anchor spread plus its own attached
            // fast-confirmed CAD evidence.
            bool haveCluster = (candidateLocalDiversityCount(*c) >= 8) &&
                               (candidateLocalFastConfirmedCadCount(*c, nowMs) >= 1);
            if (haveCluster) {
                refreshEvidence(c->fhssCluster, FAST_SCORE_FHSS_CLUSTER,
                                TTL_FHSS_CLUSTER_MS, nowMs, /*ready=*/true);
            }

            // sweepSub / protoSub / bandEnergy / proto24 are refreshed in
            // detectionEngineIngestSweep() — single-writer rule from Phase D.2.
        }
    }

    // ── 1b. Phase C.2 Fix 4: 2.4 GHz CAD as confirmer (LR1121 only) ─────────
    // The 2.4 GHz band can attach as a CONFIRMER to an existing sub-GHz
    // candidate but never creates one. Spec Part 6: attach only if there is
    // exactly one live sub-GHz candidate, OR a single dominant one (fast >= 15
    // higher than next-best). Sets bandMask bit 1 on the chosen candidate.
    if (lastBand24SummaryValid) {
        const CadBandSummary& b24 = lastBand24Summary;
        bool have24Cad = (b24.confirmedCadCount > 0);

        if (have24Cad) {
            int liveSubGHzCount = 0;
            DetectionCandidate* bestSubGHz = nullptr;
            uint8_t bestFast = 0;
            uint8_t secondFast = 0;
            for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
                DetectionCandidate& ccc = candidatePool[i];
                if (!ccc.active || !(ccc.bandMask & 0x01)) continue;
                if ((nowMs - ccc.lastSeenMs) > CAND_ASSOC_24_TTL_MS) continue;
                liveSubGHzCount++;
                uint8_t f = computeFastScore(ccc, nowMs);
                if (f > bestFast) {
                    secondFast = bestFast;
                    bestFast = f;
                    bestSubGHz = &ccc;
                } else if (f > secondFast) {
                    secondFast = f;
                }
            }

            bool canAttach24 = false;
            if (liveSubGHzCount == 1) {
                canAttach24 = true;
            } else if (liveSubGHzCount >= 2 && (uint16_t)bestFast >= (uint16_t)secondFast + 15) {
                canAttach24 = true;  // dominant candidate
            }

            if (canAttach24 && bestSubGHz &&
                candidateCanAccept24Confirmer(*bestSubGHz, nowMs)) {
                // 2.4 GHz CAD remains confirmer-only. In addition to the
                // lone/dominant association guard above, require strong
                // corroborated sub-GHz FHSS evidence before cross-band attach.
                refreshEvidence(bestSubGHz->cad24, 10, TTL_CAD24_MS, nowMs,
                                /*ready=*/true);
                bestSubGHz->bandMask |= 0x02;
                SERIAL_SAFE(Serial.printf("[CAND] 2.4GHz confirmed — attached to anchor=%.1fMHz\n",
                                          bestSubGHz->anchorFreq));
            }
        }
    }

    // ── 2. Cross-domain evidence (RID, GNSS) — Phase C.2 Fix 3 ──────────────
    // Spec Part 6: attach ONLY if exactly one live RF candidate exists. With
    // 0 or 2+ active candidates the cross-domain evidence cannot be uniquely
    // bound and must not promote anything.
    bool ridDetected = false;
    unsigned long ridLastMs = 0;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5))) {
        ridDetected = systemState.remoteIdDetected;
        ridLastMs   = systemState.remoteIdLastMs;
        xSemaphoreGive(stateMutex);
    }
    bool ridFresh    = ridDetected && ((nowMs - ridLastMs) < 10000);
    bool gnssAnomaly = integrity.jammingDetected || integrity.spoofingDetected ||
                       integrity.cnoAnomalyDetected;

    int activeCount = 0;
    DetectionCandidate* loneCandidate = nullptr;
    for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
        if (candidatePool[i].active) {
            activeCount++;
            loneCandidate = &candidatePool[i];
            // Phase 3.3: stamp RF detection time whenever any candidate meets
            // the ADVISORY fast-score threshold this cycle. Used by the GNSS
            // temporal gate below.
            if (computeFastScore(candidatePool[i], nowMs) >= POLICY_ADVISORY_FAST) {
                lastRfDetectionMs = nowMs;
            }
        }
    }

    // Phase E readiness gates:
    //  - rid: always ready (independent hardware)
    //  - gnss: ready only when GPS has a valid 3D fix AND an RF detection was
    //    active within GNSS_RF_CORRELATION_WINDOW_MS (Phase 3.3).
    //
    // Issue 7: this is the "gnssBoost" path — GNSS still attaches to the
    // candidate confirm score when RF is correlated, but the standalone
    // alert path now lives in gnss_integrity.cpp (emits DET_SOURCE_GNSS
    // DetectionEvent directly to the alert queue on rising transitions).
    // A GNSS anomaly with NO correlated RF will still sound the buzzer
    // via the alert handler; the gate below controls ONLY the candidate-
    // engine score boost, not alerting.
    //
    // GNSS anomaly only contributes to candidate confirm score when RF
    // detection has been active within the last 30s. This prevents a
    // standalone GNSS anomaly (e.g., driving under a bridge) from
    // accumulating confirm evidence on a candidate that was seeded by
    // unrelated infrastructure noise.
    bool rfRecentlyActive = (lastRfDetectionMs != 0) &&
                            ((nowMs - lastRfDetectionMs) < GNSS_RF_CORRELATION_WINDOW_MS);
    bool gnssReady = (gps.fixType >= 3) && rfRecentlyActive;

    if (activeCount == 1 && loneCandidate != nullptr) {
        if (ridFresh) {
            refreshEvidence(loneCandidate->rid, WEIGHT_CONFIRM_REMOTE_ID,
                            TTL_RID_MS, nowMs, /*ready=*/true);
        }
        if (gnssAnomaly) {
            refreshEvidence(loneCandidate->gnss, WEIGHT_CONFIRM_GNSS_TEMPORAL,
                            TTL_GNSS_MS, nowMs, /*ready=*/gnssReady);
        }
    } else {
        if (ridFresh) {
            SERIAL_SAFE(Serial.printf("[CAND] RID skipped — %d active candidates\n",
                                      activeCount));
        }
        if (gnssAnomaly) {
            SERIAL_SAFE(Serial.printf("[CAND] GNSS skipped — %d active candidates\n",
                                      activeCount));
        }
    }

    // ── 3. Run scoring policy and return decision ────────────────────────────
    // Phase E: [CAND] log line moved to detectionEngineAssess so it can print
    // both the raw decision level AND the post-hysteresis committed level.
    ThreatDecision decision = chooseBestCandidate(candidatePool, MAX_CANDIDATES, nowMs);
    decision.committedLevel = decision.level;  // placeholder, overwritten in assess
    return decision;
}

ThreatLevel detectionEngineAssess(const GpsData& gps, const IntegrityStatus& integrity) {
    // Phase G: candidate engine is the sole threat decider. The legacy
    // assessThreat() shadow path and the [CAND-DELTA] regression-alarm
    // logging that compared the two have been removed.
    const uint32_t nowMs = (uint32_t)millis();
    ThreatDecision decision = evaluateCandidateEngine(gps, integrity, nowMs);

    // Update display/log score fields from raw decision (not from currentThreat)
    lastFastScore    = (int)decision.fastScore;
    lastConfirmScore = (int)decision.confirmScore;
    lastScore        = (int)decision.fastScore + (int)decision.confirmScore;
    if (decision.level >= THREAT_ADVISORY) lastDetectionMs = nowMs;

    // Phase G: mirror candidate engine diagnostics for SystemState/display.
    lastAnchorFreq   = decision.anchorFreq;
    lastBandMask     = decision.bandMask;
    lastHasCandidate = decision.hasCandidate;
    {
        int activeCandidates = 0;
        for (uint8_t i = 0; i < MAX_CANDIDATES; i++) {
            if (candidatePool[i].active) activeCandidates++;
        }
        lastCandidateCount = activeCandidates;
    }

    // ── Candidate FSM: fast-up / slow-down hysteresis (Phase D, preserved) ──
    // DO NOT replace with currentThreat = decision.level (raw assignment).
    // The one-step-per-COOLDOWN_MS decay is intentional — it smooths out
    // per-cycle evidence churn and prevents buzzer flapping on intermittent
    // FHSS signals. See Phase D stabilization commit aaaa6e9 for context.
    //
    // Phase D Stabilization: fast-up / slow-down hysteresis on the candidate
    // FSM. Escalation is immediate, decay is gated by COOLDOWN_MS using the
    // dedicated candidateThreatEventMs timer (independent of legacy timing).
    ThreatLevel prevThreat = currentThreat;
    ThreatLevel desired    = decision.level;

    // Phase G.1b: rapid-clear. If the candidate engine itself reports CLEAR
    // (no active candidate above threshold) AND we are currently above CLEAR,
    // start a rapid-clear timer. Once it exceeds RAPID_CLEAR_CLEAN_MS, jump
    // straight to CLEAR without waiting for the one-step COOLDOWN_MS decay.
    // This restores the "drone left the area" fast-path that the legacy
    // assessThreat() owned pre-Phase-G. Must run BEFORE the hysteresis block
    // so the decay branch doesn't delay the rapid-clear by COOLDOWN_MS.
    if (desired == THREAT_CLEAR && currentThreat > THREAT_CLEAR) {
        if (rapidClearSinceMs == 0) rapidClearSinceMs = nowMs;
        if ((nowMs - rapidClearSinceMs) >= RAPID_CLEAR_CLEAN_MS) {
            SERIAL_SAFE(Serial.printf("[RAPID-CLEAR] %lums clean — forcing CLEAR\n",
                                      (unsigned long)(nowMs - rapidClearSinceMs)));
            currentThreat = THREAT_CLEAR;
            rapidClearSinceMs = 0;
            candidateThreatEventMs = nowMs;
        }
    } else {
        rapidClearSinceMs = 0;
    }

    if (desired > currentThreat) {
        currentThreat = desired;
        candidateThreatEventMs = nowMs;
    } else if (desired == currentThreat && currentThreat > THREAT_CLEAR) {
        // Steady-state at evidence-supported level — refresh decay timer
        candidateThreatEventMs = nowMs;
    } else if (desired < currentThreat && currentThreat > THREAT_CLEAR) {
        if (nowMs - candidateThreatEventMs > COOLDOWN_MS) {
            currentThreat = (ThreatLevel)(currentThreat - 1);
            candidateThreatEventMs = nowMs;
        }
    }

    // Phase E: stamp the post-hysteresis committed level back onto the
    // decision so the [CAND] log line below can print both raw and committed.
    decision.committedLevel = currentThreat;

    // Phase E: [CAND] log line — prints raw decision AND committed (post-
    // hysteresis) levels so [CAND] level matches what the FSM actually drives.
    SERIAL_SAFE(Serial.printf("[CAND] raw=%s committed=%s fast=%u conf=%u anchor=%.1fMHz band=0x%02X has=%d\n",
                              threatName(decision.level),
                              threatName(decision.committedLevel),
                              decision.fastScore, decision.confirmScore,
                              decision.anchorFreq, decision.bandMask,
                              decision.hasCandidate ? 1 : 0));

    // Fire transition emitter — drives buzzer / LED / OLED / [FSM] log
    emitThreatTransition(prevThreat, currentThreat, nowMs);

    // Alert queue emit on candidate FSM transition (Phase G: this is now
    // the only emit path — the legacy per-source block is gone).
    if (prevThreat != currentThreat) {
        emitCandidateThreatEvent(currentThreat, decision.anchorFreq);

        // Phase L: ZMQ/DragonSync line. Snapshot SystemState under lock —
        // emitZmqJson takes serialMutex internally for the printf, so we
        // drop stateMutex before calling to avoid lock ordering hazards.
        if (stateMutex) {
            SystemState snap;
            bool have = false;
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                snap = systemState;
                snap.threatLevel = currentThreat;
                snap.confidenceScore = lastScore;
                snap.fastScore = lastFastScore;
                snap.confirmScore = lastConfirmScore;
                snap.anchorFreq = lastAnchorFreq;
                snap.bandMask = lastBandMask;
                snap.hasCandidate = lastHasCandidate;
                snap.candidateCount = lastCandidateCount;
                xSemaphoreGive(stateMutex);
                have = true;
            }
            if (have) emitZmqJson(snap, "threat");
        }
    }

    // Phase G.1b: sustained-clear diversity reset. If the committed threat
    // has been CLEAR for >60 s continuously, wipe the diversity tracker to
    // prevent slow drift on LoRa-rich benches (each stale diversity slot
    // otherwise contributes to the FHSS evidence weight on the next real
    // detection). Ported from legacy assessThreat() in Phase G.1b.
    if (currentThreat == THREAT_CLEAR) {
        if (clearSinceMs == 0) clearSinceMs = nowMs;
        else if ((nowMs - clearSinceMs) > 60000) {
            resetDiversityTracker();
            clearSinceMs = nowMs;
        }
    } else {
        clearSinceMs = 0;
    }

    return currentThreat;
}

// Phase G: removed detectionEngineSetCadFsk() and detectionEngineUpdate()
// backward-compat wrappers. They were thin shims around the split ingest/
// assess API, had no callers outside detection_engine.cpp itself, and
// pointed at functions (detectionEngineIngestCad) that are now gone.
