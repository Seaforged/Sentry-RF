#include "cad_scanner.h"
#include "rf_scanner.h"
#include "board_config.h"
#include <Arduino.h>
#include <string.h>

// Access the Module object for low-level SPI commands (defined in main.cpp)
extern Module radioMod;

// ── Per-SF CAD detection parameters (Semtech AN1200.48) ────────────────────
// Biased toward higher detection probability — persistence filter handles
// false hits. Index by (SF - 6).
// #define DEBUG_CAD_PARAMS  // Uncomment to log per-SF parameter selection

struct CadParams {
    uint8_t symbolNum;   // RADIOLIB_SX126X_CAD_ON_2_SYMB=0x01, _4_SYMB=0x02
    uint8_t detPeak;
    uint8_t detMin;
};

// BW500 values from Semtech AN1200.48 Table 43 (empirically optimized).
// SF6 not tested by Semtech — extrapolated as SF+13.
// SF11 uses Semtech's 25 (not SF+13=24). SF12 uses 8 symbols per Semtech.
static const CadParams cadParams[] = {
    { 0x02, 19, 10 },  // SF6  — 4 sym, detPeak=19 (extrapolated)
    { 0x02, 21, 10 },  // SF7  — 4 sym, detPeak=21 (Semtech Table 43)
    { 0x02, 22, 10 },  // SF8  — 4 sym, detPeak=22 (Semtech Table 43)
    { 0x02, 22, 10 },  // SF9  — 4 sym, detPeak=22 (Semtech Table 43)
    { 0x02, 23, 10 },  // SF10 — 4 sym, detPeak=23 (Semtech Table 43)
    { 0x02, 25, 10 },  // SF11 — 4 sym, detPeak=25 (Semtech Table 43)
    { 0x03, 29, 10 },  // SF12 — 8 sym, detPeak=29 (Semtech Table 43)
};

static ChannelScanConfig_t buildCadConfig(uint8_t sf) {
    uint8_t idx = (sf >= 6 && sf <= 12) ? (sf - 6) : 0;
    ChannelScanConfig_t cfg = {
        .cad = {
            .symNum = cadParams[idx].symbolNum,
            .detPeak = cadParams[idx].detPeak,
            .detMin = cadParams[idx].detMin,
            .exitMode = RADIOLIB_SX126X_CAD_GOTO_STDBY,
            .timeout = 0,
            .irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS,
            .irqMask = RADIOLIB_IRQ_CAD_DEFAULT_MASK,
        },
    };
    return cfg;
}

#ifdef BOARD_T3S3_LR1121
// LR11x0 uses a DIFFERENT detPeak scale than SX126x. Semtech AN1200.48
// values (19-29) are SX126x-only. LR11x0 expects 48-65 per its internal
// detPeakValues table in LR11x0::startCad(). Passing SX126x values to an
// LR1121 produces broken CAD — triggers constantly in one frequency region
// and misses real signals elsewhere.
//
// Solution: pass RADIOLIB_LR11X0_CAD_PARAM_DEFAULT (0xFF) for per-SF tuned
// fields so the driver picks the right values from its internal table, and
// explicitly request 4 CAD symbols (raw count — LR11x0 does NOT use the
// SX126X_CAD_ON_N_SYMB enum encoding).
static ChannelScanConfig_t buildCadConfigLR(uint8_t sf) {
    // LR1121-specific CAD params. DetPeak values lowered from RadioLib defaults
    // (which the author marked "TODO probably suboptimal") for higher detection
    // sensitivity on short implicit-header ELRS packets. The FHSS spread tracker
    // and ambient filter handle any additional false CAD hits downstream.
    // Scale: LR11x0 detPeak range is ~43-72 (vs SX126x 19-29).
    static const uint8_t detPeakLR[] = {
        45,  // SF6  — primary ELRS 200Hz (default was 48)
        49,  // SF7  — ELRS 150Hz, mLRS 31Hz (default was 50)
        54,  // SF8  — ELRS 100Hz (default was 55)
        52,  // SF9  — ELRS 50Hz (default was 55)
        56,  // SF10 (default was 59)
        60,  // SF11 — mLRS 19Hz (default was 61)
        72,  // SF12 — needs higher per AN1200.48 (default was 65)
    };
    uint8_t idx = (sf >= 6 && sf <= 12) ? sf - 6 : 0;
    ChannelScanConfig_t cfg = {
        .cad = {
            .symNum  = 4,
            .detPeak = detPeakLR[idx],
            .detMin  = 10,
            .exitMode = 0,
            .timeout = 0,
            .irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS,
            .irqMask = RADIOLIB_IRQ_CAD_DEFAULT_MASK,
        },
    };
    return cfg;
}
#endif

// ── Frequency Diversity Tracker slot struct ──────────────────────────────
// (definition must precede BandTracker)
struct DiversitySlot {
    float frequency;
    uint8_t sf;
    unsigned long lastHitMs;
    uint8_t consecutiveHits;  // consecutive scan cycles with a hit
    bool hitThisCycle;        // marked during scan, cleared at cycle start
    bool active;
};

// ── Per-SF rotation counters ────────────────────────────────────────────────

static uint32_t rotSF6  = 0;
static uint32_t rotSF7  = 0;
static uint32_t rotSF8  = 0;
static uint32_t rotSF9  = 0;
static uint32_t rotSF10 = 0;
static uint32_t rotSF11 = 0;
static uint32_t rotSF12 = 0;
static uint32_t rotFSK  = 0;

// ── Channel frequency helpers ───────────────────────────────────────────────

static float elrs915Freq(int ch) { return 902.0f + (ch * 0.325f); }
static float elrs868Freq(int ch) { return 860.0f + (ch * 0.520f); }
static float crsfFskFreq(int ch) { return 902.165f + (ch * 0.260f); }

// ── Ambient CAD source tracking ─────────────────────────────────────────────
// LoRa sources present during warmup are infrastructure, not drones.
// Record their frequency/SF during the first N cycles and exclude from
// confirmed counts. A drone arriving after boot will appear on frequencies
// NOT seen during warmup.

// Constants from sentry_config.h: MAX_AMBIENT_TAPS, AMBIENT_WARMUP_MS, AMBIENT_FREQ_TOLERANCE

struct AmbientTap {
    float    frequency;
    uint8_t  sf;
    uint16_t firstSeenCycle;
    bool     active;
    bool     learnedDuringWarmup;  // true = warmup, false = auto-learned
};

static AmbientTap ambientTaps[MAX_AMBIENT_TAPS];
static uint8_t ambientTapCount = 0;
static uint16_t warmupCycleCount = 0;

// ── Per-band tracker ─────────────────────────────────────────────────────
// All state that used to be shared across sub-GHz and 2.4 GHz is now per-
// band. Each tracker owns its own tap list, diversity slots, sustained-
// diversity counters, and velocity window. Sub-GHz and 2.4 GHz detection
// cannot cross-contaminate.
struct BandTracker {
    CadTap taps[MAX_TAPS];
    DiversitySlot diversitySlots[MAX_DIVERSITY_SLOTS];
    int sustainedDiversityCycles;
    int prevSustainedDiversityCycles;
    unsigned long sustainedStartMs;
    // Velocity samples: (timestamp, crossed) pairs. Velocity = sum of crossed
    // values with timestamps inside DIVERSITY_VELOCITY_WINDOW_MS.
    struct VelocitySample { uint32_t tsMs; int crossed; };
    VelocitySample velocitySamples[DIVERSITY_VELOCITY_WINDOW];
    int velocityIdx;
};

static BandTracker subGHzTracker;
#ifdef BOARD_T3S3_LR1121
static BandTracker band24Tracker;
#endif

// Cached radio mode — avoids redundant packet type switches.
// On non-RSSI cycles, the radio never leaves LoRa mode so ensureLoRa() is a
// no-op. Reset to MODE_UNKNOWN in cadScannerInit() and on any begin() call
// that might reset the chip.
enum RadioMode { MODE_UNKNOWN, MODE_LORA, MODE_GFSK };
static RadioMode currentRadioMode = MODE_UNKNOWN;
static bool warmupComplete = false;

static uint16_t cadErrorsThisCycle = 0;
static uint16_t cadProbesThisCycle = 0;
static int16_t cadFirstError = 0;
static bool cadHwFaultFlag = false;

static inline bool isCadError(int16_t result) {
    return result != RADIOLIB_LORA_DETECTED && result != RADIOLIB_CHANNEL_FREE;
}

static inline void countCadProbe(int16_t result) {
    cadProbesThisCycle++;
    if (isCadError(result)) {
        if (cadErrorsThisCycle == 0) cadFirstError = result;
        cadErrorsThisCycle++;
    }
}
static uint8_t consecutiveFaultCycles = 0;
static uint8_t consecutiveCleanCycles = 0;

// Latest sub-GHz sweep result, captured at the top of cadFskScan() so the
// adaptive-noise-floor filter in isAmbientFrequency() can look up the RSSI at
// any hit frequency without threading an extra parameter through every call.
// Valid only within a single cadFskScan() invocation.
static const ScanResult* latestSweep = nullptr;

