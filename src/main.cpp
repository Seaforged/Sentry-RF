#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <Adafruit_SSD1306.h>
#include "board_config.h"
#include "version.h"
#include "task_config.h"
#include "detection_types.h"
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"
#include "compass.h"
#include "display.h"
#include "detection_engine.h"
#include "cad_scanner.h"
#include "data_logger.h"
#include "wifi_scanner.h"
#include "ble_scanner.h"
#include "alert_handler.h"
#include "error_messages.h"

// Hardware objects — each owned by exactly one task after setup()
SPIClass loraSPI(HSPI);
Module radioMod(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
#ifdef BOARD_T3S3_LR1121
LR1121_RSSI radio(&radioMod);
#else
SX1262 radio(&radioMod);
#endif
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, PIN_OLED_RST);

// FreeRTOS shared state — copy under lock, process outside lock
SystemState systemState = {};
SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t serialMutex = nullptr;
QueueHandle_t detectionQueue = nullptr;

// Task handles for stack monitoring
static TaskHandle_t hLoRaTask    = nullptr;
static TaskHandle_t hGPSTask     = nullptr;
static TaskHandle_t hDisplayTask = nullptr;
static TaskHandle_t hAlertTask   = nullptr;
// hWiFiTask lives in wifi_scanner.cpp (needs to be externally resumable).

// ── Phase H: Operational mode ──────────────────────────────────────────
// Default boot is STANDARD — behavior identical to pre-Phase-H. Reads
// and writes go through modeGet()/modeSet() which take stateMutex, so
// this global should never be read directly outside this file.
static volatile OperatingMode currentMode = MODE_STANDARD;

extern "C" OperatingMode modeGet() {
    OperatingMode m = MODE_STANDARD;
    if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        m = currentMode;
        xSemaphoreGive(stateMutex);
    } else {
        // Pre-mutex path (boot) — direct read is fine, only one core is live.
        m = currentMode;
    }
    return m;
}

extern "C" void modeSet(OperatingMode m) {
    if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        currentMode = m;
        xSemaphoreGive(stateMutex);
    } else {
        currentMode = m;
    }
}

extern "C" const char* modeShortLabel(OperatingMode m) {
    switch (m) {
        case MODE_COVERT:     return "COV";
        case MODE_HIGH_ALERT: return "HI-ALT";
        default:              return "STD";
    }
}

extern "C" const char* modeLongLabel(OperatingMode m) {
    switch (m) {
        case MODE_COVERT:     return "COVERT";
        case MODE_HIGH_ALERT: return "HIGH_ALERT";
        default:              return "STANDARD";
    }
}

// ── Hardware init (runs once in setup on Core 1) ────────────────────────────

static int initRadioHardware() {
    loraSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

    // SX1262 needs setTCXO() before beginFSK() — its SPI is already live.
    // LR1121 passes TCXO voltage via beginGFSK() parameter instead.
#ifndef BOARD_T3S3_LR1121
    if (HAS_TCXO) {
        int tcxoState = radio.setTCXO(1.8);
        if (tcxoState != RADIOLIB_ERR_NONE) {
            Serial.printf("[LoRa] TCXO set failed: %d\n", tcxoState);
            return tcxoState;
        }
    }
#endif

    return RADIOLIB_ERR_NONE;
}

static void initCompassBus() {
#if defined(BOARD_T3S3) || defined(BOARD_T3S3_LR1121)
    if (HAS_COMPASS) {
        Wire1.begin(PIN_COMPASS_SDA, PIN_COMPASS_SCL);
        Serial.printf("[I2C1] Compass bus on SDA=%d SCL=%d\n",
                      PIN_COMPASS_SDA, PIN_COMPASS_SCL);
    }
#endif
}

static void printBanner() {
    Serial.println("========================");
    Serial.printf(" %s v%s\n", FW_NAME, FW_VERSION);
    Serial.printf(" Build: %s\n", FW_DATE);
#ifdef BOARD_T3S3
    Serial.println(" Board: LilyGo T3S3");
#elif defined(BOARD_HELTEC_V3)
    Serial.println(" Board: Heltec V3");
#elif defined(BOARD_T3S3_LR1121)
    Serial.println(" Board: T3S3 LR1121");
#endif
    Serial.println(" Mode:  FreeRTOS dual-core");
    Serial.println("========================");
}

// ── Task functions ──────────────────────────────────────────────────────────

static const char* threatLevelStr(ThreatLevel t) {
    switch (t) {
        case THREAT_ADVISORY: return "ADVISORY";
        case THREAT_WARNING:  return "WARNING";
        case THREAT_CRITICAL: return "CRITICAL";
        default:              return "CLEAR";
    }
}

