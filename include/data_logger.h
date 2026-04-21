#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include "detection_types.h"

// Initialize storage: SD card on T3S3, SPIFFS on Heltec V3
bool loggerInit();

// Append one row from current system state (CSV legacy + JSONL field test)
void loggerWrite(const SystemState& state, uint32_t sweepNum);

// Flush buffered writes to storage — call periodically to prevent data loss
void loggerFlush();

// Phase H: write a mode-change event to the JSONL field log. Does nothing
// on boards without a logger backing store. Caller passes the short mode
// label and a pre-formatted "HH:MM:SS" uptime string.
void loggerLogModeChange(const char* modeLabel, const char* uptimeHMS);

// Phase K: one-shot boot self-test event emitted right after loggerInit.
// radioOK / antennaOK are the self-test outcomes, boot is the persistent
// boot counter from RTC memory. GPS result isn't included because it's
// evaluated asynchronously after the self-test completes.
void loggerLogSelfTest(bool radioOK, bool antennaOK, uint32_t bootCount);

#endif // DATA_LOGGER_H
