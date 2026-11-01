/**
 * @file calibration.c
 * @brief Live parameter table, persistence and settings management.
 */
#include "calibration.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"
#include "config_manager.h"
#include "error_handler.h"
#include "led_patterns.h"
#include "pipeline.h"
#include "mode_manager.h"
#include "light_control.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "calib";

static float s_values[STAGE_COUNT][STAGE_MAX_PARAMS];
static bool  s_dirty;
static settings_t s_set;
static SemaphoreHandle_t s_lock;

/* ========================================================================== */
/*  Parameters                                                                */
/* ========================================================================== */

static float clampf(float v, float lo, float hi)
{
    if (v != v) return lo;              /* NaN */
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float sanitize(const param_desc_t *d, float v)
{
    v = clampf(v, d->min, d->max);
    if (d->type != PARAM_FLOAT) {
        v = (float)(int32_t)(v + (v >= 0 ? 0.5f : -0.5f));
    }
    return v;
}

static void load_stage(const stage_def_t *s, bool from_flash)
{
    for (uint8_t p = 0; p < s->num_params; p++) {
        float v = s->params[p].def;
        if (from_flash) {
            float stored;
            if (cfg_param_read(s->id, p, &stored) == HW_OK) v = stored;
        }
        s_values[s->id][p] = sanitize(&s->params[p], v);
    }
}

static void load_all(bool from_flash)
{
    for (uint8_t id = 0; id < STAGE_COUNT; id++) {
        const stage_def_t *s = pipeline_stage(id);
        if (s) {
            load_stage(s, from_flash);
            pipeline_params_changed(id);
        }
    }
    s_dirty = false;
}

const float *calib_stage_values(uint8_t stage)
{
    return s_values[stage < STAGE_COUNT ? stage : 0];
}

int calib_get_param(uint8_t stage, uint8_t param, float *value)
{
    const stage_def_t *s = pipeline_stage(stage);
    if (!s || param >= s->num_params) return CALIB_ERR_ARG;
    *value = s_values[stage][param];
    return CALIB_OK;
}

int calib_set_param(uint8_t stage, uint8_t param, float value, float *applied)
{
    if (mode_get() != DEV_MODE_CALIBRATION) return CALIB_ERR_MODE;
    const stage_def_t *s = pipeline_stage(stage);
    if (!s || param >= s->num_params) return CALIB_ERR_ARG;
    float v = sanitize(&s->params[param], value);
    if (s_values[stage][param] != v) {
        s_values[stage][param] = v;
        s_dirty = true;
        pipeline_params_changed(stage);
    }
    if (applied) *applied = v;
    return CALIB_OK;
}

int calib_save(void)
{
    if (mode_get() != DEV_MODE_CALIBRATION) return CALIB_ERR_MODE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cfg_clear(CFG_STORE_CALIB);         /* drop parameters of stages that no longer exist */
    for (uint8_t id = 0; id < STAGE_COUNT; id++) {
        const stage_def_t *s = pipeline_stage(id);
        if (!s) continue;
        for (uint8_t p = 0; p < s->num_params; p++) {
            cfg_param_write(id, p, s_values[id][p]);
        }
    }
    int r = cfg_commit(CFG_STORE_CALIB);
    xSemaphoreGive(s_lock);
    if (r != HW_OK) {
        err_report(ERR_CFG_WRITE);
        return CALIB_ERR_FLASH;
    }
    s_dirty = false;
    ESP_LOGI(TAG, "parameters saved");
    return CALIB_OK;
}

int calib_reload(void)
{
    cfg_reload(CFG_STORE_CALIB);
    load_all(true);
    return CALIB_OK;
}

int calib_reset(uint8_t stage)
{
    if (mode_get() != DEV_MODE_CALIBRATION) return CALIB_ERR_MODE;
    if (stage == 0xFF) {
        load_all(false);
    } else {
        const stage_def_t *s = pipeline_stage(stage);
        if (!s) return CALIB_ERR_ARG;
        load_stage(s, false);
        pipeline_params_changed(stage);
    }
    s_dirty = true;
    return CALIB_OK;
}

bool calib_dirty(void)
{
    return s_dirty;
}

void calib_reload_from_store(void)
{
    load_all(true);
}

/* ========================================================================== */
/*  Settings                                                                  */
/* ========================================================================== */

static void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strncpy(s->device_name, NET_DEFAULT_DEVICE_NAME, sizeof(s->device_name) - 1);
    s->wifi_mode = NET_WIFI_MODE_AUTO;
    strncpy(s->ap_pass, NET_DEFAULT_AP_PASSWORD, sizeof(s->ap_pass) - 1);
    s->ap_channel = NET_DEFAULT_AP_CHANNEL;
    s->max_current_ma = LED_DEFAULT_MAX_CURRENT_MA;
    s->num_leds = LED_DEFAULT_COUNT;
    s->run_mode = RUN_MODE_AUDIO;
    s->algorithm = DEFAULT_ALGORITHM;
    s->pattern = 0;
    s->brightness = LED_DEFAULT_BRIGHTNESS;
    s->color_order = LED_ORDER_GRB;
}