// RSSI_SWEEP_INTERVAL_MS from sentry_config.h (Phase F wall-clock pacing)
#include "sentry_config.h"

// ── Phase K: Boot self-test + async GPS check ────────────────────────
// Results populated by runSelfTest() in setup() and read later when the
// self-test JSONL is written. gpsFixPending is cleared by gpsReadTask on
// first valid 3D fix, or left set until the timeout log fires.
static bool          selfTestRadioOK      = true;
static bool          selfTestAntennaOK    = true;
static volatile bool gpsFixPending        = false;
static unsigned long gpsFixStartMs        = 0;
static bool          gpsFixTimeoutLogged  = false;

// Boot self-test: RSSI sanity, antenna coverage probe. Must complete in
// <500ms (GPS is async, OLED summary is outside this budget). The radio
// must already be initialized by initRadioHardware() + scannerInit();
// this function must not call radio.begin() again (resetting mid-boot
// corrupts chip state on both SX1262 and LR1121).
static void runSelfTest() {
    // ── Radio health: 10 RSSI reads @ 10ms. All -127.5 dBm exact means
    // the chip clamped to the "no signal / error" sentinel across every
    // sample — almost certainly a dead receiver. Anything else = healthy.
    bool radioHealthy = false;
    radio.setFrequency(915.0);
#ifdef BOARD_T3S3_LR1121
    radio.startReceive();
#else
    radio.startReceive();
#endif
    for (int i = 0; i < 10; i++) {
        delay(10);
#ifdef BOARD_T3S3_LR1121
        float rssi = radio.getInstantRSSI();
#else
        float rssi = radio.getRSSI(false);
#endif
        if (rssi > -127.4f || rssi < -127.6f) {   // not exactly -127.5
            radioHealthy = true;
        }
    }
    selfTestRadioOK = radioHealthy;
    Serial.printf("[SELFTEST] Radio: %s\n", radioHealthy ? "OK" : "FAIL");

    // ── Antenna coverage probe: 10 frequencies across 860-930 MHz. If
    // ALL read below ANTENNA_FLOOR_DBM (-130 dBm) the antenna path is
    // disconnected or severely damaged. Anything above floor on any
    // probe passes — even a single bin of ambient pickup is sufficient
    // evidence of a radiating element.
    const float probeFreqs[] = {
        860.0f, 867.0f, 874.0f, 881.0f, 888.0f,
        895.0f, 902.0f, 909.0f, 916.0f, 930.0f
    };
    bool anyAboveFloor = false;
    for (int i = 0; i < 10; i++) {
        radio.setFrequency(probeFreqs[i]);
#ifdef BOARD_T3S3_LR1121
        radio.startReceive();   // LR1121 drops to standby on setFrequency
#endif
        delay(5);
#ifdef BOARD_T3S3_LR1121
        float rssi = radio.getInstantRSSI();
#else
        float rssi = radio.getRSSI(false);
#endif
        if (rssi > ANTENNA_FLOOR_DBM) anyAboveFloor = true;
    }
    selfTestAntennaOK = anyAboveFloor;
    Serial.printf("[SELFTEST] Antenna: %s\n",
                  anyAboveFloor ? "OK" : "WARN (no signal detected)");

    // ── GPS check: arm the async timer. gpsReadTask clears the flag on
    // first valid 3D fix; loop() logs [SELFTEST] GPS: NO FIX if the
    // deadline passes with the flag still set.
    gpsFixPending       = true;
    gpsFixStartMs       = millis();
    gpsFixTimeoutLogged = false;
    Serial.println("[SELFTEST] GPS: Acquiring... (async)");
}

