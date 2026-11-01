/**
 * @file fw_updater.h
 * @brief Firmware / calibration-region update over the network.
 *
 * Targets
 *  - FWU_TARGET_APP:   application image, streamed into the inactive OTA slot.
 *                      On success the slot is selected for the next boot; the IDF
 *                      bootloader rolls back if the new image never confirms itself.
 *  - FWU_TARGET_CALIB: calibration parameter region image (<= 4 KiB), validated and
 *                      written to the calib store.
 *
 * The transfer is protected by size + CRC-32 supplied at begin.
 */
#ifndef FW_UPDATER_H
#define FW_UPDATER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FWU_TARGET_APP = 0,
    FWU_TARGET_CALIB = 1,
} fwu_target_t;

int  fwu_begin(fwu_target_t target, uint32_t total_size, uint32_t crc32);

/** Write the next chunk; @p offset must equal the number of bytes written so far. */
int  fwu_write(uint32_t offset, const uint8_t *data, uint32_t len);

/** Verify and finalise. */
int  fwu_end(void);

void fwu_abort(void);

bool fwu_in_progress(void);

/** 0..100 */
uint8_t fwu_progress(void);

fwu_target_t fwu_target(void);

/** Bytes received so far. */
uint32_t fwu_received(void);

/** Confirm that the running (new) image works -> cancels bootloader rollback. */
void fwu_confirm_running_image(void);

/** Label of the running app partition (e.g. "ota_0"). */
const char *fwu_running_partition(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_UPDATER_H */
