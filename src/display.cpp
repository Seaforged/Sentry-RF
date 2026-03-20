#include "display.h"
#include "board_config.h"
#include "version.h"
#include "splash_logo.h"
#include <Arduino.h>
#include <Wire.h>

// ── Helpers ─────────────────────────────────────────────────────────────────

static const char* fixTypeStr(uint8_t fixType) {
    switch (fixType) {
        case 2:  return "2D";
        case 3:  return "3D";
        default: return "--";
    }
}

static const char* jammingStr(uint8_t state) {
    switch (state) {
        case 1:  return "OK";
        case 2:  return "WARN";
        case 3:  return "CRIT";
        default: return "---";
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

static const char* threatStr(ThreatLevel t) {
    switch (t) {
        case THREAT_ADVISORY: return "ADVISORY";
        case THREAT_WARNING:  return "WARNING";
        case THREAT_CRITICAL: return "CRITICAL";
        default:              return "CLEAR";
    }
}

void drawPageDots(Adafruit_SSD1306& disp, int current, int total) {
    int startX = 64 - ((total * 8) / 2) + 4;
    int y = 62;
    for (int i = 0; i < total; i++) {
        int x = startX + (i * 8);
        if (i == current) {
            disp.fillCircle(x, y, 2, SSD1306_WHITE);
        } else {
            disp.drawCircle(x, y, 2, SSD1306_WHITE);
        }
    }
}

// ── Init ────────────────────────────────────────────────────────────────────

void displayInit(Adafruit_SSD1306& disp) {
    if (HAS_OLED_VEXT) {
        pinMode(PIN_OLED_VEXT, OUTPUT);
        digitalWrite(PIN_OLED_VEXT, LOW);
        delay(10);
    }
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
    disp.drawBitmap(0, 0, splash_logo, 128, 64, SSD1306_WHITE);
    disp.display();
    delay(2000);
}

// ── Screen 0: Spectrum ──────────────────────────────────────────────────────

static const int CHART_HEIGHT   = 42;
static const int CHART_Y_OFFSET = 9;

static int rssiToBarHeight(float rssi) {
    float clamped = constrain(rssi, DISPLAY_RSSI_MIN, DISPLAY_RSSI_MAX);
    float normalized = (clamped - DISPLAY_RSSI_MIN) / (DISPLAY_RSSI_MAX - DISPLAY_RSSI_MIN);
    return (int)(normalized * CHART_HEIGHT);
}

// ── Mini spectrum bar helper (60px wide × 10px tall) ────────────────────────

static void drawMiniSpectrum(Adafruit_SSD1306& disp, int x, int y, int w, int h,
                             const float* rssi, int binCount) {
    for (int col = 0; col < w; col++) {
        int binStart = (col * binCount) / w;
        int binEnd = ((col + 1) * binCount) / w;
        float peak = -200.0;
        for (int b = binStart; b < binEnd; b++) {
            if (rssi[b] > peak) peak = rssi[b];
        }
        float clamped = constrain(peak, DISPLAY_RSSI_MIN, DISPLAY_RSSI_MAX);
        int barH = (int)(((clamped - DISPLAY_RSSI_MIN) / (DISPLAY_RSSI_MAX - DISPLAY_RSSI_MIN)) * h);
        if (barH > 0) {
            disp.drawFastVLine(x + col, y + h - barH, barH, SSD1306_WHITE);
        }
    }
}

// ── Screen 0: Dashboard Summary ─────────────────────────────────────────────

void screenDashboard(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    // Line 0: SENTRY-RF + threat level
    disp.setCursor(0, 0);
    disp.print("SENTRY-RF");
    const char* tStr = threatStr(state.threatLevel);
    int tWidth = strlen(tStr) * 6;
    if (state.threatLevel >= THREAT_WARNING) {
        disp.fillRect(128 - tWidth - 2, 0, tWidth + 2, 9, SSD1306_WHITE);
        disp.setTextColor(SSD1306_BLACK);
    }
    disp.setCursor(128 - tWidth, 0);
    disp.print(tStr);
    disp.setTextColor(SSD1306_WHITE);

    // Line 1: Mini spectrum bars
    drawMiniSpectrum(disp, 0, 10, 60, 10, state.spectrum.rssi, SCAN_BIN_COUNT);
    disp.setCursor(64, 10);
    if (state.spectrum24.valid) {
        drawMiniSpectrum(disp, 68, 10, 60, 10, state.spectrum24.rssi, SCAN_24_BIN_COUNT);
    } else {
        disp.print("2.4:N/A");
    }

    // Line 2: GPS + jamming + spoofing condensed (max 21 chars)
    disp.setCursor(0, 22);
    disp.printf("%s %dSV J:%s S:%s",
                fixTypeStr(state.gps.fixType), state.gps.numSV,
                jammingStr(state.gps.jammingState),
                (state.gps.spoofDetState >= 2) ? "!" : "OK");

    // Line 3: Sub-GHz peak (max 21 chars)
    disp.setCursor(0, 32);
    disp.printf("%.0fdB @%.0fM", state.spectrum.peakRSSI, state.spectrum.peakFreq);

    // Line 4: Battery + WiFi mode + compass
    disp.setCursor(0, 42);
    disp.printf("Bat:%d%%  WiFi:%s",
                state.batteryPercent,
                state.dashboardActive ? "AP" : "SCAN");
    if (state.compass.valid) {
        disp.printf(" %d%c", (int)state.compass.heading, 0xF8);
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 1: Sub-GHz Spectrum ──────────────────────────────────────────────

void screenSpectrum(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 0);
    disp.print("RF SCAN 860-930MHz");

    // Downsample 700 bins to 128 columns, draw bars
    for (int col = 0; col < SCREEN_WIDTH; col++) {
        int binStart = (col * SCAN_BIN_COUNT) / SCREEN_WIDTH;
        int binEnd   = ((col + 1) * SCAN_BIN_COUNT) / SCREEN_WIDTH;
        float peak = -200.0;
        for (int b = binStart; b < binEnd; b++) {
            if (state.spectrum.rssi[b] > peak) peak = state.spectrum.rssi[b];
        }
        int barH = rssiToBarHeight(peak);
        if (barH > 0) {
            disp.drawFastVLine(col, CHART_Y_OFFSET + CHART_HEIGHT - barH, barH, SSD1306_WHITE);
        }
    }

    disp.setCursor(0, 51);
    disp.printf("Pk:%.1f %.0fdBm", state.spectrum.peakFreq, state.spectrum.peakRSSI);

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 1: GPS ───────────────────────────────────────────────────────────

void screenGPS(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    const GpsData& g = state.gps;
    disp.setCursor(0, 0);
    disp.printf("GPS %s FIX  %d SVs", fixTypeStr(g.fixType), g.numSV);

    if (!g.valid || g.fixType < 2) {
        disp.setCursor(0, 20);
        disp.println("Acquiring...");
    } else {
        disp.setCursor(0, 12);
        disp.printf("%.5f,%.5f", g.latDeg7 / 1e7, g.lonDeg7 / 1e7);
        disp.setCursor(0, 24);
        disp.printf("Alt:%dm  pDOP:%.1f", g.altMM / 1000, g.pDOP / 100.0);
        disp.setCursor(0, 36);
        disp.printf("hAcc:%dm vAcc:%dm", g.hAccMM / 1000, g.vAccMM / 1000);
        disp.setCursor(0, 48);
        if (state.compass.valid) {
            disp.printf("HDG: %.0f%c %s", state.compass.heading, 0xF8, state.compass.directionStr);
        } else {
            disp.print("HDG: --");
        }
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 2: GNSS Integrity ────────────────────────────────────────────────

void screenIntegrity(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 0);
    disp.print("GNSS INTEGRITY");

    disp.setCursor(0, 12);
    disp.printf("Jam:%s  JamI:%d", jammingStr(state.gps.jammingState), state.gps.jamInd);

    disp.setCursor(0, 24);
    disp.printf("Spoof: %s", spoofStr(state.gps.spoofDetState));

    disp.setCursor(0, 36);
    disp.printf("C/N0sd:%.1f AGC:", state.integrity.cnoStdDev);
    if (state.gps.jammingState == 0 && state.gps.agcPercent == 0) {
        disp.print("--");
    } else {
        disp.printf("%d%%", state.gps.agcPercent);
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 3: Threat ────────────────────────────────────────────────────────

void screenThreat(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);

    // Invert header bar for WARNING and CRITICAL
    if (state.threatLevel >= THREAT_WARNING) {
        disp.fillRect(0, 0, 128, 10, SSD1306_WHITE);
        disp.setTextColor(SSD1306_BLACK);
    } else {
        disp.setTextColor(SSD1306_WHITE);
    }
    disp.setCursor(2, 1);
    disp.printf("THREAT: %s", threatStr(state.threatLevel));
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 14);
    if (state.spectrum.peakRSSI > -110.0) {
        disp.printf("RF: %.1fMHz %.0fdBm", state.spectrum.peakFreq, state.spectrum.peakRSSI);
    } else {
        disp.print("RF: No signals");
    }

    disp.setCursor(0, 26);
    disp.printf("GNSS: %s %dSV", fixTypeStr(state.gps.fixType), state.gps.numSV);

    disp.setCursor(0, 38);
    disp.printf("Jam:%s Spf:%s",
                jammingStr(state.gps.jammingState),
                spoofStr(state.gps.spoofDetState));

    disp.setCursor(0, 50);
    if (state.compass.valid && state.compass.peakRSSI > -200.0 &&
        (millis() - state.compass.peakTimestamp < 30000)) {
        disp.printf("Bearing: %.0f%c", state.compass.peakBearing, 0xF8);
    } else {
        disp.print("Bearing: --");
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 4: System Info ───────────────────────────────────────────────────

void screenSystem(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 0);
    disp.print("SYSTEM");

    disp.setCursor(0, 12);
    disp.printf("%s v%s", FW_NAME, FW_VERSION);

    unsigned long sec = millis() / 1000;
    unsigned long h = sec / 3600;
    unsigned long m = (sec % 3600) / 60;
    unsigned long s = sec % 60;
    disp.setCursor(0, 24);
    disp.printf("Up: %luh %lum %lus", h, m, s);

    disp.setCursor(0, 36);
    disp.printf("Heap: %u bytes", ESP.getFreeHeap());

    disp.setCursor(0, 48);
    if (compassIsCalibrating()) {
        disp.print("Compass: CAL...");
    } else if (state.compass.valid) {
        disp.print("Compass: OK");
    } else {
        disp.print("Compass: N/A");
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 6: 2.4 GHz Spectrum (LR1121 only) ───────────────────────────────

void screenSpectrum24(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 0);
    disp.print("2.4GHz 2400-2500MHz");

    if (!state.spectrum24.valid) {
        disp.setCursor(0, 28);
        disp.print("2.4 GHz: No radio");
        drawPageDots(disp, page, NUM_SCREENS);
        disp.display();
        return;
    }

    // Reuse mini spectrum helper at full width for the chart area
    drawMiniSpectrum(disp, 0, CHART_Y_OFFSET, SCREEN_WIDTH, CHART_HEIGHT,
                     state.spectrum24.rssi, SCAN_24_BIN_COUNT);

    disp.setCursor(0, 53);
    disp.printf("Pk:%.0f %.0fdBm", state.spectrum24.peakFreq, state.spectrum24.peakRSSI);

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}
