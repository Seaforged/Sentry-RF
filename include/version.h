#ifndef VERSION_H
#define VERSION_H

// Phase H complete — operational modes (STANDARD/COVERT/HIGH_ALERT)
// verified on LR1121 hardware (mode FSM, WiFi suspend/resume, HIGH_ALERT
// RSSI cadence extension, COVERT output suppression).
static const char* FW_VERSION = "1.9.0";
static const char* FW_NAME    = "SENTRY-RF";
static const char* FW_DATE    = __DATE__;

#endif // VERSION_H
