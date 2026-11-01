/**
 * @file comm.c
 * @brief Protocol command dispatcher (runs in the network server task).
 */
#include "comm.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"
#include "protocol.h"
#include "network.h"
#include "calibration.h"
#include "mode_manager.h"
#include "pipeline.h"
#include "light_control.h"
#include "led_patterns.h"
#include "config_manager.h"
#include "fw_updater.h"
#include "error_handler.h"
#include "self_diag.h"
#include "cli.h"

#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "comm";

#define FW_IDLE_TIMEOUT_US  (30LL * 1000000LL)

static esp_timer_handle_t s_reboot_timer;
static volatile bool s_reboot_after_send;
static int64_t s_fw_last_activity_us;

/* ---- little-endian helpers ---------------------------------------------- */

typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t len;
} wbuf_t;

static void w_bytes(wbuf_t *w, const void *d, uint16_t n)
{
    if (w->len + n > w->cap) { w->len = w->cap; return; }
    memcpy(w->buf + w->len, d, n);
    w->len += n;
}
static void w_u8(wbuf_t *w, uint8_t v) { w_bytes(w, &v, 1); }
static void w_u16(wbuf_t *w, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; w_bytes(w, b, 2); }
static void w_u32(wbuf_t *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    w_bytes(w, b, 4);
}
static void w_f32(wbuf_t *w, float f) { uint32_t v; memcpy(&v, &f, 4); w_u32(w, v); }
static void w_str(wbuf_t *w, const char *s) { w_bytes(w, s, (uint16_t)strlen(s) + 1u); }
static void w_fixstr(wbuf_t *w, const char *s, uint16_t size)
{
    uint8_t tmp[32] = {0};
    size_t n = strlen(s);
    if (n > size - 1u) n = size - 1u;
    memcpy(tmp, s, n);
    w_bytes(w, tmp, size);
}

static uint32_t r_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static float r_f32(const uint8_t *p) { uint32_t v = r_u32(p); float f; memcpy(&f, &v, 4); return f; }

static uint8_t calib_status(int r)
{
    switch (r) {
    case CALIB_OK:        return ST_OK;
    case CALIB_ERR_MODE:  return ST_ERR_WRONG_MODE;
    case CALIB_ERR_FLASH: return ST_ERR_FLASH;
    default:              return ST_ERR_BAD_ARG;
    }
}

static uint8_t hw_status(int r)
{
    switch (r) {
    case HW_OK:          return ST_OK;
    case HW_ERR_ARG:     return ST_ERR_BAD_ARG;
    case HW_ERR_CRC:     return ST_ERR_CRC;
    case HW_ERR_IO:      return ST_ERR_FLASH;
    case HW_ERR_BUSY:    return ST_ERR_BUSY;
    case HW_ERR_STATE:   return ST_ERR_WRONG_MODE;
    default:             return ST_ERR_INTERNAL;
    }
}

/* ---- command implementations --------------------------------------------- */

static uint8_t cmd_get_info(wbuf_t *w)
{
    const settings_t *s = settings_get();
    uint32_t flags = 0;
#ifdef INTERNAL_TEST_BUILD
    flags |= BUILD_FLAG_INTERNAL_TEST;
#endif
#ifdef ENABLE_DEBUG_CLI
    flags |= BUILD_FLAG_DEBUG_CLI;
#endif
#ifdef ENABLE_SELF_DIAGNOSTICS
    flags |= BUILD_FLAG_SELF_DIAG;
#endif
    w_u32(w, PROTOCOL_VERSION);
    w_u32(w, flags);
    w_u8(w, (uint8_t)mode_get());
    w_u8(w, (uint8_t)mode_run_mode());
    w_u8(w, pipeline_active());
    w_u8(w, NUM_ALGORITHMS);
    w_u8(w, mode_pattern());
    w_u8(w, led_pattern_count());
    w_u8(w, light_brightness());
    w_u8(w, (uint8_t)err_active());
    w_u32(w, (uint32_t)(esp_timer_get_time() / 1000));
    w_u32(w, SAMPLE_RATE);
    w_u16(w, light_num_leds());
    w_u16(w, AUDIO_HOP_SIZE);
    w_u8(w, (uint8_t)esp_reset_reason());
    w_u8(w, (uint8_t)net_state());
    w_u8(w, (uint8_t)net_rssi());
    w_u8(w, calib_dirty() ? 1 : 0);
    w_fixstr(w, FW_VERSION_STRING, 24);
    w_fixstr(w, FW_BUILD_DATE_STRING, 24);
    w_fixstr(w, s->device_name, 32);
    w_fixstr(w, net_ip_str(), 16);
    return ST_OK;
}

