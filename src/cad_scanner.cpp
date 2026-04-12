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

// ── Tap list (persists across scan cycles) ──────────────────────────────────

static CadTap tapList[MAX_TAPS];

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

struct DiversitySlot {
    float frequency;
    uint8_t sf;
    unsigned long lastHitMs;
    uint8_t consecutiveHits;  // consecutive scan cycles with a hit
    bool hitThisCycle;        // marked during scan, cleared at cycle start
    bool active;
};

static DiversitySlot diversitySlots[MAX_DIVERSITY_SLOTS];

// Sustained-diversity persistence: tracks consecutive cycles with high raw diversity.
// FHSS drones sustain high diversity every cycle; infrastructure spikes briefly.
static int sustainedDiversityCycles = 0;
static int prevSustainedDiversityCycles = 0;  // for velocity: detect threshold crossings

// Diversity velocity: track sustained-diversity threshold crossings per window
static int velocityHistory[DIVERSITY_VELOCITY_WINDOW];
static int velocityIdx = 0;

static void recordDiversityHit(float freq, uint8_t sf) {
    unsigned long now = millis();

    // Check if this frequency/SF already has a slot
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (diversitySlots[i].active &&
            diversitySlots[i].sf == sf &&
            fabsf(diversitySlots[i].frequency - freq) <= TAP_FREQ_TOL) {
            diversitySlots[i].lastHitMs = now;
            diversitySlots[i].hitThisCycle = true;
            return;  // Same frequency — no new diversity
        }
    }

    // New frequency — find an empty slot or evict the oldest
    int bestSlot = 0;
    unsigned long oldestTime = ULONG_MAX;
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (!diversitySlots[i].active) {
            bestSlot = i;
            break;
        }
        if (diversitySlots[i].lastHitMs < oldestTime) {
            oldestTime = diversitySlots[i].lastHitMs;
            bestSlot = i;
        }
    }

    diversitySlots[bestSlot].frequency = freq;
    diversitySlots[bestSlot].sf = sf;
    diversitySlots[bestSlot].lastHitMs = now;
    diversitySlots[bestSlot].consecutiveHits = 0;  // first hit, will become 1 at cycle end
    diversitySlots[bestSlot].hitThisCycle = true;
    diversitySlots[bestSlot].active = true;
}

// Forward declarations for cycle update
static int countDiversity(unsigned long windowMs);

// Call at the START of each scan cycle to update sustained-diversity tracking
// from the previous cycle's diversity count.
static void diversityCycleUpdate() {
    // Update per-slot counters (kept for future use, not used for gate)
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (!diversitySlots[i].active) continue;
        if (diversitySlots[i].hitThisCycle) {
            if (diversitySlots[i].consecutiveHits < 255) diversitySlots[i].consecutiveHits++;
            diversitySlots[i].hitThisCycle = false;
        } else {
            diversitySlots[i].consecutiveHits = 0;
        }
    }

    // Sustained-diversity gate: was last cycle's raw diversity above threshold?
    int rawDiv = countDiversity(DIVERSITY_WINDOW_MS);
    prevSustainedDiversityCycles = sustainedDiversityCycles;
    if (rawDiv >= PERSISTENCE_MIN_DIVERSITY) {
        sustainedDiversityCycles++;
    } else {
        // Drone departure prune: if we had sustained diversity (drone present)
        // and it just collapsed, aggressively clear all non-ambient taps.
        // This prevents confirmed taps from lingering and inflating the score
        // for 30+ seconds after the drone leaves.
        if (sustainedDiversityCycles >= PERSISTENCE_MIN_CONSECUTIVE) {
            for (int i = 0; i < MAX_TAPS; i++) {
                if (tapList[i].active && !tapList[i].isAmbient) {
                    tapList[i].active = false;
                }
            }
        }
        sustainedDiversityCycles = 0;
    }

    // Velocity: did we just cross the sustained threshold this cycle?
    int crossed = 0;
    if (sustainedDiversityCycles == PERSISTENCE_MIN_CONSECUTIVE &&
        prevSustainedDiversityCycles < PERSISTENCE_MIN_CONSECUTIVE) {
        crossed = rawDiv;  // all current diversity just became persistent
    }
    velocityHistory[velocityIdx] = crossed;
    velocityIdx = (velocityIdx + 1) % DIVERSITY_VELOCITY_WINDOW;
}

