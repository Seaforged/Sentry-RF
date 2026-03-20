#ifndef ALERT_HANDLER_H
#define ALERT_HANDLER_H

#include "detection_types.h"

// FreeRTOS task — blocks on detectionQueue, prints events to serial.
// Fleshed out in Sprint 6 with classification and response logic.
void alertTask(void* param);

#endif // ALERT_HANDLER_H