static void settings_load(void)
{
    settings_t d;
    settings_defaults(&d);
    cfg_store_t st = CFG_STORE_SETTINGS;
    cfg_get_str(st, SET_DEVICE_NAME, s_set.device_name, sizeof(s_set.device_name), d.device_name);
    s_set.wifi_mode = (uint8_t)cfg_get_uint(st, SET_WIFI_MODE, d.wifi_mode);
    cfg_get_str(st, SET_STA_SSID, s_set.sta_ssid, sizeof(s_set.sta_ssid), d.sta_ssid);
    cfg_get_str(st, SET_STA_PASSWORD, s_set.sta_pass, sizeof(s_set.sta_pass), d.sta_pass);
    cfg_get_str(st, SET_AP_SSID, s_set.ap_ssid, sizeof(s_set.ap_ssid), d.ap_ssid);
    cfg_get_str(st, SET_AP_PASSWORD, s_set.ap_pass, sizeof(s_set.ap_pass), d.ap_pass);
    s_set.ap_channel = (uint8_t)cfg_get_uint(st, SET_AP_CHANNEL, d.ap_channel);
    s_set.max_current_ma = (uint16_t)cfg_get_uint(st, SET_MAX_CURRENT_MA, d.max_current_ma);
    s_set.num_leds = (uint16_t)cfg_get_uint(st, SET_NUM_LEDS, d.num_leds);
    s_set.run_mode = (uint8_t)cfg_get_uint(st, SET_RUN_MODE, d.run_mode);
    s_set.algorithm = (uint8_t)cfg_get_uint(st, SET_ALGORITHM, d.algorithm);
    s_set.pattern = (uint8_t)cfg_get_uint(st, SET_PATTERN, d.pattern);
    s_set.brightness = (uint8_t)cfg_get_uint(st, SET_BRIGHTNESS, d.brightness);
    s_set.color_order = (uint8_t)cfg_get_uint(st, SET_COLOR_ORDER, d.color_order);

    /* repair out-of-range values (e.g. after a firmware with fewer algorithms) */
    if (s_set.wifi_mode > NET_WIFI_MODE_STA) s_set.wifi_mode = d.wifi_mode;
    if (s_set.ap_channel < 1 || s_set.ap_channel > 13) s_set.ap_channel = d.ap_channel;
    if (s_set.max_current_ma < 100 || s_set.max_current_ma > 3000) s_set.max_current_ma = d.max_current_ma;
    if (s_set.num_leds < 1 || s_set.num_leds > LED_MAX_COUNT) s_set.num_leds = d.num_leds;
    if (s_set.run_mode > RUN_MODE_PATTERN) s_set.run_mode = d.run_mode;
    if (s_set.algorithm >= NUM_ALGORITHMS) s_set.algorithm = d.algorithm;
    if (s_set.pattern >= led_pattern_count()) s_set.pattern = 0;
    if (s_set.color_order >= LED_ORDER_COUNT) s_set.color_order = d.color_order;
}

const settings_t *settings_get(void)
{
    return &s_set;
}

static void apply_live(void)
{
    light_set_brightness(s_set.brightness);
    light_set_max_current(s_set.max_current_ma);
    light_set_num_leds(s_set.num_leds);
    light_set_color_order(s_set.color_order);
    mode_apply_run_config(s_set.run_mode, s_set.algorithm, s_set.pattern);
}

static uint32_t decode_uint(const uint8_t *d, uint8_t len)
{
    uint32_t v = 0;
    for (int i = len - 1; i >= 0; i--) v = (v << 8) | d[i];
    return v;
}

static int set_str(char *dst, size_t cap, const uint8_t *d, uint8_t len, uint16_t key)
{
    if (len >= cap) return CALIB_ERR_ARG;
    memcpy(dst, d, len);
    dst[len] = '\0';
    return cfg_set_str(CFG_STORE_SETTINGS, key, dst) == HW_OK ? CALIB_OK : CALIB_ERR_ARG;
}