// Core 1 — LoRa SPI is only touched here, no mutex needed for the radio
static void loRaScanTask(void* param) {
    ScanResult localResult = {};
    GpsData snapGps = {};
    IntegrityStatus snapIntegrity = {};
    uint32_t sweepNum = 0;
    // Phase F (revised): wall-clock pacing for the sub-GHz RSSI sweep.
    // Seeded to 0 so the first loop iteration always sweeps (matches the
    // warm-start intent of the previous cycleCount-based gate).
    uint32_t nextSubGHzSweepMs = 0;
#ifdef BOARD_T3S3_LR1121
    ScanResult24 local24 = {};
#endif

    // ── Main scan loop ────────────────────────────────────────────────────
    for (;;) {
        unsigned long cycleStart = millis();

        // ── PHASE 1: CAD scan FIRST (~1000ms) ────────────────────────
        // Highest-confidence detection. Runs EVERY cycle.
        // Pass last RSSI data for RSSI-guided CAD scanning.
        CadFskResult cadFsk = cadFskScan(radio, sweepNum,
                                          localResult.valid ? &localResult : nullptr);
        // Feed the full sub-GHz CadBandSummary into the candidate engine.
        // Phase G: the aggregate-int detectionEngineIngestCad() shim is gone;
        // the candidate engine reads the band summary directly.
        detectionEngineIngestCadBandSummary(cadFsk.subGHz);
#ifdef BOARD_T3S3_LR1121
        // 2.4 GHz CadBandSummary is confirmer-only on LR1121.
        detectionEngineIngestCad24BandSummary(cadFsk.band24);
#endif

        unsigned long cadDone = millis();

        // Snapshot GPS/integrity under lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            snapGps = systemState.gps;
            snapIntegrity = systemState.integrity;
            xSemaphoreGive(stateMutex);
        }

        // Evaluate threat with current cached state (no fresh sweep this cycle)
        ThreatLevel threat = detectionEngineAssess(snapGps, snapIntegrity);

        // Store threat + CAD results into shared state
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            systemState.threatLevel    = threat;
            systemState.cadDiversity   = cadFsk.diversityCount;
            systemState.cadConfirmed   = cadFsk.confirmedCadCount;
            systemState.cadTotalTaps   = cadFsk.totalActiveTaps;
            systemState.confidenceScore = detectionEngineGetScore();
            systemState.fastScore      = detectionEngineGetFastScore();
            systemState.confirmScore   = detectionEngineGetConfirmScore();
            systemState.anchorFreq     = detectionEngineGetAnchorFreq();
            systemState.bandMask       = detectionEngineGetBandMask();
            systemState.hasCandidate   = detectionEngineHasCandidate();
            systemState.candidateCount = detectionEngineGetCandidateCount();
            xSemaphoreGive(stateMutex);
        }

        // ── PHASE 2: RSSI sweep on its own wall-clock cadence ──────
        // Phase F (revised): sweep fires when nextSubGHzSweepMs has been
        // crossed, independent of CAD cycle duration. The period
        // (RSSI_SWEEP_INTERVAL_MS) must exceed the long-cycle wall time
        // (cycle-with-sweep) on every board so the timer actually skips
        // iterations instead of re-firing immediately on the next pass.
        //
        // Phase H: in HIGH_ALERT the period extends to
        // HIGH_ALERT_RSSI_INTERVAL_MS so CAD gets the scan budget. The
        // sweep still runs occasionally as a sanity check — candidate
        // engine keeps working either way since CAD produces its own
        // evidence.
        OperatingMode mode = modeGet();
        uint32_t rssiPeriodMs = (mode == MODE_HIGH_ALERT)
                              ? HIGH_ALERT_RSSI_INTERVAL_MS
                              : RSSI_SWEEP_INTERVAL_MS;
        bool didRssi = false;
        if ((int32_t)(millis() - nextSubGHzSweepMs) >= 0) {
            unsigned long sweepStart = millis();
            scannerSweep(radio, localResult);

#ifdef BOARD_T3S3_LR1121
            scannerSweep24(radio, local24);
#endif
            didRssi = true;

            // Update shared state with RSSI data
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                systemState.spectrum = localResult;
#ifdef BOARD_T3S3_LR1121
                systemState.spectrum24 = local24;
#endif
                systemState.lastSweepMs = millis();
                xSemaphoreGive(stateMutex);
            }

            // Ingest fresh sweep + re-assess with new RSSI data
#ifdef BOARD_T3S3_LR1121
            detectionEngineIngestSweep(localResult, &local24);
#else
            detectionEngineIngestSweep(localResult, nullptr);
