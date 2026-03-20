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
#include "display.h"
#include "detection_engine.h"
#include "data_logger.h"
#include "wifi_dashboard.h"
#include "alert_handler.h"

// Hardware objects — each owned by exactly one task after setup()
SPIClass loraSPI(HSPI);
SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
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

// ── Hardware init (runs once in setup on Core 1) ────────────────────────────

static int initRadioHardware() {
    loraSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

    if (HAS_TCXO) {
        int tcxoState = radio.setTCXO(1.8);
        if (tcxoState != RADIOLIB_ERR_NONE) {
            Serial.printf("[LoRa] TCXO set failed: %d\n", tcxoState);
            return tcxoState;
        }
    }

    return RADIOLIB_ERR_NONE;
}

static void initCompassBus() {
#ifdef BOARD_T3S3
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

        // Snapshot GPS/integrity + update scan in shared state under one lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            systemState.spectrum = localResult;
            systemState.lastSweepMs = millis();
            snapGps = systemState.gps;
            snapIntegrity = systemState.integrity;
            xSemaphoreGive(stateMutex);
        }

        // Detection engine — single-threaded, runs only here
        ThreatLevel threat = detectionEngineUpdate(localResult, snapGps, snapIntegrity);

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

    for (;;) {
        // Drain UART every iteration — this is the whole reason GPS gets its own core
        gpsProcess();

        // Read and analyze at 1 Hz (rate-limited inside gpsUpdate)
        gpsUpdate(localGps);
        integrityUpdate(localGps, localIntegrity);

        // Copy to shared state under lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            systemState.gps = localGps;
            systemState.integrity = localIntegrity;
            systemState.lastGpsMs = millis();
            xSemaphoreGive(stateMutex);
        }

        // Print outside state lock but inside serial lock to prevent garbling
        if (localGps.valid && xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            gpsPrintStatus(localGps);
            integrityPrintStatus(localIntegrity, localGps);
            xSemaphoreGive(serialMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Screen render dispatch — one function per screen, all take the same signature
typedef void (*ScreenFn)(Adafruit_SSD1306&, const SystemState&, int);
static const ScreenFn screens[] = {
    screenSpectrum, screenGPS, screenIntegrity, screenThreat, screenSystem
};

// Core 1 — OLED I2C and BOOT button only touched here
static void displayTask(void* param) {
    static SystemState local;
    int currentScreen = 0;
    unsigned long lastButtonMs = 0;
    unsigned long lastAdvanceMs = millis();

    for (;;) {
        // Button: advance screen on press with 200ms debounce
        if (digitalRead(PIN_BOOT) == LOW && (millis() - lastButtonMs > 100)) {
            currentScreen = (currentScreen + 1) % NUM_SCREENS;
            lastButtonMs = millis();
            lastAdvanceMs = millis();
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.printf("[UI] Button press — screen %d\n", currentScreen);
                xSemaphoreGive(serialMutex);
            }
        }

        // Auto-rotate every 5 seconds if no button press
        if (millis() - lastAdvanceMs > 5000) {
            currentScreen = (currentScreen + 1) % NUM_SCREENS;
            lastAdvanceMs = millis();
        }

        // Snapshot shared state under lock
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            local = systemState;
            xSemaphoreGive(stateMutex);
        }

        // Render current screen outside lock
        screens[currentScreen](oled, local, currentScreen);

        // WiFi dashboard — update state and serve HTTP from this task only
        dashboardUpdateState(local);
        dashboardHandle();

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
    dashboardInit();
    initCompassBus();

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

    digitalWrite(PIN_LED, LOW);
    Serial.println("[INIT] FreeRTOS tasks launched — LoRa:Core1, GPS:Core0");
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
