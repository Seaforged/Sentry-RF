#include "gps_manager.h"
#include "board_config.h"
#include <Arduino.h>

static SFE_UBLOX_GNSS_SERIAL gps;

// Track when we last polled so we don't spam the module
static unsigned long lastPollMs = 0;
static const unsigned long GPS_POLL_INTERVAL_MS = 1000;

static bool connectAtBaud(uint32_t baud) {
    Serial1.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    delay(100);
    return gps.begin(Serial1);
}

static bool configureUBX() {
    // UBX-only output — NMEA floods the UART and wastes bandwidth
    if (!gps.setUART1Output(COM_TYPE_UBX)) {
        Serial.println("[GPS] Failed to set UBX-only mode");
        return false;
    }

    // 1 Hz nav rate is sufficient for a stationary/slow-moving detector
    if (!gps.setNavigationFrequency(1)) {
        Serial.println("[GPS] Failed to set 1 Hz nav rate");
        return false;
    }

    // NAV-PVT gives us fix, position, and DOP in one message
    gps.setAutoPVT(true);

    // Zero-velocity model — Kalman filter assumes no motion, reduces position wander
    gps.setDynamicModel(DYN_MODEL_STATIONARY);

    // Reject weak signals below 20 dB-Hz — reduces multipath and sets Sprint 4 spoofing baseline
    gps.setVal8(UBLOX_CFG_NAVSPG_INFIL_MINCNO, 20, VAL_LAYER_RAM_BBR);

    // Ignore satellites below 10° elevation — they contribute the most multipath error
    gps.setVal8(UBLOX_CFG_NAVSPG_INFIL_MINELEV, 10, VAL_LAYER_RAM_BBR);

    // M10 predicts orbits from stored ephemeris — faster warm starts without WiFi
    gps.setVal8(UBLOX_CFG_ANA_USE_ANA, 1, VAL_LAYER_RAM_BBR);

    return true;
}

static bool bumpBaudRate() {
    // RAM_BBR persists through GPS sleep but not full power cycle (per CLAUDE.md)
    if (!gps.setVal32(UBLOX_CFG_UART1_BAUDRATE, GPS_BAUD_TARGET, VAL_LAYER_RAM_BBR)) {
        Serial.println("[GPS] Failed to set 38400 baud via VALSET");
        return false;
    }

    // Reconnect our UART at the new speed
    delay(100);
    Serial1.end();
    Serial1.begin(GPS_BAUD_TARGET, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    delay(100);

    return gps.begin(Serial1);
}

bool gpsInit() {
    // Try target baud first — module may already be configured from a previous boot
    if (connectAtBaud(GPS_BAUD_TARGET)) {
        Serial.printf("[GPS] Connected at %d baud (already configured)\n", GPS_BAUD_TARGET);
        return configureUBX();
    }

    // Fall back to factory default baud
    if (!connectAtBaud(GPS_BAUD_DEFAULT)) {
        Serial.println("[GPS] FAIL: no response at 9600 or 38400");
        return false;
    }

    Serial.printf("[GPS] Connected at %d baud (factory default)\n", GPS_BAUD_DEFAULT);

    if (!configureUBX()) return false;

    if (!bumpBaudRate()) {
        Serial.println("[GPS] WARNING: running at 9600 — baud upgrade failed");
        return true;
    }

    Serial.printf("[GPS] Baud upgraded to %d\n", GPS_BAUD_TARGET);
    return true;
}

void gpsUpdate(GpsData& data) {
    unsigned long now = millis();
    if (now - lastPollMs < GPS_POLL_INTERVAL_MS) return;
    lastPollMs = now;

    // checkUblox() processes incoming bytes and updates autoPVT data internally.
    // We don't gate on getPVT() because with autoPVT enabled the getters
    // always return the latest cached values — getPVT() can return false
    // even when data is valid, which was causing SVs:0 despite a hardware fix.
    gps.checkUblox();

    data.fixType = gps.getFixType();
    data.numSV   = gps.getSIV();
    data.latDeg7 = gps.getLatitude();
    data.lonDeg7 = gps.getLongitude();
    data.altMM   = gps.getAltitudeMSL();
    data.pDOP    = gps.getPDOP();
    data.hAccMM  = gps.getHorizontalAccEst();
    data.vAccMM  = gps.getVerticalAccEst();
    data.valid   = true;
}

void gpsPrintStatus(const GpsData& data) {
    if (!data.valid) {
        Serial.println("[GPS] Waiting for first PVT response...");
        return;
    }

    if (data.fixType < 2) {
        Serial.printf("[GPS] No fix | Acquiring... SVs:%d\n", data.numSV);
        return;
    }

    Serial.printf("[GPS] Fix:%d SVs:%d Lat:%.6f Lon:%.6f Alt:%dm pDOP:%.2f hAcc:%dm\n",
                  data.fixType,
                  data.numSV,
                  data.latDeg7 / 1e7,
                  data.lonDeg7 / 1e7,
                  data.altMM / 1000,
                  data.pDOP / 100.0,
                  data.hAccMM / 1000);
}
