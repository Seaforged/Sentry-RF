#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include "detection_types.h"

// Initialize storage: SD card on T3S3, SPIFFS on Heltec V3
bool loggerInit();

// Append one row from current system state (CSV legacy + JSONL field test)
void loggerWrite(const SystemState& state, uint32_t sweepNum);

// Flush buffered writes to storage — call periodically to prevent data loss
void loggerFlush();

#endif // DATA_LOGGER_H
