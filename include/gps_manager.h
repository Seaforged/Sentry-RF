#ifndef GPS_MANAGER_H
#define GPS_MANAGER_H

#include <SparkFun_u-blox_GNSS_v3.h>

// Snapshot of GPS state for display and detection engine
struct GpsData {
    int32_t  latDeg7;     // latitude in degrees * 1e-7
    int32_t  lonDeg7;     // longitude in degrees * 1e-7
    int32_t  altMM;       // altitude in mm above MSL
    uint8_t  fixType;     // 0=none, 2=2D, 3=3D
    uint8_t  numSV;       // satellites used in solution
    uint16_t pDOP;        // position DOP * 100
    uint32_t hAccMM;     // horizontal accuracy estimate in mm
    uint32_t vAccMM;     // vertical accuracy estimate in mm
    bool     valid;       // true if PVT data has been read at least once
};

// Initialize GPS on Serial1: configure UBX mode, bump baud to 38400
bool gpsInit();

// Poll NAV-PVT and update the GpsData struct. Call once per loop iteration.
void gpsUpdate(GpsData& data);

// Print GPS status to serial
void gpsPrintStatus(const GpsData& data);

#endif // GPS_MANAGER_H
