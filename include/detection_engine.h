#ifndef DETECTION_ENGINE_H
#define DETECTION_ENGINE_H

#include "detection_types.h"
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"
#include "drone_signatures.h"   // for DroneProtocol (Phase C candidate struct)

void detectionEngineInit();

// ── Refactor 3: split ingest/assess ────────────────────────────────────────
// The old monolithic detectionEngineUpdate() is preserved below as a thin
// wrapper for backward compatibility, but new code should call these three
// functions directly so CAD/sweep ingest is decoupled from threat assessment.

// Ingest CAD/FSK results — called every cycle from loRaScanTask after CAD scan.
// Updates cached counts used by detectionEngineAssess().
void detectionEngineIngestCad(int cadCount, int fskCount, int strongPendingCad,
                              int activeTaps, int diversityCount,
                              int persistentDiversity, int diversityVelocity,
                              int sustainedCycles);

// Phase C: Ingest full sub-GHz CadBandSummary (anchor + counts + timestamps)
// into the shadow candidate engine. Called from loRaScanTask alongside the
// legacy detectionEngineIngestCad() path. Safe no-op if summary is empty.
void detectionEngineIngestCadBandSummary(const struct CadBandSummary& subGHz);

// Ingest fresh sweep data — called only on RSSI sweep cycles.
// Runs ambientFilterUpdate, updateBandEnergy, peak extraction, protocol tracking,
// and 2.4 GHz tracking. Rejects duplicate sweeps via seq number check.
// scan24 is optional (LR1121 only).
void detectionEngineIngestSweep(const ScanResult& scan, const ScanResult24* scan24 = nullptr);

// Run threat assessment — called every cycle. Reads cached state from both
// ingest functions and runs the assessThreat policy + FSM.
ThreatLevel detectionEngineAssess(const GpsData& gps, const IntegrityStatus& integrity);

// Feed CAD and FSK detection counts before calling detectionEngineUpdate().
// DEPRECATED: thin wrapper around detectionEngineIngestCad(). Use the new
// function directly in new code.
void detectionEngineSetCadFsk(int cadCount, int fskCount, int strongPendingCad = 0,
                              int activeTaps = 0, int diversityCount = 0,
                              int persistentDiversity = 0, int diversityVelocity = 0,
                              int sustainedCycles = 0);

// Run detection pipeline: peak extraction → freq matching → persistence → threat FSM.
// DEPRECATED: thin wrapper around detectionEngineIngestSweep + detectionEngineAssess.
// Called from loRaScanTask after each sweep. NOT thread-safe — single caller only.
ThreatLevel detectionEngineUpdate(const ScanResult& scan, const GpsData& gps,
                                  const IntegrityStatus& integrity,
                                  const ScanResult24* scan24 = nullptr,
                                  bool freshRssi = false);

// Get the last computed confidence score (for logging/display)
int detectionEngineGetScore();
int detectionEngineGetFastScore();
int detectionEngineGetConfirmScore();

// Timestamp of the most recent detection event that scored ADVISORY or higher.
uint32_t getLastDetectionMs();

// ── Phase C: Shadow-mode candidate engine data structures ─────────────────
// These run IN PARALLEL with the legacy scorer this phase. Actual threat
// output is still driven by assessThreat(); the candidate engine's result
// is logged via [CAND] and [CAND-DELTA] serial lines for comparison only.

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
    EvidenceTerm         sweepSub;
    EvidenceTerm         protoSub;
    EvidenceTerm         cad24;
    EvidenceTerm         proto24;
    EvidenceTerm         rid;
    EvidenceTerm         gnss;
};

struct ThreatDecision {
    ThreatLevel level;
    uint8_t     fastScore;
    uint8_t     confirmScore;
    float       anchorFreq;
    uint8_t     bandMask;
    bool        hasCandidate;
};

#endif // DETECTION_ENGINE_H
