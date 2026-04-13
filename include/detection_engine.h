#ifndef DETECTION_ENGINE_H
#define DETECTION_ENGINE_H

#include "detection_types.h"
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"

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

#endif // DETECTION_ENGINE_H
