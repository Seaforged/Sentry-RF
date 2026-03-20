#include "wifi_dashboard.h"
#include "board_config.h"
#include "version.h"
#include <Arduino.h>

#if defined(BOARD_T3S3) || defined(BOARD_HELTEC_V3) || defined(BOARD_T3S3_LR1121)
#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);
static SystemState dashState = {};

static const char* threatStr(ThreatLevel t) {
    switch (t) {
        case THREAT_ADVISORY: return "ADVISORY";
        case THREAT_WARNING:  return "WARNING";
        case THREAT_CRITICAL: return "CRITICAL";
        default:              return "CLEAR";
    }
}

static void handleRoot() {
    unsigned long sec = millis() / 1000;
    char html[1024];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv='refresh' content='2'>"
        "<title>SENTRY-RF</title>"
        "<style>body{font-family:monospace;background:#111;color:#0f0;padding:20px}"
        "h1{color:#0f0}td{padding:4px 12px}</style></head><body>"
        "<h1>SENTRY-RF v%s</h1>"
        "<table>"
        "<tr><td>Threat</td><td><b>%s</b></td></tr>"
        "<tr><td>RF Peak</td><td>%.1f MHz @ %.1f dBm</td></tr>"
        "<tr><td>GPS Fix</td><td>%d (%d SVs)</td></tr>"
        "<tr><td>Position</td><td>%.6f, %.6f</td></tr>"
        "<tr><td>hAcc</td><td>%d m</td></tr>"
        "<tr><td>Jamming</td><td>State:%d Ind:%d</td></tr>"
        "<tr><td>Spoofing</td><td>State:%d</td></tr>"
        "<tr><td>C/N0 StdDev</td><td>%.1f dBHz</td></tr>"
        "<tr><td>Uptime</td><td>%lus</td></tr>"
        "<tr><td>Free Heap</td><td>%u bytes</td></tr>"
        "</table></body></html>",
        FW_VERSION, threatStr(dashState.threatLevel),
        dashState.spectrum.peakFreq, dashState.spectrum.peakRSSI,
        dashState.gps.fixType, dashState.gps.numSV,
        dashState.gps.latDeg7 / 1e7, dashState.gps.lonDeg7 / 1e7,
        dashState.gps.hAccMM / 1000,
        dashState.gps.jammingState, dashState.gps.jamInd,
        dashState.gps.spoofDetState,
        dashState.integrity.cnoStdDev,
        sec, ESP.getFreeHeap());

    server.send(200, "text/html", html);
}

static void handleAPI() {
    char json[512];
    snprintf(json, sizeof(json),
        "{\"threat\":\"%s\",\"peak_freq\":%.1f,\"peak_rssi\":%.1f,"
        "\"fix_type\":%d,\"num_sv\":%d,"
        "\"lat\":%.6f,\"lon\":%.6f,\"alt_m\":%d,\"hacc_m\":%d,"
        "\"jam_state\":%d,\"jam_ind\":%d,\"spoof_state\":%d,"
        "\"cno_stddev\":%.1f,"
        "\"uptime_s\":%lu,\"free_heap\":%u}",
        threatStr(dashState.threatLevel),
        dashState.spectrum.peakFreq, dashState.spectrum.peakRSSI,
        dashState.gps.fixType, dashState.gps.numSV,
        dashState.gps.latDeg7 / 1e7, dashState.gps.lonDeg7 / 1e7,
        dashState.gps.altMM / 1000, dashState.gps.hAccMM / 1000,
        dashState.gps.jammingState, dashState.gps.jamInd,
        dashState.gps.spoofDetState,
        dashState.integrity.cnoStdDev,
        millis() / 1000, ESP.getFreeHeap());

    server.send(200, "application/json", json);
}

void dashboardInit() {
    // softAP() sets WIFI_AP mode internally — no WiFi.mode() needed.
    // softAPConfig() removed — 192.168.4.1 is already the default, and calling
    // it before softAP() can crash on DHCP lease renewal (espressif#3906).
    bool apOK = WiFi.softAP("SENTRY-RF", "SentryP@ssword", 1, 0, 4);
    if (!apOK) {
        Serial.printf("[WIFI] FAIL: softAP returned false (mode=%d)\n", WiFi.getMode());
        return;
    }

    delay(100);
    Serial.printf("[WIFI] AP started: SENTRY-RF @ %s\n", WiFi.softAPIP().toString().c_str());

    server.on("/", handleRoot);
    server.on("/api/status", handleAPI);
    server.begin();
    Serial.println("[WIFI] HTTP server started on port 80");
}

void dashboardHandle() {
    server.handleClient();
}

void dashboardUpdateState(const SystemState& state) {
    dashState = state;
}

#else // no board defined

void dashboardInit() {}
void dashboardHandle() {}
void dashboardUpdateState(const SystemState& state) { (void)state; }

#endif // WIFI_ENABLED