static void pruneExpiredDiversity() {
    unsigned long now = millis();
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (diversitySlots[i].active &&
            (now - diversitySlots[i].lastHitMs) >= DIVERSITY_WINDOW_MS) {
            diversitySlots[i].active = false;
            diversitySlots[i].consecutiveHits = 0;
        }
    }
}

// Count ALL active diversity slots (raw, for logging)
static int countDiversity(unsigned long windowMs) {
    unsigned long now = millis();
    int count = 0;
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (diversitySlots[i].active &&
            (now - diversitySlots[i].lastHitMs) < windowMs) {
            count++;
        }
    }
    return count;
}

// Sustained-diversity persistent count: if raw diversity has been >= threshold
// for PERSISTENCE_MIN_CONSECUTIVE cycles, ALL current diversity qualifies.
// Otherwise returns 0. This catches FHSS (many freqs every cycle) while
// filtering sporadic infrastructure (brief diversity spikes that don't sustain).
static int countPersistentDiversity(unsigned long windowMs) {
    if (sustainedDiversityCycles >= PERSISTENCE_MIN_CONSECUTIVE)
        return countDiversity(windowMs);
    return 0;
}

static int getSustainedDiversityCycles() {
    return sustainedDiversityCycles;
}

// Sum of newly-persistent slots over the velocity window
static int computeDiversityVelocity() {
    int sum = 0;
    for (int i = 0; i < DIVERSITY_VELOCITY_WINDOW; i++)
        sum += velocityHistory[i];
    return sum;
}

// ── FHSS Frequency-Spread Tracker ────────────────────────────────────────────
// Counts UNIQUE (freq,sf) CAD hits across FHSS_WINDOW_CYCLES rolling cycles.
// When the unique count crosses FHSS_UNIQUE_THRESHOLD, each unique hit is
// forwarded to recordDiversityHit() — bypassing the consecutiveHits>=2 gate
// that blocks hopping transmitters from ever accumulating diversity.
//
// Invariants:
//  - fhssCycleAdvance() called once at the top of each scan cycle
//  - fhssRecordHit(f,sf) called for every CAD LORA_DETECTED hit in Phase 2a
//  - fhssFlushSpread() called after Phase 2a; it pushes to recordDiversityHit
//    only if threshold crossed AND warmup is complete AND freq is non-ambient.

static const uint8_t FHSS_MAX_HITS_PER_CYCLE = 16;

struct FhssHit { float freq; uint8_t sf; };
struct FhssCycleSlot { FhssHit hits[FHSS_MAX_HITS_PER_CYCLE]; uint8_t count; };

static FhssCycleSlot fhssWindow[FHSS_WINDOW_CYCLES];
static uint8_t fhssWindowIdx = 0;

static void fhssCycleAdvance() {
    fhssWindowIdx = (fhssWindowIdx + 1) % FHSS_WINDOW_CYCLES;
    fhssWindow[fhssWindowIdx].count = 0;
}

static void fhssRecordHit(float freq, uint8_t sf) {
    FhssCycleSlot& slot = fhssWindow[fhssWindowIdx];
    for (uint8_t i = 0; i < slot.count; i++) {
        if (slot.hits[i].sf == sf &&
            fabsf(slot.hits[i].freq - freq) <= TAP_FREQ_TOL) return;
    }
    if (slot.count < FHSS_MAX_HITS_PER_CYCLE) {
        slot.hits[slot.count].freq = freq;
        slot.hits[slot.count].sf = sf;
        slot.count++;
    }
}

// Collect unique (freq,sf) across the window. Returns count written to out[].
static uint8_t fhssCollectUnique(FhssHit* out, uint8_t outMax) {
    uint8_t n = 0;
    for (uint8_t c = 0; c < FHSS_WINDOW_CYCLES; c++) {
        const FhssCycleSlot& slot = fhssWindow[c];
        for (uint8_t i = 0; i < slot.count; i++) {
            bool dup = false;
            for (uint8_t j = 0; j < n; j++) {
                if (out[j].sf == slot.hits[i].sf &&
                    fabsf(out[j].freq - slot.hits[i].freq) <= TAP_FREQ_TOL) {
                    dup = true; break;
                }
            }
            if (!dup && n < outMax) {
                out[n].freq = slot.hits[i].freq;
                out[n].sf = slot.hits[i].sf;
                n++;
            }
        }
    }
    return n;
}

