/* Host-test stand-in for freertos/task.h: the tick count is set by the test. */
#ifndef MOCK_FREERTOS_TASK_H
#define MOCK_FREERTOS_TASK_H
#include "FreeRTOS.h"
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))        /* 1 kHz tick, as CONFIG_FREERTOS_HZ */
extern TickType_t g_mock_ticks;
static inline TickType_t xTaskGetTickCount(void) { return g_mock_ticks; }
#endif