// Adaptive-NF margin: a CAD hit whose sweep RSSI is within this many dB of
// the current noise floor is treated as noise, not a real signal. 6 dB is the
// wardragon-fpv-detect AUTO_THRESHOLD default — enough headroom to ignore
// sweep-to-sweep jitter without masking weak drones at long range.
static const float NF_AMBIENT_MARGIN_DB = 6.0f;

// Look up the RSSI of `freq` in the latest RSSI sweep. Returns -200.0 if the
// sweep isn't available yet (cold boot / RSSI cycle skipped) so callers that
// fall back to "below floor" won't spuriously flag the hit as ambient.
static float rssiAtFreq(float freq) {
    if (!latestSweep) return -200.0f;
    int bin = (int)roundf((freq - SCAN_FREQ_START) / SCAN_FREQ_STEP);
    if (bin < 0 || bin >= SCAN_BIN_COUNT) return -200.0f;
    return latestSweep->rssi[bin];
}

static bool isAmbientCadSource(float freq, uint8_t sf) {
    for (int i = 0; i < MAX_AMBIENT_TAPS; i++) {
        if (ambientTaps[i].active &&
            ambientTaps[i].sf == sf &&
            fabsf(ambientTaps[i].frequency - freq) <= AMBIENT_FREQ_TOLERANCE) {
            return true;
        }
    }
    return false;
}

static void recordAmbientTap(float freq, uint8_t sf, bool duringWarmup = false) {
    if (isAmbientCadSource(freq, sf)) return;
    if (ambientTapCount < MAX_AMBIENT_TAPS) {
        ambientTaps[ambientTapCount].frequency = freq;
        ambientTaps[ambientTapCount].sf = sf;
        ambientTaps[ambientTapCount].firstSeenCycle = warmupCycleCount;
        ambientTaps[ambientTapCount].active = true;
        ambientTaps[ambientTapCount].learnedDuringWarmup = duringWarmup;
        ambientTapCount++;
    }
}

// Hybrid ambient filter: a CAD hit is ambient if EITHER
// (a) any ambient tap (warmup or auto-learned) matches, OR
// (b) its latest sweep RSSI is below adaptiveNoiseFloor + 6 dB.
static bool isAmbientFrequency(float freq, uint8_t sf) {
    for (int i = 0; i < MAX_AMBIENT_TAPS; i++) {
        if (ambientTaps[i].active &&
            ambientTaps[i].sf == sf &&
            fabsf(ambientTaps[i].frequency - freq) <= AMBIENT_FREQ_TOLERANCE) {
            return true;
        }
    }
    float rssi = rssiAtFreq(freq);
    if (rssi < getAdaptiveNoiseFloor() + NF_AMBIENT_MARGIN_DB) {
        return true;
    }
    return false;
}

// ── Frequency Diversity Tracker ──────────────────────────────────────────────
// Tracks DISTINCT frequencies that produced non-ambient CAD hits within a
// sliding time window. FHSS drones hit many different frequencies (5-10 in 5s);
// infrastructure hits the same 1-3 frequencies repeatedly.
// This is the primary FHSS discriminator — count ≠ frequency spread.

static void recordDiversityHit(BandTracker& t, float freq, uint8_t sf) {
    unsigned long now = millis();

    // Check if this frequency/SF already has a slot
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (t.diversitySlots[i].active &&
            t.diversitySlots[i].sf == sf &&
            fabsf(t.diversitySlots[i].frequency - freq) <= TAP_FREQ_TOL) {
            t.diversitySlots[i].lastHitMs = now;
            t.diversitySlots[i].hitThisCycle = true;
            return;  // Same frequency — no new diversity
        }
    }

    // New frequency — find an empty slot or evict the oldest
    int bestSlot = 0;
    unsigned long oldestTime = ULONG_MAX;
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (!t.diversitySlots[i].active) {
            bestSlot = i;
            break;
        }
        if (t.diversitySlots[i].lastHitMs < oldestTime) {
            oldestTime = t.diversitySlots[i].lastHitMs;
            bestSlot = i;
        }
    }

    t.diversitySlots[bestSlot].frequency = freq;
    t.diversitySlots[bestSlot].sf = sf;
    t.diversitySlots[bestSlot].lastHitMs = now;
    t.diversitySlots[bestSlot].consecutiveHits = 0;  // first hit, will become 1 at cycle end
    t.diversitySlots[bestSlot].hitThisCycle = true;
    t.diversitySlots[bestSlot].active = true;
}

// Forward declaration for cycle update
static int countDiversity(BandTracker& t, unsigned long windowMs);

// Call at the START of each scan cycle to update sustained-diversity tracking
// from the previous cycle's diversity count.
static void diversityCycleUpdate(BandTracker& t) {
    // Update per-slot counters (kept for future use, not used for gate)
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (!t.diversitySlots[i].active) continue;
        if (t.diversitySlots[i].hitThisCycle) {
            if (t.diversitySlots[i].consecutiveHits < 255) t.diversitySlots[i].consecutiveHits++;
            t.diversitySlots[i].hitThisCycle = false;
        } else {
            t.diversitySlots[i].consecutiveHits = 0;
        }
    }

    // Sustained-diversity gate: was last cycle's raw diversity above threshold?
    int rawDiv = countDiversity(t, DIVERSITY_WINDOW_MS);
    t.prevSustainedDiversityCycles = t.sustainedDiversityCycles;
    bool wasTimeSustained = (t.sustainedStartMs != 0) &&
                            ((millis() - t.sustainedStartMs) >= PERSISTENCE_MIN_MS);
    if (rawDiv >= PERSISTENCE_MIN_DIVERSITY) {
        t.sustainedDiversityCycles++;
        if (t.sustainedStartMs == 0) t.sustainedStartMs = millis();
    } else {
        // Drone departure prune: if we had sustained diversity (drone present)
        // and it just collapsed, aggressively clear all non-ambient taps.
        // Prevents confirmed taps from lingering and inflating the score
        // for 30+ seconds after the drone leaves.
        if (wasTimeSustained) {
            for (int i = 0; i < MAX_TAPS; i++) {
                if (t.taps[i].active && !t.taps[i].isAmbient) {
                    t.taps[i].active = false;
                }
            }
        }
        t.sustainedDiversityCycles = 0;
        t.sustainedStartMs = 0;
    }

    // Velocity: did we just cross the sustained threshold this cycle (time-based)?
    bool isTimeSustained = (t.sustainedStartMs != 0) &&
                           ((millis() - t.sustainedStartMs) >= PERSISTENCE_MIN_MS);
    if (isTimeSustained && !wasTimeSustained) {
        // Record a new crossing sample with current timestamp
        t.velocitySamples[t.velocityIdx].tsMs = millis();
        t.velocitySamples[t.velocityIdx].crossed = rawDiv;
        t.velocityIdx = (t.velocityIdx + 1) % DIVERSITY_VELOCITY_WINDOW;
    }
}

static void pruneExpiredDiversity(BandTracker& t) {
    unsigned long now = millis();
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (t.diversitySlots[i].active &&
            (now - t.diversitySlots[i].lastHitMs) >= DIVERSITY_WINDOW_MS) {
            t.diversitySlots[i].active = false;
            t.diversitySlots[i].consecutiveHits = 0;
        }
    }
}

// Count ALL active diversity slots (raw, for logging)
static int countDiversity(BandTracker& t, unsigned long windowMs) {
    unsigned long now = millis();
    int count = 0;
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (t.diversitySlots[i].active &&
            (now - t.diversitySlots[i].lastHitMs) < windowMs) {
            count++;
        }
    }
    return count;
}

// Sustained-diversity persistent count: if raw diversity has been >= threshold
// for PERSISTENCE_MIN_MS wall-clock time, ALL current diversity qualifies.
// Otherwise returns 0. Catches FHSS (many freqs every cycle) while filtering
// sporadic infrastructure. Time-based gate gives board-parity timing.
static int countPersistentDiversity(BandTracker& t, unsigned long windowMs) {
    if (t.sustainedStartMs != 0 && (millis() - t.sustainedStartMs) >= PERSISTENCE_MIN_MS)
        return countDiversity(t, windowMs);
    return 0;
}

static int getSustainedDiversityCycles(BandTracker& t) {
    return t.sustainedDiversityCycles;
}

// Sum of newly-persistent slots within DIVERSITY_VELOCITY_WINDOW_MS wall-clock.
// Time-based: samples older than the window are ignored, regardless of cycle count.
static int computeDiversityVelocity(BandTracker& t) {
    uint32_t now = millis();
    int sum = 0;
    for (int i = 0; i < DIVERSITY_VELOCITY_WINDOW; i++) {
        if (t.velocitySamples[i].tsMs != 0 &&
            (uint32_t)(now - t.velocitySamples[i].tsMs) < DIVERSITY_VELOCITY_WINDOW_MS) {
            sum += t.velocitySamples[i].crossed;
        }
    }
    return sum;
}

