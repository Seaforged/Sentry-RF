#include "data_logger.h"
#include "board_config.h"
#include "sentry_config.h"
#include "version.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static uint32_t writeCount = 0;
static const uint32_t FLUSH_INTERVAL = 10;

// Phase K/rev F7: loggerWrite() runs in loRaScanTask (Core 1);
// loggerLogModeChange() runs in displayTask (Core 0); loggerLogSelfTest()
// runs in setup() (pre-task, single-threaded). All three touch the same
// File objects (logFile, jsonlFile). SD driver writes are NOT thread-safe —
// concurrent writes from two cores can interleave bytes across JSONL rows
// or corrupt internal buffer pointers. This mutex serializes every entry
// point so a single row always lands contiguously.
static SemaphoreHandle_t loggerMutex = nullptr;

static void loggerMutexInit() {
    if (loggerMutex == nullptr) {
        loggerMutex = xSemaphoreCreateMutex();
    }
}

// Issue 12: block on the mutex until acquired (portMAX_DELAY) rather than
// the previous 50 ms timeout that silently dropped rows under contention.
// With portMAX_DELAY the only bound on latency is the duration of the
// holder's I/O, which is itself bounded by SD write speed (single-digit
// ms typical). Silent drops masked real data loss that would have hit
// analysts via `field_analyzer.py` later.
static bool loggerLock() {
    if (loggerMutex == nullptr) return true;
    xSemaphoreTake(loggerMutex, portMAX_DELAY);
    return true;
}

static void loggerUnlock() {
    if (loggerMutex != nullptr) xSemaphoreGive(loggerMutex);
}

// Phase L: ZMQ emit debouncers — enforce ≤ 1 Hz per event type so a tight
// transition flap or a busy RID beacon can't flood the serial link. Kept
// at file scope (not inside #ifdef blocks) since emitZmqJson is board-
// agnostic.
static unsigned long lastZmqThreatMs = 0;
static unsigned long lastZmqRidMs    = 0;

void emitZmqJson(const SystemState& state, const char* eventType) {
#if ENABLE_ZMQ_OUTPUT
    if (eventType == nullptr) return;
    unsigned long now = millis();

    // Per-event-type 1 Hz debounce. strcmp() over "threat" / "rid" costs
    // ~10ns, trivial against the Serial.printf below.
    if (strcmp(eventType, "threat") == 0) {
        if (now - lastZmqThreatMs < 1000UL && lastZmqThreatMs != 0) return;
        lastZmqThreatMs = now;
    } else if (strcmp(eventType, "rid") == 0) {
        if (now - lastZmqRidMs < 1000UL && lastZmqRidMs != 0) return;
        lastZmqRidMs = now;
    } else {
        return;  // unknown event type — drop silently
    }

    // Prefix is EXACTLY "[ZMQ] " (6 chars) — the Python bridge strips with
    // line[6:] so any change here breaks the wire format. Issue 3 fix:
    // route through SERIAL_SAFE (blocking portMAX_DELAY) instead of the
    // prior take-with-timeout-and-fallback pattern — the fallback printed
    // without the mutex on contention, which could interleave bytes into
    // the JSON frame and corrupt the ZMQ pipe.
    if (strcmp(eventType, "threat") == 0) {
        SERIAL_SAFE(Serial.printf("[ZMQ] {\"type\":\"threat\",\"ts\":%lu,\"threat\":%d,"
                                  "\"score\":%d,\"freq_mhz\":%.1f,\"rssi_dbm\":%.1f,"
                                  "\"lat\":%.6f,\"lon\":%.6f,\"alt_m\":%d,"
                                  "\"cad_conf\":%d,\"div\":%d,\"mode\":\"%s\"}\n",
                                  now,
                                  (int)state.threatLevel,
                                  state.confidenceScore,
                                  state.spectrum.peakFreq, state.spectrum.peakRSSI,
                                  state.gps.latDeg7 / 1e7,
                                  state.gps.lonDeg7 / 1e7,
                                  (int)(state.gps.altMM / 1000),
                                  state.cadConfirmed,
                                  state.cadDiversity,
                                  modeShortLabel(modeGet())));
    } else {  // "rid" (checked above)
        const DecodedRID& r = state.lastRID;
        SERIAL_SAFE(Serial.printf("[ZMQ] {\"type\":\"rid\",\"ts\":%lu,\"uas_id\":\"%s\","
                                  "\"uas_type\":\"%s\",\"d_lat\":%.4f,\"d_lon\":%.4f,"
                                  "\"d_alt_m\":%.1f,\"o_lat\":%.4f,\"o_lon\":%.4f,"
                                  "\"spd_mps\":%.1f,\"hdg\":%u}\n",
                                  now,
                                  r.uasID, r.uasIDType,
                                  r.droneLat, r.droneLon, r.droneAltM,
                                  r.operatorLat, r.operatorLon,
                                  r.speedMps, (unsigned)r.headingDeg));
    }
#else
    (void)state;
    (void)eventType;
#endif
}

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
    // Create the cross-task serialization mutex before any logger entry
    // point could be racing. Idempotent.
    loggerMutexInit();

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
    if (!loggerLock()) return;   // contention > 50 ms — drop this row
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

    // JSONL field test log — one JSON object per line. peak_bw and peak_bins
    // (Phase I) describe the bandwidth class of the strongest sub-GHz peak:
    // 0=NARROW, 1=MEDIUM, 2=WIDE (see BandwidthClass in detection_types.h).
    // Phase J: rid_* fields are appended when lastRID.valid is true — each
    // row is self-describing JSON so the parser tolerates missing keys.
    if (jsonlFile) {
        jsonlFile.printf("{\"t\":%lu,\"c\":%u,\"threat\":%d,\"score\":%d,"
                         "\"div\":%d,\"conf\":%d,\"taps\":%d,"
                         "\"peak_mhz\":%.1f,\"peak_dbm\":%.1f,"
                         "\"peak_bw\":%u,\"peak_bins\":%u,"
                         "\"lat\":%.7f,\"lon\":%.7f,\"fix\":%d,\"sv\":%d,"
                         "\"jam\":%d,\"spoof\":%d,\"cno_sd\":%.1f",
                         ts, sweepNum, (int)state.threatLevel, state.confidenceScore,
                         state.cadDiversity, state.cadConfirmed, state.cadTotalTaps,
                         state.spectrum.peakFreq, state.spectrum.peakRSSI,
                         (unsigned)state.peakBwClass, (unsigned)state.peakAdjBins,
                         state.gps.latDeg7 / 1e7, state.gps.lonDeg7 / 1e7,
                         state.gps.fixType, state.gps.numSV,
                         state.gps.jamInd, state.gps.spoofDetState,
                         state.integrity.cnoStdDev);
        if (state.lastRID.valid) {
            jsonlFile.printf(",\"rid_id\":\"%s\","
                             "\"rid_dlat\":%.6f,\"rid_dlon\":%.6f,\"rid_dalt\":%.1f,"
                             "\"rid_olat\":%.6f,\"rid_olon\":%.6f",
                             state.lastRID.uasID,
                             state.lastRID.droneLat, state.lastRID.droneLon,
                             state.lastRID.droneAltM,
                             state.lastRID.operatorLat, state.lastRID.operatorLon);
        }
        jsonlFile.print("}\n");
    }

    writeCount++;
    if (writeCount % FLUSH_INTERVAL == 0) {
        if (logFile) logFile.flush();
        if (jsonlFile) jsonlFile.flush();
    }
    loggerUnlock();
}

