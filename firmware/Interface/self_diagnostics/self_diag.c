/**
 * @file self_diag.c
 * @brief Self diagnostics report generation.
 */
#include "self_diag.h"

#ifdef ENABLE_SELF_DIAGNOSTICS

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_idf_version.h"

#define DIAG_MAX_PROVIDERS  6
#define DIAG_MAX_TASKS      16

static diag_bench_t *s_benches;
static diag_provider_t s_providers[DIAG_MAX_PROVIDERS];
static uint8_t s_num_providers;
static uint32_t s_min_heap_seen = UINT32_MAX;

/* previous runtime counters for per-task CPU load between two reports */
static struct {
    UBaseType_t num;
    configRUN_TIME_COUNTER_TYPE rt;
} s_prev[DIAG_MAX_TASKS];
static configRUN_TIME_COUNTER_TYPE s_prev_total;

void diag_init(void)
{
    s_min_heap_seen = esp_get_free_heap_size();
}

void diag_bench_register(diag_bench_t *b)
{
    for (diag_bench_t *p = s_benches; p; p = p->next) {
        if (p == b) return;
    }
    b->next = s_benches;
    s_benches = b;
}

void diag_register_provider(diag_provider_t fn)
{
    if (s_num_providers < DIAG_MAX_PROVIDERS) s_providers[s_num_providers++] = fn;
}

void diag_write(diag_writer_t *w, const char *group, const char *name, const char *unit, const char *fmt, ...)
{
    if (w->len + 1 >= w->cap) return;
    int n = snprintf(w->buf + w->len, w->cap - w->len, "%s\t%s\t", group, name);
    if (n < 0) return;
    w->len += (size_t)n;
    if (w->len >= w->cap) { w->len = w->cap - 1; return; }
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    w->len += (size_t)n;
    if (w->len >= w->cap) { w->len = w->cap - 1; return; }
    n = snprintf(w->buf + w->len, w->cap - w->len, "\t%s\n", unit ? unit : "");
    if (n > 0) w->len += (size_t)n;
    if (w->len >= w->cap) w->len = w->cap - 1;
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int-wdt";
    case ESP_RST_TASK_WDT: return "task-wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    case ESP_RST_USB:      return "usb";
    default:               return "other";
    }
}

static void report_system(diag_writer_t *w)
{
    diag_write(w, "system", "uptime", "s", "%llu", (unsigned long long)(esp_timer_get_time() / 1000000));
    diag_write(w, "system", "reset_reason", "", "%s", reset_reason_str(esp_reset_reason()));
    diag_write(w, "system", "fw_version", "", "%s", FW_VERSION_STRING);
    diag_write(w, "system", "idf_version", "", "%s", esp_get_idf_version());
    diag_write(w, "system", "cpu_freq", "MHz", "%u", (unsigned)(HW_CPU_HZ / 1000000u));
}

static void report_memory(diag_writer_t *w)
{
    uint32_t free_now = esp_get_free_heap_size();
    if (free_now < s_min_heap_seen) s_min_heap_seen = free_now;
    diag_write(w, "memory", "heap_free", "B", "%lu", (unsigned long)free_now);
    diag_write(w, "memory", "heap_min_free", "B", "%lu", (unsigned long)esp_get_minimum_free_heap_size());
    diag_write(w, "memory", "heap_largest_block", "B", "%u",
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    diag_write(w, "memory", "dram_total", "B", "%u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
}

static void report_flash(diag_writer_t *w)
{
    uint32_t size = 0;
    esp_flash_get_size(NULL, &size);
    diag_write(w, "flash", "chip_size", "B", "%lu", (unsigned long)size);
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
        diag_write(w, "flash", "running_partition", "", "%s@0x%06lx", run->label, (unsigned long)run->address);
        diag_write(w, "flash", "app_slot_size", "B", "%lu", (unsigned long)run->size);
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        diag_write(w, "flash", "update_partition", "", "%s@0x%06lx", next->label, (unsigned long)next->address);
    }
}

static void report_tasks(diag_writer_t *w)
{
    static TaskStatus_t st[DIAG_MAX_TASKS];
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t n = uxTaskGetSystemState(st, DIAG_MAX_TASKS, &total);
    configRUN_TIME_COUNTER_TYPE dt_total = total - s_prev_total;
    uint32_t idle_pct10 = 0;

    for (UBaseType_t i = 0; i < n; i++) {
        configRUN_TIME_COUNTER_TYPE prev = 0;
        for (int k = 0; k < DIAG_MAX_TASKS; k++) {
            if (s_prev[k].num == st[i].xTaskNumber) { prev = s_prev[k].rt; break; }
        }
        configRUN_TIME_COUNTER_TYPE d = st[i].ulRunTimeCounter - prev;
        uint32_t pct10 = dt_total ? (uint32_t)((d * 1000u) / dt_total) : 0;
        if (strncmp(st[i].pcTaskName, "IDLE", 4) == 0) idle_pct10 += pct10;
        diag_write(w, "tasks", st[i].pcTaskName, "", "prio %u, stack free %u B, cpu %lu.%lu %%",
                   (unsigned)st[i].uxCurrentPriority, (unsigned)st[i].usStackHighWaterMark,
                   (unsigned long)(pct10 / 10), (unsigned long)(pct10 % 10));
    }
    if (dt_total) {
        uint32_t load = idle_pct10 > 1000 ? 0 : 1000 - idle_pct10;
        diag_write(w, "cpu", "load", "%", "%lu.%lu", (unsigned long)(load / 10), (unsigned long)(load % 10));
    }
    /* remember counters for the next delta */
    memset(s_prev, 0, sizeof(s_prev));
    for (UBaseType_t i = 0; i < n && i < DIAG_MAX_TASKS; i++) {
        s_prev[i].num = st[i].xTaskNumber;
        s_prev[i].rt = st[i].ulRunTimeCounter;
    }
    s_prev_total = total;
}

static void report_benches(diag_writer_t *w)
{
    for (diag_bench_t *b = s_benches; b; b = b->next) {
        uint32_t avg = b->count ? (uint32_t)(b->total / b->count) : 0;
        diag_write(w, b->group, b->name, "us", "avg %lu.%02lu  max %lu.%02lu  last %lu.%02lu  n=%lu",
                   (unsigned long)(avg / 160), (unsigned long)((avg % 160) * 100 / 160),
                   (unsigned long)(b->max / 160), (unsigned long)((b->max % 160) * 100 / 160),
                   (unsigned long)(b->last / 160), (unsigned long)((b->last % 160) * 100 / 160),
                   (unsigned long)b->count);
    }
}

size_t diag_report(char *out, size_t cap)
{
    diag_writer_t w = {.buf = out, .cap = cap, .len = 0};
    if (cap == 0) return 0;
    out[0] = '\0';
    report_system(&w);
    report_memory(&w);
    report_flash(&w);
    report_tasks(&w);
    report_benches(&w);
    for (uint8_t i = 0; i < s_num_providers; i++) {
        s_providers[i](&w);
    }
    return w.len;
}

void diag_reset(void)
{
    for (diag_bench_t *b = s_benches; b; b = b->next) {
        b->max = 0;
        b->total = 0;
        b->count = 0;
    }
    s_min_heap_seen = esp_get_free_heap_size();
}

#endif /* ENABLE_SELF_DIAGNOSTICS */