#endif
            threat = detectionEngineAssess(snapGps, snapIntegrity);
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                systemState.threatLevel    = threat;
                systemState.fastScore      = detectionEngineGetFastScore();
                systemState.confirmScore   = detectionEngineGetConfirmScore();
                systemState.anchorFreq     = detectionEngineGetAnchorFreq();
                systemState.bandMask       = detectionEngineGetBandMask();
                systemState.hasCandidate   = detectionEngineHasCandidate();
                systemState.candidateCount = detectionEngineGetCandidateCount();
                xSemaphoreGive(stateMutex);
            }

            // Advance timer AFTER sweep completes so the period is measured
            // from sweep-end to next-fire-start. Advancing before the sweep
            // would make the next deadline expire while the current sweep
            // is still running, causing the gate to fire on every iteration.
            nextSubGHzSweepMs = millis() + rssiPeriodMs;
        }

        // Log to SD/SPIFFS — zero-init prevents garbage if mutex path is skipped.
        // Only write when the snapshot is real (mutex acquired).
        SystemState logSnapshot = {};
        bool snapshotValid = false;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10))) {
            logSnapshot = systemState;
            xSemaphoreGive(stateMutex);
            snapshotValid = true;
        }
        if (snapshotValid) {
            loggerWrite(logSnapshot, sweepNum++);
        }

        unsigned long cycleEnd = millis();

        // Serial output
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            // Timing instrumentation
            if (didRssi) {
                Serial.printf("[SCAN] CAD:%lums RSSI:%lums Total:%lums | Threat: %s\n",
                              cadDone - cycleStart,
                              cycleEnd - cadDone,
                              cycleEnd - cycleStart,
                              threatLevelStr(threat));
                scannerPrintCSV(localResult);
                Serial.printf("[SCAN] Peak: %.1f MHz @ %.1f dBm (%lu ms)\n",
                              localResult.peakFreq, localResult.peakRSSI,
                              localResult.sweepTimeMs);

                // 902-928 MHz US band peak analysis
                {
                    int startBin = (int)((902.0f - SCAN_FREQ_START) / SCAN_FREQ_STEP);
                    int endBin   = (int)((928.0f - SCAN_FREQ_START) / SCAN_FREQ_STEP);
                    if (endBin >= SCAN_BIN_COUNT) endBin = SCAN_BIN_COUNT - 1;
                    int bandBins = endBin - startBin + 1;

                    float nf = -120.0f;
                    for (int step = startBin; step <= endBin; step += 7) {
                        float candidate = localResult.rssi[step];
                        int below = 0, equal = 0;
                        for (int j = startBin; j <= endBin; j++) {
                            if (localResult.rssi[j] < candidate) below++;
                            else if (localResult.rssi[j] == candidate) equal++;
                        }
                        if (below <= bandBins / 2 && (below + equal) > bandBins / 2) {
                            nf = candidate;
                            break;
                        }
                    }

                    float relThresh = nf + 15.0f;
                    float absThresh = -85.0f;
                    float thresh = (relThresh > absThresh) ? relThresh : absThresh;

                    int elrsPeaks = 0;
                    float bestFreq = 0, bestRssi = -200;
                    for (int i = startBin + 1; i < endBin; i++) {
                        if (localResult.rssi[i] > thresh &&
                            localResult.rssi[i] > localResult.rssi[i-1] &&
                            localResult.rssi[i] > localResult.rssi[i+1]) {
                            elrsPeaks++;
                            if (localResult.rssi[i] > bestRssi) {
                                bestRssi = localResult.rssi[i];
                                bestFreq = SCAN_FREQ_START + (i * SCAN_FREQ_STEP);
                            }
                        }
                    }
                    if (elrsPeaks > 0) {
                        Serial.printf("[SCAN-PEAKS] 902-928: %d peaks, best %.1f MHz @ %.1f dBm (NF:%.0f)\n",
                                      elrsPeaks, bestFreq, bestRssi, nf);
                    }
                }
            } else {
                Serial.printf("[SCAN] CAD:%lums | Threat: %s\n",
                              cadDone - cycleStart, threatLevelStr(threat));
            }

            // Phase E: per-band visibility — subConf (sub-GHz confirmed CAD,
            // time-gated), sub24Conf (2.4 GHz confirmed CAD, LR1121 only),
            // fastConf (sub-GHz fast-confirmed CAD, no time gate). subConf and
            // sub24Conf together replace the aggregate `conf` field that mixed
            // both bands and made LR1121 debugging blind.
            int sub24ConfLog = 0;
#ifdef BOARD_T3S3_LR1121
            sub24ConfLog = cadFsk.band24.confirmedCadCount;