// ── FHSS Frequency-Spread Tracker (time-based) ──────────────────────────────
// Tracks UNIQUE (freq,sf) CAD hits within the last FHSS_WINDOW_MS wall-clock
// window. When the unique count crosses the baseline+threshold, each unique
// hit is forwarded to recordDiversityHit() — bypassing the consecutiveHits>=2
// gate that blocks hopping transmitters from ever accumulating diversity.
//
// Storage: a flat array of timestamped hits. Expired hits (older than
// FHSS_WINDOW_MS) are pruned on insertion and collection. Deduplication by
// (freq, sf) with TAP_FREQ_TOL tolerance — duplicate hits refresh the
// timestamp rather than adding a new entry.
//
// Sub-GHz and 2.4 GHz use independent hit arrays so cross-band contamination
// cannot create false FHSS spread detections.

static const uint8_t FHSS_MAX_HITS = 96;

struct FhssHit { float freq; uint8_t sf; uint32_t tsMs; bool active; };

static FhssHit fhssHitsSubGHz[FHSS_MAX_HITS];
#ifdef BOARD_T3S3_LR1121
static FhssHit fhssHits24[FHSS_MAX_HITS];
#endif

// Prune hits older than FHSS_WINDOW_MS. Call from add and collect.
static void fhssPruneExpired(FhssHit* hits) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < FHSS_MAX_HITS; i++) {
        if (hits[i].active && (uint32_t)(now - hits[i].tsMs) > FHSS_WINDOW_MS) {
            hits[i].active = false;
        }
    }
}

// Insert or refresh a hit. Duplicate (within TAP_FREQ_TOL on same SF) refreshes
// timestamp. If array is full, the insert is dropped silently — this is fine
// because we only care about unique count, not every individual hit.
static void fhssRecordHitGeneric(FhssHit* hits, float freq, uint8_t sf) {
    fhssPruneExpired(hits);
    uint32_t now = millis();
    for (uint8_t i = 0; i < FHSS_MAX_HITS; i++) {
        if (hits[i].active && hits[i].sf == sf &&
            fabsf(hits[i].freq - freq) <= TAP_FREQ_TOL) {
            hits[i].tsMs = now;
            return;
        }
    }
    for (uint8_t i = 0; i < FHSS_MAX_HITS; i++) {
        if (!hits[i].active) {
            hits[i].freq = freq;
            hits[i].sf = sf;
            hits[i].tsMs = now;
            hits[i].active = true;
            return;
        }
    }
}

static void fhssRecordHitSubGHz(float freq, uint8_t sf) {
    fhssRecordHitGeneric(fhssHitsSubGHz, freq, sf);
}

#ifdef BOARD_T3S3_LR1121
static void fhssRecordHit24(float freq, uint8_t sf) {
    fhssRecordHitGeneric(fhssHits24, freq, sf);
}
#endif

// Collect unique active hits into out[]. Returns count written.
static uint8_t fhssCollectUnique(FhssHit* hits, FhssHit* out, uint8_t outMax) {
    fhssPruneExpired(hits);
    uint8_t n = 0;
    for (uint8_t i = 0; i < FHSS_MAX_HITS && n < outMax; i++) {
        if (!hits[i].active) continue;
        out[n] = hits[i];
        n++;
    }
    return n;
}

// Call after sub-GHz scan completes. Uses a baseline-delta discriminator.
static uint8_t fhssFlushSpread() {
    static uint8_t baselineFhssUnique = 0;
    static uint8_t baselineCalibCycles = 0;
    static const uint8_t BASELINE_CALIB_N = 5;

    FhssHit uniq[FHSS_MAX_HITS];
    uint8_t uniqCount = fhssCollectUnique(fhssHitsSubGHz, uniq, FHSS_MAX_HITS);

    if (!warmupComplete) return 0;

    if (baselineCalibCycles < BASELINE_CALIB_N) {
        if (uniqCount > baselineFhssUnique) baselineFhssUnique = uniqCount;
        baselineCalibCycles++;
        return 0;
    }

    if (uniqCount < baselineFhssUnique) {
        baselineFhssUnique--;
    } else if (uniqCount > baselineFhssUnique &&
               uniqCount < baselineFhssUnique + FHSS_UNIQUE_THRESHOLD) {
        baselineFhssUnique++;
    }

    if (uniqCount < baselineFhssUnique + FHSS_UNIQUE_THRESHOLD) return 0;

    uint8_t fired = 0;
    for (uint8_t i = 0; i < uniqCount; i++) {
        recordDiversityHit(subGHzTracker, uniq[i].freq, uniq[i].sf);
        fired++;
    }
    if (fired > 0) {
        Serial.printf("[FHSS-SPREAD] %u unique (baseline=%u) -> diversity fired\n",
                      uniqCount, baselineFhssUnique);
    }
    return fired;
}

#ifdef BOARD_T3S3_LR1121
// 2.4 GHz FHSS spread flush — independent baseline, feeds band24Tracker.
static uint8_t fhssFlushSpread24() {
    static uint8_t baselineFhssUnique = 0;
    static uint8_t baselineCalibCycles = 0;
    static const uint8_t BASELINE_CALIB_N = 5;

    FhssHit uniq[FHSS_MAX_HITS];
    uint8_t uniqCount = fhssCollectUnique(fhssHits24, uniq, FHSS_MAX_HITS);

    if (!warmupComplete) return 0;

    if (baselineCalibCycles < BASELINE_CALIB_N) {
        if (uniqCount > baselineFhssUnique) baselineFhssUnique = uniqCount;
        baselineCalibCycles++;
        return 0;
    }

    if (uniqCount < baselineFhssUnique) {
        baselineFhssUnique--;
    } else if (uniqCount > baselineFhssUnique &&
               uniqCount < baselineFhssUnique + FHSS_UNIQUE_THRESHOLD) {
        baselineFhssUnique++;
    }

    if (uniqCount < baselineFhssUnique + FHSS_UNIQUE_THRESHOLD) return 0;

    uint8_t fired = 0;
    for (uint8_t i = 0; i < uniqCount; i++) {
        recordDiversityHit(band24Tracker, uniq[i].freq, uniq[i].sf);
        fired++;
    }
    if (fired > 0) {
        Serial.printf("[FHSS-SPREAD-2G4] %u unique (baseline=%u) -> diversity fired\n",
                      uniqCount, baselineFhssUnique);
    }
    return fired;
}
#endif

// ── Tap list management ─────────────────────────────────────────────────────

bool cadWarmupComplete() {
    return warmupComplete;
}

bool cadHwFault() {
    return cadHwFaultFlag;
}

void resetDiversityTracker() {
    memset(subGHzTracker.diversitySlots, 0, sizeof(subGHzTracker.diversitySlots));
    memset(subGHzTracker.velocitySamples, 0, sizeof(subGHzTracker.velocitySamples));
    subGHzTracker.velocityIdx = 0;
    subGHzTracker.sustainedDiversityCycles = 0;
    subGHzTracker.prevSustainedDiversityCycles = 0;
    subGHzTracker.sustainedStartMs = 0;
#ifdef BOARD_T3S3_LR1121
    memset(band24Tracker.diversitySlots, 0, sizeof(band24Tracker.diversitySlots));
    memset(band24Tracker.velocitySamples, 0, sizeof(band24Tracker.velocitySamples));
    band24Tracker.velocityIdx = 0;
    band24Tracker.sustainedDiversityCycles = 0;
    band24Tracker.prevSustainedDiversityCycles = 0;
    band24Tracker.sustainedStartMs = 0;
    memset(fhssHits24, 0, sizeof(fhssHits24));
#endif
    memset(fhssHitsSubGHz, 0, sizeof(fhssHitsSubGHz));
}

void cadScannerInit() {
    memset(&subGHzTracker, 0, sizeof(subGHzTracker));
#ifdef BOARD_T3S3_LR1121
    memset(&band24Tracker, 0, sizeof(band24Tracker));
    memset(fhssHits24, 0, sizeof(fhssHits24));
#endif
    memset(ambientTaps, 0, sizeof(ambientTaps));
    memset(fhssHitsSubGHz, 0, sizeof(fhssHitsSubGHz));
    ambientTapCount = 0;
    warmupCycleCount = 0;
    warmupComplete = false;
    cadHwFaultFlag = false;
    cadErrorsThisCycle = 0;
    cadProbesThisCycle = 0;
    cadFirstError = 0;
    consecutiveFaultCycles = 0;
    consecutiveCleanCycles = 0;
    currentRadioMode = MODE_UNKNOWN;
    rotSF6 = rotSF7 = rotSF8 = rotSF9 = rotSF10 = rotSF11 = rotSF12 = rotFSK = 0;
}

static CadTap* findTap(BandTracker& t, float freq, uint8_t sf) {
    for (int i = 0; i < MAX_TAPS; i++) {
        if (t.taps[i].active &&
            t.taps[i].sf == sf &&
            fabsf(t.taps[i].frequency - freq) < TAP_FREQ_TOL) {
            return &t.taps[i];
        }
    }
    return nullptr;
}