int settings_set(uint8_t id, const uint8_t *data, uint8_t len, bool *reboot_required)
{
    bool reboot = false;
    int r = CALIB_OK;
    uint32_t v = (len >= 1 && len <= 4) ? decode_uint(data, len) : 0;
    bool is_uint = (len >= 1 && len <= 4);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (id) {
    case SET_DEVICE_NAME:
        r = (len >= 1) ? set_str(s_set.device_name, sizeof(s_set.device_name), data, len, id) : CALIB_ERR_ARG;
        reboot = true;
        break;
    case SET_STA_SSID:
        r = set_str(s_set.sta_ssid, sizeof(s_set.sta_ssid), data, len, id);
        reboot = true;
        break;
    case SET_STA_PASSWORD:
        r = set_str(s_set.sta_pass, sizeof(s_set.sta_pass), data, len, id);
        reboot = true;
        break;
    case SET_AP_SSID:
        r = set_str(s_set.ap_ssid, sizeof(s_set.ap_ssid), data, len, id);
        reboot = true;
        break;
    case SET_AP_PASSWORD:
        r = (len == 0 || len >= 8) ? set_str(s_set.ap_pass, sizeof(s_set.ap_pass), data, len, id) : CALIB_ERR_ARG;
        reboot = true;
        break;
    case SET_WIFI_MODE:
        if (!is_uint || v > NET_WIFI_MODE_STA) { r = CALIB_ERR_ARG; break; }
        s_set.wifi_mode = (uint8_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 1);
        reboot = true;
        break;
    case SET_AP_CHANNEL:
        if (!is_uint || v < 1 || v > 13) { r = CALIB_ERR_ARG; break; }
        s_set.ap_channel = (uint8_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 1);
        reboot = true;
        break;
    case SET_MAX_CURRENT_MA:
        if (!is_uint || v < 100 || v > 3000) { r = CALIB_ERR_ARG; break; }
        s_set.max_current_ma = (uint16_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 2);
        break;
    case SET_NUM_LEDS:
        if (!is_uint || v < 1 || v > LED_MAX_COUNT) { r = CALIB_ERR_ARG; break; }
        s_set.num_leds = (uint16_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 2);
        break;
    case SET_RUN_MODE:
        if (!is_uint || v > RUN_MODE_PATTERN) { r = CALIB_ERR_ARG; break; }
        s_set.run_mode = (uint8_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 1);
        break;
    case SET_ALGORITHM:
        if (!is_uint || v >= NUM_ALGORITHMS) { r = CALIB_ERR_ARG; break; }
        s_set.algorithm = (uint8_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 1);
        break;
    case SET_PATTERN:
        if (!is_uint || v >= led_pattern_count()) { r = CALIB_ERR_ARG; break; }
        s_set.pattern = (uint8_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 1);
        break;
    case SET_BRIGHTNESS:
        if (!is_uint || v > 255) { r = CALIB_ERR_ARG; break; }
        s_set.brightness = (uint8_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 1);
        break;
    case SET_COLOR_ORDER:
        if (!is_uint || v >= LED_ORDER_COUNT) { r = CALIB_ERR_ARG; break; }
        s_set.color_order = (uint8_t)v;
        cfg_set_uint(CFG_STORE_SETTINGS, id, v, 1);
        break;
    default:
        r = CALIB_ERR_ARG;
        break;
    }
    if (r == CALIB_OK && cfg_is_dirty(CFG_STORE_SETTINGS) && cfg_commit(CFG_STORE_SETTINGS) != HW_OK) {
        err_report(ERR_CFG_WRITE);
        r = CALIB_ERR_FLASH;
    }
    xSemaphoreGive(s_lock);

    if (r == CALIB_OK) apply_live();
    if (reboot_required) *reboot_required = (r == CALIB_OK) && reboot;
    return r;
}

int settings_set_run_config(uint8_t run_mode, uint8_t algorithm, uint8_t pattern, uint8_t brightness)
{
    if ((run_mode != 0xFF && run_mode > RUN_MODE_PATTERN) || (algorithm != 0xFF && algorithm >= NUM_ALGORITHMS) ||
        (pattern != 0xFF && pattern >= led_pattern_count())) {
        return CALIB_ERR_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (run_mode != 0xFF) { s_set.run_mode = run_mode; cfg_set_uint(CFG_STORE_SETTINGS, SET_RUN_MODE, run_mode, 1); }
    if (algorithm != 0xFF) { s_set.algorithm = algorithm; cfg_set_uint(CFG_STORE_SETTINGS, SET_ALGORITHM, algorithm, 1); }
    if (pattern != 0xFF) { s_set.pattern = pattern; cfg_set_uint(CFG_STORE_SETTINGS, SET_PATTERN, pattern, 1); }
    if (brightness != 0xFF) { s_set.brightness = brightness; cfg_set_uint(CFG_STORE_SETTINGS, SET_BRIGHTNESS, brightness, 1); }
    int r = CALIB_OK;
    if (cfg_is_dirty(CFG_STORE_SETTINGS) && cfg_commit(CFG_STORE_SETTINGS) != HW_OK) {
        err_report(ERR_CFG_WRITE);
        r = CALIB_ERR_FLASH;
    }
    xSemaphoreGive(s_lock);
    apply_live();
    return r;
}

static size_t put_tlv(uint8_t *out, size_t cap, size_t off, uint8_t id, uint8_t type, const void *d, uint8_t len)
{
    if (off + 3u + len > cap) return off;
    out[off] = id;
    out[off + 1] = type;
    out[off + 2] = len;
    memcpy(out + off + 3, d, len);
    return off + 3u + len;
}

static size_t put_str(uint8_t *out, size_t cap, size_t off, uint8_t id, const char *s)
{
    return put_tlv(out, cap, off, id, CFG_TYPE_STR, s, (uint8_t)strlen(s));
}

static size_t put_uint(uint8_t *out, size_t cap, size_t off, uint8_t id, uint32_t v, uint8_t size)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return put_tlv(out, cap, off, id, CFG_TYPE_UINT, b, size);
}

