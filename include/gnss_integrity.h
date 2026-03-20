#ifndef GNSS_INTEGRITY_H
#define GNSS_INTEGRITY_H

#include "gps_manager.h"

static const int CNO_HISTORY_SIZE = 20;

struct IntegrityStatus {
    bool  jammingDetected;
    bool  spoofingDetected;
    bool  cnoAnomalyDetected;
    float cnoStdDev;
    uint8_t threatLevel;       // 0=clear, 1=advisory, 2=warning, 3=critical
};

// Reset circular buffer and baselines
void integrityInit();

// Run all detection algorithms against latest GPS data
void integrityUpdate(const GpsData& gps, IntegrityStatus& status);

// Print integrity status to serial (includes raw jamInd for diagnostics)
void integrityPrintStatus(const IntegrityStatus& status, const GpsData& gps);

#endif // GNSS_INTEGRITY_H
