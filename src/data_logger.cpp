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

#if defined(BOARD_T3S3) || defined(BOARD_T3S3_LR1121)
#include <SD.h>
#include <SPI.h>
#include <SD_MMC.h>

static SPIClass sdSPI(FSPI);
static File logFile;
static File jsonlFile;
static bool useMMC = false;  // true if SD_MMC worked instead of SPI

static int findNextIndex() {
    for (int i = 0; i < 10000; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "/log_%04d.csv", i);
        bool exists = useMMC ? SD_MMC.exists(buf) : SD.exists(buf);
        if (!exists) return i;
    }
    return -1;
}

bool loggerInit() {
    Serial.printf("[LOG] SD pins: CS=%d SCK=%d MISO=%d MOSI=%d\n",
                  PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI);

    Serial.printf("[LOG] SD pins: CS=%d SCK=%d MISO=%d MOSI=%d\n",
                  PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI);

    // Method 1: Try SPI mode first
    SD.end();
    delay(100);
    sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    delay(100);

    if (SD.begin(PIN_SD_CS, sdSPI, 4000000)) {
        Serial.printf("[LOG] SD SPI mode OK! Size: %lluMB\n", SD.cardSize() / (1024 * 1024));
    } else {
        Serial.println("[LOG] SD SPI failed, trying SD_MMC 1-bit mode...");
        SD.end();
        delay(100);

        // Method 2: Try SD_MMC 1-bit mode
        // In 1-bit mode: CLK=SCK, CMD=MOSI, D0=MISO
        SD_MMC.setPins(PIN_SD_SCK, PIN_SD_MOSI, PIN_SD_MISO);
        if (SD_MMC.begin("/sdcard", true)) {  // true = 1-bit mode
            useMMC = true;
            Serial.printf("[LOG] SD_MMC 1-bit mode OK! Size: %lluMB\n",
                          SD_MMC.cardSize() / (1024 * 1024));
        } else {
            Serial.println("[LOG] SD card init failed (SPI and MMC)");
            return false;
        }
    }

    int idx = findNextIndex();
    if (idx < 0) {
        Serial.println("[LOG] No available filename");
        return false;
    }

    char csvName[24], jsonlName[28];
    snprintf(csvName, sizeof(csvName), "/log_%04d.csv", idx);
    snprintf(jsonlName, sizeof(jsonlName), "/field_%04d.jsonl", idx);

    logFile = useMMC ? SD_MMC.open(csvName, FILE_WRITE) : SD.open(csvName, FILE_WRITE);
    if (!logFile) {
        Serial.printf("[LOG] Failed to create %s\n", csvName);
        return false;
    }
    logFile.println(CSV_HEADER);
    logFile.flush();

    jsonlFile = useMMC ? SD_MMC.open(jsonlName, FILE_WRITE) : SD.open(jsonlName, FILE_WRITE);
    if (jsonlFile) {
        Serial.printf("[LOG] Field test JSONL: %s\n", jsonlName);
    }

    Serial.printf("[LOG] SD logging to %s\n", csvName);
    return true;
}

void loggerWrite(const SystemState& state, uint32_t sweepNum) {
    unsigned long ts = millis();

    // Legacy CSV
    if (logFile) {
        logFile.printf("%lu,%u,%d,%.1f,%.1f,%.6f,%.6f,%d,%d,%d,%d,%.1f\n",
                       ts, sweepNum, state.threatLevel,
                       state.spectrum.peakFreq, state.spectrum.peakRSSI,
                       state.gps.latDeg7 / 1e7, state.gps.lonDeg7 / 1e7,
                       state.gps.fixType, state.gps.numSV,
                       state.gps.jamInd, state.gps.spoofDetState,
                       state.integrity.cnoStdDev);
    }

    // JSONL field test log — one JSON object per line
    if (jsonlFile) {
        jsonlFile.printf("{\"t\":%lu,\"c\":%u,\"threat\":%d,\"score\":%d,"
                         "\"div\":%d,\"conf\":%d,\"taps\":%d,"
                         "\"peak_mhz\":%.1f,\"peak_dbm\":%.1f,"
                         "\"lat\":%.7f,\"lon\":%.7f,\"fix\":%d,\"sv\":%d,"
                         "\"jam\":%d,\"spoof\":%d,\"cno_sd\":%.1f}\n",
                         ts, sweepNum, (int)state.threatLevel, state.confidenceScore,
                         state.cadDiversity, state.cadConfirmed, state.cadTotalTaps,
                         state.spectrum.peakFreq, state.spectrum.peakRSSI,
                         state.gps.latDeg7 / 1e7, state.gps.lonDeg7 / 1e7,
                         state.gps.fixType, state.gps.numSV,
                         state.gps.jamInd, state.gps.spoofDetState,
                         state.integrity.cnoStdDev);
    }

    writeCount++;
    if (writeCount % FLUSH_INTERVAL == 0) {
        if (logFile) logFile.flush();
        if (jsonlFile) jsonlFile.flush();
    }
}

void loggerFlush() {
    if (logFile) logFile.flush();
    if (jsonlFile) jsonlFile.flush();
}

void loggerLogModeChange(const char* modeLabel, const char* uptimeHMS) {
    if (jsonlFile) {
        jsonlFile.printf("{\"t\":%lu,\"event\":\"mode_change\","
                         "\"mode\":\"%s\",\"uptime\":\"%s\"}\n",
                         millis(), modeLabel, uptimeHMS);
        jsonlFile.flush();
    }
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

void loggerLogModeChange(const char* modeLabel, const char* uptimeHMS) {
    // Heltec V3 uses SPIFFS CSV only (no JSONL sibling), so we inline
    // the mode change as a CSV comment line. Humans skim the log; the
    // "# mode_change," prefix makes it grep-friendly.
    if (logFile) {
        logFile.printf("# mode_change,%lu,%s,%s\n",
                       millis(), modeLabel, uptimeHMS);
        logFile.flush();
    }
}

#endif // BOARD_HELTEC_V3
