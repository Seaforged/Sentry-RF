#ifndef DISPLAY_H
#define DISPLAY_H

#include <Adafruit_SSD1306.h>
#include "detection_types.h"

static const int SCREEN_WIDTH  = 128;
static const int SCREEN_HEIGHT = 64;

#ifdef BOARD_T3S3_LR1121
static const int NUM_SCREENS = 7;  // +dashboard +2.4GHz spectrum
#else
static const int NUM_SCREENS = 6;  // +dashboard
#endif

// RSSI range for bar chart vertical scaling
// LR1121 noise floor is ~-127 dBm (lower than SX1262's ~-110), so -130 ensures
// even quiet channels show a small bar instead of nothing
static const float DISPLAY_RSSI_MIN = -130.0;
static const float DISPLAY_RSSI_MAX = -40.0;

void displayInit(Adafruit_SSD1306& disp);
void displayBootSplash(Adafruit_SSD1306& disp);

// Screen 0: Dashboard summary — all-in-one glance
void screenDashboard(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 1: Sub-GHz spectrum bar chart
void screenSpectrum(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 2: GPS + compass
void screenGPS(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 3: GNSS integrity
void screenIntegrity(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 4: Threat detail
void screenThreat(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 5: System info
void screenSystem(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 6: 2.4 GHz spectrum (LR1121 only)
void screenSpectrum24(Adafruit_SSD1306& disp, const SystemState& state, int page);

void drawPageDots(Adafruit_SSD1306& disp, int current, int total);

#endif // DISPLAY_H
