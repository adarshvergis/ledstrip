/**
 * @file config_manager.h
 * @brief Persistent key/value storage for calibration parameters and settings.
 *
 * Two independent stores, each in its own flash partition:
 *   CFG_STORE_CALIB    - audio algorithm parameters, key = (stage << 8) | param
 *   CFG_STORE_SETTINGS - device / network settings, key = setting id
 *
 * Each partition holds two 4 KiB slots (A/B). A commit writes the inactive slot with
 * sequence+1, so a power loss during a write never destroys the previous data.
 * Values live in a RAM image; reads never touch flash. Format: Docs/protocol.md s.5.
 * All functions are thread safe.
 */
#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_STORE_CALIB = 0,
    CFG_STORE_SETTINGS = 1,
    CFG_STORE_COUNT
} cfg_store_t;

/** Record value types (stored as one byte; meaning is store specific). */
#define CFG_TYPE_F32        0       /**< calib store: IEEE float              */
#define CFG_TYPE_UINT       0       /**< settings store: little-endian uint    */
#define CFG_TYPE_STR        1       /**< settings store: string without NUL    */

#define CFG_MAGIC           0x4746434Cu     /* "LCFG" */
#define CFG_FORMAT_VERSION  1u
#define CFG_HEADER_SIZE     20u
#define CFG_MAX_VALUE_LEN   64u

/** Key of an algorithm parameter in the calib store. */
#define CFG_PARAM_KEY(stage, param)  ((uint16_t)(((stage) << 8) | (param)))

/** Mount both stores (load the newest valid slot of each). Missing data is not an error. */
int cfg_init(void);

/** True if the store was loaded from flash (false = empty / corrupt -> defaults). */
bool cfg_store_loaded(cfg_store_t store);

/**
 * Read a record. @return HW_OK, HW_ERR_NOT_FOUND, or HW_ERR_ARG.
 * @p len in: capacity of @p out, out: actual length. @p type may be NULL.
 */
int cfg_read(cfg_store_t store, uint16_t key, void *out, uint8_t *len, uint8_t *type);

/** Create/replace a record in RAM (not persisted until cfg_commit). */
int cfg_write(cfg_store_t store, uint16_t key, uint8_t type, const void *data, uint8_t len);

/** Remove a record from RAM. */
int cfg_remove(cfg_store_t store, uint16_t key);

/** Remove all records from RAM. */
void cfg_clear(cfg_store_t store);

/** Persist the RAM image of @p store to flash. */
int cfg_commit(cfg_store_t store);

/** True if RAM differs from flash. */
bool cfg_is_dirty(cfg_store_t store);

/** Discard RAM changes (reload from flash). */
int cfg_reload(cfg_store_t store);

/**
 * Copy the current image (header + records) of @p store into @p out.
 * @return image length or negative error.
 */
int cfg_get_image(cfg_store_t store, uint8_t *out, size_t cap);

/** Validate an externally supplied image for @p store (magic, version, store id, CRC). */
int cfg_validate_image(cfg_store_t store, const uint8_t *img, size_t len);

/** Replace the store content with @p img (validated) and commit it. */
int cfg_load_image(cfg_store_t store, const uint8_t *img, size_t len);

/* --- Typed helpers -------------------------------------------------------- */

/** Read an algorithm parameter by stage enum / param enum. */
int cfg_param_read(uint8_t stage, uint8_t param, float *value);

/** Write an algorithm parameter by stage enum / param enum (RAM). */
int cfg_param_write(uint8_t stage, uint8_t param, float value);

/** Read an unsigned setting (1/2/4 bytes). Returns @p def if missing. */
uint32_t cfg_get_uint(cfg_store_t store, uint16_t key, uint32_t def);

int cfg_set_uint(cfg_store_t store, uint16_t key, uint32_t value, uint8_t size);

/** Read a string setting into @p out (always NUL terminated). Returns @p def if missing. */
void cfg_get_str(cfg_store_t store, uint16_t key, char *out, size_t cap, const char *def);

int cfg_set_str(cfg_store_t store, uint16_t key, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MANAGER_H */
