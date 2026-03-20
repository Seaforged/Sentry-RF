#ifndef DISPLAY_H
#define DISPLAY_H

#include <Adafruit_SSD1306.h>
#include "detection_types.h"

static const int SCREEN_WIDTH  = 128;
static const int SCREEN_HEIGHT = 64;
static const int NUM_SCREENS   = 5;

// RSSI range for bar chart vertical scaling
static const float DISPLAY_RSSI_MIN = -120.0;
static const float DISPLAY_RSSI_MAX = -40.0;

// Initialize OLED hardware (Vext, reset pulse, I2C)
void displayInit(Adafruit_SSD1306& disp);

// Show boot splash with firmware version and board name
void displayBootSplash(Adafruit_SSD1306& disp);

// Screen 0: RF spectrum bar chart
void screenSpectrum(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 1: GPS position and fix info
void screenGPS(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 2: GNSS integrity (jamming/spoofing)
void screenIntegrity(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 3: Threat status and active detections
void screenThreat(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Screen 4: System info (version, uptime, heap)
void screenSystem(Adafruit_SSD1306& disp, const SystemState& state, int page);

// Draw page indicator dots at bottom center
void drawPageDots(Adafruit_SSD1306& disp, int current, int total);

#endif // DISPLAY_H