static uint8_t cmd_get_live(wbuf_t *w)
{
    live_snapshot_t ls;
    mode_get_live(&ls);
    static led_rgb_t px[LED_MAX_COUNT];
    uint16_t n = light_get_output(px, LED_MAX_COUNT);
    w_u16(w, ls.level);
    w_u16(w, ls.bpm_x10);
    w_u16(w, ls.beat_count);
    w_u16(w, ls.centroid);
    w_u8(w, AUDIO_NUM_BANDS);
    w_u8(w, 0);
    w_u16(w, n);
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) w_u16(w, ls.bands[b]);
    w_bytes(w, px, (uint16_t)(n * 3u));
    return ST_OK;
}

static uint8_t cmd_get_algorithms(wbuf_t *w)
{
    w_u8(w, NUM_ALGORITHMS);
    for (uint8_t i = 0; i < NUM_ALGORITHMS; i++) {
        const algorithm_def_t *a = &g_algorithms[i];
        w_u8(w, a->id);
        w_u8(w, a->num_stages);
        for (uint8_t s = 0; s < a->num_stages; s++) w_u8(w, a->stages[s]->id);
        w_str(w, a->name);
        w_str(w, a->desc);
    }
    return ST_OK;
}

static uint8_t cmd_get_stage_schema(const uint8_t *req, uint16_t len, wbuf_t *w)
{
    if (len != 1) return ST_ERR_BAD_LEN;
    const stage_def_t *s = pipeline_stage(req[0]);
    if (!s) return ST_ERR_BAD_ARG;
    w_u8(w, s->id);
    w_u8(w, s->num_params);
    w_str(w, s->name);
    w_str(w, s->desc);
    for (uint8_t p = 0; p < s->num_params; p++) {
        const param_desc_t *d = &s->params[p];
        w_u8(w, p);
        w_u8(w, d->type);
        w_f32(w, d->min);
        w_f32(w, d->max);
        w_f32(w, d->def);
        w_f32(w, d->step);
        w_str(w, d->name);
        w_str(w, d->unit);
    }
    return ST_OK;
}

static uint8_t cmd_get_params(const uint8_t *req, uint16_t len, wbuf_t *w)
{
    if (len != 1) return ST_ERR_BAD_LEN;
    const stage_def_t *s = pipeline_stage(req[0]);
    if (!s) return ST_ERR_BAD_ARG;
    const float *v = calib_stage_values(s->id);
    w_u8(w, s->id);
    w_u8(w, s->num_params);
    for (uint8_t p = 0; p < s->num_params; p++) w_f32(w, v[p]);
    return ST_OK;
}

static uint8_t cmd_fw_begin(const uint8_t *req, uint16_t len, wbuf_t *w)
{
    if (len != 9) return ST_ERR_BAD_LEN;
    uint8_t target = req[0];
    if (target != FWU_TARGET_APP && target != FWU_TARGET_CALIB) return ST_ERR_BAD_ARG;
    mode_enter_fw_update();
    int r = fwu_begin((fwu_target_t)target, r_u32(req + 1), r_u32(req + 5));
    if (r != HW_OK) {
        mode_exit_fw_update();
        return hw_status(r);
    }
    s_fw_last_activity_us = esp_timer_get_time();
    w_u16(w, NET_FW_CHUNK_SIZE);
    return ST_OK;
}

