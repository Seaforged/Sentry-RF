#include "ambient_filter.h"
#include <math.h>
#include <string.h>

// Ring buffer of per-bin RSSI readings — 350 bins × 10 sweeps = ~14 KB
static float history[SCAN_BIN_COUNT][AMBIENT_HISTORY_DEPTH];

// Baseline snapshot — taken once after warmup, then frozen
static float baselineMean[SCAN_BIN_COUNT];
static bool  baselineLocked[SCAN_BIN_COUNT];
static bool  baselineTaken = false;

static int   writeIndex  = 0;
static int   sampleCount = 0;

void ambientFilterInit() {
    memset(history, 0, sizeof(history));
    memset(baselineMean, 0, sizeof(baselineMean));
    memset(baselineLocked, 0, sizeof(baselineLocked));
    baselineTaken = false;
    writeIndex  = 0;
    sampleCount = 0;
}

// Compute median noise floor (same algorithm as detection_engine)
static float computeNF(const float* rssi, int count) {
    for (int step = 0; step < count; step += 7) {
        float candidate = rssi[step];
        int below = 0, equal = 0;
        for (int j = 0; j < count; j++) {
            if (rssi[j] < candidate) below++;
            else if (rssi[j] == candidate) equal++;
        }
        if (below <= count / 2 && (below + equal) > count / 2) {
            return candidate;
        }
    }
    return -127.5f;
}

void ambientFilterUpdate(const ScanResult& scan) {
    // Always store into ring buffer
    for (int i = 0; i < SCAN_BIN_COUNT; i++) {
        history[i][writeIndex] = scan.rssi[i];
    }
    writeIndex = (writeIndex + 1) % AMBIENT_HISTORY_DEPTH;

    if (!baselineTaken) {
        if (sampleCount < AMBIENT_HISTORY_DEPTH) {
            sampleCount++;
            return;  // Still collecting warmup data
        }

        // ── ONE-TIME baseline snapshot ──────────────────────────────────
        float noiseFloor = computeNF(scan.rssi, SCAN_BIN_COUNT);
        float nfGate = noiseFloor + AMBIENT_MIN_ABOVE_NF;

        for (int i = 0; i < SCAN_BIN_COUNT; i++) {
            float sum = 0;
            for (int j = 0; j < AMBIENT_HISTORY_DEPTH; j++) {
                sum += history[i][j];
            }
            float mean = sum / (float)AMBIENT_HISTORY_DEPTH;

            float varSum = 0;
            for (int j = 0; j < AMBIENT_HISTORY_DEPTH; j++) {
                float diff = history[i][j] - mean;
                varSum += diff * diff;
            }
            float variance = varSum / (float)AMBIENT_HISTORY_DEPTH;

            if (variance < AMBIENT_VARIANCE_THRESH && mean > nfGate) {
                baselineLocked[i] = true;
                baselineMean[i] = mean;
            }
        }

        baselineTaken = true;
        return;
    }

    // ── Post-baseline: unlock bins if infrastructure disappeared ─────────
    for (int i = 0; i < SCAN_BIN_COUNT; i++) {
        if (baselineLocked[i] &&
            scan.rssi[i] < baselineMean[i] - AMBIENT_UNLOCK_SHIFT) {
            baselineLocked[i] = false;
        }
    }
}

bool ambientFilterIsAmbient(int bin) {
    if (bin < 0 || bin >= SCAN_BIN_COUNT) return false;
    return baselineLocked[bin];
}

bool ambientFilterReady() {
    return baselineTaken;
}
