/**
 * @file error_handler.h
 * @brief Error registry, fault handling and the watchdog.
 *
 * Errors
 *  - Every error has a fixed code (shown on the strip as a blink count), a name and a
 *    severity. err_report() records occurrences; the most severe active error is
 *    displayed by light control via led_pattern_error().
 *  - FATAL errors schedule a reboot after ERR_FATAL_REBOOT_DELAY_MS.
 *  - The previous reset reason (panic, watchdog, brown-out) is turned into an error
 *    entry at boot so it can be read from the host.
 *
 * Watchdog
 *  - Hardware: Timer Group 0 main watchdog (MWDT0), programmed at register level,
 *    resets the chip if not fed within WDT_HW_TIMEOUT_MS.
 *  - Software: tasks register as clients and must call wdt_checkin() within their
 *    timeout; the supervisor (wdt_service()) feeds the hardware only while every
 *    client is alive, so a hung task causes a reset. The culprit is kept in
 *    RTC no-init memory and reported after the reboot.
 */
#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ERR_SEV_INFO = 0,
    ERR_SEV_WARNING,
    ERR_SEV_ERROR,
    ERR_SEV_FATAL,
} err_severity_t;

/** Error codes. The value is the blink count shown on the strip: keep stable. */
typedef enum {
    ERR_NONE = 0,
    ERR_MIC_INIT = 1,           /**< I2S/DMA setup failed                        */
    ERR_MIC_NO_SIGNAL = 2,      /**< microphone delivers constant data           */
    ERR_MIC_TIMEOUT = 3,        /**< no DMA block received                       */
    ERR_AUDIO_OVERRUN = 4,      /**< processing slower than real time            */
    ERR_LED_INIT = 5,           /**< SPI/DMA setup failed                        */
    ERR_LED_TIMEOUT = 6,        /**< LED frame transfer hung                     */
    ERR_CFG_CORRUPT = 7,        /**< a config store was invalid, defaults used   */
    ERR_CFG_WRITE = 8,          /**< flash write of a config store failed        */
    ERR_NET_INIT = 9,           /**< WiFi / TCP-IP start failed                  */
    ERR_WIFI_CONNECT = 10,      /**< station connect failed, fell back to AP     */
    ERR_FW_UPDATE = 11,         /**< firmware update failed                      */
    ERR_RESET_WATCHDOG = 12,    /**< previous reset was caused by a watchdog     */
    ERR_RESET_PANIC = 13,       /**< previous reset was a crash                  */
    ERR_RESET_BROWNOUT = 14,    /**< previous reset was a brown-out              */
    ERR_HEAP_LOW = 15,
    ERR_STACK_LOW = 16,
    ERR_PARTITION = 17,         /**< partition table does not match build_config */
    ERR_INTERNAL = 18,
    ERR_COUNT
} err_code_t;

typedef struct {
    uint8_t  code;
    uint8_t  severity;
    uint16_t count;
    uint32_t last_ms;
    const char *name;
} err_entry_t;

/** Initialise; converts the previous reset reason into error entries. */
void err_init(void);

/** Record an occurrence of @p code. Safe from any task (not from ISRs). */
void err_report(err_code_t code);

/** Clear one error / all errors. */
void err_clear(err_code_t code);
void err_clear_all(void);

/** Most relevant active error for display (highest severity, then most recent). */
err_code_t err_active(void);

err_severity_t err_severity(err_code_t code);
const char *err_name(err_code_t code);

/** Fill @p out with all errors that occurred; returns the count. */
uint8_t err_get_entries(err_entry_t *out, uint8_t max);

/** True if a fatal error is pending (reboot scheduled). */
bool err_fatal_pending(void);

/** Call periodically: handles fatal reboot timing and warning expiry. */
void err_service(void);

/* ---- Watchdog ------------------------------------------------------------ */

typedef int8_t wdt_client_t;

/** Start the hardware watchdog. */
void wdt_init(uint32_t hw_timeout_ms);

/** Register a monitored task. Returns a client id (or -1). */
wdt_client_t wdt_register(const char *name, uint32_t timeout_ms);

/** Pause / resume monitoring of a client (e.g. during a known long operation). */
void wdt_set_enabled(wdt_client_t id, bool enabled);

/** Signal that the client is alive. */
void wdt_checkin(wdt_client_t id);

/** Supervisor: feed the hardware watchdog if all clients are alive. */
void wdt_service(void);

/** Name of the client that caused the previous watchdog reset ("" if none). */
const char *wdt_last_culprit(void);

#ifdef __cplusplus
}
#endif

#endif /* ERROR_HANDLER_H */
