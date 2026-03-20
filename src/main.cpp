#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <Adafruit_SSD1306.h>
#include "board_config.h"
#include "version.h"
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"
#include "display.h"

// LoRa radio on custom SPI pins
SPIClass loraSPI(HSPI);
SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);

// OLED display on I2C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, PIN_OLED_RST);

// Shared data
ScanResult scanResult;
GpsData gpsData = {};
IntegrityStatus integrityStatus = {};

// Alternate OLED between spectrum and GPS every N loops
static const int DISPLAY_SWAP_INTERVAL = 5;
static int loopCount = 0;

int initRadioHardware() {
    loraSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

    // Heltec V3 requires TCXO voltage before any radio begin() call
    if (HAS_TCXO) {
        int tcxoState = radio.setTCXO(1.8);
        if (tcxoState != RADIOLIB_ERR_NONE) {
            Serial.printf("[LoRa] TCXO set failed: %d\n", tcxoState);
            return tcxoState;
        }
    }

    return RADIOLIB_ERR_NONE;
}

void initCompassBus() {
    // Second I2C bus for compass — separate from OLED to avoid clock conflicts
#ifdef BOARD_T3S3
    if (HAS_COMPASS) {
        Wire1.begin(PIN_COMPASS_SDA, PIN_COMPASS_SCL);
        Serial.printf("[I2C1] Compass bus on SDA=%d SCL=%d\n",
                      PIN_COMPASS_SDA, PIN_COMPASS_SCL);
    }
#endif
}

void printBanner() {
    Serial.println("========================");
    Serial.printf(" %s v%s\n", FW_NAME, FW_VERSION);
    Serial.printf(" Build: %s\n", FW_DATE);
#ifdef BOARD_T3S3
    Serial.println(" Board: LilyGo T3S3");
#elif defined(BOARD_HELTEC_V3)
    Serial.println(" Board: Heltec V3");
#endif
    Serial.println("========================");
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    printBanner();

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    // Display init and boot splash
    displayInit(display);
    displayBootSplash(display);

    // Radio hardware init (SPI + TCXO), then switch to FSK scanning mode
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

    // GPS: configure UBX mode and bump baud to 38400
    if (!gpsInit()) {
        Serial.println("[INIT] GPS init failed — continuing without GPS");
    }

    // GNSS integrity monitor
    integrityInit();

    // Compass I2C bus (T3S3 only, read in future sprint)
    initCompassBus();

    digitalWrite(PIN_LED, LOW);
    Serial.println("[INIT] Ready — scanning + GPS + integrity active");
}

void loop() {
    // Drain GPS UART buffer every iteration — must run before anything else
    gpsProcess();

    digitalWrite(PIN_LED, HIGH);

    // RF sweep
    scannerSweep(radio, scanResult);
    scannerPrintCSV(scanResult);
    scannerPrintSummary(scanResult);

    // GPS poll (rate-limited internally to 1 Hz)
    gpsUpdate(gpsData);
    gpsPrintStatus(gpsData);

    // GNSS integrity analysis
    integrityUpdate(gpsData, integrityStatus);
    integrityPrintStatus(integrityStatus, gpsData);

    // Rotate OLED: spectrum → GPS → integrity (5 loops each)
    int screen = (loopCount / DISPLAY_SWAP_INTERVAL) % 3;
    if (screen == 0) {
        displaySpectrum(display, scanResult);
    } else if (screen == 1) {
        displayGPS(display, gpsData);
    } else {
        displayIntegrity(display, gpsData, integrityStatus);
    }
    loopCount++;

    digitalWrite(PIN_LED, LOW);
    delay(100);
}
