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
#include "alert_handler.h"

// Hardware objects — each owned by exactly one task after setup()
SPIClass loraSPI(HSPI);
Module radioMod(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
#ifdef BOARD_T3S3_LR1121
LR1121 radio(&radioMod);
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
static TaskHandle_t hWiFiTask    = nullptr;

// ── Hardware init (runs once in setup on Core 1) ────────────────────────────

static int initRadioHardware() {
    loraSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

    // Heltec SX1262 needs TCXO set before beginFSK — LR1121 handles it in beginGFSK
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

// RSSI_SWEEP_INTERVAL from sentry_config.h
#include "sentry_config.h"

// Core 1 — LoRa SPI is only touched here, no mutex needed for the radio
static void loRaScanTask(void* param) {
    ScanResult localResult;
    GpsData snapGps = {};
    IntegrityStatus snapIntegrity = {};
    uint32_t sweepNum = 0;
    uint32_t cycleCount = 0;
#ifdef BOARD_T3S3_LR1121
    ScanResult24 local24;
#endif

    // ── Main scan loop ────────────────────────────────────────────────────
    for (;;) {
        cycleCount++;
        unsigned long cycleStart = millis();

        // ── PHASE 1: CAD scan FIRST (~1000ms) ────────────────────────
        // Highest-confidence detection. Runs EVERY cycle.
        // Pass last RSSI data for RSSI-guided CAD scanning.
        CadFskResult cadFsk = cadFskScan(radio, sweepNum,
                                          localResult.sweepTimeMs > 0 ? &localResult : nullptr);
        detectionEngineSetCadFsk(cadFsk.confirmedCadCount, cadFsk.confirmedFskCount,
                                cadFsk.strongPendingCad, cadFsk.totalActiveTaps,
                                cadFsk.diversityCount,
                                cadFsk.persistentDiversityCount,
                                cadFsk.diversityVelocity);

        unsigned long cadDone = millis();

        // Snapshot GPS/integrity under lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            snapGps = systemState.gps;
            snapIntegrity = systemState.integrity;
            xSemaphoreGive(stateMutex);
        }

        // Evaluate threat immediately with CAD results + last RSSI data
#ifdef BOARD_T3S3_LR1121
        ThreatLevel threat = detectionEngineUpdate(localResult, snapGps, snapIntegrity, &local24);
#else
        ThreatLevel threat = detectionEngineUpdate(localResult, snapGps, snapIntegrity);
#endif

        // Store threat + CAD results into shared state
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            systemState.threatLevel = threat;
            systemState.cadDiversity = cadFsk.diversityCount;
            systemState.cadConfirmed = cadFsk.confirmedCadCount;
            systemState.cadTotalTaps = cadFsk.totalActiveTaps;
            systemState.confidenceScore = detectionEngineGetScore();
            xSemaphoreGive(stateMutex);
        }

        // ── PHASE 2: RSSI sweep every Nth cycle (~2.2s) ─────────────
        bool didRssi = false;
        if (cycleCount % RSSI_SWEEP_INTERVAL == 0) {
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

            // Re-assess with fresh RSSI data
#ifdef BOARD_T3S3_LR1121
            threat = detectionEngineUpdate(localResult, snapGps, snapIntegrity, &local24);
#else
            threat = detectionEngineUpdate(localResult, snapGps, snapIntegrity);
#endif
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                systemState.threatLevel = threat;
                xSemaphoreGive(stateMutex);
            }
        }

        // Log to SD/SPIFFS
        loggerWrite(systemState, sweepNum++);

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

            Serial.printf("[CAD] cycle=%u conf=%d taps=%d div=%d persDiv=%d vel=%d sustainedCycles=%d score=%d\n",
                          sweepNum, cadFsk.confirmedCadCount,
                          cadFsk.totalActiveTaps, cadFsk.diversityCount,
                          cadFsk.persistentDiversityCount,
                          cadFsk.diversityVelocity,
                          cadFsk.sustainedCycles,
                          detectionEngineGetScore());

            xSemaphoreGive(serialMutex);
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

        // Print outside state lock but inside serial lock
        if (localGps.valid && xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            gpsPrintStatus(localGps);
            integrityPrintStatus(localIntegrity, localGps);
            if (localCompass.valid) {
                Serial.printf("[COMPASS] HDG:%.0f° %s\n", localCompass.heading, localCompass.directionStr);
            }
            xSemaphoreGive(serialMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Screen render dispatch — one function per screen, all take the same signature
typedef void (*ScreenFn)(Adafruit_SSD1306&, const SystemState&, int);
static const ScreenFn screens[] = {
    screenDashboard, screenSpectrum, screenGPS, screenIntegrity, screenThreat, screenSystem
#ifdef BOARD_T3S3_LR1121
    , screenSpectrum24
#endif
};

// Core 1 — OLED I2C and BOOT button only touched here
static void displayTask(void* param) {
    static SystemState local;
    int currentScreen = 0;
    int lastRenderedScreen = -1;
    unsigned long lastAdvanceMs = millis();
    unsigned long lastRenderMs = 0;
    bool buttonWasDown = false;
    unsigned long buttonDownMs = 0;

    for (;;) {
        // ── Button input (polled at ~50ms) ─────────────────────
        bool buttonDown = (digitalRead(PIN_BOOT) == LOW);

        if (buttonDown) {
            if (!buttonWasDown) {
                buttonDownMs = millis();
                buttonWasDown = true;
            }
        } else {
            if (buttonWasDown) {
                unsigned long held = millis() - buttonDownMs;
                if (held >= 3000) {
                    alertToggleMute();
                } else if (held >= 1000) {
                    alertAcknowledge();
                } else if (held >= 100) {
                    currentScreen = (currentScreen + 1) % NUM_SCREENS;
                    lastAdvanceMs = millis();
                    lastRenderedScreen = -1;  // force re-render
                }
            }
            buttonWasDown = false;
        }

        // ── Auto-rotate every 5 seconds ────────────────────────
        if (millis() - lastAdvanceMs > 5000) {
            currentScreen = (currentScreen + 1) % NUM_SCREENS;
            lastAdvanceMs = millis();
            lastRenderedScreen = -1;  // force re-render
        }

        // ── Render at 2 Hz (every 500ms) ───────────────────────
        if (millis() - lastRenderMs >= 500) {
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
    delay(300);

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

    // Hardware init — splash stays on screen during entire init sequence
    displayInit(oled);
    displayBootSplash(oled);  // Shows logo, stays visible until first screen renders

    int hwState = initRadioHardware();
    if (hwState != RADIOLIB_ERR_NONE) {
        Serial.printf("[INIT] Radio hardware init failed: %d\n", hwState);
        return;
    }

    int scanState = scannerInit(radio);
    if (scanState != RADIOLIB_ERR_NONE) {
        Serial.printf("[INIT] Scanner init failed: %d\n", scanState);
        return;
    }

    cadScannerInit();

    if (!gpsInit()) {
        Serial.println("[INIT] GPS init failed — continuing without GPS");
    }

    integrityInit();
    detectionEngineInit();
    loggerInit();

    // WiFi radio dedicated to promiscuous scanning for drone Remote ID
    wifiScannerInit();

    initCompassBus();
    if (!compassInit()) {
        Serial.println("[COMPASS] Not detected — continuing without compass");
    }

    // Create FreeRTOS primitives
    stateMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    detectionQueue = xQueueCreate(10, sizeof(DetectionEvent));

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