size_t settings_serialize(uint8_t *out, size_t cap)
{
    size_t o = 0;
    o = put_str(out, cap, o, SET_DEVICE_NAME, s_set.device_name);
    o = put_uint(out, cap, o, SET_WIFI_MODE, s_set.wifi_mode, 1);
    o = put_str(out, cap, o, SET_STA_SSID, s_set.sta_ssid);
    o = put_str(out, cap, o, SET_STA_PASSWORD, s_set.sta_pass);
    o = put_str(out, cap, o, SET_AP_SSID, s_set.ap_ssid);
    o = put_str(out, cap, o, SET_AP_PASSWORD, s_set.ap_pass);
    o = put_uint(out, cap, o, SET_AP_CHANNEL, s_set.ap_channel, 1);
    o = put_uint(out, cap, o, SET_MAX_CURRENT_MA, s_set.max_current_ma, 2);
    o = put_uint(out, cap, o, SET_NUM_LEDS, s_set.num_leds, 2);
    o = put_uint(out, cap, o, SET_RUN_MODE, s_set.run_mode, 1);
    o = put_uint(out, cap, o, SET_ALGORITHM, s_set.algorithm, 1);
    o = put_uint(out, cap, o, SET_PATTERN, s_set.pattern, 1);
    o = put_uint(out, cap, o, SET_BRIGHTNESS, s_set.brightness, 1);
    o = put_uint(out, cap, o, SET_COLOR_ORDER, s_set.color_order, 1);
    return o;
}

int settings_factory_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cfg_clear(CFG_STORE_SETTINGS);
    int r = cfg_commit(CFG_STORE_SETTINGS);
    settings_defaults(&s_set);
    xSemaphoreGive(s_lock);
    apply_live();
    if (r != HW_OK) {
        err_report(ERR_CFG_WRITE);
        return CALIB_ERR_FLASH;
    }
    return CALIB_OK;
}

void settings_net_config(net_config_t *nc)
{
    _Static_assert(sizeof(nc->sta_ssid) == sizeof(s_set.sta_ssid) && sizeof(nc->sta_pass) == sizeof(s_set.sta_pass) &&
                   sizeof(nc->ap_ssid) == sizeof(s_set.ap_ssid) && sizeof(nc->ap_pass) == sizeof(s_set.ap_pass) &&
                   sizeof(nc->device_name) == sizeof(s_set.device_name), "settings / net_config size mismatch");
    memset(nc, 0, sizeof(*nc));
    nc->wifi_mode = s_set.wifi_mode;
    memcpy(nc->sta_ssid, s_set.sta_ssid, sizeof(nc->sta_ssid));
    memcpy(nc->sta_pass, s_set.sta_pass, sizeof(nc->sta_pass));
    memcpy(nc->ap_ssid, s_set.ap_ssid, sizeof(nc->ap_ssid));
    memcpy(nc->ap_pass, s_set.ap_pass, sizeof(nc->ap_pass));
    nc->ap_channel = s_set.ap_channel;
    memcpy(nc->device_name, s_set.device_name, sizeof(nc->device_name));
}

/* ========================================================================== */

void calib_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int s = 0; s < CFG_STORE_COUNT; s++) {
        if (!cfg_store_loaded((cfg_store_t)s)) {
            ESP_LOGW(TAG, "store %d empty/corrupt -> defaults", s);
        }
    }
    load_all(true);
    settings_load();
    light_set_brightness(s_set.brightness);
    light_set_max_current(s_set.max_current_ma);
    light_set_num_leds(s_set.num_leds);
    light_set_color_order(s_set.color_order);
}
