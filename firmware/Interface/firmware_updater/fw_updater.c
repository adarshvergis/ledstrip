/**
 * @file fw_updater.c
 * @brief OTA application update and calibration region update.
 */
#include "fw_updater.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"
#include "config_manager.h"

#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "fwu";

static struct {
    bool          active;
    fwu_target_t  target;
    uint32_t      total;
    uint32_t      expected_crc;
    uint32_t      received;
    uint32_t      crc;
    esp_ota_handle_t ota;
    const esp_partition_t *part;
} s_fwu;

static uint8_t s_calib_buf[CFG_SLOT_SIZE];

int fwu_begin(fwu_target_t target, uint32_t total_size, uint32_t crc32)
{
    if (s_fwu.active) {
        fwu_abort();
    }
    memset(&s_fwu, 0, sizeof(s_fwu));
    if (total_size == 0) return HW_ERR_ARG;

    if (target == FWU_TARGET_APP) {
        s_fwu.part = esp_ota_get_next_update_partition(NULL);
        if (!s_fwu.part || total_size > s_fwu.part->size) {
            return HW_ERR_ARG;
        }
        /* sequential writes: sectors are erased on the fly, no long blocking erase */
        if (esp_ota_begin(s_fwu.part, OTA_WITH_SEQUENTIAL_WRITES, &s_fwu.ota) != ESP_OK) {
            return HW_ERR_IO;
        }
        ESP_LOGI(TAG, "app update -> %s (%lu bytes)", s_fwu.part->label, (unsigned long)total_size);
    } else if (target == FWU_TARGET_CALIB) {
        if (total_size > sizeof(s_calib_buf)) return HW_ERR_ARG;
        ESP_LOGI(TAG, "calib region update (%lu bytes)", (unsigned long)total_size);
    } else {
        return HW_ERR_ARG;
    }
    s_fwu.target = target;
    s_fwu.total = total_size;
    s_fwu.expected_crc = crc32;
    s_fwu.active = true;
    return HW_OK;
}

int fwu_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (!s_fwu.active) return HW_ERR_STATE;
    if (offset != s_fwu.received || len == 0 || offset + len > s_fwu.total) return HW_ERR_ARG;

    if (s_fwu.target == FWU_TARGET_APP) {
        if (esp_ota_write(s_fwu.ota, data, len) != ESP_OK) {
            fwu_abort();
            return HW_ERR_IO;
        }
    } else {
        memcpy(s_calib_buf + offset, data, len);
    }
    s_fwu.crc = hw_crc32(s_fwu.crc, data, len);
    s_fwu.received += len;
    return HW_OK;
}

int fwu_end(void)
{
    if (!s_fwu.active) return HW_ERR_STATE;
    int ret = HW_OK;
    if (s_fwu.received != s_fwu.total || s_fwu.crc != s_fwu.expected_crc) {
        ESP_LOGE(TAG, "size/crc mismatch (%lu/%lu, %08lx/%08lx)", (unsigned long)s_fwu.received,
                 (unsigned long)s_fwu.total, (unsigned long)s_fwu.crc, (unsigned long)s_fwu.expected_crc);
        fwu_abort();
        return HW_ERR_CRC;
    }
    if (s_fwu.target == FWU_TARGET_APP) {
        esp_err_t e = esp_ota_end(s_fwu.ota);       /* validates the ESP image */
        if (e == ESP_OK) {
            e = esp_ota_set_boot_partition(s_fwu.part);
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(e));
            ret = (e == ESP_ERR_OTA_VALIDATE_FAILED) ? HW_ERR_CRC : HW_ERR_IO;
        } else {
            ESP_LOGI(TAG, "app update complete, boots from %s next", s_fwu.part->label);
        }
    } else {
        ret = cfg_load_image(CFG_STORE_CALIB, s_calib_buf, s_fwu.total);
        ESP_LOGI(TAG, "calib region update %s", ret == HW_OK ? "complete" : "rejected");
    }
    s_fwu.active = false;
    return ret;
}

void fwu_abort(void)
{
    if (s_fwu.active && s_fwu.target == FWU_TARGET_APP) {
        esp_ota_abort(s_fwu.ota);
    }
    s_fwu.active = false;
}

bool fwu_in_progress(void)
{
    return s_fwu.active;
}

uint8_t fwu_progress(void)
{
    if (s_fwu.total == 0) return 0;
    return (uint8_t)((uint64_t)s_fwu.received * 100u / s_fwu.total);
}

fwu_target_t fwu_target(void)
{
    return s_fwu.target;
}

uint32_t fwu_received(void)
{
    return s_fwu.received;
}

void fwu_confirm_running_image(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

const char *fwu_running_partition(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}