// Call after Phase 2a completes. Uses a baseline-delta discriminator: track
// the steady-state unique-frequency count from infrastructure, then only fire
// when the count INCREASES by FHSS_UNIQUE_THRESHOLD — indicating new FHSS
// activity on top of the existing environment. This scales regardless of how
// many infrastructure channels exist.
static uint8_t fhssFlushSpread() {
    static uint8_t baselineFhssUnique = 0;
    static bool baselineSet = false;

    FhssHit uniq[FHSS_WINDOW_CYCLES * FHSS_MAX_HITS_PER_CYCLE];
    uint8_t uniqCount = fhssCollectUnique(uniq, sizeof(uniq) / sizeof(uniq[0]));

    if (!baselineSet && warmupComplete) {
        baselineFhssUnique = uniqCount;
        baselineSet = true;
        return 0;
    }
    if (!baselineSet) return 0;

    // Update baseline: fast drop when environment quiets, stable when steady
    if (uniqCount <= baselineFhssUnique) {
        baselineFhssUnique = uniqCount;
    }

    // Only fire when unique count exceeds baseline by threshold
    if (uniqCount < baselineFhssUnique + FHSS_UNIQUE_THRESHOLD) return 0;

    uint8_t fired = 0;
    for (uint8_t i = 0; i < uniqCount; i++) {
        recordDiversityHit(uniq[i].freq, uniq[i].sf);
        fired++;
    }
    if (fired > 0) {
        Serial.printf("[FHSS-SPREAD] %u unique (baseline=%u) -> diversity fired\n",
                      uniqCount, baselineFhssUnique);
    }
    return fired;
}

// ── Tap list management ─────────────────────────────────────────────────────

bool cadWarmupComplete() {
    return warmupComplete;
}

bool cadHwFault() {
    return cadHwFaultFlag;
}

void resetDiversityTracker() {
    memset(diversitySlots, 0, sizeof(diversitySlots));
    memset(velocityHistory, 0, sizeof(velocityHistory));
    velocityIdx = 0;
    sustainedDiversityCycles = 0;
    prevSustainedDiversityCycles = 0;
    memset(fhssWindow, 0, sizeof(fhssWindow));
    fhssWindowIdx = 0;
}

void cadScannerInit() {
    memset(tapList, 0, sizeof(tapList));
    memset(ambientTaps, 0, sizeof(ambientTaps));
    memset(diversitySlots, 0, sizeof(diversitySlots));
    memset(velocityHistory, 0, sizeof(velocityHistory));
    velocityIdx = 0;
    sustainedDiversityCycles = 0;
    prevSustainedDiversityCycles = 0;
    memset(fhssWindow, 0, sizeof(fhssWindow));
    fhssWindowIdx = 0;
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

static CadTap* findTap(float freq, uint8_t sf) {
    for (int i = 0; i < MAX_TAPS; i++) {
        if (tapList[i].active &&
            tapList[i].sf == sf &&
            fabsf(tapList[i].frequency - freq) < TAP_FREQ_TOL) {
            return &tapList[i];
        }
    }
    return nullptr;
}

static CadTap* addTap(float freq, uint8_t sf, RfBand band = BAND_SUB_GHZ) {
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) {
            tapList[i].frequency = freq;
            tapList[i].sf = sf;
            tapList[i].band = band;
            tapList[i].isFsk = false;
            tapList[i].isAmbient = false;
            tapList[i].consecutiveHits = 1;
            tapList[i].missCount = 0;
            tapList[i].firstSeenMs = millis();
            tapList[i].lastSeenMs = millis();
            tapList[i].active = true;
            return &tapList[i];
        }
    }
    return nullptr;
}

static CadTap* addFskTap(float freq) {
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) {
            tapList[i].frequency = freq;
            tapList[i].sf = 0;
            tapList[i].isFsk = true;
            tapList[i].isAmbient = false;
            tapList[i].consecutiveHits = 1;
            tapList[i].missCount = 0;
            tapList[i].firstSeenMs = millis();
            tapList[i].lastSeenMs = millis();
            tapList[i].active = true;
            return &tapList[i];
        }
    }
    return nullptr;
}

