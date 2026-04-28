#include "display.h"
#include "board_config.h"
#include "sentry_config.h"
#include "alert_handler.h"
#include "buzzer_manager.h"
#include "cad_scanner.h"
#include "detection_engine.h"
#include "gps_manager.h"
#include "env_mode.h"
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

// Unified buzzer state label across Threat and System screens.
// Priority: Off > MUTED > ACK'd > Active (actually sounding) > Armed.
// "Active" is derived from buzzerIsPlaying() — real hardware state — so
// the display reflects what the operator can actually hear, not a proxy
// based on threat level. This does cause brief "Armed <-> Active" flicker
// between beeps within a pattern, but that matches ground truth.
static const char* buzzerStateStr() {
    if (!HAS_BUZZER) return "Off";
    if (alertIsMuted()) return "MUTED";
    if (alertIsAcknowledged()) return "ACK'd";
    if (buzzerIsPlaying()) return "Active";
    return "Armed";
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

// Phase K: boot self-test summary screen. Three status lines under the
// version banner, using the same header pattern as the other screens.
void displayBootSummary(Adafruit_SSD1306& disp, bool radioOK, bool antennaOK,
                        const char* gpsLine) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    // Banner uses the live FW_VERSION rather than a hardcoded "v2.0" so
    // version bumps don't leave this screen out of sync.
    disp.setCursor(0, 0);
    disp.printf("%s v%s", FW_NAME, FW_VERSION);
    disp.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);

    disp.setCursor(0, 16);
    disp.printf("Radio:   %s",   radioOK   ? "OK" : "FAIL");
    disp.setCursor(0, 28);
    disp.printf("Antenna: %s",   antennaOK ? "OK" : "WARN");
    disp.setCursor(0, 40);
    disp.printf("GPS:     %s",   gpsLine);

    disp.setCursor(0, 54);
    disp.print("Self-test complete");

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

