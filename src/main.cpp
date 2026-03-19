#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <Adafruit_SSD1306.h>
#include "board_config.h"
#include "version.h"
#include "rf_scanner.h"
#include "display.h"

// LoRa radio on custom SPI pins
SPIClass loraSPI(HSPI);
SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);

// OLED display on I2C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, PIN_OLED_RST);

// Scan results buffer
ScanResult scanResult;

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

    // GPS UART — port open only, full init in Sprint 3
    Serial1.begin(GPS_BAUD_DEFAULT, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    Serial.printf("[GPS] UART1 open at %d baud (RX=%d, TX=%d)\n",
                  GPS_BAUD_DEFAULT, PIN_GPS_RX, PIN_GPS_TX);

    digitalWrite(PIN_LED, LOW);
    Serial.println("[INIT] Ready — starting spectrum sweep");
}

void loop() {
    digitalWrite(PIN_LED, HIGH);

    scannerSweep(radio, scanResult);
    scannerPrintCSV(scanResult);
    scannerPrintSummary(scanResult);
    displaySpectrum(display, scanResult);

    digitalWrite(PIN_LED, LOW);

    // Brief pause between sweeps so serial output stays readable
    delay(100);
}