#endif
            if (cadFsk.subGHz.anchor.valid) {
                Serial.printf("[CAD] cycle=%u subConf=%d sub24Conf=%d fastConf=%d taps=%d div=%d persDiv=%d vel=%d sustainedCycles=%d score=%d fast=%d confirm=%d anchor=%.1fMHz SF%u hits=%u\n",
                              sweepNum,
                              cadFsk.subGHz.confirmedCadCount,
                              sub24ConfLog,
                              cadFsk.subGHz.fastConfirmedCadCount,
                              cadFsk.totalActiveTaps, cadFsk.diversityCount,
                              cadFsk.persistentDiversityCount,
                              cadFsk.diversityVelocity,
                              cadFsk.sustainedCycles,
                              detectionEngineGetScore(),
                              detectionEngineGetFastScore(),
                              detectionEngineGetConfirmScore(),
                              cadFsk.subGHz.anchor.frequency,
                              cadFsk.subGHz.anchor.sf,
                              cadFsk.subGHz.anchor.consecutiveHits);
            } else {
                Serial.printf("[CAD] cycle=%u subConf=%d sub24Conf=%d fastConf=%d taps=%d div=%d persDiv=%d vel=%d sustainedCycles=%d score=%d fast=%d confirm=%d anchor=none\n",
                              sweepNum,
                              cadFsk.subGHz.confirmedCadCount,
                              sub24ConfLog,
                              cadFsk.subGHz.fastConfirmedCadCount,
                              cadFsk.totalActiveTaps, cadFsk.diversityCount,
                              cadFsk.persistentDiversityCount,
                              cadFsk.diversityVelocity,
                              cadFsk.sustainedCycles,
                              detectionEngineGetScore(),
                              detectionEngineGetFastScore(),
                              detectionEngineGetConfirmScore());
            }

            xSemaphoreGive(serialMutex);
        }

        // Phase K: scan-cycle watchdog. cycleStart was captured at the top
        // of the loop; cycleEnd has already been computed above. We log but
        // do NOT reset — aborting mid-cycle would leave the SX1262/LR1121
        // in an unknown state (pending SPI transaction, incomplete CAD).
        // Raising the threshold is preferable to acting on this signal.
        if ((cycleEnd - cycleStart) > SCAN_WATCHDOG_MS) {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.printf("[WATCHDOG] Scan cycle exceeded %lums — cycle:%u duration:%lums\n",
                              (unsigned long)SCAN_WATCHDOG_MS,
                              sweepNum,
                              cycleEnd - cycleStart);
                xSemaphoreGive(serialMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Core 0 — GPS UART is only touched here, continuous buffer drain
static void gpsReadTask(void* param) {
    GpsData localGps = {};
    IntegrityStatus localIntegrity = {};
    CompassData localCompass = {};
    localCompass.scaleX = localCompass.scaleY = localCompass.scaleZ = 1.0f;
    localCompass.peakRSSI = -200.0f;
    unsigned long lastGpsPrintMs = 0;
    const unsigned long GPS_PRINT_INTERVAL_MS = 5000;

    for (;;) {
        gpsProcess();

        gpsUpdate(localGps);
        integrityUpdate(localGps, localIntegrity);

        // Compass on Wire1 — different bus from OLED, no contention
        compassRead(localCompass);
        if (compassIsCalibrating()) compassCalibrationTick(localCompass);

        // Track peak bearing using current strongest sub-GHz signal
        float peakRSSI = -200.0;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            peakRSSI = systemState.spectrum.peakRSSI;
            xSemaphoreGive(stateMutex);
        }
        compassUpdatePeakBearing(localCompass, peakRSSI);

        // Copy to shared state under lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            systemState.gps = localGps;
            systemState.integrity = localIntegrity;
            systemState.compass = localCompass;
            systemState.lastGpsMs = millis();
            xSemaphoreGive(stateMutex);
        }

        // Phase K: self-test GPS outcome. First valid 3D fix clears the
        // pending flag. If we exceed GPS_FIX_TIMEOUT_MS without a fix, log
        // the outcome once and stop checking. This runs on Core 0, so it
        // doesn't block setup() on Core 1 or delay task launch.
        if (gpsFixPending && localGps.valid && localGps.fixType >= 3) {
            gpsFixPending = false;
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.printf("[SELFTEST] GPS: OK (%d SVs)\n", localGps.numSV);
                xSemaphoreGive(serialMutex);
            }
        } else if (gpsFixPending && !gpsFixTimeoutLogged &&
                   (millis() - gpsFixStartMs) > GPS_FIX_TIMEOUT_MS) {
            gpsFixTimeoutLogged = true;
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.println("[SELFTEST] GPS: NO FIX after 120s");
                xSemaphoreGive(serialMutex);
            }
        }

        // Rate-limited serial output — GPS/integrity every 5s to prevent
        // serial buffer overflow during long soak tests
        unsigned long now = millis();
        if (localGps.valid && (now - lastGpsPrintMs >= GPS_PRINT_INTERVAL_MS) &&
            xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            gpsPrintStatus(localGps);
            integrityPrintStatus(localIntegrity, localGps);
            if (localCompass.valid) {
                Serial.printf("[COMPASS] HDG:%.0f° %s\n", localCompass.heading, localCompass.directionStr);
            }
            xSemaphoreGive(serialMutex);
            lastGpsPrintMs = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Screen render dispatch — one function per screen, all take the same signature
typedef void (*ScreenFn)(Adafruit_SSD1306&, const SystemState&, int);
static const ScreenFn screens[] = {
    screenDashboard, screenSpectrum, screenGPS, screenIntegrity, screenThreat, screenSystem,
    // Phase J: RID screen appears on every board — all three have WIFI_ENABLED.
    screenRID
#ifdef BOARD_T3S3_LR1121
    , screenSpectrum24
#endif
};

// Phase H: apply a mode change. Called by the button FSM on multi-press.
// Handles the side effects that a raw modeSet() wouldn't: waking the WiFi
// task on exit from COVERT, turning the OLED back on, announcing to
// serial + SD. Called from displayTask only — safe to touch oled/screen
// state directly here.
static void applyModeChange(OperatingMode newMode, Adafruit_SSD1306& disp) {
    OperatingMode prev = modeGet();
    if (prev == newMode) return;
    modeSet(newMode);

    // Format "HH:MM:SS" uptime once so serial + SD agree.
    char uptime[9];
    unsigned long sec = millis() / 1000;
    snprintf(uptime, sizeof(uptime), "%02lu:%02lu:%02lu",
             sec / 3600, (sec % 3600) / 60, sec % 60);

    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        Serial.printf("[MODE] Switched to %s at %s\n",
                      modeLongLabel(newMode), uptime);
        xSemaphoreGive(serialMutex);
    }
    loggerLogModeChange(modeLongLabel(newMode), uptime);

    // OLED: leaving COVERT re-powers the panel; entering COVERT blanks
    // and powers it off. ssd1306_command isn't exposed so we drive the
    // I2C display-off via the Adafruit API's dim()/display() primitives:
    // clearDisplay() + display() leaves the panel on but dark. That's
    // close enough to "off" for a passive detector in the field — the
    // backlight is the pixel matrix itself, so a dark screen emits
    // near-zero light. The next render cycle in non-COVERT repaints.
    if (newMode == MODE_COVERT) {
        disp.clearDisplay();
        disp.display();
    }

    // WiFi task: resuming on exit from COVERT — it reinits WiFi itself
    // at the top of its loop. Entering COVERT is handled by the task
    // noticing modeGet() == COVERT on its next iteration and then calling
    // vTaskSuspend(NULL) on itself.
    if (prev == MODE_COVERT && newMode != MODE_COVERT && hWiFiTask != nullptr) {
        vTaskResume(hWiFiTask);
    }
}

