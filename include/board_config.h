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

// SD card SPI — separate bus from LoRa (T3S3 v1.3 hardware doc)
static const int PIN_SD_CS   = 13;
static const int PIN_SD_SCK  = 14;
static const int PIN_SD_MISO = 2;
static const int PIN_SD_MOSI = 11;

// Status LED and button
static const int PIN_LED  = 37;
static const int PIN_BOOT = 0;   // BOOT button, active LOW

// Buzzer
static const int PIN_BUZZER = 16;  // GPIO 16 — PWM-capable, on header
static const bool HAS_BUZZER = true;

// Board capabilities
static const bool HAS_OLED_RST  = false;
static const bool HAS_OLED_VEXT = false;
static const bool HAS_TCXO      = false;
static const bool HAS_PSRAM     = true;
static const bool HAS_SD_CARD   = true;
static const bool HAS_COMPASS   = true;
static const bool HAS_LR1121    = false;
static const bool HAS_24GHZ     = false;
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

// Buzzer
static const int PIN_BUZZER = 47;  // GPIO 47 — PWM-capable
static const bool HAS_BUZZER = true;

// Board capabilities
static const bool HAS_OLED_RST  = true;
static const bool HAS_OLED_VEXT = true;
static const bool HAS_TCXO      = true;  // Must call setTCXO(1.8) before begin()
static const bool HAS_PSRAM     = false;
static const bool HAS_SD_CARD   = false;
static const bool HAS_COMPASS   = false;
static const bool HAS_LR1121    = false;
static const bool HAS_24GHZ     = false;
static const bool WIFI_ENABLED  = true;

#elif defined(BOARD_T3S3_LR1121)
// ----- LilyGo T3S3 with LR1121 (dual-band: sub-GHz + 2.4 GHz) -----
// Identical pin mapping to T3S3 SX1262 — only the radio chip differs

// LR1121 LoRa SPI (same SPI bus as T3S3 SX1262, but IRQ is DIO9 not DIO1)
static const int PIN_LORA_SCK  = 5;
static const int PIN_LORA_MISO = 3;
static const int PIN_LORA_MOSI = 6;
static const int PIN_LORA_CS   = 7;
static const int PIN_LORA_RST  = 8;
static const int PIN_LORA_DIO1 = 36;   // LR1121 uses DIO9 (GPIO 36) as IRQ, not DIO1
static const int PIN_LORA_BUSY = 34;
static const float LR1121_TCXO_VOLTAGE = 3.0f;  // LR1121 TCXO is 3.0V (2.85-3.15V range)

// OLED SSD1306 I2C
static const int PIN_OLED_SDA = 18;
static const int PIN_OLED_SCL = 17;
static const int PIN_OLED_RST = -1;
static const int PIN_OLED_VEXT = -1;

// Compass QMC5883L I2C
static const int PIN_COMPASS_SDA = 21;
static const int PIN_COMPASS_SCL = 10;

// GPS UART1
static const int PIN_GPS_RX = 44;
static const int PIN_GPS_TX = 43;

// SD card SPI (T3S3 v1.3 hardware doc)
static const int PIN_SD_CS   = 13;
static const int PIN_SD_SCK  = 14;
static const int PIN_SD_MISO = 2;
static const int PIN_SD_MOSI = 11;  // was 15 (wrong)

// Status LED and button
static const int PIN_LED  = 37;
static const int PIN_BOOT = 0;

// Buzzer
static const int PIN_BUZZER = 16;  // GPIO 16 — same as T3S3
static const bool HAS_BUZZER = true;

// Board capabilities
static const bool HAS_OLED_RST  = false;
static const bool HAS_OLED_VEXT = false;
static const bool HAS_TCXO      = true;   // LR1121 needs TCXO like Heltec
static const bool HAS_PSRAM     = true;
static const bool HAS_SD_CARD   = true;
static const bool HAS_COMPASS   = true;
static const bool HAS_LR1121    = true;
static const bool HAS_24GHZ     = true;
static const bool WIFI_ENABLED  = true;

#else
#error "No board defined! Use -DBOARD_T3S3, -DBOARD_HELTEC_V3, or -DBOARD_T3S3_LR1121"
#endif

// Common constants
static const uint32_t OLED_I2C_ADDR  = 0x3C;
static const uint32_t GPS_BAUD_DEFAULT = 9600;
static const uint32_t GPS_BAUD_TARGET  = 38400;

// Buzzer configuration (common to all boards)
static const int BUZZER_LEDC_CHANNEL    = 1;  // Channel 1 — LED uses digitalWrite, no conflict
static const int BUZZER_LEDC_RESOLUTION = 8;  // 8-bit duty cycle

#endif // BOARD_CONFIG_H
