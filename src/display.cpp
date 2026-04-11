#include "display.h"
#include "board_config.h"
#include "alert_handler.h"
#include "version.h"
#include "splash_logo.h"
#include <Arduino.h>
#include <Wire.h>

// ============================================================
// Display rendering — all screens follow consistent layout:
//   y=0:  Title text
//   y=9:  Separator line (drawFastHLine)
//   y=12: Data line 1
//   y=22: Data line 2
//   y=32: Data line 3
//   y=42: Data line 4
//   y=52: Data line 5
//   y=57: Page dots
// Max 21 characters per line at font size 1 (6x8 pixels)
// ============================================================

// ── Helpers ─────────────────────────────────────────────────

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
        case 2:  return "Warn";
        case 3:  return "Multi";
        default: return "N/A";
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

// Check if GPS data is valid and sane (not uninitialized garbage)
static bool gpsIsValid(const GpsData& g) {
    if (!g.valid) return false;
    if (g.numSV > 100) return false;       // garbage: uninitialized memory
    if (g.hAccMM > 100000000) return false; // > 100km accuracy = garbage
    return true;
}

void drawPageDots(Adafruit_SSD1306& disp, int current, int total) {
    int startX = 64 - ((total * 8) / 2) + 4;
    int y = 60;
    for (int i = 0; i < total; i++) {
        int x = startX + (i * 8);
        if (i == current) {
            disp.fillCircle(x, y, 2, SSD1306_WHITE);
        } else {
            disp.drawCircle(x, y, 2, SSD1306_WHITE);
        }
    }
}

// Standard screen header: title + separator line
static void drawHeader(Adafruit_SSD1306& disp, const char* title) {
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);
    disp.setCursor(0, 0);
    disp.print(title);
    disp.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);
}

// ── Init ────────────────────────────────────────────────────

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
    Wire.setClock(400000);  // 400kHz fast mode — SSD1306 supports up to 400kHz
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
    // Splash stays visible during hardware init — no delay needed here
    // (the init sequence takes 8-10 seconds with GPS timeout)
}

void displayFatalError(Adafruit_SSD1306& disp, const char* line1, const char* line2) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    // Version header — tester can identify firmware even without serial
    disp.setCursor(0, 0);
    disp.printf("%s v%s", FW_NAME, FW_VERSION);
    disp.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);

    // Inverted "FATAL" band for visibility
    disp.fillRect(0, 12, SCREEN_WIDTH, 11, SSD1306_WHITE);
    disp.setTextColor(SSD1306_BLACK);
    disp.setCursor(2, 14);
    disp.print("FATAL ERROR");
    disp.setTextColor(SSD1306_WHITE);

    // Error lines
    disp.setCursor(0, 28);
    disp.print(line1);
    if (line2 != nullptr) {
        disp.setCursor(0, 40);
        disp.print(line2);
    }

    // Hint line — reboot required
    disp.setCursor(0, 54);
    disp.print("Fix + power cycle");

    disp.display();
}

void displayWarning(Adafruit_SSD1306& disp, const char* line1, const char* line2) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    // Version header
    disp.setCursor(0, 0);
    disp.printf("%s v%s", FW_NAME, FW_VERSION);
    disp.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);

    // Inverted "WARNING" band — same layout as fatal but non-blocking
    disp.fillRect(0, 12, SCREEN_WIDTH, 11, SSD1306_WHITE);
    disp.setTextColor(SSD1306_BLACK);
    disp.setCursor(2, 14);
    disp.print("WARNING");
    disp.setTextColor(SSD1306_WHITE);

    // Warning lines
    disp.setCursor(0, 28);
    disp.print(line1);
    if (line2 != nullptr) {
        disp.setCursor(0, 40);
        disp.print(line2);
    }

    // Hint — boot is continuing, not halted
    disp.setCursor(0, 54);
    disp.print("Continuing boot...");

    disp.display();
}

// ── Spectrum helpers ────────────────────────────────────────

static const int CHART_HEIGHT   = 38;  // reduced from 42 to leave room for peak text
static const int CHART_Y_OFFSET = 10;

static int rssiToBarHeight(float rssi) {
    float clamped = constrain(rssi, DISPLAY_RSSI_MIN, DISPLAY_RSSI_MAX);
    float normalized = (clamped - DISPLAY_RSSI_MIN) / (DISPLAY_RSSI_MAX - DISPLAY_RSSI_MIN);
    return (int)(normalized * CHART_HEIGHT);
}

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

// ── Screen 0: Dashboard Summary ─────────────────────────────