// Core 1 — OLED I2C and BOOT button only touched here
static void displayTask(void* param) {
    static SystemState local;
    int currentScreen = 0;
    int lastRenderedScreen = -1;
    unsigned long lastAdvanceMs = millis();
    unsigned long lastRenderMs = 0;
    bool buttonWasDown = false;
    unsigned long buttonDownMs = 0;

    // Phase H: multi-press FSM state.
    //   Tap (100-999 ms held) increments pressCount.
    //   After MULTI_PRESS_QUIET_MS of quiet following the last release,
    //   fire action based on count: 1=screen cycle, 2=HIGH_ALERT,
    //   3+=COVERT. Holds ≥1s retain their existing semantics
    //   (acknowledge / mute) and reset the counter.
    static const unsigned long MULTI_PRESS_QUIET_MS = 800;
    int pressCount = 0;
    unsigned long lastReleaseMs = 0;
    OperatingMode prevMode = modeGet();

    for (;;) {
        // Detect mode changes driven from outside this task (none today,
        // but future hook points) — would re-render on transition.
        OperatingMode mode = modeGet();
        if (mode != prevMode) {
            lastRenderedScreen = -1;
            prevMode = mode;
        }

        // ── Button input (polled at ~50ms) ─────────────────────
        bool buttonDown = (digitalRead(PIN_BOOT) == LOW);
        unsigned long nowMs = millis();

        if (buttonDown) {
            if (!buttonWasDown) {
                buttonDownMs = nowMs;
                buttonWasDown = true;
            }
        } else {
            if (buttonWasDown) {
                unsigned long held = nowMs - buttonDownMs;
                if (held >= 3000) {
                    // Long press — mute. Clears any pending multi-press
                    // so a tap-tap-hold sequence doesn't also toggle
                    // HIGH_ALERT after the mute fires.
                    alertToggleMute();
                    pressCount = 0;
                } else if (held >= 1000) {
                    // 1-2.999 s hold — acknowledge current alert.
                    alertAcknowledge();
                    pressCount = 0;
                } else if (held >= 100) {
                    // Tap — add to multi-press counter, fire after quiet.
                    pressCount++;
                    lastReleaseMs = nowMs;
                }
                // held < 100 ms: debounce, ignore.
            }
            buttonWasDown = false;
        }

        // Fire the multi-press action once the quiet window has elapsed
        // and the button is still released. This is what delays a single
        // tap's screen cycle by MULTI_PRESS_QUIET_MS — the cost of
        // disambiguating 1 vs 2 vs 3 taps without dedicated IRQ timing.
        if (pressCount > 0 && !buttonDown &&
            (nowMs - lastReleaseMs) >= MULTI_PRESS_QUIET_MS) {
            if (pressCount == 1) {
                // Single tap — cycle screen, but not in COVERT (display off).
                if (mode != MODE_COVERT) {
                    currentScreen = (currentScreen + 1) % NUM_SCREENS;
                    lastAdvanceMs = nowMs;
                    lastRenderedScreen = -1;
                }
            } else if (pressCount == 2) {
                // Double tap — toggle HIGH_ALERT.
                applyModeChange(
                    (mode == MODE_HIGH_ALERT) ? MODE_STANDARD : MODE_HIGH_ALERT,
                    oled);
            } else {
                // Triple+ tap — toggle COVERT.
                applyModeChange(
                    (mode == MODE_COVERT) ? MODE_STANDARD : MODE_COVERT,
                    oled);
            }
            pressCount = 0;
        }

        // ── Auto-rotate every 5 seconds (not in COVERT) ────────
        if (mode != MODE_COVERT && millis() - lastAdvanceMs > 5000) {
            currentScreen = (currentScreen + 1) % NUM_SCREENS;
            lastAdvanceMs = millis();
            lastRenderedScreen = -1;  // force re-render
        }

        // ── Render at 2 Hz (every 500ms) ───────────────────────
        if (mode != MODE_COVERT && millis() - lastRenderMs >= 500) {
            lastRenderMs = millis();

            // Battery voltage
#if defined(BOARD_T3S3) || defined(BOARD_T3S3_LR1121)
            {
                float voltage = analogReadMilliVolts(1) * 2.0 / 1000.0;
                int pct = (int)((voltage - 3.0) / (4.2 - 3.0) * 100.0);
                if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    systemState.batteryPercent = constrain(pct, 0, 100);
                    xSemaphoreGive(stateMutex);
                }
            }
#endif

            // Snapshot state
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                local = systemState;
                xSemaphoreGive(stateMutex);
            }

            // Freeze screen index for this render
            int renderScreen = currentScreen;

            // Render one complete frame
            screens[renderScreen](oled, local, renderScreen);
            lastRenderedScreen = renderScreen;
        }

        vTaskDelay(pdMS_TO_TICKS(50));  // 20 Hz button polling
    }
}

