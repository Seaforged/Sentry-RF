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
#include "data_logger.h"
#include "wifi_dashboard.h"
#include "wifi_scanner.h"
#include "alert_handler.h"

// Hardware objects — each owned by exactly one task after setup()
SPIClass loraSPI(HSPI);
#ifdef BOARD_T3S3_LR1121
LR1121 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
#else
SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
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

// Core 1 — LoRa SPI is only touched here, no mutex needed for the radio
static void loRaScanTask(void* param) {
    ScanResult localResult;
    GpsData snapGps = {};
    IntegrityStatus snapIntegrity = {};
    uint32_t sweepNum = 0;

    for (;;) {
        scannerSweep(radio, localResult);

#ifdef BOARD_T3S3_LR1121
        // 2.4 GHz sweep — LR1121 handles band switch transparently
        ScanResult24 local24;
        scannerSweep24(radio, local24);
#endif

        // Snapshot GPS/integrity + update scan in shared state under one lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            systemState.spectrum = localResult;
#ifdef BOARD_T3S3_LR1121
            systemState.spectrum24 = local24;
#endif
            systemState.lastSweepMs = millis();
            snapGps = systemState.gps;
            snapIntegrity = systemState.integrity;
            xSemaphoreGive(stateMutex);
        }

        // Detection engine — single-threaded, runs only here
#ifdef BOARD_T3S3_LR1121
        ThreatLevel threat = detectionEngineUpdate(localResult, snapGps, snapIntegrity, &local24);
#else
        ThreatLevel threat = detectionEngineUpdate(localResult, snapGps, snapIntegrity);
#endif

        // Store threat level back into shared state
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            systemState.threatLevel = threat;
            xSemaphoreGive(stateMutex);
        }

        // Log to SD/SPIFFS — uses its own SPI bus (FSPI), no conflict with LoRa (HSPI)
        loggerWrite(systemState, sweepNum++);

        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            scannerPrintCSV(localResult);
            Serial.printf("[SCAN] Peak: %.1f MHz @ %.1f dBm (%lu ms) | Threat: %s\n",
                          localResult.peakFreq, localResult.peakRSSI,
                          localResult.sweepTimeMs, threatLevelStr(threat));
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
    unsigned long lastAdvanceMs = millis();
    bool buttonWasDown = false;
    unsigned long buttonDownMs = 0;
    bool dashboardMode = false;
    unsigned long lastShortPressMs = 0;  // for double-press calibration detection
    unsigned long calDisplayStartMs = 0;
    bool wasCalibrating = false;

    for (;;) {
        bool buttonDown = (digitalRead(PIN_BOOT) == LOW);

        if (buttonDown) {
            if (!buttonWasDown) {
                buttonDownMs = millis();
                buttonWasDown = true;
            }

            unsigned long held = millis() - buttonDownMs;

            // Show countdown on OLED while holding for dashboard mode
            if (held > 1000 && !dashboardMode) {
                int remaining = 5 - (int)(held / 1000);
                if (remaining > 0) {
                    oled.clearDisplay();
                    oled.setTextSize(1);
                    oled.setTextColor(SSD1306_WHITE);
                    oled.setCursor(10, 24);
                    oled.printf("Dashboard in %d...", remaining);
                    oled.display();
                }
            }

            // 5-second hold: switch to dashboard mode
            if (held >= 5000 && !dashboardMode) {
                wifiScannerStop();
                dashboardInit();
                dashboardMode = true;
                if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    systemState.wifiScannerActive = false;
                    systemState.dashboardActive = true;
                    xSemaphoreGive(stateMutex);
                }
                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    Serial.println("[UI] Dashboard mode activated — power cycle to resume scanning");
                    xSemaphoreGive(serialMutex);
                }
            }
        } else {
            if (buttonWasDown && !dashboardMode) {
                unsigned long held = millis() - buttonDownMs;
                if (held >= 100 && held < 1000) {
                    // Double-press within 500ms: toggle compass calibration
                    if (lastShortPressMs > 0 && millis() - lastShortPressMs < 500) {
                        compassStartCalibration();
                        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            Serial.println("[UI] Double-press — calibration toggle");
                            xSemaphoreGive(serialMutex);
                        }
                        lastShortPressMs = 0;
                    } else {
                        // Single press: advance screen
                        currentScreen = (currentScreen + 1) % NUM_SCREENS;
                        lastAdvanceMs = millis();
                        lastShortPressMs = millis();
                        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            Serial.printf("[UI] Button press — screen %d\n", currentScreen);
                            xSemaphoreGive(serialMutex);
                        }
                    }
                }
            }
            buttonWasDown = false;
        }

        // Auto-rotate every 5 seconds if no button press
        if (!dashboardMode && (millis() - lastAdvanceMs > 5000)) {
            currentScreen = (currentScreen + 1) % NUM_SCREENS;
            lastAdvanceMs = millis();
        }

        // Battery voltage on GPIO 1 (T3S3 voltage divider halves battery voltage)
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

        // Snapshot shared state under lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            local = systemState;
            xSemaphoreGive(stateMutex);
        }

        if (!dashboardMode) {
            if (compassIsCalibrating()) {
                if (!wasCalibrating) {
                    calDisplayStartMs = millis();
                    wasCalibrating = true;
                }
                unsigned long elapsed = (millis() - calDisplayStartMs) / 1000;
                oled.clearDisplay();
                oled.setTextSize(1);
                oled.setTextColor(SSD1306_WHITE);
                oled.setCursor(10, 8);
                oled.println("CALIBRATING");
                oled.setCursor(10, 24);
                oled.printf("Rotate 360%c (%lus)", 0xF8, elapsed);
                oled.setCursor(10, 44);
                oled.println("Double-press to stop");
                oled.display();
            } else {
                wasCalibrating = false;
                screens[currentScreen](oled, local, currentScreen);
            }
        } else {
            // Dashboard mode: serve HTTP + show status on OLED
            dashboardUpdateState(local);
            dashboardHandle();

            oled.clearDisplay();
            oled.setTextSize(1);
            oled.setTextColor(SSD1306_WHITE);
            oled.setCursor(0, 0);
            oled.println("DASHBOARD MODE");
            oled.setCursor(0, 16);
            oled.println("SSID: SENTRY-RF");
            oled.setCursor(0, 28);
            oled.println("http://192.168.4.1");
            oled.setCursor(0, 44);
            oled.println("Reset to scan mode");
            oled.display();
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// Print stack high-water marks — called once 30s after boot
static void printStackStats() {
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.println("[STACK] High-water marks (bytes free):");
        Serial.printf("  LoRa:    %u / %u\n", uxTaskGetStackHighWaterMark(hLoRaTask) * 4, STACK_LORA_SCAN);
        Serial.printf("  GPS:     %u / %u\n", uxTaskGetStackHighWaterMark(hGPSTask) * 4, STACK_GPS_READ);
        Serial.printf("  Display: %u / %u\n", uxTaskGetStackHighWaterMark(hDisplayTask) * 4, STACK_DISPLAY);
        Serial.printf("  Alert:   %u / %u\n", uxTaskGetStackHighWaterMark(hAlertTask) * 4, STACK_ALERT);
        xSemaphoreGive(serialMutex);
    }
}

