#ifndef DETECTION_ENGINE_H
#define DETECTION_ENGINE_H

#include "detection_types.h"
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"
#include "drone_signatures.h"   // for DroneProtocol (Phase C candidate struct)

void detectionEngineInit();

// ── Refactor 3: split ingest/assess ────────────────────────────────────────
// The candidate engine is the sole decision path as of Phase G. Callers feed
// CadBandSummary + ScanResult into the engine, then call detectionEngineAssess()
// to compute the committed threat level.

// Ingest full sub-GHz CadBandSummary (anchor + counts + timestamps) into the
// real candidate engine. Called from loRaScanTask alongside the legacy
// comparison path. Safe no-op if summary is empty.
void detectionEngineIngestCadBandSummary(const struct CadBandSummary& subGHz);

// Ingest full 2.4 GHz CadBandSummary (LR1121 only). The candidate engine uses
// this strictly as a confirmer — it never seeds a new candidate from 2.4 GHz
// evidence alone.
void detectionEngineIngestCad24BandSummary(const struct CadBandSummary& band24);

// Ingest fresh sweep data — called only on RSSI sweep cycles.
// Runs ambientFilterUpdate, updateBandEnergy, peak extraction, protocol tracking,
// and 2.4 GHz tracking. Rejects duplicate sweeps via seq number check.
// scan24 is optional (LR1121 only).
void detectionEngineIngestSweep(const ScanResult& scan, const ScanResult24* scan24 = nullptr);

// Run threat assessment — called every cycle. Reads cached state from both
// ingest functions, evaluates the candidate engine, applies the FSM, and
// caches the last ThreatDecision for the accessors below.
ThreatLevel detectionEngineAssess(const GpsData& gps, const IntegrityStatus& integrity);

// Get the last computed confidence score (for logging/display)
int detectionEngineGetScore();
int detectionEngineGetFastScore();
int detectionEngineGetConfirmScore();

// Phase G: expose the last ThreatDecision fields so the display/logger can
// mirror them into SystemState without reaching into detection_engine internals.
float   detectionEngineGetAnchorFreq();
uint8_t detectionEngineGetBandMask();
bool    detectionEngineHasCandidate();
int     detectionEngineGetCandidateCount();

// Timestamp of the most recent detection event that scored ADVISORY or higher.
uint32_t getLastDetectionMs();

// ── Candidate engine data structures (introduced Phase C, cutover Phase D) ─
// As of Phase G, the candidate engine is the sole threat decider. The legacy
// assessThreat() comparison path and its [CAND-DELTA] regression-alarm logging
// have been removed. Per-evidence readiness gates were added in Phase E
// (spec Part 7) and remain the gate logic for candidate evidence attachment.

enum CandidateState : uint8_t {
    CAND_EMPTY,
    CAND_SEEDING,
    CAND_TRACKING,
    CAND_CONFIRMED
};

struct EvidenceTerm {
    uint8_t  score;
    uint32_t lastSeenMs;
    uint32_t ttlMs;
    bool     ready;
};

struct DetectionCandidate {
    CandidateState       state;
    bool                 active;
    uint8_t              bandMask;        // bit0 = sub-GHz, bit1 = 2.4 GHz
    float                anchorFreq;
    float                minFreqSeen;
    float                maxFreqSeen;
    const DroneProtocol* protoHint;       // ALWAYS nullptr on reset, NEVER store stale pointer
    uint32_t             firstSeenMs;
    uint32_t             lastSeenMs;
    EvidenceTerm         cadConfirmed;
    EvidenceTerm         cadPending;
    EvidenceTerm         fskConfirmed;
    EvidenceTerm         fhssSub;
    // Phase E: cluster evidence — fires when valid anchor + spread + at least
    // one fast-confirmed CAD hit exist simultaneously. Closes the LR1121
    // fast-FHSS detection gap where consecutiveHits>=3 isn't reached.
    EvidenceTerm         fhssCluster;
    EvidenceTerm         sweepSub;
    EvidenceTerm         protoSub;
    // Phase E: bandEnergy as candidate-confirm only. Gated by ambientFilterReady()
    // so it never fires pre-warmup (the band-energy baseline isn't valid yet).
    EvidenceTerm         bandEnergy;
    EvidenceTerm         cad24;
    EvidenceTerm         proto24;
    EvidenceTerm         rid;
    EvidenceTerm         gnss;
};

struct ThreatDecision {
    ThreatLevel level;            // Raw decision from chooseBestCandidate
    ThreatLevel committedLevel;   // Phase E: post-hysteresis FSM level
    uint8_t     fastScore;
    uint8_t     confirmScore;
    float       anchorFreq;
    uint8_t     bandMask;
    bool        hasCandidate;
};

#endif // DETECTION_ENGINE_H
