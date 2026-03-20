#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// ============================================================
// SENTRY-RF Board Pin Configuration
// All hardware pin assignments live here — never hardcode pins
// in source files. Use these symbols everywhere.
// ============================================================

#ifdef BOARD_T3S3
// ----- LilyGo T3S3 -----

// SX1262 LoRa SPI
static const int PIN_LORA_SCK  = 5;
static const int PIN_LORA_MISO = 3;
static const int PIN_LORA_MOSI = 6;
static const int PIN_LORA_CS   = 7;
static const int PIN_LORA_RST  = 8;
static const int PIN_LORA_DIO1 = 33;
static const int PIN_LORA_BUSY = 34;

// OLED SSD1306 I2C
static const int PIN_OLED_SDA = 18;
static const int PIN_OLED_SCL = 17;
static const int PIN_OLED_RST = -1;  // T3S3 has no OLED reset pin
static const int PIN_OLED_VEXT = -1; // T3S3 has no Vext control

// Compass QMC5883L I2C — connected via QWIIC connector (GPIO 10/21)
static const int PIN_COMPASS_SDA = 21;
static const int PIN_COMPASS_SCL = 10;

// GPS UART1
static const int PIN_GPS_RX = 44;
static const int PIN_GPS_TX = 43;

// SD card SPI — separate bus from LoRa
static const int PIN_SD_CS   = 13;
static const int PIN_SD_SCK  = 14;
static const int PIN_SD_MISO = 2;
static const int PIN_SD_MOSI = 15;

// Status LED and button
static const int PIN_LED  = 37;
static const int PIN_BOOT = 0;   // BOOT button, active LOW

// Board capabilities
static const bool HAS_OLED_RST  = false;
static const bool HAS_OLED_VEXT = false;
static const bool HAS_TCXO      = false;
static const bool HAS_PSRAM     = true;
static const bool HAS_SD_CARD   = true;
static const bool HAS_COMPASS   = true;
static const bool WIFI_ENABLED  = true;

#elif defined(BOARD_HELTEC_V3)
// ----- Heltec WiFi LoRa 32 V3 -----

// SX1262 LoRa SPI
static const int PIN_LORA_SCK  = 9;
static const int PIN_LORA_MISO = 11;
static const int PIN_LORA_MOSI = 10;
static const int PIN_LORA_CS   = 8;
static const int PIN_LORA_RST  = 12;
static const int PIN_LORA_DIO1 = 14;
static const int PIN_LORA_BUSY = 13;

// OLED SSD1306 I2C
static const int PIN_OLED_SDA = 17;
static const int PIN_OLED_SCL = 18;
static const int PIN_OLED_RST = 21;   // Must pulse LOW 50ms then HIGH
static const int PIN_OLED_VEXT = 36;  // Set LOW to power OLED

// GPS UART1
static const int PIN_GPS_RX = 46;
static const int PIN_GPS_TX = 45;

// Status LED and button
static const int PIN_LED  = 35;
static const int PIN_BOOT = 0;   // BOOT button, active LOW

// Board capabilities
static const bool HAS_OLED_RST  = true;
static const bool HAS_OLED_VEXT = true;
static const bool HAS_TCXO      = true;  // Must call setTCXO(1.8) before begin()
static const bool HAS_PSRAM     = false;
static const bool HAS_SD_CARD   = false;
static const bool HAS_COMPASS   = false;
static const bool WIFI_ENABLED  = true;

#else
#error "No board defined! Use -DBOARD_T3S3 or -DBOARD_HELTEC_V3"
#endif

// Common constants
static const uint32_t OLED_I2C_ADDR  = 0x3C;
static const uint32_t GPS_BAUD_DEFAULT = 9600;
static const uint32_t GPS_BAUD_TARGET  = 38400;

#endif // BOARD_CONFIG_H
