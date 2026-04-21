#ifndef ALERT_HANDLER_H
#define ALERT_HANDLER_H

#include "detection_types.h"

// FreeRTOS task — processes detection events, drives LED + buzzer
void alertTask(void* param);

// Called from display task on double-press to acknowledge current alert
void alertAcknowledge();

// Called from display task to toggle mute (3-second hold)
void alertToggleMute();

// Query state for OLED display
bool alertIsMuted();
bool alertIsAcknowledged();
unsigned long alertMuteRemainingMs();

// Issue 8: producers call this when xQueueSend to detectionQueue fails
// (5 ms timeout exceeded, queue genuinely full). alertTask logs the
// accumulated count once per 10 s.
void alertQueueDropInc();

#endif // ALERT_HANDLER_H