static void tapHit(CadTap* tap) {
    if (tap->consecutiveHits < 255) tap->consecutiveHits++;
    tap->missCount = 0;
    tap->lastSeenMs = millis();
}

static void tapMiss(CadTap* tap) {
    tap->missCount++;
    if (tap->missCount >= TAP_EXPIRE_MISSES) {
        tap->active = false;
    }
}

static void countConfirmed(int& cadCount, int& fskCount, int& strongPending, int& pending, int& totalActive) {
    cadCount = 0; fskCount = 0; strongPending = 0; pending = 0; totalActive = 0;
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) continue;
        // Skip ambient taps from all counts that feed threat escalation
        if (tapList[i].isAmbient) continue;
        totalActive++;
        if (tapList[i].consecutiveHits >= TAP_CONFIRM_HITS) {
            if (tapList[i].isFsk) fskCount++;
            else cadCount++;
        } else if (tapList[i].consecutiveHits == 2 && !tapList[i].isFsk) {
            strongPending++;
        } else {
            pending++;
        }
    }
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
static const int ELRS_24_CHANNELS = 40;  // 2400-2480 MHz at 2 MHz steps
static float elrs24Freq(int ch) { return 2400.0f + (ch * 2.0f); }

// 2.4 GHz CAD channel allocation
static const int CAD_24_SF6  = 20;
static const int CAD_24_SF7  = 10;
static const int CAD_24_SF8  = 5;
static uint32_t rot24SF6 = 0;
static uint32_t rot24SF7 = 0;
static uint32_t rot24SF8 = 0;