void screenDashboard(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    // Line 0: SENTRY-RF + threat level (right-aligned, inverted if WARNING+)
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

    // Line 1: Mini spectrum bars (sub-GHz)
    drawMiniSpectrum(disp, 0, 12, 60, 8, state.spectrum.rssi, SCAN_BIN_COUNT);
    disp.setCursor(64, 12);
    if (state.spectrum24.valid) {
        drawMiniSpectrum(disp, 68, 12, 60, 8, state.spectrum24.rssi, SCAN_24_BIN_COUNT);
    } else {
        disp.print("2.4:N/A");
    }

    // Line 2: GPS condensed
    disp.setCursor(0, 22);
    if (gpsIsValid(state.gps)) {
        char buf[22];
        snprintf(buf, sizeof(buf), "%s %dSV J:%s S:%s",
                 fixTypeStr(state.gps.fixType), state.gps.numSV,
                 jammingStr(state.gps.jammingState),
                 (state.gps.spoofDetState >= 2) ? "!" : "OK");
        disp.print(buf);
    } else {
        disp.print("GPS: --");
    }

    // Line 3: Sub-GHz peak
    disp.setCursor(0, 32);
    if (state.spectrum.peakRSSI > -110.0) {
        char buf[22];
        snprintf(buf, sizeof(buf), "%.0fdB @%.0fMHz",
                 state.spectrum.peakRSSI, state.spectrum.peakFreq);
        disp.print(buf);
    } else {
        disp.print("RF: quiet");
    }

    // Line 4: Battery + buzzer status
    disp.setCursor(0, 42);
    if (alertIsMuted()) {
        unsigned long remain = alertMuteRemainingMs() / 1000;
        char buf[22];
        snprintf(buf, sizeof(buf), "Bat:%d%% MUTED %lus",
                 state.batteryPercent, remain);
        disp.print(buf);
    } else if (alertIsAcknowledged()) {
        char buf[22];
        snprintf(buf, sizeof(buf), "Bat:%d%% ACK'd", state.batteryPercent);
        disp.print(buf);
    } else {
        char buf[22];
        snprintf(buf, sizeof(buf), "Bat:%d%% WiFi:SCAN", state.batteryPercent);
        disp.print(buf);
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 1: Sub-GHz Spectrum ──────────────────────────────

void screenSpectrum(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    drawHeader(disp, "RF 860-930MHz");

    // Bars from y=10 to y=48 (height=38), leaving y=50+ clear for text
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

    // Peak text below the chart area
    disp.setCursor(0, 50);
    char buf[22];
    snprintf(buf, sizeof(buf), "Pk:%.1f %.0fdBm",
             state.spectrum.peakFreq, state.spectrum.peakRSSI);
    disp.print(buf);

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 2: GPS ───────────────────────────────────────────

void screenGPS(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    drawHeader(disp, "GPS");

    const GpsData& g = state.gps;

    if (!gpsIsValid(g)) {
        // No GPS connected or garbage data
        disp.setTextSize(2);
        disp.setCursor(20, 24);
        disp.print("NO GPS");
        disp.setTextSize(1);
        disp.setCursor(20, 44);
        disp.print("Module not found");
    } else if (g.fixType < 2) {
        disp.setCursor(0, 12);
        disp.printf("%d SVs  Acquiring...", g.numSV);
        disp.setCursor(0, 32);
        disp.print("Waiting for fix");
    } else {
        disp.setCursor(0, 12);
        char buf[22];
        snprintf(buf, sizeof(buf), "%s %dSV pDOP:%.1f",
                 fixTypeStr(g.fixType), g.numSV, g.pDOP / 100.0);
        disp.print(buf);

        disp.setCursor(0, 22);
        snprintf(buf, sizeof(buf), "%.5f", g.latDeg7 / 1e7);
        disp.print(buf);

        disp.setCursor(0, 32);
        snprintf(buf, sizeof(buf), "%.5f", g.lonDeg7 / 1e7);
        disp.print(buf);

        disp.setCursor(0, 42);
        snprintf(buf, sizeof(buf), "Alt:%dm hAcc:%dm",
                 (int)(g.altMM / 1000), (int)(g.hAccMM / 1000));
        disp.print(buf);

        disp.setCursor(0, 52);
        if (state.compass.valid) {
            snprintf(buf, sizeof(buf), "HDG:%.0f%c %s",
                     state.compass.heading, 0xF8, state.compass.directionStr);
            disp.print(buf);
        } else {
            disp.print("HDG: --");
        }
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 3: GNSS Integrity ────────────────────────────────

void screenIntegrity(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    drawHeader(disp, "GNSS INTEGRITY");

    if (!gpsIsValid(state.gps)) {
        disp.setTextSize(2);
        disp.setCursor(6, 24);
        disp.print("NO GPS");
        disp.setTextSize(1);
        disp.setCursor(6, 44);
        disp.print("Module not found");
    } else {
        disp.setCursor(0, 12);
        char buf[22];
        snprintf(buf, sizeof(buf), "Jam:%s  JamI:%d",
                 jammingStr(state.gps.jammingState), state.gps.jamInd);
        disp.print(buf);

        disp.setCursor(0, 22);
        snprintf(buf, sizeof(buf), "Spoof: %s", spoofStr(state.gps.spoofDetState));
        disp.print(buf);

        disp.setCursor(0, 32);
        snprintf(buf, sizeof(buf), "C/N0sd:%.1f", state.integrity.cnoStdDev);
        disp.print(buf);

        disp.setCursor(0, 42);
        if (state.gps.jammingState == 0 && state.gps.agcPercent == 0) {
            disp.print("AGC: --");
        } else {
            snprintf(buf, sizeof(buf), "AGC: %d%%", state.gps.agcPercent);
            disp.print(buf);
        }
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 4: Threat ────────────────────────────────────────

void screenThreat(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);

    // Inverted header bar for WARNING and CRITICAL
    if (state.threatLevel >= THREAT_WARNING) {
        disp.fillRect(0, 0, 128, 10, SSD1306_WHITE);
        disp.setTextColor(SSD1306_BLACK);
    } else {
        disp.setTextColor(SSD1306_WHITE);
    }
    disp.setCursor(2, 1);
    disp.printf("THREAT: %s", threatStr(state.threatLevel));
    disp.setTextColor(SSD1306_WHITE);

    // RF status
    disp.setCursor(0, 12);
    if (state.spectrum.peakRSSI > -110.0) {
        char buf[22];
        snprintf(buf, sizeof(buf), "RF:%.0fMHz %.0fdBm",
                 state.spectrum.peakFreq, state.spectrum.peakRSSI);
        disp.print(buf);
    } else {
        disp.print("RF: No signals");
    }

    // GPS status
    disp.setCursor(0, 22);
    if (gpsIsValid(state.gps)) {
        char buf[22];
        snprintf(buf, sizeof(buf), "GPS:%s %dSV",
                 fixTypeStr(state.gps.fixType), state.gps.numSV);
        disp.print(buf);
    } else {
        disp.print("GPS: --");
    }

    // Jamming/spoofing
    disp.setCursor(0, 32);
    if (gpsIsValid(state.gps)) {
        char buf[22];
        snprintf(buf, sizeof(buf), "Jam:%s Spf:%s",
                 jammingStr(state.gps.jammingState),
                 spoofStr(state.gps.spoofDetState));
        disp.print(buf);
    } else {
        disp.print("Jam:-- Spf:--");
    }

    // Buzzer status
    disp.setCursor(0, 42);
    if (alertIsMuted()) {
        disp.print("Buzzer: MUTED");
    } else if (alertIsAcknowledged()) {
        disp.print("Buzzer: ACK'd");
    } else if (state.threatLevel >= THREAT_WARNING) {
        disp.print("Buzzer: ARMED");
    } else {
        disp.print("Buzzer: standby");
    }

    // Bearing
    disp.setCursor(0, 52);
    if (state.compass.valid && state.compass.peakRSSI > -200.0 &&
        (millis() - state.compass.peakTimestamp < 30000)) {
        char buf[22];
        snprintf(buf, sizeof(buf), "Bearing: %.0f%c",
                 state.compass.peakBearing, 0xF8);
        disp.print(buf);
    } else {
        disp.print("Bearing: --");
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 5: System Info ───────────────────────────────────

void screenSystem(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    drawHeader(disp, "SYSTEM");

    char buf[22];
    disp.setCursor(0, 12);
    snprintf(buf, sizeof(buf), "%s v%s", FW_NAME, FW_VERSION);
    disp.print(buf);

    unsigned long sec = millis() / 1000;
    disp.setCursor(0, 22);
    snprintf(buf, sizeof(buf), "Up: %luh %lum %lus",
             sec / 3600, (sec % 3600) / 60, sec % 60);
    disp.print(buf);

    disp.setCursor(0, 32);
    snprintf(buf, sizeof(buf), "Heap: %u", ESP.getFreeHeap());
    disp.print(buf);

    disp.setCursor(0, 42);
    if (alertIsMuted()) {
        unsigned long remain = alertMuteRemainingMs() / 1000;
        snprintf(buf, sizeof(buf), "Buz:MUTED %lus", remain);
    } else {
        snprintf(buf, sizeof(buf), "Buz:Armed Cmp:%s",
                 state.compass.valid ? "OK" : "--");
    }
    disp.print(buf);

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 6: 2.4 GHz Spectrum (LR1121 only) ───────────────

void screenSpectrum24(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    drawHeader(disp, "2.4GHz 2400-2500MHz");

    if (!state.spectrum24.valid) {
        disp.setCursor(0, 28);
        disp.print("2.4 GHz: No radio");
        drawPageDots(disp, page, NUM_SCREENS);
        disp.display();
        return;
    }

    drawMiniSpectrum(disp, 0, CHART_Y_OFFSET, SCREEN_WIDTH, CHART_HEIGHT,
                     state.spectrum24.rssi, SCAN_24_BIN_COUNT);

    disp.setCursor(0, 50);
    char buf[22];
    snprintf(buf, sizeof(buf), "Pk:%.0f %.0fdBm",
             state.spectrum24.peakFreq, state.spectrum24.peakRSSI);
    disp.print(buf);

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}