void loggerFlush() {
    if (!loggerLock()) return;
    if (logFile) logFile.flush();
    if (jsonlFile) jsonlFile.flush();
    loggerUnlock();
}

void loggerLogModeChange(const char* modeLabel, const char* uptimeHMS) {
    if (!loggerLock()) return;
    if (jsonlFile) {
        jsonlFile.printf("{\"t\":%lu,\"event\":\"mode_change\","
                         "\"mode\":\"%s\",\"uptime\":\"%s\"}\n",
                         millis(), modeLabel, uptimeHMS);
        jsonlFile.flush();
    }
    loggerUnlock();
}

void loggerLogSelfTest(bool radioOK, bool antennaOK, uint32_t bootCount) {
    if (!loggerLock()) return;
    if (jsonlFile) {
        jsonlFile.printf("{\"event\":\"selftest\","
                         "\"radio\":\"%s\",\"antenna\":\"%s\","
                         "\"fw\":\"%s\",\"boot\":%u}\n",
                         radioOK   ? "OK" : "FAIL",
                         antennaOK ? "OK" : "WARN",
                         FW_VERSION, (unsigned)bootCount);
        jsonlFile.flush();
    }
    loggerUnlock();
}

#endif // BOARD_T3S3

// ── Heltec V3: SPIFFS logging ──────────────────────────────────────────────

#ifdef BOARD_HELTEC_V3
#include <SPIFFS.h>

static File logFile;
static const size_t MAX_LOG_SIZE = 100 * 1024;  // 100 KB rotation

bool loggerInit() {
    loggerMutexInit();

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
    if (!loggerLock()) return;
    if (!logFile) { loggerUnlock(); return; }

    logFile.printf("%lu,%u,%d,%.1f,%.1f,%.6f,%.6f,%d,%d,%d,%d,%.1f\n",
                   millis(), sweepNum, state.threatLevel,
                   state.spectrum.peakFreq, state.spectrum.peakRSSI,
                   state.gps.latDeg7 / 1e7, state.gps.lonDeg7 / 1e7,
                   state.gps.fixType, state.gps.numSV,
                   state.gps.jamInd, state.gps.spoofDetState,
                   state.integrity.cnoStdDev);

    writeCount++;
    if (writeCount % FLUSH_INTERVAL == 0) logFile.flush();
    loggerUnlock();
}

void loggerFlush() {
    if (!loggerLock()) return;
    if (logFile) logFile.flush();
    loggerUnlock();
}

void loggerLogModeChange(const char* modeLabel, const char* uptimeHMS) {
    // Heltec V3 uses SPIFFS CSV only (no JSONL sibling), so we inline
    // the mode change as a CSV comment line. Humans skim the log; the
    // "# mode_change," prefix makes it grep-friendly.
    if (!loggerLock()) return;
    if (logFile) {
        logFile.printf("# mode_change,%lu,%s,%s\n",
                       millis(), modeLabel, uptimeHMS);
        logFile.flush();
    }
    loggerUnlock();
}

void loggerLogSelfTest(bool radioOK, bool antennaOK, uint32_t bootCount) {
    if (!loggerLock()) return;
    if (logFile) {
        logFile.printf("# selftest,%s,%s,%s,%u\n",
                       radioOK   ? "OK" : "FAIL",
                       antennaOK ? "OK" : "WARN",
                       FW_VERSION, (unsigned)bootCount);
        logFile.flush();
    }
    loggerUnlock();
}

#endif // BOARD_HELTEC_V3