CadFskResult cadFskScan(LR1121_RSSI& radio, uint32_t cycleNum, const ScanResult* rssi) {
    CadFskResult result = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    latestSweep = rssi;
    cadErrorsThisCycle = 0;
    cadProbesThisCycle = 0;
    cadFirstError = 0;

    diversityCycleUpdate();
    pruneExpiredDiversity();
    fhssCycleAdvance();

    // ── Enter LoRa mode for CAD scanning ──────────────────────────────
    ensureLoRa_LR(radio);

    // ── PHASE 1: Priority re-check active sub-GHz LoRa taps ──────────
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active || tapList[i].isFsk) continue;
        if (tapList[i].band == BAND_2G4) continue;  // 2.4 GHz taps re-checked in Phase 2b
        radio.setSpreadingFactor(tapList[i].sf);
        ChannelScanConfig_t cfg = buildCadConfigLR(tapList[i].sf);
        radio.setFrequency(tapList[i].frequency);
        int16_t cadResult = radio.scanChannel(cfg);
        countCadProbe(cadResult);
        if (cadResult == RADIOLIB_LORA_DETECTED) {
            tapHit(&tapList[i]);
            if (tapList[i].consecutiveHits >= 2 &&
                !isAmbientFrequency(tapList[i].frequency, tapList[i].sf))
                recordDiversityHit(tapList[i].frequency, tapList[i].sf);
        } else {
            // Adjacent channel check
            bool adjHit = false;
            float spacing = 0.325f;
            for (float delta : {-spacing, spacing}) {
                float adjFreq = tapList[i].frequency + delta;
                if (adjFreq >= 902.0f && adjFreq <= 928.0f) {
                    radio.setFrequency(adjFreq);
                    int16_t adjResult = radio.scanChannel(cfg);
                    countCadProbe(adjResult);
                    if (adjResult == RADIOLIB_LORA_DETECTED) {
                        tapHit(&tapList[i]);
                        if (tapList[i].consecutiveHits >= 2 &&
                            !isAmbientFrequency(adjFreq, tapList[i].sf))
                            recordDiversityHit(adjFreq, tapList[i].sf);
                        adjHit = true;
                        break;
                    }
                }
            }
            if (!adjHit) tapMiss(&tapList[i]);
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
                    CadTap* existing = findTap(freq, 6);
                    if (existing) {
                        tapHit(existing);
                        if (existing->consecutiveHits >= 2 &&
                            !isAmbientFrequency(freq, 6))
                            recordDiversityHit(freq, 6);
                    } else {
                        addTap(freq, 6, BAND_SUB_GHZ);
                    }
                }
                guidedScans++;
            }
        }
    }

    // ── PHASE 2a: Broad sub-GHz CAD scan — SF6-SF12 rotating channels ─
    // Count confirmed taps FROM THIS CYCLE's Phase 1 hits so pursuit mode
    // reacts on the same cycle the drone is first seen (was 1 cycle late).
    countConfirmed(result.confirmedCadCount, result.confirmedFskCount,
                   result.strongPendingCad, result.pendingTaps, result.totalActiveTaps);
    static bool lastPursuitMode = false;
    bool pursuitMode = (countDiversity(DIVERSITY_WINDOW_MS) >= DIVERSITY_WARNING)
                       || (result.confirmedCadCount > 0);
    if (pursuitMode && !lastPursuitMode)
        Serial.println("[PURSUIT] Activated");
    else if (!pursuitMode && lastPursuitMode)
        Serial.println("[PURSUIT] Deactivated");
    lastPursuitMode = pursuitMode;

    auto sfHasActiveTaps = [](uint8_t sf) -> bool {
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active && !tapList[i].isFsk && tapList[i].sf == sf)
                return true;
        }
        return false;
    };

    int sf6ch  = pursuitMode && sfHasActiveTaps(6)  ? 80 : CAD_CH_SF6;
    int sf7ch  = pursuitMode && sfHasActiveTaps(7)  ? 60 : CAD_CH_SF7;
    int sf8ch  = pursuitMode && sfHasActiveTaps(8)  ? 30 : CAD_CH_SF8;
    int sf9ch  = pursuitMode && !sfHasActiveTaps(9)  ? 2 : CAD_CH_SF9;
    int sf10ch = pursuitMode && !sfHasActiveTaps(10) ? 1 : CAD_CH_SF10;
    int sf11ch = pursuitMode && !sfHasActiveTaps(11) ? 1 : CAD_CH_SF11;
    int sf12ch = pursuitMode && !sfHasActiveTaps(12) ? 1 : CAD_CH_SF12;

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
                CadTap* existing = findTap(freq, sc.sf);
                if (!existing) addTap(freq, sc.sf, BAND_SUB_GHZ);
                if (existing && existing->consecutiveHits >= 2 &&
                    !isAmbientFrequency(freq, sc.sf))
                    recordDiversityHit(freq, sc.sf);
                fhssRecordHit(freq, sc.sf);
            }
        }
    }

    // Flush FHSS frequency-spread tracker: if unique-hit count across the
    // rolling window crossed the threshold, each non-ambient unique hit
    // is pushed into recordDiversityHit() — bypassing the consecutiveHits>=2
    // gate that kept hopping transmitters from accumulating diversity.
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

    // Re-check active 2.4 GHz taps first
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active || tapList[i].isFsk) continue;
        if (tapList[i].band != BAND_2G4) continue;
        radio.setSpreadingFactor(tapList[i].sf);
        ChannelScanConfig_t cfg24 = buildCadConfigLR(tapList[i].sf);
        radio.setFrequency(tapList[i].frequency);
        int16_t cadResult = radio.scanChannel(cfg24);
        countCadProbe(cadResult);
        if (cadResult == RADIOLIB_LORA_DETECTED) {
            tapHit(&tapList[i]);
            if (tapList[i].consecutiveHits >= 2)
                recordDiversityHit(tapList[i].frequency, tapList[i].sf);
        } else {
            tapMiss(&tapList[i]);
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
                CadTap* existing = findTap(freq, sc.sf);
                if (!existing) addTap(freq, sc.sf, BAND_2G4);
                if (existing && existing->consecutiveHits >= 2)
                    recordDiversityHit(freq, sc.sf);
            }
        }
    }
    }

    // ── PHASE 4: Switch back to GFSK for RSSI sweep ──────────────────
    ensureGFSK_LR(radio);

    // ── PHASE 3: FSK Crossfire scan ──────────────────────────────────
    // Radio is in GFSK mode. Reconfigure for Crossfire 150 Hz detection.
    {
        int16_t fskErr = radio.setBitRate(85.1);
        if (fskErr == RADIOLIB_ERR_NONE) {
            radio.setFrequencyDeviation(25.0);
            radio.setRxBandwidth(117.3);

            // Re-check existing FSK taps
            for (int i = 0; i < MAX_TAPS; i++) {
                if (!tapList[i].active || !tapList[i].isFsk) continue;
                radio.setFrequency(tapList[i].frequency);
                radio.startReceive();
                delayMicroseconds(FSK_DWELL_US);
                float r = radio.getInstantRSSI();
                if (r > FSK_DETECT_THRESHOLD_DBM) tapHit(&tapList[i]);
                else tapMiss(&tapList[i]);
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
                    CadTap* existing = findTap(freq, 0);
                    if (existing) tapHit(existing);
                    else addFskTap(freq);
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

    if (!warmupComplete) {
        bool normalEnd = (millis() >= AMBIENT_WARMUP_MS);
        bool earlyExit = (millis() >= AMBIENT_EARLY_EXIT_MS) && (result.totalActiveTaps == 0)
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
                if (tapList[i].active && !tapList[i].isFsk) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf, true);
                }
            }
        }
    }

    // Post-warmup continuous ambient learning
    if (warmupComplete) {
        unsigned long now = millis();
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active &&
                !tapList[i].isFsk &&
                tapList[i].consecutiveHits >= TAP_CONFIRM_HITS &&
                !isAmbientCadSource(tapList[i].frequency, tapList[i].sf)) {
                if (tapList[i].firstSeenMs < AMBIENT_WARMUP_MS) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf, true);
                }
                else if ((now - tapList[i].firstSeenMs) > AMBIENT_AUTOLEARN_MS) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf, false);
                }
            }
        }
    }

    // Tag taps with ambient status
    if (warmupComplete) {
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active) {
                tapList[i].isAmbient = isAmbientCadSource(tapList[i].frequency, tapList[i].sf);
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

    countConfirmed(result.confirmedCadCount, result.confirmedFskCount,
                   result.strongPendingCad, result.pendingTaps, result.totalActiveTaps);
    result.diversityCount = countDiversity(DIVERSITY_WINDOW_MS);
    result.persistentDiversityCount = countPersistentDiversity(DIVERSITY_WINDOW_MS);
    result.diversityVelocity = computeDiversityVelocity();
    result.sustainedCycles = getSustainedDiversityCycles();

    return result;
}

