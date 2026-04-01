#include "cad_scanner.h"
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
};

static AmbientTap ambientTaps[MAX_AMBIENT_TAPS];
static uint8_t ambientTapCount = 0;
static uint16_t warmupCycleCount = 0;
static bool warmupComplete = false;

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

static void recordAmbientTap(float freq, uint8_t sf) {
    if (isAmbientCadSource(freq, sf)) return;
    if (ambientTapCount < MAX_AMBIENT_TAPS) {
        ambientTaps[ambientTapCount].frequency = freq;
        ambientTaps[ambientTapCount].sf = sf;
        ambientTaps[ambientTapCount].firstSeenCycle = warmupCycleCount;
        ambientTaps[ambientTapCount].active = true;
        ambientTapCount++;
    }
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
    bool active;
};

static DiversitySlot diversitySlots[MAX_DIVERSITY_SLOTS];

static void recordDiversityHit(float freq, uint8_t sf) {
    unsigned long now = millis();

    // Check if this frequency/SF already has a slot
    for (int i = 0; i < MAX_DIVERSITY_SLOTS; i++) {
        if (diversitySlots[i].active &&
            diversitySlots[i].sf == sf &&
            fabsf(diversitySlots[i].frequency - freq) <= TAP_FREQ_TOL) {
            diversitySlots[i].lastHitMs = now;
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
    diversitySlots[bestSlot].active = true;
}

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

// ── Tap list management ─────────────────────────────────────────────────────

bool cadWarmupComplete() {
    return warmupComplete;
}

void cadScannerInit() {
    memset(tapList, 0, sizeof(tapList));
    memset(ambientTaps, 0, sizeof(ambientTaps));
    memset(diversitySlots, 0, sizeof(diversitySlots));
    ambientTapCount = 0;
    warmupCycleCount = 0;
    warmupComplete = false;
    rotSF6 = rotSF7 = rotSF8 = rotSF9 = rotSF10 = rotSF11 = rotSF12 = 0;
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

static CadTap* addTap(float freq, uint8_t sf) {
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) {
            tapList[i].frequency = freq;
            tapList[i].sf = sf;
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

static void tapHit(CadTap* tap) {
    tap->consecutiveHits++;
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

static void switchToLoRa(SX1262& radio) {
    // Switch SX1262 from FSK to LoRa packet type via raw SPI command.
    // CRITICAL: Must set ALL LoRa modem params (CR, sync word, preamble) after
    // switching — RadioLib's internal state for these fields is zeroed after
    // beginFSK(), and setSpreadingFactor/setBandwidth pass those zeroed values
    // to setModulationParams(), resulting in invalid CAD configuration.
    radio.standby();
    uint8_t loraType = 0x01;  // RADIOLIB_SX126X_PACKET_TYPE_LORA
    radioMod.SPIwriteStream(0x8A, &loraType, 1);

    // Set CR FIRST — this initializes codingRate internal state before SF/BW
    // call setModulationParams() with it
    radio.setCodingRate(5);          // CR 4/5 — sets this->codingRate = 1
    radio.setSpreadingFactor(6);     // SF6 — now setModulationParams uses correct CR
    radio.setBandwidth(500.0);       // BW500
    radio.setSyncWord(RADIOLIB_SX126X_SYNC_WORD_PRIVATE);  // 0x12
    radio.setPreambleLength(8);
}

static void switchToFSK(SX1262& radio) {
    radio.standby();
    uint8_t fskType = 0x00;  // RADIOLIB_SX126X_PACKET_TYPE_GFSK
    radioMod.SPIwriteStream(0x8A, &fskType, 1);
    radio.setBitRate(4.8);
    radio.setFrequencyDeviation(5.0);
    radio.setRxBandwidth(234.3);
    radio.setFrequency(860.0);
}

#endif

// ── Scan implementation ─────────────────────────────────────────────────────

#ifdef BOARD_T3S3_LR1121

CadFskResult cadFskScan(LR1121& radio, uint32_t cycleNum, const ScanResult* rssi) {
    CadFskResult result = {0, 0, 0, 0, 0, 0};
    return result;
}

#else // SX1262 boards

CadFskResult cadFskScan(SX1262& radio, uint32_t cycleNum, const ScanResult* rssi) {
    CadFskResult result = {0, 0, 0, 0, 0, 0};

    // Switch from FSK to LoRa packet type via low-level SPI command
    switchToLoRa(radio);

    // ── PHASE 1: Priority re-check active taps + adjacent channels ─────
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) continue;
        radio.setSpreadingFactor(tapList[i].sf);
        ChannelScanConfig_t cfg = buildCadConfig(tapList[i].sf);
        radio.setFrequency(tapList[i].frequency);
        if (radio.scanChannel(cfg) == RADIOLIB_LORA_DETECTED) {
            tapHit(&tapList[i]);
            if (warmupComplete)
                recordDiversityHit(tapList[i].frequency, tapList[i].sf);
        } else {
            bool adjHit = false;
            float spacing = 0.325f;
            for (float delta : {-spacing, spacing}) {
                float adjFreq = tapList[i].frequency + delta;
                if (adjFreq >= 902.0f && adjFreq <= 928.0f) {
                    radio.setFrequency(adjFreq);
                    if (radio.scanChannel(cfg) == RADIOLIB_LORA_DETECTED) {
                        tapHit(&tapList[i]);
                        if (warmupComplete)
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
                if (radio.scanChannel(cfg6) == RADIOLIB_LORA_DETECTED) {
                    CadTap* existing = findTap(freq, 6);
                    if (existing) tapHit(existing);
                    else addTap(freq, 6);
                    if (warmupComplete)
                        recordDiversityHit(freq, 6);
                }
                guidedScans++;
            }
        }
    }

    // ── PHASE 2: Broad CAD scan — all SF values, rotating channels ──────
    struct SFScan {
        uint8_t sf; int chCount; int totalCh; uint32_t* rot;
        float (*fn)(int);
    };
    SFScan sfScans[] = {
        { 6,  CAD_CH_SF6,  ELRS_915_CHANNELS, &rotSF6,  elrs915Freq },
        { 7,  CAD_CH_SF7,  ELRS_915_CHANNELS, &rotSF7,  elrs915Freq },
        { 8,  CAD_CH_SF8,  ELRS_915_CHANNELS, &rotSF8,  elrs915Freq },
        { 9,  CAD_CH_SF9,  ELRS_915_CHANNELS, &rotSF9,  elrs915Freq },
        { 10, CAD_CH_SF10, ELRS_915_CHANNELS, &rotSF10, elrs915Freq },
        { 11, CAD_CH_SF11, ELRS_868_CHANNELS, &rotSF11, elrs868Freq },
        { 12, CAD_CH_SF12, ELRS_915_CHANNELS, &rotSF12, elrs915Freq },
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

        int stride = sc.totalCh / sc.chCount;
        if (stride < 1) stride = 1;
        int offset = (*sc.rot) % stride;
        (*sc.rot)++;

        for (int i = 0; i < sc.chCount; i++) {
            int ch = (offset + i * stride) % sc.totalCh;
            float freq = sc.fn(ch);
            radio.setFrequency(freq);

            if (radio.scanChannel(cfg) == RADIOLIB_LORA_DETECTED) {
                CadTap* existing = findTap(freq, sc.sf);
                if (!existing) addTap(freq, sc.sf);
                if (warmupComplete)
                    recordDiversityHit(freq, sc.sf);
            }
        }
    }

    // PHASE 3 (FSK listen) — skipped until CAD is proven working

    // ── PHASE 4: Switch back to FSK for next RSSI sweep ────────────────
    switchToFSK(radio);
    radio.setRxBoostedGainMode(true);

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
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf);
                }
            }
        }
    }

    // Post-warmup continuous ambient learning:
    // Infrastructure LoRa sources transmit on fixed frequencies indefinitely.
    // Drone FHSS taps are short-lived per-frequency (3 misses → deactivated).
    // Any confirmed tap alive for 30+ seconds on a fixed frequency is infrastructure.
    if (warmupComplete) {
        unsigned long now = millis();
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active &&
                !tapList[i].isFsk &&
                tapList[i].consecutiveHits >= TAP_CONFIRM_HITS &&
                !isAmbientCadSource(tapList[i].frequency, tapList[i].sf)) {
                // Taps first seen during warmup: auto-add
                if (tapList[i].firstSeenMs < AMBIENT_WARMUP_MS) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf);
                }
                // Taps alive 10+ seconds post-warmup: auto-learn as ambient.
                // FHSS drone taps expire in ~3 cycles ≈ 2s per frequency (drone hops
                // away). Infrastructure persists on fixed frequencies for minutes+.
                else if ((now - tapList[i].firstSeenMs) > AMBIENT_AUTOLEARN_MS) {
                    recordAmbientTap(tapList[i].frequency, tapList[i].sf);
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

    countConfirmed(result.confirmedCadCount, result.confirmedFskCount, result.strongPendingCad, result.pendingTaps, result.totalActiveTaps);
    result.diversityCount = countDiversity(DIVERSITY_WINDOW_MS);

    return result;
}

#endif
