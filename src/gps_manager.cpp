#include "gps_manager.h"
#include "board_config.h"
#include <Arduino.h>

static SFE_UBLOX_GNSS_SERIAL gps;

// Track when we last polled so we don't spam the module
static unsigned long lastPollMs = 0;
static const unsigned long GPS_POLL_INTERVAL_MS = 1000;

// millis() of most recent MON-HW message arrival, set by the SparkFun
// auto-callback registered in setupLibraryState(). Volatile because the
// callback may fire from any task context running checkUblox().
static volatile unsigned long g_monHwLastArrivalMs = 0;

// Freshness window for MON-HW data — Dashboard and Integrity screens
// gate their displayed jam/spoof/AGC fields on (now - lastArrival) < this.
static const unsigned long MON_HW_FRESH_WINDOW_MS = 5000;

static void onMonHwArrived(UBX_MON_HW_data_t* /*unused*/) {
    g_monHwLastArrivalMs = millis();
}

bool gpsMonHwIsFresh(const GpsData& data) {
    if (data.monHwLastUpdateMs == 0) return false;  // never populated
    return (millis() - data.monHwLastUpdateMs) < MON_HW_FRESH_WINDOW_MS;
}

// Production C/N0 minimum — 15 dB-Hz rejects noise while keeping usable satellites.
// GPS_MIN_CNO from sentry_config.h (15=field, 6=indoor bench)
#include "sentry_config.h"

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
    // MON-HW via callback mode so we can timestamp each fresh arrival.
    // The callback sets g_monHwLastArrivalMs; readMonHW() copies it into
    // GpsData so the UI can gate display on freshness.
    gps.setAutoMONHWcallbackPtr(&onMonHwArrived);
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

    // Try 115200 first — HGLRC M100 Mini factory default per manufacturer spec
    if (connectAtBaud(GPS_BAUD_HGLRC)) {
        Serial.printf("[GPS] Connected at %d baud — configuring\n", GPS_BAUD_HGLRC);
        if (!configureUBX()) return false;
        // Step down to operational baud — lower CPU overhead during scan loops
        if (!bumpBaudRate()) {
            Serial.printf("[GPS] WARNING: running at %d — baud step-down failed\n", GPS_BAUD_HGLRC);
            return true;
        }
        Serial.printf("[GPS] Baud reduced to %d\n", GPS_BAUD_TARGET);
        return true;
    }

    // ESP32-S3 wedges UART1 if begin() is called twice without end() between
    Serial1.end();
    delay(10);
    Serial1.setRxBufferSize(1024);

    if (connectAtBaud(GPS_BAUD_TARGET)) {
        Serial.printf("[GPS] Connected at %d baud — configuring\n", GPS_BAUD_TARGET);
        return configureUBX();
    }

    Serial1.end();
    delay(10);
    Serial1.setRxBufferSize(1024);

    // Fall back to u-blox factory default baud
    if (!connectAtBaud(GPS_BAUD_DEFAULT)) {
        Serial.println("[GPS] FAIL: no response at 115200, 38400, or 9600");
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

    // Propagate the most recent callback arrival time so UI can gate on
    // freshness. 0 means no MON-HW has ever arrived since boot.
    data.monHwLastUpdateMs = g_monHwLastArrivalMs;
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

        // Phase K + Issue 7: filter satellites at MIN_ELEV_FOR_CNO (20° prod)
        // AND cno >= GPS_MIN_CNO (15 dB-Hz prod). Low-elev sats suffer
        // multipath; low-SNR sats just add noise to the uniformity calc.
        // Both constraints match the sentry_config.h thresholds used by the
        // downstream spoof/jam gates, so sats that wouldn't count toward a
        // threat assessment don't distort the stddev that drives it either.
        if (elev >= MIN_ELEV_FOR_CNO && cno >= GPS_MIN_CNO) {
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
