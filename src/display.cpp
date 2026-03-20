#include "display.h"
#include "board_config.h"
#include "version.h"
#include <Arduino.h>
#include <Wire.h>

static const char* fixTypeStr(uint8_t fixType) {
    switch (fixType) {
        case 2:  return "2D";
        case 3:  return "3D";
        default: return "None";
    }
}

void displayInit(Adafruit_SSD1306& disp) {
    // Heltec V3: Vext pin controls OLED power rail
    if (HAS_OLED_VEXT) {
        pinMode(PIN_OLED_VEXT, OUTPUT);
        digitalWrite(PIN_OLED_VEXT, LOW);
        delay(10);
    }

    // Heltec V3: SSD1306 needs a hardware reset pulse
    if (HAS_OLED_RST) {
        pinMode(PIN_OLED_RST, OUTPUT);
        digitalWrite(PIN_OLED_RST, LOW);
        delay(50);
        digitalWrite(PIN_OLED_RST, HIGH);
        delay(100);
    }

    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

    if (!disp.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
        Serial.println("[OLED] FAIL: SSD1306 not found at 0x3C");
        return;
    }

    Serial.println("[OLED] OK");
}

void displayBootSplash(Adafruit_SSD1306& disp) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);
    disp.setCursor(0, 0);
    disp.printf("%s v%s", FW_NAME, FW_VERSION);
    disp.setCursor(0, 16);

#ifdef BOARD_T3S3
    disp.println("Board: T3S3");
#elif defined(BOARD_HELTEC_V3)
    disp.println("Board: Heltec V3");
#endif

    disp.display();
}

// Map an RSSI value to a pixel height within the chart area
static int rssiToBarHeight(float rssi) {
    float clamped = constrain(rssi, DISPLAY_RSSI_MIN, DISPLAY_RSSI_MAX);
    float normalized = (clamped - DISPLAY_RSSI_MIN) / (DISPLAY_RSSI_MAX - DISPLAY_RSSI_MIN);
    return (int)(normalized * CHART_HEIGHT);
}

// Downsample 700 bins to 128 columns, keeping peak RSSI per group
static void downsampleBins(const float* rssi, float* columns) {
    // Each column covers ~5.47 bins — use peak (not average) to catch spikes
    for (int col = 0; col < SCREEN_WIDTH; col++) {
        int binStart = (col * SCAN_BIN_COUNT) / SCREEN_WIDTH;
        int binEnd   = ((col + 1) * SCAN_BIN_COUNT) / SCREEN_WIDTH;

        float peak = -200.0;
        for (int b = binStart; b < binEnd; b++) {
            if (rssi[b] > peak) peak = rssi[b];
        }
        columns[col] = peak;
    }
}

void displaySpectrum(Adafruit_SSD1306& disp, const ScanResult& result) {
    disp.clearDisplay();

    float columns[SCREEN_WIDTH];
    downsampleBins(result.rssi, columns);

    // Draw vertical bars from bottom of chart area upward
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        int barH = rssiToBarHeight(columns[x]);
        if (barH > 0) {
            int yTop = CHART_Y_OFFSET + CHART_HEIGHT - barH;
            disp.drawFastVLine(x, yTop, barH, SSD1306_WHITE);
        }
    }

    // Peak info text below the chart
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);
    disp.setCursor(0, TEXT_Y_OFFSET);
    disp.printf("Peak:%.1fMHz %.0fdBm", result.peakFreq, result.peakRSSI);

    disp.setCursor(0, TEXT_Y_OFFSET + 9);
    disp.printf("860-930MHz  %lums", result.sweepTimeMs);

    disp.display();
}

void displayGPS(Adafruit_SSD1306& disp, const GpsData& data) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 0);
    disp.printf("GPS  Fix:%s  SVs:%d", fixTypeStr(data.fixType), data.numSV);

    if (!data.valid || data.fixType < 2) {
        disp.setCursor(0, 20);
        disp.println("Acquiring...");
        disp.setCursor(0, 36);
        disp.printf("Searching %d sats", data.numSV);
    } else {
        disp.setCursor(0, 16);
        disp.printf("Lat: %.6f", data.latDeg7 / 1e7);
        disp.setCursor(0, 28);
        disp.printf("Lon: %.6f", data.lonDeg7 / 1e7);
        disp.setCursor(0, 40);
        disp.printf("Alt: %dm", data.altMM / 1000);
        disp.setCursor(0, 52);
        disp.printf("pDOP: %.2f", data.pDOP / 100.0);
    }

    disp.display();
}

static const char* jammingStr(uint8_t state) {
    switch (state) {
        case 1:  return "OK";
        case 2:  return "WARN";
        case 3:  return "CRIT";
        default: return "UNK";
    }
}

static const char* spoofStr(uint8_t state) {
    switch (state) {
        case 1:  return "Clean";
        case 2:  return "Indicated";
        case 3:  return "Multiple";
        default: return "Unknown";
    }
}

static const char* threatStr(uint8_t level) {
    switch (level) {
        case 0:  return "CLEAR";
        case 1:  return "ADVISORY";
        case 2:  return "WARNING";
        case 3:  return "CRITICAL";
        default: return "?";
    }
}

void displayIntegrity(Adafruit_SSD1306& disp, const GpsData& gps,
                      const IntegrityStatus& status) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 0);
    disp.printf("GNSS INTEGRITY");

    disp.setCursor(0, 12);
    disp.printf("Jam: %s  JamI:%d", jammingStr(gps.jammingState), gps.jamInd);

    disp.setCursor(0, 24);
    disp.printf("Spoof: %s", spoofStr(gps.spoofDetState));

    disp.setCursor(0, 36);
    disp.printf("C/N0sd:%.1f AGC:%d%%", status.cnoStdDev, gps.agcPercent);

    disp.setCursor(0, 52);
    disp.printf("Threat: %s", threatStr(status.threatLevel));

    disp.display();
}
