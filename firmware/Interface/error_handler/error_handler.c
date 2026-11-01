/**
 * @file error_handler.c
 * @brief Error registry, fatal handling and watchdog (TG0 MWDT, register level).
 */
#include "error_handler.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/timer_group_struct.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "err";

typedef struct {
    const char *name;
    err_severity_t sev;
} err_info_t;

static const err_info_t s_info[ERR_COUNT] = {
    [ERR_NONE]           = {"none",            ERR_SEV_INFO},
    [ERR_MIC_INIT]       = {"mic_init",        ERR_SEV_FATAL},
    [ERR_MIC_NO_SIGNAL]  = {"mic_no_signal",   ERR_SEV_ERROR},
    [ERR_MIC_TIMEOUT]    = {"mic_timeout",     ERR_SEV_ERROR},
    [ERR_AUDIO_OVERRUN]  = {"audio_overrun",   ERR_SEV_WARNING},
    [ERR_LED_INIT]       = {"led_init",        ERR_SEV_FATAL},
    [ERR_LED_TIMEOUT]    = {"led_timeout",     ERR_SEV_WARNING},
    [ERR_CFG_CORRUPT]    = {"cfg_corrupt",     ERR_SEV_WARNING},
    [ERR_CFG_WRITE]      = {"cfg_write",       ERR_SEV_ERROR},
    [ERR_NET_INIT]       = {"net_init",        ERR_SEV_ERROR},
    [ERR_WIFI_CONNECT]   = {"wifi_connect",    ERR_SEV_WARNING},
    [ERR_FW_UPDATE]      = {"fw_update",       ERR_SEV_ERROR},
    [ERR_RESET_WATCHDOG] = {"reset_watchdog",  ERR_SEV_WARNING},
    [ERR_RESET_PANIC]    = {"reset_panic",     ERR_SEV_WARNING},
    [ERR_RESET_BROWNOUT] = {"reset_brownout",  ERR_SEV_WARNING},
    [ERR_HEAP_LOW]       = {"heap_low",        ERR_SEV_WARNING},
    [ERR_STACK_LOW]      = {"stack_low",       ERR_SEV_WARNING},
    [ERR_PARTITION]      = {"partition",       ERR_SEV_ERROR},
    [ERR_INTERNAL]       = {"internal",        ERR_SEV_ERROR},
};

typedef struct {
    uint16_t count;
    uint32_t last_ms;
    bool     active;
} err_state_t;

static err_state_t s_err[ERR_COUNT];
static portMUX_TYPE s_err_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_fatal_at_ms;
static bool s_fatal_pending;

/* Survives software/watchdog resets (not power cycles). */
#define NOINIT_MAGIC 0x57445447u
typedef struct {
    uint32_t magic;
    char     culprit[16];
} wdt_noinit_t;
static RTC_NOINIT_ATTR wdt_noinit_t s_noinit;
static char s_last_culprit[16];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void err_init(void)
{
    memset(s_err, 0, sizeof(s_err));
    if (s_noinit.magic == NOINIT_MAGIC) {
        memcpy(s_last_culprit, s_noinit.culprit, sizeof(s_last_culprit));
        s_last_culprit[sizeof(s_last_culprit) - 1] = '\0';
    }
    s_noinit.magic = NOINIT_MAGIC;
    memset(s_noinit.culprit, 0, sizeof(s_noinit.culprit));

    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
        err_report(ERR_RESET_PANIC);
        break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        err_report(ERR_RESET_WATCHDOG);
        ESP_LOGW(TAG, "previous reset by watchdog (client '%s')", s_last_culprit);
        break;
    case ESP_RST_BROWNOUT:
        err_report(ERR_RESET_BROWNOUT);
        break;
    default:
        s_last_culprit[0] = '\0';
        break;
    }
}

void err_report(err_code_t code)
{
    if (code <= ERR_NONE || code >= ERR_COUNT) return;
    uint32_t t = now_ms();
    bool first;
    portENTER_CRITICAL(&s_err_lock);
    first = !s_err[code].active;
    if (s_err[code].count < UINT16_MAX) s_err[code].count++;
    s_err[code].last_ms = t;
    s_err[code].active = true;
    if (s_info[code].sev == ERR_SEV_FATAL && !s_fatal_pending) {
        s_fatal_pending = true;
        s_fatal_at_ms = t;
    }
    portEXIT_CRITICAL(&s_err_lock);
    if (first) {
        ESP_LOGW(TAG, "error %d (%s) severity %d", code, s_info[code].name, s_info[code].sev);
    }
}

void err_clear(err_code_t code)
{
    if (code <= ERR_NONE || code >= ERR_COUNT) return;
    portENTER_CRITICAL(&s_err_lock);
    s_err[code].active = false;
    portEXIT_CRITICAL(&s_err_lock);
}

void err_clear_all(void)
{
    portENTER_CRITICAL(&s_err_lock);
    for (int i = 0; i < ERR_COUNT; i++) {
        s_err[i].active = false;
        s_err[i].count = 0;
    }
    portEXIT_CRITICAL(&s_err_lock);
}

err_code_t err_active(void)
{
    err_code_t best = ERR_NONE;
    portENTER_CRITICAL(&s_err_lock);
    for (int i = 1; i < ERR_COUNT; i++) {
        if (!s_err[i].active || s_info[i].sev < ERR_SEV_WARNING) continue;
        if (best == ERR_NONE || s_info[i].sev > s_info[best].sev ||
            (s_info[i].sev == s_info[best].sev && s_err[i].last_ms > s_err[best].last_ms)) {
            best = (err_code_t)i;
        }
    }
    portEXIT_CRITICAL(&s_err_lock);
    return best;
}

