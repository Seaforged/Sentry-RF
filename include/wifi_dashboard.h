#ifndef WIFI_DASHBOARD_H
#define WIFI_DASHBOARD_H

#include "detection_types.h"

// Start WiFi AP and HTTP server — call once from setup()
void dashboardInit();

// Process pending HTTP requests — call from a single task each iteration
void dashboardHandle();

// Update the dashboard's local copy of system state
void dashboardUpdateState(const SystemState& state);

#endif // WIFI_DASHBOARD_H
