#ifndef BLE_SCANNER_H
#define BLE_SCANNER_H

// Phase M: BLE Remote ID scanner (ASTM F3411 over BLE advertising).
// All code gated behind HAS_BLE_RID — defined in board_config.h for every
// target that has an ESP32-S3 BLE radio (i.e. all three current targets).
// When the flag is removed the function bodies still link as no-ops.
#include "board_config.h"

#ifdef HAS_BLE_RID

#include "detection_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Initialize the NimBLE stack, set passive-scan parameters (50 ms window in
// a 500 ms interval = 10% duty), register the advertisement callback.
// Does NOT start scanning — call bleScannerStart() or let bleScanTask
// drive it based on current OperatingMode.
void bleScannerInit();

// Start/stop the BLE scan. Safe to call repeatedly — internal NimBLE state
// ignores duplicate start/stop. bleScanTask uses these to honor COVERT.
void bleScannerStart();
void bleScannerStop();

// FreeRTOS task: dequeues frames produced by the advertisement callback,
// runs odid_message_process_pack() in task context (callback is forbidden
// from calling Serial.printf), updates systemState.lastRID, emits the
// [BLE-RID] serial line, enqueues a DetectionEvent, and triggers the ZMQ
// emit. Also polls modeGet() at the top of each iteration: stops scanning
// on entry to COVERT, restarts on exit.
void bleScanTask(void* param);

// Exposed so display/main may inspect whether BLE is running (not used
// today but handy for diagnostics screens).
extern TaskHandle_t hBLETask;

#endif // HAS_BLE_RID

#endif // BLE_SCANNER_H