static CadTap* addTap(BandTracker& t, float freq, uint8_t sf, RfBand band = BAND_SUB_GHZ) {
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!t.taps[i].active) {
            t.taps[i].frequency = freq;
            t.taps[i].sf = sf;
            t.taps[i].band = band;
            t.taps[i].isFsk = false;
            t.taps[i].isAmbient = false;
            t.taps[i].consecutiveHits = 1;
            t.taps[i].missCount = 0;
            t.taps[i].firstSeenMs = millis();
            t.taps[i].firstConfirmedMs = 0;
            t.taps[i].lastSeenMs = millis();
            t.taps[i].active = true;
            return &t.taps[i];
        }
    }
    return nullptr;
}

static CadTap* addFskTap(BandTracker& t, float freq) {
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!t.taps[i].active) {
            t.taps[i].frequency = freq;
            t.taps[i].sf = 0;
            t.taps[i].isFsk = true;
            t.taps[i].isAmbient = false;
            t.taps[i].consecutiveHits = 1;
            t.taps[i].missCount = 0;
            t.taps[i].firstSeenMs = millis();
            t.taps[i].firstConfirmedMs = 0;
            t.taps[i].lastSeenMs = millis();
            t.taps[i].active = true;
            return &t.taps[i];
        }
    }
    return nullptr;
}

static void tapHit(CadTap* tap) {
    if (tap->consecutiveHits < 255) tap->consecutiveHits++;
    if (tap->firstConfirmedMs == 0 && tap->consecutiveHits >= TAP_CONFIRM_HITS) {
        tap->firstConfirmedMs = millis();
    }
    tap->missCount = 0;
    tap->lastSeenMs = millis();
}

static void tapMiss(CadTap* tap) {
    tap->missCount++;
    if (tap->missCount >= TAP_EXPIRE_MISSES) {
        tap->active = false;
    }
}

static void countConfirmed(BandTracker& t, int& cadCount, int& fskCount, int& strongPending, int& pending, int& totalActive) {
    cadCount = 0; fskCount = 0; strongPending = 0; pending = 0; totalActive = 0;
    unsigned long now = millis();
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!t.taps[i].active) continue;
        // Skip ambient taps from all counts that feed threat escalation
        if (t.taps[i].isAmbient) continue;
        totalActive++;
        // Time-based persistence: a tap only counts as confirmed when it has
        // held the confirmation state for PERSISTENCE_MIN_MS wall-clock time.
        // Gives board-parity timing regardless of scan cycle speed.
        bool timeConfirmed = (t.taps[i].firstConfirmedMs != 0) &&
                             ((now - t.taps[i].firstConfirmedMs) >= PERSISTENCE_MIN_MS);
        if (t.taps[i].consecutiveHits >= TAP_CONFIRM_HITS && timeConfirmed) {
            if (t.taps[i].isFsk) fskCount++;
            else cadCount++;
        } else if (t.taps[i].consecutiveHits >= 2 && !t.taps[i].isFsk) {
            strongPending++;
        } else {
            pending++;
        }
    }
}

// Populate a CadBandSummary from a tracker at the end of a scan cycle.
static void fillBandSummary(BandTracker& t, CadBandSummary& s) {
    countConfirmed(t, s.confirmedCadCount, s.confirmedFskCount,
                   s.strongPendingCad, s.pendingTaps, s.totalActiveTaps);
    s.diversityCount = countDiversity(t, DIVERSITY_WINDOW_MS);
    s.persistentDiversityCount = countPersistentDiversity(t, DIVERSITY_WINDOW_MS);
    s.diversityVelocity = computeDiversityVelocity(t);
    s.sustainedCycles = getSustainedDiversityCycles(t);
}

// ── LoRa ↔ FSK mode switching ────────────────────────────────────────────────
// Must use full begin()/beginFSK() to properly initialize RadioLib's internal
// modem state. Raw SPI packet type switches leave LoRa params (codingRate,
// ldrOptimize) undefined, causing broken CAD detection.

#ifndef BOARD_T3S3_LR1121

static void ensureLoRa(SX1262& radio) {
    if (currentRadioMode == MODE_LORA) return;
    radio.standby();
    uint8_t loraType = RADIOLIB_SX126X_PACKET_TYPE_LORA;
    radioMod.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_PACKET_TYPE, &loraType, 1);

    radio.setCodingRate(5);
    radio.setSpreadingFactor(6);
    radio.setBandwidth(500.0);
    radio.setSyncWord(RADIOLIB_SX126X_SYNC_WORD_PRIVATE);
    radio.setPreambleLength(8);
    currentRadioMode = MODE_LORA;
}

static void ensureFSK(SX1262& radio) {
    if (currentRadioMode == MODE_GFSK) return;
    radio.standby();
    uint8_t fskType = RADIOLIB_SX126X_PACKET_TYPE_GFSK;
    radioMod.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_PACKET_TYPE, &fskType, 1);
    radio.setBitRate(4.8);
    radio.setFrequencyDeviation(5.0);
    radio.setRxBandwidth(234.3);
    radio.setFrequency(860.0);
    currentRadioMode = MODE_GFSK;
}

#endif

// ── Scan implementation ─────────────────────────────────────────────────────

#ifdef BOARD_T3S3_LR1121

// ── LR1121 mode switching — lightweight packet-type switch ────────────────
// Uses setPacketType + parameter setters instead of begin()/beginGFSK().
// Avoids the full hardware reset + recalibration that begin() triggers via
// modSetup() → findChip() → reset(). Saves ~700ms per cycle.

static void ensureLoRa_LR(LR1121_RSSI& radio) {
    if (currentRadioMode == MODE_LORA) return;
    radio.standby();
    radio.setPacketTypeDirect(RADIOLIB_LR11X0_PACKET_TYPE_LORA);
    radio.setBandwidth(500.0);
    radio.setSpreadingFactor(6);
    radio.setCodingRate(5);
    radio.setSyncWord(RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE);
    radio.setPreambleLength(8);
    currentRadioMode = MODE_LORA;
}

static void ensureGFSK_LR(LR1121_RSSI& radio) {
    if (currentRadioMode == MODE_GFSK) return;
    radio.standby();
    radio.setPacketTypeDirect(RADIOLIB_LR11X0_PACKET_TYPE_GFSK);
    radio.setBitRate(4.8);
    radio.setFrequencyDeviation(50.0);
    radio.setRxBandwidth(156.2);
    radio.setFrequency(SCAN_FREQ_START, true);
    currentRadioMode = MODE_GFSK;
}

// 2.4 GHz channel helpers
static const int ELRS_24_CHANNELS = 80;  // 2400-2479 MHz at 1 MHz steps (ELRS 2.4 hop grid)
static float elrs24Freq(int ch) { return 2400.0f + (ch * 1.0f); }

// 2.4 GHz CAD channel allocation — same scan budget as before (20/10/5),
// but now rotated across 80 channels instead of 40, giving broader coverage
// over more cycles at the cost of slower per-channel revisit.
static const int CAD_24_SF6  = 20;
static const int CAD_24_SF7  = 10;
static const int CAD_24_SF8  = 5;
static uint32_t rot24SF6 = 0;
static uint32_t rot24SF7 = 0;
static uint32_t rot24SF8 = 0;

