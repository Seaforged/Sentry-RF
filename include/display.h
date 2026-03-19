#ifndef DISPLAY_H
#define DISPLAY_H

#include <Adafruit_SSD1306.h>
#include "rf_scanner.h"

static const int SCREEN_WIDTH  = 128;
static const int SCREEN_HEIGHT = 64;

// RSSI range for bar chart vertical scaling
static const float DISPLAY_RSSI_MIN = -120.0;  // bottom of chart
static const float DISPLAY_RSSI_MAX = -40.0;   // top of chart

// Chart layout: top portion for bars, bottom for text
static const int CHART_HEIGHT   = 48;
static const int CHART_Y_OFFSET = 0;
static const int TEXT_Y_OFFSET  = 50;

// Initialize OLED hardware (Vext, reset pulse, I2C)
void displayInit(Adafruit_SSD1306& disp);

// Show boot splash with firmware version and board name
void displayBootSplash(Adafruit_SSD1306& disp);

// Render spectrum bar chart with peak info text
void displaySpectrum(Adafruit_SSD1306& disp, const ScanResult& result);

#endif // DISPLAY_H
