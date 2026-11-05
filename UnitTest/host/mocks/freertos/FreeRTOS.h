/* Host-test stand-in for FreeRTOS.h (single threaded: locks are no-ops). */
#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H
#include <stdint.h>
#define portMAX_DELAY 0xFFFFFFFFu
typedef uint32_t TickType_t;
typedef int BaseType_t;
#endif