CadFskResult cadFskScan(LR1121_RSSI& radio, uint32_t cycleNum, const ScanResult* rssi) {
    CadFskResult result = {};
    latestSweep = rssi;
    cadErrorsThisCycle = 0;
    cadProbesThisCycle = 0;
    cadFirstError = 0;

    diversityCycleUpdate(subGHzTracker);
    diversityCycleUpdate(band24Tracker);
    pruneExpiredDiversity(subGHzTracker);
    pruneExpiredDiversity(band24Tracker);
    // FHSS window is time-based now — hits expire by timestamp, no cycle advance needed

    // ── Enter LoRa mode for CAD scanning ──────────────────────────────
    ensureLoRa_LR(radio);

    // ── PHASE 1: Priority re-check active sub-GHz LoRa taps ──────────
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!subGHzTracker.taps[i].active || subGHzTracker.taps[i].isFsk) continue;
        radio.setSpreadingFactor(subGHzTracker.taps[i].sf);
        ChannelScanConfig_t cfg = buildCadConfigLR(subGHzTracker.taps[i].sf);
        radio.setFrequency(subGHzTracker.taps[i].frequency);
        int16_t cadResult = radio.scanChannel(cfg);
        countCadProbe(cadResult);
        if (cadResult == RADIOLIB_LORA_DETECTED) {
            tapHit(&subGHzTracker.taps[i]);
            if (subGHzTracker.taps[i].consecutiveHits >= 2 &&
                !isAmbientFrequency(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf))
                recordDiversityHit(subGHzTracker, subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf);
        } else {
            // Adjacent channel check
            bool adjHit = false;
            float spacing = 0.325f;
            for (float delta : {-spacing, spacing}) {
                float adjFreq = subGHzTracker.taps[i].frequency + delta;
                if (adjFreq >= 902.0f && adjFreq <= 928.0f) {
                    radio.setFrequency(adjFreq);
                    int16_t adjResult = radio.scanChannel(cfg);
                    countCadProbe(adjResult);
                    if (adjResult == RADIOLIB_LORA_DETECTED) {
                        tapHit(&subGHzTracker.taps[i]);
                        if (subGHzTracker.taps[i].consecutiveHits >= 2 &&
                            !isAmbientFrequency(adjFreq, subGHzTracker.taps[i].sf))
                            recordDiversityHit(subGHzTracker, adjFreq, subGHzTracker.taps[i].sf);
                        adjHit = true;
                        break;
                    }
                }
            }
            if (!adjHit) tapMiss(&subGHzTracker.taps[i]);
        }
    }

    // ── PHASE 1.5: RSSI-guided CAD on elevated US-band bins ──────────
    if (rssi != nullptr && rssi->sweepTimeMs > 0) {
        int startBin = (int)((902.0f - SCAN_FREQ_START) / SCAN_FREQ_STEP);
        int endBin   = (int)((928.0f - SCAN_FREQ_START) / SCAN_FREQ_STEP);
        if (endBin >= SCAN_BIN_COUNT) endBin = SCAN_BIN_COUNT - 1;

        float nf = -120.0f;
        for (int step = startBin; step <= endBin; step += 11) {
            float c = rssi->rssi[step];
            int below = 0, equal = 0;
            for (int j = startBin; j <= endBin; j += 3) {
                if (rssi->rssi[j] < c) below++;
                else if (rssi->rssi[j] == c) equal++;
            }
            int total = (endBin - startBin) / 3 + 1;
            if (below <= total / 2 && (below + equal) > total / 2) { nf = c; break; }
        }

        float thresh = nf + RSSI_GUIDED_THRESH_DB;
        int guidedScans = 0;
        radio.setSpreadingFactor(6);
        ChannelScanConfig_t cfg6 = buildCadConfigLR(6);

        for (int bin = startBin; bin <= endBin && guidedScans < RSSI_GUIDED_MAX_BINS; bin++) {
            if (rssi->rssi[bin] > thresh) {
                float freq = SCAN_FREQ_START + (bin * SCAN_FREQ_STEP);
                radio.setFrequency(freq);
                int16_t cadResult = radio.scanChannel(cfg6);
                countCadProbe(cadResult);
                if (cadResult == RADIOLIB_LORA_DETECTED) {
                    CadTap* existing = findTap(subGHzTracker, freq, 6);
                    if (existing) {
                        tapHit(existing);
                        if (existing->consecutiveHits >= 2 &&
                            !isAmbientFrequency(freq, 6))
                            recordDiversityHit(subGHzTracker, freq, 6);
                    } else {
                        addTap(subGHzTracker, freq, 6, BAND_SUB_GHZ);
                    }
                }
                guidedScans++;
            }
        }
    }

    // ── PHASE 2a: Broad sub-GHz CAD scan — SF6-SF12 rotating channels ─
    // Count confirmed taps FROM THIS CYCLE's Phase 1 hits so pursuit mode
    // reacts on the same cycle the drone is first seen (was 1 cycle late).
    int phase2aCadCount, phase2aFskCount, phase2aStrongPending, phase2aPending, phase2aTotal;
    countConfirmed(subGHzTracker, phase2aCadCount, phase2aFskCount,
                   phase2aStrongPending, phase2aPending, phase2aTotal);
    static bool lastPursuitMode = false;
    bool pursuitMode = (countDiversity(subGHzTracker, DIVERSITY_WINDOW_MS) >= DIVERSITY_WARNING)
                       || (phase2aCadCount > 0);
    if (pursuitMode && !lastPursuitMode)
        Serial.println("[PURSUIT] Activated");
    else if (!pursuitMode && lastPursuitMode)
        Serial.println("[PURSUIT] Deactivated");
    lastPursuitMode = pursuitMode;

    auto sfHasActiveTapsSub = [](uint8_t sf) -> bool {
        for (int i = 0; i < MAX_TAPS; i++) {
            if (subGHzTracker.taps[i].active && !subGHzTracker.taps[i].isFsk && subGHzTracker.taps[i].sf == sf)
                return true;
        }
        return false;
    };

    int sf6ch  = pursuitMode && sfHasActiveTapsSub(6)  ? 80 : CAD_CH_SF6;
    int sf7ch  = pursuitMode && sfHasActiveTapsSub(7)  ? 60 : CAD_CH_SF7;
    int sf8ch  = pursuitMode && sfHasActiveTapsSub(8)  ? 30 : CAD_CH_SF8;
    int sf9ch  = pursuitMode && !sfHasActiveTapsSub(9)  ? 2 : CAD_CH_SF9;
    int sf10ch = pursuitMode && !sfHasActiveTapsSub(10) ? 1 : CAD_CH_SF10;
    int sf11ch = pursuitMode && !sfHasActiveTapsSub(11) ? 1 : CAD_CH_SF11;
    int sf12ch = pursuitMode && !sfHasActiveTapsSub(12) ? 1 : CAD_CH_SF12;

    struct SFScan {
        uint8_t sf; int chCount; int totalCh; uint32_t* rot;
        float (*fn)(int);
    };

    SFScan sfScans[] = {
        { 6,  sf6ch,  ELRS_915_CHANNELS, &rotSF6,  elrs915Freq },
        { 7,  sf7ch,  ELRS_915_CHANNELS, &rotSF7,  elrs915Freq },
        { 8,  sf8ch,  ELRS_915_CHANNELS, &rotSF8,  elrs915Freq },
        { 9,  sf9ch,  ELRS_915_CHANNELS, &rotSF9,  elrs915Freq },
        { 10, sf10ch, ELRS_915_CHANNELS, &rotSF10, elrs915Freq },
        { 11, sf11ch, ELRS_868_CHANNELS, &rotSF11, elrs868Freq },
        { 12, sf12ch, ELRS_915_CHANNELS, &rotSF12, elrs915Freq },
    };

    for (int s = 0; s < 7; s++) {
        SFScan& sc = sfScans[s];
        radio.setSpreadingFactor(sc.sf);
        ChannelScanConfig_t cfg = buildCadConfigLR(sc.sf);

        int stride = sc.totalCh / sc.chCount;
        if (stride < 1) stride = 1;
        int offset = (*sc.rot) % stride;
        (*sc.rot)++;

        for (int i = 0; i < sc.chCount; i++) {
            int ch = (offset + i * stride) % sc.totalCh;
            float freq = sc.fn(ch);
            radio.setFrequency(freq);

            int16_t cadResult = radio.scanChannel(cfg);
            countCadProbe(cadResult);
            if (cadResult == RADIOLIB_LORA_DETECTED) {
                CadTap* existing = findTap(subGHzTracker, freq, sc.sf);
                if (!existing) addTap(subGHzTracker, freq, sc.sf, BAND_SUB_GHZ);
                if (existing && existing->consecutiveHits >= 2 &&
                    !isAmbientFrequency(freq, sc.sf))
                    recordDiversityHit(subGHzTracker, freq, sc.sf);
                fhssRecordHitSubGHz(freq, sc.sf);
            }
        }
    }

    // Flush sub-GHz FHSS frequency-spread tracker before moving to 2.4 GHz.
    fhssFlushSpread();

    // ── PHASE 2b: 2.4 GHz CAD scan — SF6-SF8, BW800 ──────────────────
    // LR1121 handles band switching transparently via setFrequency().
    // The `true` (high-band) arg is REQUIRED — without it, the driver
    // validates bw against the 0-510 kHz sub-GHz range and silently
    // rejects BW800, leaving the modem at the previous BW (BW500). This
    // was a hidden bug until the 2026-04-11 audit: 2.4 GHz CAD was
    // running at BW500 and missing every BW800 signal (ELRS 2.4 / Ghost /
    // Tracer all use BW800). See LR11x0.cpp:516 for the range check.
    radio.setBandwidth(812.5, true);

    // Re-check active 2.4 GHz taps first (band24Tracker contains only 2.4 GHz)
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!band24Tracker.taps[i].active || band24Tracker.taps[i].isFsk) continue;
        radio.setSpreadingFactor(band24Tracker.taps[i].sf);
        ChannelScanConfig_t cfg24 = buildCadConfigLR(band24Tracker.taps[i].sf);
        radio.setFrequency(band24Tracker.taps[i].frequency);
        int16_t cadResult = radio.scanChannel(cfg24);
        countCadProbe(cadResult);
        if (cadResult == RADIOLIB_LORA_DETECTED) {
            tapHit(&band24Tracker.taps[i]);
            if (band24Tracker.taps[i].consecutiveHits >= 2)
                recordDiversityHit(band24Tracker, band24Tracker.taps[i].frequency, band24Tracker.taps[i].sf);
        } else {
            tapMiss(&band24Tracker.taps[i]);
        }
    }

    // Broad 2.4 GHz scan — SF6/SF7/SF8 rotating, every 3rd cycle only.
    // Drone signals persist for seconds, not milliseconds, so per-cycle
    // coverage isn't needed. Skipping 2 of 3 cycles saves ~280ms per skip.
    // Active 2.4 GHz taps (re-checked above) still run every cycle.
    if (cycleNum % 3 == 0) {
    SFScan sf24Scans[] = {
        { 6, CAD_24_SF6, ELRS_24_CHANNELS, &rot24SF6, elrs24Freq },
        { 7, CAD_24_SF7, ELRS_24_CHANNELS, &rot24SF7, elrs24Freq },
        { 8, CAD_24_SF8, ELRS_24_CHANNELS, &rot24SF8, elrs24Freq },
    };

    for (int s = 0; s < 3; s++) {
        SFScan& sc = sf24Scans[s];
        radio.setSpreadingFactor(sc.sf);
        ChannelScanConfig_t cfg24b = buildCadConfigLR(sc.sf);

        int stride = sc.totalCh / sc.chCount;
        if (stride < 1) stride = 1;
        int offset = (*sc.rot) % stride;
        (*sc.rot)++;

        for (int i = 0; i < sc.chCount; i++) {
            int ch = (offset + i * stride) % sc.totalCh;
            float freq = sc.fn(ch);
            radio.setFrequency(freq);

            int16_t cadResult = radio.scanChannel(cfg24b);
            countCadProbe(cadResult);
            if (cadResult == RADIOLIB_LORA_DETECTED) {
                CadTap* existing = findTap(band24Tracker, freq, sc.sf);
                if (!existing) addTap(band24Tracker, freq, sc.sf, BAND_2G4);
                if (existing && existing->consecutiveHits >= 2)
                    recordDiversityHit(band24Tracker, freq, sc.sf);
                // Feed 2.4 GHz CAD hits into the 2.4 GHz FHSS spread tracker.
                fhssRecordHit24(freq, sc.sf);
            }
        }
    }
    }

    // Flush 2.4 GHz FHSS frequency-spread tracker.
    fhssFlushSpread24();

    // ── PHASE 4: Switch back to GFSK for RSSI sweep ──────────────────
    ensureGFSK_LR(radio);

    // ── PHASE 3: FSK Crossfire scan ──────────────────────────────────
    // Radio is in GFSK mode. Reconfigure for Crossfire 150 Hz detection.
    // FSK taps live in subGHzTracker (Crossfire is ~900 MHz band).
    {
        int16_t fskErr = radio.setBitRate(85.1);
        if (fskErr == RADIOLIB_ERR_NONE) {
            radio.setFrequencyDeviation(25.0);
            radio.setRxBandwidth(117.3);

            // Re-check existing FSK taps
            for (int i = 0; i < MAX_TAPS; i++) {
                if (!subGHzTracker.taps[i].active || !subGHzTracker.taps[i].isFsk) continue;
                radio.setFrequency(subGHzTracker.taps[i].frequency);
                radio.startReceive();
                delayMicroseconds(FSK_DWELL_US);
                float r = radio.getInstantRSSI();
                if (r > FSK_DETECT_THRESHOLD_DBM) tapHit(&subGHzTracker.taps[i]);
                else tapMiss(&subGHzTracker.taps[i]);
            }

            // Scan new Crossfire channels (rotating)
            int stride = CRSF_CHANNELS / FSK_CH;
            if (stride < 1) stride = 1;
            int offset = rotFSK % stride;
            rotFSK++;

            for (int i = 0; i < FSK_CH; i++) {
                int ch = (offset + i * stride) % CRSF_CHANNELS;
                float freq = crsfFskFreq(ch);
                radio.setFrequency(freq);
                radio.startReceive();
                delayMicroseconds(FSK_DWELL_US);

                float r = radio.getInstantRSSI();
                if (r > FSK_DETECT_THRESHOLD_DBM) {
                    CadTap* existing = findTap(subGHzTracker, freq, 0);
                    if (existing) tapHit(existing);
                    else addFskTap(subGHzTracker, freq);
                }
            }
        }

        // Restore RSSI sweep GFSK params
        radio.setBitRate(4.8);
        radio.setFrequencyDeviation(50.0);
        radio.setRxBandwidth(156.2);
        radio.setRxBoostedGainMode(true);
    }

    // ── Warmup and ambient learning (shared with SX1262) ─────────────
    warmupCycleCount++;

    // Need current totalActiveTaps (across both bands) for early-exit check
    int totalActiveBothBands = 0;
    {
        int tmpCad, tmpFsk, tmpSP, tmpP, tmpTA;
        countConfirmed(subGHzTracker, tmpCad, tmpFsk, tmpSP, tmpP, tmpTA);
        totalActiveBothBands += tmpTA;
        countConfirmed(band24Tracker, tmpCad, tmpFsk, tmpSP, tmpP, tmpTA);
        totalActiveBothBands += tmpTA;
    }

    if (!warmupComplete) {
        bool normalEnd = (millis() >= AMBIENT_WARMUP_MS);
        bool earlyExit = (millis() >= AMBIENT_EARLY_EXIT_MS) && (totalActiveBothBands == 0)
                         && (ambientTapCount == 0);
        if (normalEnd || earlyExit) {
            warmupComplete = true;
            if (earlyExit && !normalEnd) {
                Serial.printf("[WARMUP] Early completion at %lus — clean environment. ",
                              millis() / 1000);
            }
            Serial.printf("[WARMUP] Complete after %u cycles (%lus). %u ambient taps recorded:\n",
                          warmupCycleCount, millis() / 1000, ambientTapCount);
            for (uint8_t i = 0; i < ambientTapCount; i++) {
                if (ambientTaps[i].active) {
                    Serial.printf("  - %.1f MHz / SF%u (first seen cycle %u)\n",
                                  ambientTaps[i].frequency, ambientTaps[i].sf,
                                  ambientTaps[i].firstSeenCycle);
                }
            }
        } else {
            for (int i = 0; i < MAX_TAPS; i++) {
                if (subGHzTracker.taps[i].active && !subGHzTracker.taps[i].isFsk) {
                    recordAmbientTap(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf, true);
                }
                if (band24Tracker.taps[i].active && !band24Tracker.taps[i].isFsk) {
                    recordAmbientTap(band24Tracker.taps[i].frequency, band24Tracker.taps[i].sf, true);
                }
            }
        }
    }

    // Post-warmup continuous ambient learning
    if (warmupComplete) {
        unsigned long now = millis();
        BandTracker* trackers[] = { &subGHzTracker, &band24Tracker };
        for (BandTracker* tp : trackers) {
            BandTracker& tk = *tp;
            for (int i = 0; i < MAX_TAPS; i++) {
                if (tk.taps[i].active &&
                    !tk.taps[i].isFsk &&
                    tk.taps[i].consecutiveHits >= TAP_CONFIRM_HITS &&
                    !isAmbientCadSource(tk.taps[i].frequency, tk.taps[i].sf)) {
                    if (tk.taps[i].firstSeenMs < AMBIENT_WARMUP_MS) {
                        recordAmbientTap(tk.taps[i].frequency, tk.taps[i].sf, true);
                    }
                    else if ((now - tk.taps[i].firstSeenMs) > AMBIENT_AUTOLEARN_MS) {
                        recordAmbientTap(tk.taps[i].frequency, tk.taps[i].sf, false);
                    }
                }
            }
        }
    }

    // Tag taps with ambient status
    if (warmupComplete) {
        BandTracker* trackers[] = { &subGHzTracker, &band24Tracker };
        for (BandTracker* tp : trackers) {
            BandTracker& tk = *tp;
            for (int i = 0; i < MAX_TAPS; i++) {
                if (tk.taps[i].active) {
                    tk.taps[i].isAmbient = isAmbientCadSource(tk.taps[i].frequency, tk.taps[i].sf);
                }
            }
        }
    }

    if (cadErrorsThisCycle > 0) {
        Serial.printf("[CAD] %u/%u probes returned errors (first=%d)\n", cadErrorsThisCycle, cadProbesThisCycle, cadFirstError);
    }
    {
        bool faultThisCycle = (cadProbesThisCycle > 0) && (cadErrorsThisCycle > cadProbesThisCycle / 2);
        if (faultThisCycle) {
            consecutiveCleanCycles = 0;
            if (consecutiveFaultCycles < 255) consecutiveFaultCycles++;
            if (consecutiveFaultCycles >= 3) cadHwFaultFlag = true;
        } else {
            consecutiveFaultCycles = 0;
            if (consecutiveCleanCycles < 255) consecutiveCleanCycles++;
            if (consecutiveCleanCycles >= 3) cadHwFaultFlag = false;
        }
    }

    // Fill per-band summaries from each tracker
    fillBandSummary(subGHzTracker, result.subGHz);
    fillBandSummary(band24Tracker, result.band24);

    // Aggregate totals for backward compat with main.cpp / detection_engine
    result.confirmedCadCount       = result.subGHz.confirmedCadCount       + result.band24.confirmedCadCount;
    result.confirmedFskCount       = result.subGHz.confirmedFskCount       + result.band24.confirmedFskCount;
    result.strongPendingCad        = result.subGHz.strongPendingCad        + result.band24.strongPendingCad;
    result.pendingTaps             = result.subGHz.pendingTaps             + result.band24.pendingTaps;
    result.totalActiveTaps         = result.subGHz.totalActiveTaps         + result.band24.totalActiveTaps;
    result.diversityCount          = result.subGHz.diversityCount          + result.band24.diversityCount;
    result.persistentDiversityCount= result.subGHz.persistentDiversityCount+ result.band24.persistentDiversityCount;
    result.diversityVelocity       = result.subGHz.diversityVelocity       + result.band24.diversityVelocity;
    // Sustained cycles: take the max of the two bands (not sum)
    result.sustainedCycles         = (result.subGHz.sustainedCycles > result.band24.sustainedCycles)
                                     ? result.subGHz.sustainedCycles : result.band24.sustainedCycles;

    return result;
}

