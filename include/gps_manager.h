#ifndef GPS_MANAGER_H
#define GPS_MANAGER_H

#include <SparkFun_u-blox_GNSS_v3.h>

// Max satellites to track for C/N0 analysis
static const int MAX_SAT_CNO = 40;

// Snapshot of GPS state for display and detection engine
struct GpsData {
    int32_t  latDeg7;        // latitude in degrees * 1e-7
    int32_t  lonDeg7;        // longitude in degrees * 1e-7
    int32_t  altMM;          // altitude in mm above MSL
    uint8_t  fixType;        // 0=none, 2=2D, 3=3D
    uint8_t  numSV;          // satellites used in solution
    uint16_t pDOP;           // position DOP * 100
    uint32_t hAccMM;         // horizontal accuracy estimate in mm
    uint32_t vAccMM;         // vertical accuracy estimate in mm

    // Integrity fields from MON-HW and NAV-STATUS
    uint8_t  jamInd;         // CW jamming indicator 0-255
    uint8_t  jammingState;   // 0=unknown, 1=OK, 2=warning, 3=critical
    uint8_t  spoofDetState;  // 0=unknown, 1=clean, 2=indicated, 3=multiple
    uint8_t  agcPercent;     // AGC level as percentage 0-100 (approx from agcCnt)

    // Per-satellite C/N0 for high-elevation sats (filtered in gpsUpdate)
    float    satCno[MAX_SAT_CNO];
    int      satCnoCount;

    bool     valid;          // true if PVT data has been read at least once
};

// Initialize GPS on Serial1: configure UBX mode, bump baud to 38400
bool gpsInit();

// Drain UART receive buffer — call every loop iteration, no rate limiting
void gpsProcess();

// Poll NAV-PVT and update the GpsData struct. Call once per loop iteration.
void gpsUpdate(GpsData& data);

// Print GPS status to serial
void gpsPrintStatus(const GpsData& data);

#endif // GPS_MANAGER_H
