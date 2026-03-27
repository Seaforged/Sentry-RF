#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Core assignments — GPS gets its own core so UART drain never competes with LoRa SPI
static const BaseType_t CORE_GPS   = 0;
static const BaseType_t CORE_LORA  = 1;

// Task priorities — higher number = higher priority
static const UBaseType_t PRIO_LORA_SCAN = 3;
static const UBaseType_t PRIO_GPS_READ  = 3;
static const UBaseType_t PRIO_ALERT     = 2;
static const UBaseType_t PRIO_DISPLAY   = 1;

// Stack sizes in bytes — NAV-SAT packets are large and SparkFun's parser is stack-hungry
static const uint32_t STACK_LORA_SCAN = 8192;
static const uint32_t STACK_GPS_READ  = 8192;
static const uint32_t STACK_ALERT     = 6144;
static const uint32_t STACK_DISPLAY   = 8192;

#endif // TASK_CONFIG_H