static uint8_t cmd_fw_data(const uint8_t *req, uint16_t len, wbuf_t *w)
{
    if (mode_get() != DEV_MODE_FW_UPDATE || !fwu_in_progress()) return ST_ERR_WRONG_MODE;
    if (len < 5 || len - 4 > NET_FW_CHUNK_SIZE) return ST_ERR_BAD_LEN;
    int r = fwu_write(r_u32(req), req + 4, len - 4u);
    if (r != HW_OK) {
        if (!fwu_in_progress()) {           /* flash failure aborted the update */
            err_report(ERR_FW_UPDATE);
            mode_exit_fw_update();
        }
        return hw_status(r);
    }
    s_fw_last_activity_us = esp_timer_get_time();
    w_u32(w, fwu_received());
    return ST_OK;
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

static void schedule_reboot(uint32_t ms)
{
    esp_timer_stop(s_reboot_timer);
    esp_timer_start_once(s_reboot_timer, (uint64_t)ms * 1000u);
}

static uint8_t cmd_fw_end(void)
{
    if (mode_get() != DEV_MODE_FW_UPDATE || !fwu_in_progress()) return ST_ERR_WRONG_MODE;
    fwu_target_t target = fwu_target();
    int r = fwu_end();
    if (r != HW_OK) {
        err_report(ERR_FW_UPDATE);
        mode_exit_fw_update();
        return hw_status(r);
    }
    if (target == FWU_TARGET_CALIB) {
        calib_reload_from_store();
        mode_exit_fw_update();
    } else {
        ESP_LOGI(TAG, "new firmware installed, rebooting in 5 s unless the host does it sooner");
        schedule_reboot(5000);
    }
    return ST_OK;
}

static uint8_t cmd_get_errors(wbuf_t *w)
{
    err_entry_t e[ERR_COUNT];
    uint8_t n = err_get_entries(e, ERR_COUNT);
    w_u8(w, n);
    for (uint8_t i = 0; i < n; i++) {
        w_u8(w, e[i].code);
        w_u8(w, e[i].severity);
        w_u16(w, e[i].count);
        w_u32(w, e[i].last_ms);
        w_str(w, e[i].name);
    }
    return ST_OK;
}

static uint8_t handle(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *resp, uint16_t cap, uint16_t *resp_len)
{
    wbuf_t w = {.buf = resp, .cap = cap, .len = 0};
    uint8_t st = ST_OK;

    switch (cmd) {
    case CMD_PING:
        break;

    case CMD_GET_INFO:
        st = cmd_get_info(&w);
        break;

    case CMD_REBOOT:
        s_reboot_after_send = true;
        break;

    case CMD_SET_DEVICE_MODE:
        if (len != 1) { st = ST_ERR_BAD_LEN; break; }
        if (req[0] > DEV_MODE_CALIBRATION) { st = ST_ERR_BAD_ARG; break; }
        st = (mode_set((dev_mode_t)req[0]) == HW_OK) ? ST_OK : ST_ERR_WRONG_MODE;
        break;

    case CMD_SET_RUN_CONFIG:
        if (len != 4) { st = ST_ERR_BAD_LEN; break; }
        st = calib_status(settings_set_run_config(req[0], req[1], req[2], req[3]));
        break;

    case CMD_GET_LIVE:
        st = cmd_get_live(&w);
        break;

    case CMD_GET_ALGORITHMS:
        st = cmd_get_algorithms(&w);
        break;

    case CMD_GET_STAGE_LIST: {
        uint8_t ids[STAGE_COUNT];
        uint8_t n = pipeline_stage_ids(ids, STAGE_COUNT);
        w_u8(&w, n);
        w_bytes(&w, ids, n);
        break;
    }

    case CMD_GET_STAGE_SCHEMA:
        st = cmd_get_stage_schema(req, len, &w);
        break;

    case CMD_GET_PARAMS:
        st = cmd_get_params(req, len, &w);
        break;

    case CMD_SET_PARAM: {
        if (len != 6) { st = ST_ERR_BAD_LEN; break; }
        float applied;
        st = calib_status(calib_set_param(req[0], req[1], r_f32(req + 2), &applied));
        if (st == ST_OK) w_f32(&w, applied);
        break;
    }

    case CMD_SAVE_PARAMS:
        st = calib_status(calib_save());
        break;

    case CMD_RESET_PARAMS:
        if (len != 1) { st = ST_ERR_BAD_LEN; break; }
        st = calib_status(calib_reset(req[0]));
        break;

    case CMD_RELOAD_PARAMS:
        st = (mode_get() == DEV_MODE_CALIBRATION) ? calib_status(calib_reload()) : ST_ERR_WRONG_MODE;
        break;

    case CMD_GET_SETTINGS:
        w.len = (uint16_t)settings_serialize(resp, cap);
        break;

    case CMD_SET_SETTING: {
        if (len < 2 || req[1] != len - 2u) { st = ST_ERR_BAD_LEN; break; }
        bool reboot = false;
        st = calib_status(settings_set(req[0], req + 2, req[1], &reboot));
        if (st == ST_OK) w_u8(&w, reboot ? 1 : 0);
        break;
    }

    case CMD_FACTORY_RESET:
        st = calib_status(settings_factory_reset());
        break;

    case CMD_FW_BEGIN:
        st = cmd_fw_begin(req, len, &w);
        break;

    case CMD_FW_DATA:
        st = cmd_fw_data(req, len, &w);
        break;

    case CMD_FW_END:
        st = cmd_fw_end();
        break;

    case CMD_FW_ABORT:
        fwu_abort();
        mode_exit_fw_update();
        break;

    case CMD_GET_CALIB_IMAGE: {
        int n = cfg_get_image(CFG_STORE_CALIB, resp, cap);
        if (n < 0) st = ST_ERR_INTERNAL;
        else w.len = (uint16_t)n;
        break;
    }

    case CMD_CLI_EXEC: {
#ifdef ENABLE_DEBUG_CLI
        char line[256];
        uint16_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, req, n);
        line[n] = '\0';
        uint16_t out_cap = cap < NET_FW_CHUNK_SIZE ? cap : NET_FW_CHUNK_SIZE;
        w.len = (uint16_t)cli_execute(line, (char *)resp, out_cap);
#else
        st = ST_ERR_NOT_SUPPORTED;
#endif
        break;
    }

    case CMD_GET_DIAG:
#ifdef ENABLE_SELF_DIAGNOSTICS
        w.len = (uint16_t)diag_report((char *)resp, cap);
#else
        st = ST_ERR_NOT_SUPPORTED;
#endif
        break;

    case CMD_RESET_DIAG:
#ifdef ENABLE_SELF_DIAGNOSTICS
        diag_reset();
        mode_reset_rt_stats();
        pipeline_reset_timing();
#else
        st = ST_ERR_NOT_SUPPORTED;
#endif
        break;

    case CMD_GET_ERRORS:
        st = cmd_get_errors(&w);
        break;

    case CMD_CLEAR_ERRORS:
        err_clear_all();
        break;

    default:
        st = ST_ERR_UNKNOWN_CMD;
        break;
    }
    *resp_len = (st == ST_OK) ? w.len : 0;
    return st;
}

static void post_send(uint8_t cmd)
{
    if (cmd == CMD_REBOOT && s_reboot_after_send) {
        ESP_LOGI(TAG, "reboot requested by host");
        schedule_reboot(300);
    }
}

void comm_service(void)
{
    if (mode_get() == DEV_MODE_FW_UPDATE && fwu_in_progress() &&
        esp_timer_get_time() - s_fw_last_activity_us > FW_IDLE_TIMEOUT_US) {
        ESP_LOGW(TAG, "firmware update stalled -> abort");
        fwu_abort();
        err_report(ERR_FW_UPDATE);
        mode_exit_fw_update();
    }
}

void comm_init(void)
{
    esp_timer_create_args_t ta = {.callback = reboot_cb, .name = "reboot"};
    esp_timer_create(&ta, &s_reboot_timer);
    cli_commands_register();

    net_config_t nc;
    settings_net_config(&nc);
    if (net_start(&nc, handle, post_send) != HW_OK) {
        ESP_LOGE(TAG, "network start failed - running stand-alone");
    }
}