// ── Arduino entry points ────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);

    printBanner();

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);
    pinMode(PIN_BOOT, INPUT);  // External pull-up on GPIO 0 — no INPUT_PULLUP needed

    // Hardware init — all on Core 1 before tasks start
    displayInit(oled);
    displayBootSplash(oled);

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

    if (!gpsInit()) {
        Serial.println("[INIT] GPS init failed — continuing without GPS");
    }

    integrityInit();
    detectionEngineInit();
    loggerInit();

    // Default to WiFi scanner mode — dashboard only activates on 5-second button hold
    wifiScannerInit();

    initCompassBus();
    if (!compassInit()) {
        Serial.println("[COMPASS] Not detected — continuing without compass");
    }

    // Create FreeRTOS primitives
    stateMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    detectionQueue = xQueueCreate(10, sizeof(DetectionEvent));

    // Launch tasks — GPS on Core 0, everything else on Core 1
    xTaskCreatePinnedToCore(loRaScanTask, "LoRaScan", STACK_LORA_SCAN, nullptr,
                            PRIO_LORA_SCAN, &hLoRaTask, CORE_LORA);
    xTaskCreatePinnedToCore(gpsReadTask, "GPSRead", STACK_GPS_READ, nullptr,
                            PRIO_GPS_READ, &hGPSTask, CORE_GPS);
    xTaskCreatePinnedToCore(displayTask, "Display", STACK_DISPLAY, nullptr,
                            PRIO_DISPLAY, &hDisplayTask, CORE_LORA);
    xTaskCreatePinnedToCore(alertTask, "Alert", STACK_ALERT, nullptr,
                            PRIO_ALERT, &hAlertTask, CORE_GPS);
    xTaskCreatePinnedToCore(wifiScanTask, "WiFiScan", STACK_GPS_READ, nullptr,
                            PRIO_ALERT, &hWiFiTask, CORE_GPS);

    // Set initial WiFi mode
    systemState.wifiScannerActive = true;
    systemState.dashboardActive = false;

    digitalWrite(PIN_LED, LOW);
    Serial.println("[INIT] FreeRTOS tasks launched — LoRa:Core1, GPS+WiFi:Core0");
}

void loop() {
    // Print stack stats once after 30 seconds, then sleep forever
    static bool statsPrinted = false;
    if (!statsPrinted && millis() > 30000) {
        printStackStats();
        statsPrinted = true;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}
