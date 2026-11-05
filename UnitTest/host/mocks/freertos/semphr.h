/* Host-test stand-in for FreeRTOS semphr.h. */
#ifndef MOCK_FREERTOS_SEMPHR_H
#define MOCK_FREERTOS_SEMPHR_H
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
#define xSemaphoreCreateMutex()      ((SemaphoreHandle_t)1)
static inline int xSemaphoreTake(SemaphoreHandle_t h, TickType_t t) { (void)h; (void)t; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t h) { (void)h; return 1; }
#endif