// noinline: xtensa-esp32-elf-gcc ICEs (recog.c:2210, postreload RTL pass)
// when this function gets inlined into screenSpectrum24 after the Dashboard
// stopped calling it. Forcing noinline avoids the buggy register-constraint
// path without changing behavior.
__attribute__((noinline))
static void drawMiniSpectrum(Adafruit_SSD1306& disp, int x, int y, int w, int h,
                             const float* rssi, int binCount) {
    for (int col = 0; col < w; col++) {
        int binStart = (col * binCount) / w;
        int binEnd = ((col + 1) * binCount) / w;
        float peak = -200.0f;
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

// WiFi channel activity mini chart — "WiFi" label + 13 wide discrete bars
// (channels 1-13) spanning nearly the full Dashboard width. Label occupies
// x..x+23 (4 chars, 24 px). Bars start at x+26 with 6 px width + 1 px gap =
// 7 px per channel × 13 − 1 trailing gap = 90 px span (x+26..x+115). Heights
// are log-scaled from frames-per-second counts captured by wifi_scanner.cpp.
// If the scanner has not yet produced a snapshot, renders "WiFi scan" text
// instead so the operator knows the region isn't broken.
// Note: the `w` parameter is retained for API symmetry with drawMiniSpectrum
// but internal layout is fixed at 116 px regardless of the passed value.
static void drawMiniWifiChannels(Adafruit_SSD1306& disp, int x, int y, int w, int h,
                                 const SystemState& state) {
    (void)w;  // layout is fixed-width, see header comment

    // Fallback: no data yet (first ~1 second of boot or scanner wedged).
    if (state.wifiChannelSnapshotMs == 0) {
        disp.setCursor(x, y);
        disp.print("WiFi scan");
        return;
    }

    // "WiFi" label flush left
    disp.setCursor(x, y);
    disp.print("WiFi");

    // 13 bars, 6 px wide + 1 px gap, starting at x+26 (label width + 2 px pad)
    const int barStartX = x + 26;
    const int barWidth = 6;
    const int gap = 1;
    for (int ch = 0; ch < 13; ch++) {
        uint8_t count = state.wifiChannelCount[ch];
        int barH = 0;
        if (count > 0) {
            // Log-scale: 1->2, 2->3, 4->4, 8->5, 16->6, 32->7, 64+->h
            // Gives a compressed visual that still distinguishes activity
            // levels without a single busy AP saturating the chart.
            int step = 0;
            uint32_t c = count;
            while (c > 0) { step++; c >>= 1; }  // step = floor(log2(count))+1
            barH = 1 + step;
            if (barH > h) barH = h;
        }
        int barX = barStartX + ch * (barWidth + gap);
        if (barH > 0) {
            disp.fillRect(barX, y + h - barH, barWidth, barH, SSD1306_WHITE);
        } else {
            // Single-pixel floor mark so the operator sees "channel exists,
            // just quiet" rather than "channel missing from chart".
            disp.drawFastHLine(barX, y + h - 1, barWidth, SSD1306_WHITE);
        }
    }
}

// ── Screen 0: Dashboard Summary ─────────────────────────────

void screenDashboard(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);

    // Line 0: banner + mode tag + threat level (right-aligned, inverted
    // if WARNING+). STANDARD keeps the original "SENTRY-RF" banner;
    // COVERT/HIGH_ALERT drop the banner and show the bracketed mode
    // label instead so the right-aligned threat badge (up to 8 chars
    // for "CRITICAL") never collides at 128-pixel width.
    disp.setCursor(0, 0);
    OperatingMode m = modeGet();
    if (m == MODE_STANDARD) {
        disp.print("SENTRY-RF");
    } else {
        disp.printf("[%s]", modeShortLabel(m));
    }
    const char* tStr = threatStr(state.threatLevel);
    int tWidth = strlen(tStr) * 6;
    if (state.threatLevel >= THREAT_WARNING) {
        disp.fillRect(128 - tWidth - 2, 0, tWidth + 2, 9, SSD1306_WHITE);
        disp.setTextColor(SSD1306_BLACK);
    }
    disp.setCursor(128 - tWidth, 0);
    disp.print(tStr);
    disp.setTextColor(SSD1306_WHITE);

    // Line 1: Full-width WiFi channel activity chart. The 2.4 GHz mini
    // spectrum that used to share this row was removed — it has its own
    // dedicated full-screen view (Screen 6) and was leaving artifacts
    // bleed-through on the Dashboard.
    //
    // Clear the full row before drawing — fillRect with height 10 (not just
    // the chart's 8 px) gives safety margin against any prior render reaching
    // into y=20..21 from a previous frame or a dirty buffer state.
    disp.fillRect(0, 12, 128, 10, SSD1306_BLACK);
    drawMiniWifiChannels(disp, 0, 12, 128, 8, state);

    // Line 2: GPS condensed
    disp.setCursor(0, 22);
    if (gpsIsValid(state.gps)) {
        char buf[22];
        // J: shows raw jamInd (0-255) ONLY when MON-HW has arrived within
        // the freshness window — otherwise "--" to distinguish "telemetry
        // unavailable/stale" from a legitimate "no jamming" reading of 0.
        // jammingState is often stuck at 0 on u-blox M10 MON-HW, which is
        // why we use jamInd + freshness rather than jammingStr().
        char jStr[6];
        if (gpsMonHwIsFresh(state.gps)) {
            snprintf(jStr, sizeof(jStr), "%d", state.gps.jamInd);
        } else {
            snprintf(jStr, sizeof(jStr), "--");
        }
        snprintf(buf, sizeof(buf), "%s %dSV J:%s S:%s",
                 fixTypeStr(state.gps.fixType), state.gps.numSV,
                 jStr,
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

        // HDG line removed: compass isn't connected on current hardware and
        // the y=52 row was colliding with the page dots. Alt+hAcc at y=42 is
        // the last data line; dots render at y=60 with clean clearance.
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

        // AGC line removed — u-blox M10 MON-HW does not reliably populate
        // agcCnt, so the reading stayed at 0 and the display always showed
        // "AGC: --". Left blank rather than printing misleading placeholder.
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

    // GPS + jamming/spoofing combined on one line (was 2 lines at y=22/y=32).
    // Freed vertical space keeps Buzzer and Bearing above the page-dot row.
    // J: uses jammingStr when MON-HW is fresh; stale reads show "--" to
    // distinguish "no data yet" from a real "no jamming" reading. S: is a
    // binary "!" / "OK" from spoofDetState (Integrity screen has the full
    // text label).
    disp.setCursor(0, 22);
    if (gpsIsValid(state.gps)) {
        char buf[22];
        const char* jStr = gpsMonHwIsFresh(state.gps)
            ? jammingStr(state.gps.jammingState)
            : "--";
        snprintf(buf, sizeof(buf), "%s %dSV J:%s S:%s",
                 fixTypeStr(state.gps.fixType), state.gps.numSV,
                 jStr,
                 (state.gps.spoofDetState >= 2) ? "!" : "OK");
        disp.print(buf);
    } else {
        disp.print("GPS: --");
    }

    // Buzzer status — unified Armed/Active/MUTED/ACK'd/Off via helper (moved y=42 -> y=32)
    disp.setCursor(0, 32);
    disp.printf("Buzzer: %s", buzzerStateStr());

    // Bearing (moved y=52 -> y=42 — clean clearance from page dots at y=60)
    disp.setCursor(0, 42);
    if (state.compass.valid && state.compass.peakRSSI > -200.0 &&
        (millis() - state.compass.peakTimestamp < 30000)) {
        char buf[22];
        snprintf(buf, sizeof(buf), "Bearing: %.0f%c",
                 state.compass.peakBearing, 0xF8);
        disp.print(buf);
    } else {
        disp.print("Bearing: --");
    }

    // Phase G: show live candidate engine diagnostics when a candidate is
    // active; otherwise fall back to the "LAST: Xs ago" line. The two are
    // mutually exclusive on y=52 — there's no room for both.
    disp.setCursor(0, 52);
    if (state.hasCandidate) {
        // CAND: A=XXX.X B=XX F=XX C=XX — anchor MHz, bandMask hex, fast, confirm
        disp.printf("CAND:%.0f B%X F%d C%d",
                    state.anchorFreq, state.bandMask,
                    state.fastScore, state.confirmScore);
    } else if (state.threatLevel >= THREAT_ADVISORY && getLastDetectionMs() > 0) {
        uint32_t ago = (millis() - getLastDetectionMs()) / 1000;
        disp.printf("LAST: %lus ago", ago);
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

    // Buzzer + compass status — unified buzzer label via helper
    disp.setCursor(0, 42);
    snprintf(buf, sizeof(buf), "Buz:%s Cmp:%s",
             buzzerStateStr(),
             state.compass.valid ? "OK" : "--");
    disp.print(buf);

    if (cadHwFault()) {
        disp.setCursor(0, 52);
        disp.print("CAD: HW FAULT");
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 6: ASTM F3411 Remote ID decoded payload (Phase J) ───

void screenRID(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    disp.clearDisplay();
    drawHeader(disp, "REMOTE ID");

    const DecodedRID& r = state.lastRID;
    bool stale = (!r.valid) ||
                 (r.lastUpdateMs == 0) ||
                 (millis() - r.lastUpdateMs > 10000);

    if (stale) {
        disp.setCursor(0, 28);
        disp.print("RID: No data");
    } else {
        // Line 1: UAS ID, truncated to 21 chars (OLED width). uasID itself
        // is already ≤20 chars + null; type suffix packs in if there's room.
        char l1[22];
        snprintf(l1, sizeof(l1), "ID:%s", r.uasID);
        disp.setCursor(0, 12);
        disp.print(l1);

        // Line 2: Drone lat/lon — 4 decimals each fits in 21 chars
        char l2[22];
        snprintf(l2, sizeof(l2), "D:%.4f %.4f",
                 r.droneLat, r.droneLon);
        disp.setCursor(0, 22);
        disp.print(l2);

        // Line 3: Altitude + speed + heading
        char l3[22];
        snprintf(l3, sizeof(l3), "Alt:%.0fm %.0fm/s %ud",
                 r.droneAltM, r.speedMps, r.headingDeg);
        disp.setCursor(0, 32);
        disp.print(l3);

        // Line 4: Operator (pilot) lat/lon
        char l4[22];
        snprintf(l4, sizeof(l4), "P:%.4f %.4f",
                 r.operatorLat, r.operatorLon);
        disp.setCursor(0, 42);
        disp.print(l4);

        // Line 5: ID type + age
        char l5[22];
        uint32_t ageS = (millis() - r.lastUpdateMs) / 1000;
        snprintf(l5, sizeof(l5), "%s  %lus ago", r.uasIDType, ageS);
        disp.setCursor(0, 52);
        disp.print(l5);
    }

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}

// ── Screen 7: 2.4 GHz Spectrum (LR1121 only) ───────────────

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

// ── Sprint 7: Env-Mode page ─────────────────────────────────
//
// Layout for 128x64 SSD1306. The bracketed mode shows the CURRENTLY active
// EnvMode; "Hold 3-5s to change" is the operator instruction. The screen is
// re-rendered at the 2 Hz cadence used by every other page, so a long-press
// cycle reflects on the next frame after envModeCycle() runs.
void screenEnvMode(Adafruit_SSD1306& disp, const SystemState& state, int page) {
    (void)state;
    disp.clearDisplay();
    drawHeader(disp, "ENV MODE");

    EnvMode m = envModeGet();

    char modeBuf[16];
    snprintf(modeBuf, sizeof(modeBuf), "[%s]", envModeLabel(m));
    disp.setTextSize(2);
    int16_t bx, by; uint16_t bw, bh;
    disp.getTextBounds(modeBuf, 0, 0, &bx, &by, &bw, &bh);
    int cx = (SCREEN_WIDTH - (int)bw) / 2;
    if (cx < 0) cx = 0;
    disp.setCursor(cx, 14);
    disp.print(modeBuf);
    disp.setTextSize(1);

    char buf[22];
    disp.setCursor(0, 36);
    snprintf(buf, sizeof(buf), " Tap thresh: %.0f dB",
             currentTapThresholdDb());
    disp.print(buf);

    disp.setCursor(0, 46);
    uint32_t ttlMs = currentSkipTtlMs();
    snprintf(buf, sizeof(buf), " WiFi skip:  %u min",
             (unsigned)(ttlMs / 60000));
    disp.print(buf);

    disp.setCursor(0, 56);
    disp.print("Hold 3+ s to change");

    drawPageDots(disp, page, NUM_SCREENS);
    disp.display();
}