// Boot counter persists across soft resets via RTC memory
RTC_NOINIT_ATTR static uint32_t bootCount;

// Print stack high-water marks + heap — called once 30s after boot
static void printStackStats() {
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.println("[STACK] High-water marks (bytes free):");
        Serial.printf("  LoRa:    %u / %u\n", uxTaskGetStackHighWaterMark(hLoRaTask) * 4, STACK_LORA_SCAN);
        Serial.printf("  GPS:     %u / %u\n", uxTaskGetStackHighWaterMark(hGPSTask) * 4, STACK_GPS_READ);
        Serial.printf("  Display: %u / %u\n", uxTaskGetStackHighWaterMark(hDisplayTask) * 4, STACK_DISPLAY);
        Serial.printf("  Alert:   %u / %u\n", uxTaskGetStackHighWaterMark(hAlertTask) * 4, STACK_ALERT);
        Serial.printf("[HEAP] Free: %u bytes (min: %u)\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
        xSemaphoreGive(serialMutex);
    }
}

// ── Arduino entry points ────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);  // USB CDC enumeration time

    Serial.println();
    Serial.printf("========== %s v%s ==========\n", FW_NAME, FW_VERSION);

    // Boot counter — tracks resets for stability monitoring
    if (bootCount > 1000) bootCount = 0;
    bootCount++;
    Serial.printf("[BOOT] Boot #%u\n", bootCount);

    printBanner();

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);  // LED starts off — alert task takes over after boot
    pinMode(PIN_BOOT, INPUT);  // External pull-up on GPIO 0 — no INPUT_PULLUP needed

    // Create FreeRTOS primitives early — avoids mutex crash if setup() returns early
    stateMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    detectionQueue = xQueueCreate(10, sizeof(DetectionEvent));

    // Hardware init — splash stays on screen during entire init sequence
    displayInit(oled);
    displayBootSplash(oled);  // Shows logo, stays visible until first screen renders

    int hwState = initRadioHardware();
    if (hwState != RADIOLIB_ERR_NONE) {
        Serial.printf("[INIT] Radio hardware init failed: %d\n", hwState);
        char detail[22];
        formatRadioError(detail, sizeof(detail), hwState);
        displayFatalError(oled, "Radio HW init", detail);
        for (;;) delay(1000);
    }

    int scanState = scannerInit(radio);
    if (scanState != RADIOLIB_ERR_NONE) {
        Serial.printf("[INIT] Scanner init failed: %d\n", scanState);
        char detail[22];
        formatRadioError(detail, sizeof(detail), scanState);
        displayFatalError(oled, "Scanner init", detail);
        for (;;) delay(1000);
    }

    // Phase K: boot self-test replaces the old scannerAntennaCheck call.
    // runSelfTest() does both a receiver sanity check (10 RSSI reads @ 915
    // MHz) and a wider antenna coverage probe (10 frequencies across the
    // band), then arms the async GPS-fix timer. Warnings only — never halts.
    runSelfTest();
    displayBootSummary(oled, selfTestRadioOK, selfTestAntennaOK, "Acquiring...");
    delay(3000);

    cadScannerInit();

    if (!gpsInit()) {
        Serial.println("[INIT] GPS init failed — continuing without GPS");
    }

    integrityInit();
    detectionEngineInit();
    loggerInit();
    // Phase K: persist the self-test outcome to SD right after loggerInit
    // so even a single-boot run has a record.
    loggerLogSelfTest(selfTestRadioOK, selfTestAntennaOK, bootCount);

    // WiFi radio dedicated to promiscuous scanning for drone Remote ID
    wifiScannerInit();

    // Phase M: BLE scanner for ASTM F3411 BLE advertising (complements
    // WiFi RID). Shares the 2.4 GHz radio with WiFi via ESP-IDF coexistence;
    // 10% BLE duty cycle keeps WiFi largely undisturbed.
