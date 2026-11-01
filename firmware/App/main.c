/**
 * @file main.c
 * @brief Music Reactive LED Strip - application entry point and supervisor.
 *
 * Start-up order matters:
 *   errors -> DSP tables -> config stores -> LED output -> calibration/settings ->
 *   real-time task (mic + pipeline) -> network -> watchdog.
 * The device is fully functional (audio -> LEDs) even if the network never comes up.
 *
 * Afterwards app_main becomes the supervisor: it feeds the hardware watchdog (only if
 * all monitored tasks are alive), handles fatal-error reboots, health checks and the
 * confirmation of a freshly updated firmware image.
 */
#include <stdio.h>
#include "build_config.h"
#include "hw_common.h"
#include "dsp.h"
#include "config_manager.h"
#include "error_handler.h"
#include "fw_updater.h"
#include "self_diag.h"
#include "calibration.h"
#include "light_control.h"
#include "mode_manager.h"
#include "comm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "main";

/* Verify that the flashed partition table matches the addresses in build_config.h. */
static void check_partitions(void)
{
    static const struct {
        esp_partition_type_t type;
        uint8_t subtype;
        const char *label;
        uint32_t addr;
    } expect[] = {
        {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0", APP_SLOT0_ADDR},
        {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1", APP_SLOT1_ADDR},
        {ESP_PARTITION_TYPE_DATA, CALIB_PARTITION_SUBTYPE, CALIB_PARTITION_LABEL, CALIB_REGION_ADDR},
        {ESP_PARTITION_TYPE_DATA, SETTINGS_PARTITION_SUBTYPE, SETTINGS_PARTITION_LABEL, SETTINGS_REGION_ADDR},
    };
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        const esp_partition_t *p = esp_partition_find_first(expect[i].type, (esp_partition_subtype_t)expect[i].subtype,
                                                            expect[i].label);
        if (!p || p->address != expect[i].addr) {
            ESP_LOGE(TAG, "partition '%s' missing or not at 0x%06lx", expect[i].label, (unsigned long)expect[i].addr);
            err_report(ERR_PARTITION);
        }
    }
}

static void health_check(void)
{
    if (esp_get_free_heap_size() < HEAP_LOW_WARN_BYTES) {
        err_report(ERR_HEAP_LOW);
    }
    static const char *const tasks[] = {"rt", "comm", "discovery"};
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(tasks[i]);
        if (h && uxTaskGetStackHighWaterMark(h) < STACK_LOW_WARN_BYTES) {
            ESP_LOGW(TAG, "task '%s' stack low", tasks[i]);
            err_report(ERR_STACK_LOW);
        }
    }
}

void app_main(void)
{
    printf(APP_BOOT_MSG_BANNER APP_BOOT_MSG_VERSION APP_BOOT_MSG_HW APP_BOOT_MSG_VARIANT);

    err_init();
    dsp_init();
    if (cfg_init() != HW_OK) {
        err_report(ERR_PARTITION);
    }
    check_partitions();

    light_init();
    calib_init();
    diag_init();

    const settings_t *s = settings_get();
    mode_init(s->run_mode, s->algorithm, s->pattern);
    comm_init();

    wdt_init(WDT_HW_TIMEOUT_MS);
    printf(APP_BOOT_MSG_READY);
    ESP_LOGI(TAG, "running from %s, %lu B heap free", fwu_running_partition(),
             (unsigned long)esp_get_free_heap_size());

    bool confirmed = false;
    uint32_t ticks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WDT_SUPERVISOR_PERIOD_MS));
        wdt_service();
        err_service();
        comm_service();

        if (++ticks % (1000 / WDT_SUPERVISOR_PERIOD_MS) == 0) {
            health_check();
        }
        if (!confirmed && esp_timer_get_time() > (int64_t)OTA_CONFIRM_UPTIME_MS * 1000 && !err_fatal_pending()) {
            fwu_confirm_running_image();
            confirmed = true;
        }
    }
}
