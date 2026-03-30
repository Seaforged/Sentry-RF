#include "gps_manager.h"
#include "board_config.h"
#include <Arduino.h>

static SFE_UBLOX_GNSS_SERIAL gps;

// Track when we last polled so we don't spam the module
static unsigned long lastPollMs = 0;
static const unsigned long GPS_POLL_INTERVAL_MS = 1000;

// Production C/N0 minimum — 15 dB-Hz rejects noise while keeping usable satellites.
// For indoor bench testing, temporarily lower to 6.
// WARNING: Values below 10 will allow weak/multipath signals into the fix,
// degrading position accuracy and integrity monitoring reliability.
static const uint8_t GPS_MIN_CNO = 15;

static bool connectAtBaud(uint32_t baud) {
    Serial1.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    delay(100);
    return gps.begin(Serial1);
}

// Setup library-side auto callbacks — these tell the SparkFun library to parse
// incoming messages into cached structs as checkUblox() processes UART bytes.
// This replaces blocking poll requests that were starving the GPS of processing time.
static void setupLibraryState() {
    gps.setAutoPVT(true);
    gps.setAutoMONHW(true);
    gps.setAutoNAVSTATUS(true);
    gps.setAutoNAVSAT(true);
    gps.setPacketCfgPayloadSize(UBX_NAV_SAT_MAX_LEN);
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

    setupLibraryState();

    // Portable model — works on foot, in vehicles, and everywhere in between.
    // Stationary model rejects all motion as noise, breaking GPS at highway speeds.
    gps.setDynamicModel(DYN_MODEL_PORTABLE);

    // Indoor-safe C/N0 threshold — low enough to keep weak satellites in the solution
    gps.setVal8(UBLOX_CFG_NAVSPG_INFIL_MINCNO, GPS_MIN_CNO, VAL_LAYER_RAM_BBR);

    // Ignore satellites below 10° elevation — they contribute the most multipath error
    gps.setVal8(UBLOX_CFG_NAVSPG_INFIL_MINELEV, 10, VAL_LAYER_RAM_BBR);

    // M10 predicts orbits from stored ephemeris — faster warm starts without WiFi
    gps.setVal8(UBLOX_CFG_ANA_USE_ANA, 1, VAL_LAYER_RAM_BBR);

    // Enable integrity-related UBX messages at 1 Hz on UART1
    // MON-HW has jamInd/jammingState/agcCnt; NAV-STATUS has spoofDetState; NAV-SAT has per-sat C/N0
    // Note: MON-RF would be preferred on M10 but SparkFun v3 has no setAutoMONRF — MON-HW is the
    // only monitor message with auto-cached support, so we use it to avoid blocking polls.
    gps.setVal8(UBLOX_CFG_MSGOUT_UBX_MON_HW_UART1, 1, VAL_LAYER_RAM_BBR);
    gps.setVal8(UBLOX_CFG_MSGOUT_UBX_NAV_STATUS_UART1, 1, VAL_LAYER_RAM_BBR);
    gps.setVal8(UBLOX_CFG_MSGOUT_UBX_NAV_SAT_UART1, 1, VAL_LAYER_RAM_BBR);

    return true;
}

static bool bumpBaudRate() {
    // RAM_BBR persists through GPS sleep but not full power cycle (per CLAUDE.md)
    if (!gps.setVal32(UBLOX_CFG_UART1_BAUDRATE, GPS_BAUD_TARGET, VAL_LAYER_RAM_BBR)) {
        Serial.println("[GPS] Failed to set 38400 baud via VALSET");
        return false;
    }

    // Reconnect at the new speed — must re-set buffer size after Serial1.end()
    delay(100);
    Serial1.end();
    Serial1.setRxBufferSize(1024);
    Serial1.begin(GPS_BAUD_TARGET, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    delay(100);

    return gps.begin(Serial1);
}

bool gpsInit() {
    // T3S3 native USB CDC triggers a reset on connect — short settle time
#if defined(BOARD_T3S3) || defined(BOARD_T3S3_LR1121)
    delay(500);
#endif

    // Set UART RX buffer once before any Serial1.begin() — 1024 bytes prevents
    // overflow during the ~457ms LoRa sweep at 38400 baud
    Serial1.setRxBufferSize(1024);

    // Always run full configuration — ensures MON-HW/NAV-STATUS/NAV-SAT are enabled.
    // The isAlreadyConfigured() fast path was removed because it skipped module-side
    // message output configuration, causing MON-HW to never populate (Jam:--- bug).
    // Satellite tracking state survives reconfiguration via RAM_BBR layer.
    if (connectAtBaud(GPS_BAUD_TARGET)) {
        Serial.printf("[GPS] Connected at %d baud — configuring\n", GPS_BAUD_TARGET);
        return configureUBX();
    }

    // Fall back to factory default baud
    if (!connectAtBaud(GPS_BAUD_DEFAULT)) {
        Serial.println("[GPS] FAIL: no response at 9600 or 38400");
        return false;
    }

    Serial.printf("[GPS] Connected at %d baud (factory default) — full configuration\n",
                  GPS_BAUD_DEFAULT);

    if (!configureUBX()) return false;

    if (!bumpBaudRate()) {
        Serial.println("[GPS] WARNING: running at 9600 — baud upgrade failed");
        return true;
    }

    Serial.printf("[GPS] Baud upgraded to %d\n", GPS_BAUD_TARGET);
    return true;
}

// All three readers below use auto-cached data updated by checkUblox() —
// no blocking polls, so the GPS module spends its time tracking, not responding.

static void readMonHW(GpsData& data) {
    if (gps.packetUBXMONHW == nullptr) return;

    UBX_MON_HW_data_t &hw = gps.packetUBXMONHW->data;
    data.jamInd       = hw.jamInd;
    data.jammingState = hw.flags.bits.jammingState;

    // agcCnt is 0-8191 — scale to 0-100 for display
    data.agcPercent = (uint8_t)((hw.agcCnt * 100UL) / 8191);
}

static void readNavStatus(GpsData& data) {
    if (gps.packetUBXNAVSTATUS == nullptr) return;

    data.spoofDetState = gps.packetUBXNAVSTATUS->data.flags2.bits.spoofDetState;
}

static void readNavSat(GpsData& data) {
    data.satCnoCount = 0;
    if (gps.packetUBXNAVSAT == nullptr) return;

    uint16_t numSats = gps.packetUBXNAVSAT->data.header.numSvs;
    for (uint16_t i = 0; i < numSats && data.satCnoCount < MAX_SAT_CNO; i++) {
        int16_t elev = gps.packetUBXNAVSAT->data.blocks[i].elev;
        uint8_t cno  = gps.packetUBXNAVSAT->data.blocks[i].cno;

        // Filter very low sats (heavy multipath) but keep 5°+ for indoor use
        if (elev >= 5 && cno > 0) {
            data.satCno[data.satCnoCount++] = (float)cno;
        }
    }
}

void gpsProcess() {
    gps.checkUblox();
}

void gpsUpdate(GpsData& data) {
    unsigned long now = millis();
    if (now - lastPollMs < GPS_POLL_INTERVAL_MS) return;
    lastPollMs = now;

    data.fixType = gps.getFixType();
    data.numSV   = gps.getSIV();
    data.latDeg7 = gps.getLatitude();
    data.lonDeg7 = gps.getLongitude();
    data.altMM   = gps.getAltitudeMSL();
    data.pDOP    = gps.getPDOP();
    data.hAccMM  = gps.getHorizontalAccEst();
    data.vAccMM  = gps.getVerticalAccEst();

    readMonHW(data);
    readNavStatus(data);
    readNavSat(data);

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