err_severity_t err_severity(err_code_t code)
{
    return (code < ERR_COUNT) ? s_info[code].sev : ERR_SEV_ERROR;
}

const char *err_name(err_code_t code)
{
    return (code < ERR_COUNT) ? s_info[code].name : "?";
}

uint8_t err_get_entries(err_entry_t *out, uint8_t max)
{
    uint8_t n = 0;
    portENTER_CRITICAL(&s_err_lock);
    for (int i = 1; i < ERR_COUNT && n < max; i++) {
        if (s_err[i].count == 0) continue;
        out[n].code = (uint8_t)i;
        out[n].severity = (uint8_t)s_info[i].sev;
        out[n].count = s_err[i].count;
        out[n].last_ms = s_err[i].last_ms;
        out[n].name = s_info[i].name;
        n++;
    }
    portEXIT_CRITICAL(&s_err_lock);
    return n;
}

bool err_fatal_pending(void)
{
    return s_fatal_pending;
}

void err_service(void)
{
    uint32_t t = now_ms();
    if (s_fatal_pending && (t - s_fatal_at_ms) > ERR_FATAL_REBOOT_DELAY_MS) {
        ESP_LOGE(TAG, "fatal error -> reboot");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    /* warnings are only displayed for a while; they stay in the log (count) */
    portENTER_CRITICAL(&s_err_lock);
    for (int i = 1; i < ERR_COUNT; i++) {
        if (s_err[i].active && s_info[i].sev == ERR_SEV_WARNING &&
            (t - s_err[i].last_ms) > ERR_WARNING_DISPLAY_MS) {
            s_err[i].active = false;
        }
    }
    portEXIT_CRITICAL(&s_err_lock);
}

/* ========================================================================== */
/*  Watchdog                                                                  */
/* ========================================================================== */

#define WDT_MAX_CLIENTS     6
#define MWDT_WKEY           0x50D83AA1u
#define MWDT_ACTION_RESET_SYSTEM 3u
#define MWDT_TICK_US        100u        /* prescaler: 80 MHz APB / 8000 = 10 kHz */

typedef struct {
    const char *name;
    uint32_t timeout_ms;
    volatile uint32_t last_ms;
    bool enabled;
} wdt_client_info_t;

static wdt_client_info_t s_clients[WDT_MAX_CLIENTS];
static uint8_t s_num_clients;
static bool s_hw_running;

static void mwdt_feed(void)
{
    TIMERG0.wdtwprotect.wdt_wkey = MWDT_WKEY;
    TIMERG0.wdtfeed.wdt_feed = 1;
    TIMERG0.wdtwprotect.wdt_wkey = 0;
}

void wdt_init(uint32_t hw_timeout_ms)
{
    TIMERG0.wdtwprotect.wdt_wkey = MWDT_WKEY;
    TIMERG0.wdtconfig0.wdt_en = 0;
    TIMERG0.wdtconfig0.wdt_flashboot_mod_en = 0;
    TIMERG0.wdtconfig0.wdt_use_xtal = 0;                /* APB 80 MHz */
    HW_REG_SET_FIELD(TIMERG0.wdtconfig1, wdt_clk_prescale, 80u * MWDT_TICK_US);
    TIMERG0.wdtconfig0.wdt_sys_reset_length = 7;
    TIMERG0.wdtconfig0.wdt_cpu_reset_length = 7;
    TIMERG0.wdtconfig0.wdt_stg0 = MWDT_ACTION_RESET_SYSTEM;
    TIMERG0.wdtconfig0.wdt_stg1 = 0;
    TIMERG0.wdtconfig0.wdt_stg2 = 0;
    TIMERG0.wdtconfig0.wdt_stg3 = 0;
    TIMERG0.wdtconfig2.wdt_stg0_hold = hw_timeout_ms * 1000u / MWDT_TICK_US;
    TIMERG0.wdtconfig0.wdt_conf_update_en = 1;
    TIMERG0.wdtfeed.wdt_feed = 1;
    TIMERG0.wdtconfig0.wdt_en = 1;
    TIMERG0.wdtconfig0.wdt_conf_update_en = 1;
    TIMERG0.wdtwprotect.wdt_wkey = 0;
    s_hw_running = true;
}

wdt_client_t wdt_register(const char *name, uint32_t timeout_ms)
{
    if (s_num_clients >= WDT_MAX_CLIENTS) return -1;
    wdt_client_info_t *c = &s_clients[s_num_clients];
    c->name = name;
    c->timeout_ms = timeout_ms;
    c->last_ms = now_ms();
    c->enabled = true;
    return (wdt_client_t)s_num_clients++;
}

void wdt_set_enabled(wdt_client_t id, bool enabled)
{
    if (id < 0 || id >= s_num_clients) return;
    s_clients[id].last_ms = now_ms();
    s_clients[id].enabled = enabled;
}

void wdt_checkin(wdt_client_t id)
{
    if (id < 0 || id >= s_num_clients) return;
    s_clients[id].last_ms = now_ms();
}

void wdt_service(void)
{
    if (!s_hw_running) return;
    uint32_t t = now_ms();
    for (int i = 0; i < s_num_clients; i++) {
        const wdt_client_info_t *c = &s_clients[i];
        if (c->enabled && (t - c->last_ms) > c->timeout_ms) {
            /* starve the hardware watchdog; remember who hung */
            strncpy(s_noinit.culprit, c->name, sizeof(s_noinit.culprit) - 1);
            ESP_LOGE(TAG, "task '%s' unresponsive -> watchdog reset", c->name);
            return;
        }
    }
    mwdt_feed();
}

const char *wdt_last_culprit(void)
{
    return s_last_culprit;
}
