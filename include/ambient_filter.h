#ifndef AMBIENT_FILTER_H
#define AMBIENT_FILTER_H

#include "rf_scanner.h"

// Sweeps of history to accumulate before taking the baseline snapshot.
// At ~2.4s per sweep, 10 sweeps = ~24 second warmup.
static const int AMBIENT_HISTORY_DEPTH = 10;

// Bin must have variance below this (dB²) at snapshot time to be baseline-locked.
// stddev ≈ 2.2 dB — captures cell towers with multipath fading (3-5 dB² typical).
static const float AMBIENT_VARIANCE_THRESH = 5.0f;

// A baseline-locked bin is unlocked if its RSSI drifts this far from its
// baseline mean (infrastructure turned off, or we drove away from a tower).
static const float AMBIENT_UNLOCK_SHIFT = 6.0f;

// Baseline bins must also be above noise floor + this many dB to qualify.
// 5 dB captures weaker cell tower band-edge energy (869-878 MHz) that
// the original 10 dB gate missed.
static const float AMBIENT_MIN_ABOVE_NF = 5.0f;

void ambientFilterInit();

// Feed a new sweep — call once per sweep cycle, before extractPeaks()
void ambientFilterUpdate(const ScanResult& scan);

// Returns true if the bin was baseline-locked at boot AND still within
// 6 dB of its baseline mean. New signals appearing after the baseline
// snapshot are never classified as ambient.
bool ambientFilterIsAmbient(int bin);

// Returns true once the baseline snapshot has been taken.
// During warmup the detection engine should cap threat escalation.
bool ambientFilterReady();

#endif // AMBIENT_FILTER_H