#else // SX1262 boards

CadFskResult cadFskScan(SX1262& radio, uint32_t cycleNum, const ScanResult* rssi) {
    CadFskResult result = {0, 0, 0, 0, 0, 0, 0, 0};
    latestSweep = rssi;
    cadErrorsThisCycle = 0;
    cadProbesThisCycle = 0;

    // Update persistence counters from previous cycle's hits, then clear flags
    diversityCycleUpdate();

    // Housekeeping: prune expired diversity slots
    pruneExpiredDiversity();
    fhssCycleAdvance();

    // Switch from FSK to LoRa packet type via low-level SPI command
    ensureLoRa(radio);

    // ── PHASE 1: Priority re-check active LoRa taps + adjacent channels ──
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) continue;
        if (tapList[i].isFsk) continue;  // FSK taps re-checked in Phase 3
        radio.setSpreadingFactor(tapList[i].sf);
        ChannelScanConfig_t cfg = buildCadConfig(tapList[i].sf);
        radio.setFrequency(tapList[i].frequency);
        int16_t cadResult = radio.scanChannel(cfg);
        countCadProbe(cadResult);
        if (cadResult == RADIOLIB_LORA_DETECTED) {
            tapHit(&tapList[i]);
            if (tapList[i].consecutiveHits >= 2 &&
                !isAmbientFrequency(tapList[i].frequency, tapList[i].sf))
                recordDiversityHit(tapList[i].frequency, tapList[i].sf);
        } else {
            bool adjHit = false;
            float spacing = 0.325f;
            for (float delta : {-spacing, spacing}) {
                float adjFreq = tapList[i].frequency + delta;
                if (adjFreq >= 902.0f && adjFreq <= 928.0f) {
                    radio.setFrequency(adjFreq);
                    int16_t adjResult = radio.scanChannel(cfg);
                    countCadProbe(adjResult);
                    if (adjResult == RADIOLIB_LORA_DETECTED) {
                        tapHit(&tapList[i]);
                        if (tapList[i].consecutiveHits >= 2 &&
                            !isAmbientFrequency(adjFreq, tapList[i].sf))
                            recordDiversityHit(adjFreq, tapList[i].sf);
                        adjHit = true;
                        break;
                    }
                }
            }
            if (!adjHit) tapMiss(&tapList[i]);
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
                    CadTap* existing = findTap(freq, 6);
                    if (existing) {
                        tapHit(existing);
                        if (existing->consecutiveHits >= 2 &&
                            !isAmbientFrequency(freq, 6))
                            recordDiversityHit(freq, 6);
                    } else {
                        addTap(freq, 6);
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
    countConfirmed(result.confirmedCadCount, result.confirmedFskCount,
                   result.strongPendingCad, result.pendingTaps, result.totalActiveTaps);
    static bool lastPursuitMode = false;
    bool pursuitMode = (countDiversity(DIVERSITY_WINDOW_MS) >= DIVERSITY_WARNING)
                       || (result.confirmedCadCount > 0);
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
    auto sfHasActiveTaps = [](uint8_t sf) -> bool {
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active && !tapList[i].isFsk && tapList[i].sf == sf)
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
        sf6ch  = sfHasActiveTaps(6)  ? 80 : CAD_CH_SF6;
        sf7ch  = sfHasActiveTaps(7)  ? 60 : CAD_CH_SF7;
        sf8ch  = sfHasActiveTaps(8)  ? 30 : CAD_CH_SF8;
        sf9ch  = sfHasActiveTaps(9)  ? CAD_CH_SF9  : 2;
        sf10ch = sfHasActiveTaps(10) ? CAD_CH_SF10 : 1;
        sf11ch = sfHasActiveTaps(11) ? CAD_CH_SF11 : 1;
        sf12ch = sfHasActiveTaps(12) ? CAD_CH_SF12 : 1;
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
                CadTap* existing = findTap(freq, sc.sf);
                if (!existing) addTap(freq, sc.sf);
                if (existing && existing->consecutiveHits >= 2 &&
                    !isAmbientFrequency(freq, sc.sf))
                    recordDiversityHit(freq, sc.sf);
                fhssRecordHit(freq, sc.sf);
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
                if (!tapList[i].active || !tapList[i].isFsk) continue;
                radio.setFrequency(tapList[i].frequency);
                radio.startReceive();
                delayMicroseconds(FSK_DWELL_US);
                float r = radio.getRSSI(false);
                if (r > FSK_DETECT_THRESHOLD_DBM) tapHit(&tapList[i]);
                else tapMiss(&tapList[i]);
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
                    CadTap* existing = findTap(freq, 0);
                    if (existing) tapHit(existing);
                    else addFskTap(freq);
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

    if (!warmupComplete) {
        bool normalEnd = (millis() >= AMBIENT_WARMUP_MS);
        // Early exit: after minimum warmup, if environment is clean (no active taps),
        // complete early to reduce blind time on power-on in clean field environments.
        bool earlyExit = (millis() >= AMBIENT_EARLY_EXIT_MS) && (result.totalActiveTaps == 0)
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
                if (tapList[i].active && !tapList[i].isFsk) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf, true);
                }
            }
        }
    }

    // Post-warmup continuous ambient learning (auto-learned, NOT warmup):
    if (warmupComplete) {
        unsigned long now = millis();
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active &&
                !tapList[i].isFsk &&
                tapList[i].consecutiveHits >= TAP_CONFIRM_HITS &&
                !isAmbientCadSource(tapList[i].frequency, tapList[i].sf)) {
                if (tapList[i].firstSeenMs < AMBIENT_WARMUP_MS) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf, true);
                }
                else if ((now - tapList[i].firstSeenMs) > AMBIENT_AUTOLEARN_MS) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf, false);
                }
            }
        }
    }

    // Tag each active tap with its ambient status
    if (warmupComplete) {
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active) {
                tapList[i].isAmbient = isAmbientCadSource(tapList[i].frequency, tapList[i].sf);
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

    countConfirmed(result.confirmedCadCount, result.confirmedFskCount, result.strongPendingCad, result.pendingTaps, result.totalActiveTaps);
    result.diversityCount = countDiversity(DIVERSITY_WINDOW_MS);
    result.persistentDiversityCount = countPersistentDiversity(DIVERSITY_WINDOW_MS);
    result.diversityVelocity = computeDiversityVelocity();
    result.sustainedCycles = getSustainedDiversityCycles();

    return result;
}

#endif
