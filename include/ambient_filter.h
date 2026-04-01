#ifndef AMBIENT_FILTER_H
#define AMBIENT_FILTER_H

#include "rf_scanner.h"
#include "sentry_config.h"

// Constants from sentry_config.h:
// AMBIENT_HISTORY_DEPTH, AMBIENT_VARIANCE_THRESH, AMBIENT_UNLOCK_SHIFT, AMBIENT_MIN_ABOVE_NF

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