#else // SX1262 boards

CadFskResult cadFskScan(SX1262& radio, uint32_t cycleNum, const ScanResult* rssi) {
    CadFskResult result = {};
    latestSweep = rssi;
    cadErrorsThisCycle = 0;
    cadProbesThisCycle = 0;

    // Update persistence counters from previous cycle's hits, then clear flags
    diversityCycleUpdate(subGHzTracker);

    // Housekeeping: prune expired diversity slots
    pruneExpiredDiversity(subGHzTracker);
    // FHSS window is time-based now — hits expire by timestamp, no cycle advance needed

    // Switch from FSK to LoRa packet type via low-level SPI command
    ensureLoRa(radio);

    // ── PHASE 1: Priority re-check active LoRa taps + adjacent channels ──
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!subGHzTracker.taps[i].active) continue;
        if (subGHzTracker.taps[i].isFsk) continue;  // FSK taps re-checked in Phase 3
        radio.setSpreadingFactor(subGHzTracker.taps[i].sf);
        ChannelScanConfig_t cfg = buildCadConfig(subGHzTracker.taps[i].sf);
        radio.setFrequency(subGHzTracker.taps[i].frequency);
        int16_t cadResult = radio.scanChannel(cfg);
        countCadProbe(cadResult);
        if (cadResult == RADIOLIB_LORA_DETECTED) {
            tapHit(&subGHzTracker.taps[i]);
            if (subGHzTracker.taps[i].consecutiveHits >= 2 &&
                !isAmbientFrequency(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf))
                recordDiversityHit(subGHzTracker, subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf);
        } else {
            bool adjHit = false;
            float spacing = 0.325f;
            for (float delta : {-spacing, spacing}) {
                float adjFreq = subGHzTracker.taps[i].frequency + delta;
                if (adjFreq >= 902.0f && adjFreq <= 928.0f) {
                    radio.setFrequency(adjFreq);
                    int16_t adjResult = radio.scanChannel(cfg);
                    countCadProbe(adjResult);
                    if (adjResult == RADIOLIB_LORA_DETECTED) {
                        tapHit(&subGHzTracker.taps[i]);
                        if (subGHzTracker.taps[i].consecutiveHits >= 2 &&
                            !isAmbientFrequency(adjFreq, subGHzTracker.taps[i].sf))
                            recordDiversityHit(subGHzTracker, adjFreq, subGHzTracker.taps[i].sf);
                        adjHit = true;
                        break;
                    }
                }
            }
            if (!adjHit) tapMiss(&subGHzTracker.taps[i]);
        }
    }

    // ── PHASE 1.5: RSSI-guided CAD on elevated US-band bins ─────────────
    // Focus CAD budget where RSSI already shows energy. Even stale RSSI data
    // (1-2 cycles old) is valid because drone signals are persistent.
    if (rssi != nullptr && rssi->sweepTimeMs > 0) {
        int startBin = (int)((902.0f - SCAN_FREQ_START) / SCAN_FREQ_STEP);
        int endBin   = (int)((928.0f - SCAN_FREQ_START) / SCAN_FREQ_STEP);
        if (endBin >= SCAN_BIN_COUNT) endBin = SCAN_BIN_COUNT - 1;

        // Quick noise floor estimate from the US sub-band
        float nf = -120.0f;
        for (int step = startBin; step <= endBin; step += 11) {
            float c = rssi->rssi[step];
            int below = 0, equal = 0;
            for (int j = startBin; j <= endBin; j += 3) {
                if (rssi->rssi[j] < c) below++;
                else if (rssi->rssi[j] == c) equal++;
            }
            int total = (endBin - startBin) / 3 + 1;
            if (below <= total / 2 && (below + equal) > total / 2) { nf = c; break; }
        }

        float thresh = nf + RSSI_GUIDED_THRESH_DB;
        int guidedScans = 0;
        radio.setSpreadingFactor(6);
        ChannelScanConfig_t cfg6 = buildCadConfig(6);

        for (int bin = startBin; bin <= endBin && guidedScans < RSSI_GUIDED_MAX_BINS; bin++) {
            if (rssi->rssi[bin] > thresh) {
                float freq = SCAN_FREQ_START + (bin * SCAN_FREQ_STEP);
                radio.setFrequency(freq);
                int16_t cadResult = radio.scanChannel(cfg6);
                countCadProbe(cadResult);
                if (cadResult == RADIOLIB_LORA_DETECTED) {
                    CadTap* existing = findTap(subGHzTracker, freq, 6);
                    if (existing) {
                        tapHit(existing);
                        if (existing->consecutiveHits >= 2 &&
                            !isAmbientFrequency(freq, 6))
                            recordDiversityHit(subGHzTracker, freq, 6);
                    } else {
                        addTap(subGHzTracker, freq, 6);
                    }
                }
                guidedScans++;
            }
        }
    }

    // ── PHASE 2: Broad CAD scan — all SF values, rotating channels ──────
    // Pursuit mode: when a drone is detected, focus on active SFs at max coverage.
    // Count confirmed taps FROM THIS CYCLE's Phase 1 hits so pursuit mode reacts
    // on the same cycle the drone is first seen (was 1 cycle late).
    int phase2CadCount, phase2FskCount, phase2StrongPending, phase2Pending, phase2Total;
    countConfirmed(subGHzTracker, phase2CadCount, phase2FskCount,
                   phase2StrongPending, phase2Pending, phase2Total);
    static bool lastPursuitMode = false;
    bool pursuitMode = (countDiversity(subGHzTracker, DIVERSITY_WINDOW_MS) >= DIVERSITY_WARNING)
                       || (phase2CadCount > 0);
    if (pursuitMode && !lastPursuitMode)
        Serial.println("[PURSUIT] Activated — focusing scan on active SFs");
    else if (!pursuitMode && lastPursuitMode)
        Serial.println("[PURSUIT] Deactivated — normal scan");
    lastPursuitMode = pursuitMode;

    struct SFScan {
        uint8_t sf; int chCount; int totalCh; uint32_t* rot;
        float (*fn)(int);
    };

    // In pursuit mode, SFs WITH active taps get more channels; inactive SFs
    // get reduced. This prevents losing a drone on SF9 when pursuit fires.
    auto sfHasActiveTapsSub = [](uint8_t sf) -> bool {
        for (int i = 0; i < MAX_TAPS; i++) {
            if (subGHzTracker.taps[i].active && !subGHzTracker.taps[i].isFsk && subGHzTracker.taps[i].sf == sf)
                return true;
        }
        return false;
    };

    int sf6ch  = CAD_CH_SF6;
    int sf7ch  = CAD_CH_SF7;
    int sf8ch  = CAD_CH_SF8;
    int sf9ch  = CAD_CH_SF9;
    int sf10ch = CAD_CH_SF10;
    int sf11ch = CAD_CH_SF11;
    int sf12ch = CAD_CH_SF12;

    if (pursuitMode) {
        // Boost SFs with active taps, reduce only inactive SFs
        sf6ch  = sfHasActiveTapsSub(6)  ? 80 : CAD_CH_SF6;
        sf7ch  = sfHasActiveTapsSub(7)  ? 60 : CAD_CH_SF7;
        sf8ch  = sfHasActiveTapsSub(8)  ? 30 : CAD_CH_SF8;
        sf9ch  = sfHasActiveTapsSub(9)  ? CAD_CH_SF9  : 2;
        sf10ch = sfHasActiveTapsSub(10) ? CAD_CH_SF10 : 1;
        sf11ch = sfHasActiveTapsSub(11) ? CAD_CH_SF11 : 1;
        sf12ch = sfHasActiveTapsSub(12) ? CAD_CH_SF12 : 1;
    }

    SFScan sfScans[] = {
        { 6,  sf6ch,  ELRS_915_CHANNELS, &rotSF6,  elrs915Freq },
        { 7,  sf7ch,  ELRS_915_CHANNELS, &rotSF7,  elrs915Freq },
        { 8,  sf8ch,  ELRS_915_CHANNELS, &rotSF8,  elrs915Freq },
        { 9,  sf9ch,  ELRS_915_CHANNELS, &rotSF9,  elrs915Freq },
        { 10, sf10ch, ELRS_915_CHANNELS, &rotSF10, elrs915Freq },
        { 11, sf11ch, ELRS_868_CHANNELS, &rotSF11, elrs868Freq },
        { 12, sf12ch, ELRS_915_CHANNELS, &rotSF12, elrs915Freq },
    };

    for (int s = 0; s < 7; s++) {
        SFScan& sc = sfScans[s];
        radio.setSpreadingFactor(sc.sf);
        ChannelScanConfig_t cfg = buildCadConfig(sc.sf);

#ifdef DEBUG_CAD_PARAMS
        uint8_t idx = sc.sf - 6;
        Serial.printf("[CAD-PARAMS] SF%u: sym=0x%02X peak=%u min=%u\n",
                      sc.sf, cadParams[idx].symbolNum,
                      cadParams[idx].detPeak, cadParams[idx].detMin);
#endif

        // Sorted monotonic sweep: channels are scanned in ascending frequency
        // order to minimize PLL settling time between hops.
        int stride = sc.totalCh / sc.chCount;
        if (stride < 1) stride = 1;
        int offset = (*sc.rot) % stride;
        (*sc.rot)++;

        for (int i = 0; i < sc.chCount; i++) {
            int ch = (offset + i * stride) % sc.totalCh;
            float freq = sc.fn(ch);
            radio.setFrequency(freq);

            int16_t cadResult = radio.scanChannel(cfg);
            countCadProbe(cadResult);
            if (cadResult == RADIOLIB_LORA_DETECTED) {
                CadTap* existing = findTap(subGHzTracker, freq, sc.sf);
                if (!existing) addTap(subGHzTracker, freq, sc.sf);
                if (existing && existing->consecutiveHits >= 2 &&
                    !isAmbientFrequency(freq, sc.sf))
                    recordDiversityHit(subGHzTracker, freq, sc.sf);
                fhssRecordHitSubGHz(freq, sc.sf);
            }
        }
    }

    // Flush FHSS frequency-spread tracker (see LR1121 path for rationale).
    fhssFlushSpread();

    // ── PHASE 4: Switch back to FSK for next RSSI sweep ────────────────
    ensureFSK(radio);

    // ── PHASE 3: FSK Crossfire scan ────────────────────────────────────
    // Runs AFTER ensureFSK() — radio is already in FSK mode.
    // This avoids the LoRa→FSK→LoRa sandwich that corrupted RadioLib
    // internal state. Sequence is now FSK→FSK(Crossfire)→FSK(RSSI).
    {
        // Reconfigure for Crossfire 150 Hz: 85.1 kbps, ~25 kHz deviation
        int16_t fskErr = radio.setBitRate(85.1);
        if (fskErr == RADIOLIB_ERR_NONE) {
            radio.setFrequencyDeviation(25.0);
            radio.setRxBandwidth(117.3);

            // Re-check existing FSK taps
            for (int i = 0; i < MAX_TAPS; i++) {
                if (!subGHzTracker.taps[i].active || !subGHzTracker.taps[i].isFsk) continue;
                radio.setFrequency(subGHzTracker.taps[i].frequency);
                radio.startReceive();
                delayMicroseconds(FSK_DWELL_US);
                float r = radio.getRSSI(false);
                if (r > FSK_DETECT_THRESHOLD_DBM) tapHit(&subGHzTracker.taps[i]);
                else tapMiss(&subGHzTracker.taps[i]);
            }

            // Scan new Crossfire channels (rotating)
            int stride = CRSF_CHANNELS / FSK_CH;
            if (stride < 1) stride = 1;
            int offset = rotFSK % stride;
            rotFSK++;

            for (int i = 0; i < FSK_CH; i++) {
                int ch = (offset + i * stride) % CRSF_CHANNELS;
                float freq = crsfFskFreq(ch);

                radio.setFrequency(freq);
                radio.startReceive();
                delayMicroseconds(FSK_DWELL_US);

                float r = radio.getRSSI(false);
                if (r > FSK_DETECT_THRESHOLD_DBM) {
                    CadTap* existing = findTap(subGHzTracker, freq, 0);
                    if (existing) tapHit(existing);
                    else addFskTap(subGHzTracker, freq);
                }
            }
        }
        // else: 85.1 kbps rejected — skip FSK scan, just restore params

        // Restore RSSI sweep FSK params + boosted gain
        radio.setBitRate(4.8);
        radio.setFrequencyDeviation(5.0);
        radio.setRxBandwidth(234.3);
        radio.setRxBoostedGainMode(true);  // after BW restore, not before Phase 3
    }

    // ── Warmup: record taps as ambient LoRa sources ──────────────────────
    warmupCycleCount++;

    // Compute current total for early-exit gate
    int earlyExitTotal = 0;
    {
        int tmpCad, tmpFsk, tmpSP, tmpP, tmpTA;
        countConfirmed(subGHzTracker, tmpCad, tmpFsk, tmpSP, tmpP, tmpTA);
        earlyExitTotal = tmpTA;
    }

    if (!warmupComplete) {
        bool normalEnd = (millis() >= AMBIENT_WARMUP_MS);
        // Early exit: after minimum warmup, if environment is clean (no active taps),
        // complete early to reduce blind time on power-on in clean field environments.
        bool earlyExit = (millis() >= AMBIENT_EARLY_EXIT_MS) && (earlyExitTotal == 0)
                         && (ambientTapCount == 0);
        if (normalEnd || earlyExit) {
            warmupComplete = true;
            if (earlyExit && !normalEnd) {
                Serial.printf("[WARMUP] Early completion at %lus — clean environment. ",
                              millis() / 1000);
            }
            Serial.printf("[WARMUP] Complete after %u cycles (%lus). %u ambient taps recorded:\n",
                          warmupCycleCount, millis() / 1000, ambientTapCount);
            for (uint8_t i = 0; i < ambientTapCount; i++) {
                if (ambientTaps[i].active) {
                    Serial.printf("  - %.1f MHz / SF%u (first seen cycle %u)\n",
                                  ambientTaps[i].frequency, ambientTaps[i].sf,
                                  ambientTaps[i].firstSeenCycle);
                }
            }
        } else {
            // Still in warmup — record ANY active LoRa tap as infrastructure.
            // Threshold is 1 hit (not 2 or 3) because ambient sources are
            // intermittent and may not accumulate consecutive hits in time.
            for (int i = 0; i < MAX_TAPS; i++) {
                if (subGHzTracker.taps[i].active && !subGHzTracker.taps[i].isFsk) {
                    recordAmbientTap(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf, true);
                }
            }
        }
    }

    // Post-warmup continuous ambient learning (auto-learned, NOT warmup):
    if (warmupComplete) {
        unsigned long now = millis();
        for (int i = 0; i < MAX_TAPS; i++) {
            if (subGHzTracker.taps[i].active &&
                !subGHzTracker.taps[i].isFsk &&
                subGHzTracker.taps[i].consecutiveHits >= TAP_CONFIRM_HITS &&
                !isAmbientCadSource(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf)) {
                if (subGHzTracker.taps[i].firstSeenMs < AMBIENT_WARMUP_MS) {
                    recordAmbientTap(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf, true);
                }
                else if ((now - subGHzTracker.taps[i].firstSeenMs) > AMBIENT_AUTOLEARN_MS) {
                    recordAmbientTap(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf, false);
                }
            }
        }
    }

    // Tag each active tap with its ambient status
    if (warmupComplete) {
        for (int i = 0; i < MAX_TAPS; i++) {
            if (subGHzTracker.taps[i].active) {
                subGHzTracker.taps[i].isAmbient = isAmbientCadSource(subGHzTracker.taps[i].frequency, subGHzTracker.taps[i].sf);
            }
        }
    }

    if (cadErrorsThisCycle > 0) {
        Serial.printf("[CAD] %u/%u probes returned errors (first=%d)\n", cadErrorsThisCycle, cadProbesThisCycle, cadFirstError);
    }
    {
        bool faultThisCycle = (cadProbesThisCycle > 0) && (cadErrorsThisCycle > cadProbesThisCycle / 2);
        if (faultThisCycle) {
            consecutiveCleanCycles = 0;
            if (consecutiveFaultCycles < 255) consecutiveFaultCycles++;
            if (consecutiveFaultCycles >= 3) cadHwFaultFlag = true;
        } else {
            consecutiveFaultCycles = 0;
            if (consecutiveCleanCycles < 255) consecutiveCleanCycles++;
            if (consecutiveCleanCycles >= 3) cadHwFaultFlag = false;
        }
    }

    // Fill the sub-GHz summary (SX1262 has no 2.4 GHz path)
    fillBandSummary(subGHzTracker, result.subGHz);

    // Aggregated totals mirror sub-GHz (no band24 on SX1262)
    result.confirmedCadCount        = result.subGHz.confirmedCadCount;
    result.confirmedFskCount        = result.subGHz.confirmedFskCount;
    result.strongPendingCad         = result.subGHz.strongPendingCad;
    result.pendingTaps              = result.subGHz.pendingTaps;
    result.totalActiveTaps          = result.subGHz.totalActiveTaps;
    result.diversityCount           = result.subGHz.diversityCount;
    result.persistentDiversityCount = result.subGHz.persistentDiversityCount;
    result.diversityVelocity        = result.subGHz.diversityVelocity;
    result.sustainedCycles          = result.subGHz.sustainedCycles;

    return result;
}

#endif
