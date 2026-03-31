#include "cad_scanner.h"
#include "board_config.h"
#include <Arduino.h>
#include <string.h>

// Access the Module object for low-level SPI commands (defined in main.cpp)
extern Module radioMod;

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
// LoRa sources present during warmup (first 10 cycles) are infrastructure,
// not drones. Record their frequency/SF and exclude from confirmed counts.

static const int MAX_AMBIENT_CAD = 16;
static const int AMBIENT_CAD_WARMUP_CYCLES = 20;

struct AmbientCadSource {
    float   frequency;
    uint8_t sf;
    bool    active;
};

static AmbientCadSource ambientCad[MAX_AMBIENT_CAD];
static bool ambientCadLocked = false;
static uint32_t cadCycleCount = 0;

static bool isAmbientCadSource(float freq, uint8_t sf) {
    for (int i = 0; i < MAX_AMBIENT_CAD; i++) {
        if (ambientCad[i].active &&
            ambientCad[i].sf == sf &&
            fabsf(ambientCad[i].frequency - freq) < TAP_FREQ_TOL) {
            return true;
        }
    }
    return false;
}

static void addAmbientCadSource(float freq, uint8_t sf) {
    if (isAmbientCadSource(freq, sf)) return;  // already recorded
    for (int i = 0; i < MAX_AMBIENT_CAD; i++) {
        if (!ambientCad[i].active) {
            ambientCad[i].frequency = freq;
            ambientCad[i].sf = sf;
            ambientCad[i].active = true;
            return;
        }
    }
}

// ── Tap list management ─────────────────────────────────────────────────────

void cadScannerInit() {
    memset(tapList, 0, sizeof(tapList));
    memset(ambientCad, 0, sizeof(ambientCad));
    ambientCadLocked = false;
    cadCycleCount = 0;
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

static void countConfirmed(int& cadCount, int& fskCount, int& strongPending, int& pending) {
    cadCount = 0; fskCount = 0; strongPending = 0; pending = 0;
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) continue;
        if (tapList[i].consecutiveHits >= TAP_CONFIRM_HITS) {
            // Skip confirmed taps that match ambient LoRa sources from warmup
            if (!tapList[i].isFsk && isAmbientCadSource(tapList[i].frequency, tapList[i].sf))
                continue;
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

CadFskResult cadFskScan(LR1121& radio, uint32_t cycleNum) {
    CadFskResult result = {0, 0, 0, 0};
    return result;
}

#else // SX1262 boards

CadFskResult cadFskScan(SX1262& radio, uint32_t cycleNum) {
    CadFskResult result = {0, 0, 0, 0};

    // Switch from FSK to LoRa packet type via low-level SPI command
    switchToLoRa(radio);

    // ── PHASE 1: Priority re-check active taps + adjacent channels ─────
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!tapList[i].active) continue;
        radio.setSpreadingFactor(tapList[i].sf);
        radio.setFrequency(tapList[i].frequency);
        if (radio.scanChannel() == RADIOLIB_LORA_DETECTED) {
            tapHit(&tapList[i]);
        } else {
            // Adjacent channel re-check: ELRS hops pseudo-randomly, so the
            // drone likely moved to a nearby channel. Check ±1 channel.
            bool adjHit = false;
            float spacing = 0.325f;  // ELRS 915 US channel spacing
            for (float delta : {-spacing, spacing}) {
                float adjFreq = tapList[i].frequency + delta;
                if (adjFreq >= 902.0f && adjFreq <= 928.0f) {
                    radio.setFrequency(adjFreq);
                    if (radio.scanChannel() == RADIOLIB_LORA_DETECTED) {
                        tapHit(&tapList[i]);
                        adjHit = true;
                        break;
                    }
                }
            }
            if (!adjHit) tapMiss(&tapList[i]);
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

        // Stride-based channel spread: instead of scanning sequential blocks,
        // spread across the full band. E.g., SF6 scans 40 of 80 channels
        // with stride=2: ch 0,2,4,6... on cycle 0; ch 1,3,5,7... on cycle 1.
        int stride = sc.totalCh / sc.chCount;
        if (stride < 1) stride = 1;
        int offset = (*sc.rot) % stride;
        (*sc.rot)++;

        for (int i = 0; i < sc.chCount; i++) {
            int ch = (offset + i * stride) % sc.totalCh;
            float freq = sc.fn(ch);
            radio.setFrequency(freq);

            if (radio.scanChannel() == RADIOLIB_LORA_DETECTED) {
                CadTap* existing = findTap(freq, sc.sf);
                if (!existing) addTap(freq, sc.sf);
            }
        }
    }

    // PHASE 3 (FSK listen) — skipped until CAD is proven working

    // ── PHASE 4: Switch back to FSK for next RSSI sweep ────────────────
    switchToFSK(radio);
    radio.setRxBoostedGainMode(true);

    // ── Warmup: record confirmed taps as ambient LoRa sources ───────────
    cadCycleCount++;
    if (!ambientCadLocked) {
        if (cadCycleCount >= AMBIENT_CAD_WARMUP_CYCLES) {
            ambientCadLocked = true;
        }
        // During warmup, any confirmed tap is assumed to be infrastructure
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active &&
                !tapList[i].isFsk &&
                tapList[i].consecutiveHits >= TAP_CONFIRM_HITS) {
                addAmbientCadSource(tapList[i].frequency, tapList[i].sf);
            }
        }
    }

    countConfirmed(result.confirmedCadCount, result.confirmedFskCount, result.strongPendingCad, result.pendingTaps);

    // Post-warmup ambient catch: if a newly confirmed tap was FIRST SEEN during
    // the warmup window, it's infrastructure that took time to accumulate 3 hits.
    // Add it to the ambient set so it stops counting as a detection.
    // A drone arriving after warmup will have firstSeenMs after the warmup window.
    if (ambientCadLocked) {
        unsigned long warmupEndMs = AMBIENT_CAD_WARMUP_CYCLES * 2500UL;
        for (int i = 0; i < MAX_TAPS; i++) {
            if (tapList[i].active &&
                !tapList[i].isFsk &&
                tapList[i].consecutiveHits >= TAP_CONFIRM_HITS &&
                tapList[i].firstSeenMs < warmupEndMs &&
                !isAmbientCadSource(tapList[i].frequency, tapList[i].sf)) {
                addAmbientCadSource(tapList[i].frequency, tapList[i].sf);
            }
        }
    }

    return result;
}

#endif