#ifdef HAS_BLE_RID
    bleScannerInit();
#endif

    initCompassBus();
    if (!compassInit()) {
        Serial.println("[COMPASS] Not detected — continuing without compass");
    }

    // Launch tasks — LoRa on Core 1 (exclusive, uses busy-wait sweep)
    // Display on Core 0 so OLED I2C transfers aren't starved by LoRa sweep
    xTaskCreatePinnedToCore(loRaScanTask, "LoRaScan", STACK_LORA_SCAN, nullptr,
                            PRIO_LORA_SCAN, &hLoRaTask, CORE_LORA);
    xTaskCreatePinnedToCore(gpsReadTask, "GPSRead", STACK_GPS_READ, nullptr,
                            PRIO_GPS_READ, &hGPSTask, CORE_GPS);
    xTaskCreatePinnedToCore(displayTask, "Display", STACK_DISPLAY, nullptr,
                            PRIO_DISPLAY, &hDisplayTask, CORE_GPS);
    xTaskCreatePinnedToCore(alertTask, "Alert", STACK_ALERT, nullptr,
                            PRIO_ALERT, &hAlertTask, CORE_GPS);
    xTaskCreatePinnedToCore(wifiScanTask, "WiFiScan", STACK_GPS_READ, nullptr,
                            PRIO_ALERT, &hWiFiTask, CORE_GPS);

#ifdef HAS_BLE_RID
    // Phase M: BLE RID scan task. Priority 1 (lowest of the tasks) because
    // it's purely asynchronous and its latency targets (one advertisement
    // per 500 ms) are orders of magnitude slacker than LoRa/GPS.
    xTaskCreatePinnedToCore(bleScanTask, "BLEScan", 4096, nullptr,
                            1, &hBLETask, CORE_GPS);
#endif

    // Set initial WiFi mode
    systemState.wifiScannerActive = true;

    digitalWrite(PIN_LED, LOW);
    Serial.println("[INIT] FreeRTOS tasks launched — LoRa:Core1, GPS+WiFi:Core0");
}

void loop() {
    // Print stack stats once after 30 seconds
    static bool statsPrinted = false;
    if (!statsPrinted && millis() > 30000) {
        printStackStats();
        statsPrinted = true;
    }

    // Periodic heap monitoring — detect memory leaks over time
    static unsigned long lastHeapPrint = 0;
    if (millis() - lastHeapPrint > 300000) {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            Serial.printf("[HEAP] Free: %u bytes (min: %u)\n",
                          ESP.getFreeHeap(), ESP.getMinFreeHeap());
            xSemaphoreGive(serialMutex);
        }
        lastHeapPrint = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}
