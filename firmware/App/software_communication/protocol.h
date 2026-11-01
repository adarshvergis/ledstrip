/**
 * @file protocol.h
 * @brief Host <-> device protocol constants (see Docs/protocol.md).
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

/* Commands. 0x20-0x22, 0x40, 0x50/0x51 and 0x60/0x61 exist only in debug builds
 * (ENABLE_DEBUG_HOST / ENABLE_DEBUG_CLI / ENABLE_SELF_DIAGNOSTICS); a release build
 * answers them with ST_ERR_UNKNOWN_CMD. */
#define CMD_PING                0x01
#define CMD_GET_INFO            0x02
#define CMD_REBOOT              0x03
#define CMD_SET_DEVICE_MODE     0x04
#define CMD_SET_RUN_CONFIG      0x05
#define CMD_GET_LIVE            0x06
#define CMD_LIVE_SUBSCRIBE      0x07

/* Command id of the UDP live stream datagrams (same payload as the GET_LIVE response) */
#define CMD_LIVE_FRAME          (CMD_GET_LIVE | 0x80)

#define CMD_GET_ALGORITHMS      0x10
#define CMD_GET_STAGE_LIST      0x11
#define CMD_GET_STAGE_SCHEMA    0x12
#define CMD_GET_PARAMS          0x13
#define CMD_SET_PARAM           0x14
#define CMD_SAVE_PARAMS         0x15
#define CMD_RESET_PARAMS        0x16
#define CMD_RELOAD_PARAMS       0x17

#define CMD_GET_SETTINGS        0x20
#define CMD_SET_SETTING         0x21
#define CMD_FACTORY_RESET       0x22

#define CMD_FW_BEGIN            0x30
#define CMD_FW_DATA             0x31
#define CMD_FW_END              0x32
#define CMD_FW_ABORT            0x33
#define CMD_GET_CALIB_IMAGE     0x34

#define CMD_CLI_EXEC            0x40

#define CMD_GET_DIAG            0x50
#define CMD_RESET_DIAG          0x51

#define CMD_GET_ERRORS          0x60
#define CMD_CLEAR_ERRORS        0x61

/* Status codes */
#define ST_OK                   0
#define ST_ERR_UNKNOWN_CMD      1
#define ST_ERR_BAD_LEN          2
#define ST_ERR_BAD_ARG          3
#define ST_ERR_WRONG_MODE       4
#define ST_ERR_BUSY             5
#define ST_ERR_FLASH            6
#define ST_ERR_CRC              7
#define ST_ERR_NOT_SUPPORTED    8
#define ST_ERR_INTERNAL         9

#define INFO_RESP_SIZE          128

#endif /* PROTOCOL_H */
