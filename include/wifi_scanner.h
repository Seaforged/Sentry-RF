#ifndef WIFI_SCANNER_H
#define WIFI_SCANNER_H

#include "detection_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Known drone manufacturer MAC OUI prefixes (first 3 bytes)
struct DroneOUI {
    uint8_t oui[3];
    const char* name;
};

// Initialize WiFi in promiscuous mode for drone frame capture
void wifiScannerInit();

// Stop promiscuous mode — call before switching to dashboard AP mode
void wifiScannerStop();

// Phase H (COVERT): fully tear down the WiFi stack — stop promiscuous
// mode, stop the driver, deinit. After this call the ESP32 emits zero
// WiFi RF energy until wifiScannerInit() is called again.
void wifiScannerDeinit();

// FreeRTOS task: dequeues captured packets, channel-hops, matches MACs.
// In COVERT mode the task deinitializes WiFi and self-suspends via
// vTaskSuspend(NULL). Another task calls vTaskResume(hWiFiTask) when
// mode leaves COVERT and the task reinitializes WiFi.
void wifiScanTask(void* param);

// Task handle — exposed so mode changes in displayTask can resume the
// WiFi task after a COVERT -> non-COVERT transition.
extern TaskHandle_t hWiFiTask;

#endif // WIFI_SCANNER_H
