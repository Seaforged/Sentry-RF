#include "data_logger.h"
#include "board_config.h"
#include <Arduino.h>

static uint32_t writeCount = 0;
static const uint32_t FLUSH_INTERVAL = 10;

// CSV header written once at file creation
static const char* CSV_HEADER =
    "timestamp_ms,sweep_num,threat_level,peak_freq_mhz,peak_rssi_dbm,"
    "gps_lat,gps_lon,fix_type,num_sv,jam_ind,spoof_state,cno_stddev";

// ── T3S3: SD card logging ───────────────────────────────────────────────────

#ifdef BOARD_T3S3
#include <SD.h>
#include <SPI.h>

static SPIClass sdSPI(FSPI);
static File logFile;

static bool findNextFilename(char* buf, int bufLen) {
    for (int i = 0; i < 10000; i++) {
        snprintf(buf, bufLen, "/log_%04d.csv", i);
        if (!SD.exists(buf)) return true;
    }
    return false;
}

bool loggerInit() {
    sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, sdSPI)) {
        Serial.println("[LOG] SD card init failed");
        return false;
    }

    char filename[24];
    if (!findNextFilename(filename, sizeof(filename))) {
        Serial.println("[LOG] No available filename");
        return false;
    }

    logFile = SD.open(filename, FILE_WRITE);
    if (!logFile) {
        Serial.printf("[LOG] Failed to create %s\n", filename);
        return false;
    }

    logFile.println(CSV_HEADER);
    logFile.flush();
    Serial.printf("[LOG] SD logging to %s\n", filename);
    return true;
}

void loggerWrite(const SystemState& state, uint32_t sweepNum) {
    if (!logFile) return;

    logFile.printf("%lu,%u,%d,%.1f,%.1f,%.6f,%.6f,%d,%d,%d,%d,%.1f\n",
                   millis(), sweepNum, state.threatLevel,
                   state.spectrum.peakFreq, state.spectrum.peakRSSI,
                   state.gps.latDeg7 / 1e7, state.gps.lonDeg7 / 1e7,
                   state.gps.fixType, state.gps.numSV,
                   state.gps.jamInd, state.gps.spoofDetState,
                   state.integrity.cnoStdDev);

    writeCount++;
    if (writeCount % FLUSH_INTERVAL == 0) logFile.flush();
}

void loggerFlush() {
    if (logFile) logFile.flush();
}

#endif // BOARD_T3S3

// ── Heltec V3: SPIFFS logging ──────────────────────────────────────────────

#ifdef BOARD_HELTEC_V3
#include <SPIFFS.h>

static File logFile;
static const size_t MAX_LOG_SIZE = 100 * 1024;  // 100 KB rotation

bool loggerInit() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[LOG] SPIFFS init failed");
        return false;
    }

    // Rotate if log is too large — prevents SPIFFS from filling up
    if (SPIFFS.exists("/log.csv")) {
        File check = SPIFFS.open("/log.csv", "r");
        if (check && check.size() > MAX_LOG_SIZE) {
            check.close();
            SPIFFS.remove("/log_old.csv");
            SPIFFS.rename("/log.csv", "/log_old.csv");
            Serial.println("[LOG] Rotated log_old.csv");
        } else {
            check.close();
        }
    }

    logFile = SPIFFS.open("/log.csv", FILE_APPEND);
    if (!logFile) {
        Serial.println("[LOG] Failed to open /log.csv");
        return false;
    }

    // Write header if file is new/empty
    if (logFile.size() == 0) {
        logFile.println(CSV_HEADER);
        logFile.flush();
    }

    Serial.printf("[LOG] SPIFFS logging to /log.csv (%u bytes)\n", logFile.size());
    return true;
}

void loggerWrite(const SystemState& state, uint32_t sweepNum) {
    if (!logFile) return;

    logFile.printf("%lu,%u,%d,%.1f,%.1f,%.6f,%.6f,%d,%d,%d,%d,%.1f\n",
                   millis(), sweepNum, state.threatLevel,
                   state.spectrum.peakFreq, state.spectrum.peakRSSI,
                   state.gps.latDeg7 / 1e7, state.gps.lonDeg7 / 1e7,
                   state.gps.fixType, state.gps.numSV,
                   state.gps.jamInd, state.gps.spoofDetState,
                   state.integrity.cnoStdDev);

    writeCount++;
    if (writeCount % FLUSH_INTERVAL == 0) logFile.flush();
}

void loggerFlush() {
    if (logFile) logFile.flush();
}

#endif // BOARD_HELTEC_V3
