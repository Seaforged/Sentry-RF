#ifndef VERSION_H
#define VERSION_H

// Phase K complete — GPS_MIN_CNO=15 production threshold, runSelfTest() at
// boot (radio RSSI health, antenna coverage probe, async GPS fix check,
// OLED summary screen), scan-cycle watchdog (log-only), self-test JSONL
// event. Builds on Phase I (bandwidth discrim) and Phase J (full ASTM RID).
static const char* FW_VERSION = "2.0.0";
static const char* FW_NAME    = "SENTRY-RF";
static const char* FW_DATE    = __DATE__;

#endif // VERSION_H
