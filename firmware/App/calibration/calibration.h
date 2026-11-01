/**
 * @file calibration.h
 * @brief Calibration of the audio algorithm parameters and management of the
 *        device / network settings.
 *
 * Parameters: a live table [stage][param] (floats) is initialised from the calib
 * store (missing -> defaults from the stage descriptors). In CALIBRATION mode the
 * host can change values (applied to the running pipeline immediately), save them to
 * flash, reload or reset them.
 *
 * Settings: device name, WiFi, LED strip setup and run configuration, stored in the
 * settings store with the ids of the protocol (Docs/protocol.md, cmd 0x20).
 */
#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "build_config.h"
#include "network.h"

/* calibration result codes (negative = error) */
#define CALIB_OK            0
#define CALIB_ERR_ARG       (-1)
#define CALIB_ERR_MODE      (-2)
#define CALIB_ERR_FLASH     (-3)

/* ---- Algorithm parameters ------------------------------------------------ */

/** Load parameters and settings from flash (defaults for anything missing). */
void calib_init(void);

/** Live parameter array of @p stage (index = param id). Never NULL. */
const float *calib_stage_values(uint8_t stage);

int  calib_get_param(uint8_t stage, uint8_t param, float *value);

/** Set a parameter (CALIBRATION mode only); value is clamped. */
int  calib_set_param(uint8_t stage, uint8_t param, float value, float *applied);

/** Write all live parameters to flash (CALIBRATION mode only). */
int  calib_save(void);

/** Discard unsaved changes. */
int  calib_reload(void);

/** Reset @p stage (0xFF = all) to defaults (not saved). */
int  calib_reset(uint8_t stage);

/** True if live values differ from flash. */
bool calib_dirty(void);

/** Re-read the calib store after it was replaced (calibration region update). */
void calib_reload_from_store(void);

/* ---- Settings ------------------------------------------------------------ */

/* Ids 2 (wifi_mode) and 5-7 (soft-AP) are retired: the device is station-only.
 * Records with these keys in an old settings store are ignored. */
typedef enum {
    SET_DEVICE_NAME = 1,
    SET_STA_SSID = 3,
    SET_STA_PASSWORD = 4,
    SET_MAX_CURRENT_MA = 8,
    SET_NUM_LEDS = 9,
    SET_RUN_MODE = 10,
    SET_ALGORITHM = 11,
    SET_PATTERN = 12,
    SET_BRIGHTNESS = 13,
    SET_COLOR_ORDER = 14,
    SET_ID_MAX
} setting_id_t;

typedef struct {
    char     device_name[32];
    char     sta_ssid[33];
    char     sta_pass[65];
    uint16_t max_current_ma;
    uint16_t num_leds;
    uint8_t  run_mode;
    uint8_t  algorithm;
    uint8_t  pattern;
    uint8_t  brightness;
    uint8_t  color_order;
} settings_t;

const settings_t *settings_get(void);

#ifdef ENABLE_DEBUG_HOST
/**
 * Set one setting from its protocol encoding; validated, persisted and applied live
 * where possible. @p reboot_required is set for network settings.
 */
int settings_set(uint8_t id, const uint8_t *data, uint8_t len, bool *reboot_required);

/** Serialise all settings as TLV (protocol cmd 0x20). Returns length. */
size_t settings_serialize(uint8_t *out, size_t cap);

/** Restore and persist default settings. */
int settings_factory_reset(void);
#endif

/** Change the run configuration (0xFF = unchanged), persist and apply. */
int settings_set_run_config(uint8_t run_mode, uint8_t algorithm, uint8_t pattern, uint8_t brightness);

/** Network configuration derived from the settings. */
void settings_net_config(net_config_t *nc);

#endif /* CALIBRATION_H */
