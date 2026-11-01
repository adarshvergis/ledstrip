# Host <-> Device Protocol (v1)

All multi-byte integers are **little-endian**. `f32` is IEEE-754 single precision.
Strings inside payloads are **NUL-terminated** unless a fixed-size field is stated
(fixed fields are NUL-padded).

## 1. Transport

| Item              | Value                                                        |
|-------------------|--------------------------------------------------------------|
| Command channel   | TCP, device listens on port **4210**, one client at a time   |
| Discovery         | UDP port **4211**                                            |
| AP-mode device IP | `192.168.4.1`                                                |

### 1.1 Discovery

* Host broadcasts the ASCII datagram `LEDSTRIP_DISCOVER` to `255.255.255.255:4211`.
* Every device answers (unicast, to the sender's address and port) with one ASCII line:

```
LEDSTRIP proto=1 name=<device_name> ip=<a.b.c.d> port=4210 fw=<fw_version> mac=<aa:bb:cc:dd:ee:ff>
```

* Devices also broadcast the same line, unsolicited, to `255.255.255.255:4211`
  every 3 s. A host may listen on port 4211 to collect these beacons.

## 2. Framing

Every message in both directions uses one frame format:

| Offset | Size | Field   | Notes                                                       |
|--------|------|---------|-------------------------------------------------------------|
| 0      | 2    | magic   | `0x534C` (bytes `'L','S'`)                                  |
| 2      | 1    | cmd     | command id; response = request cmd OR `0x80`                |
| 3      | 1    | status  | request: 0; response: status code (table 2.1)               |
| 4      | 2    | seq     | chosen by host, echoed in response                          |
| 6      | 2    | len     | payload length N, `0 <= N <= 4160`                          |
| 8      | N    | payload |                                                             |

The device processes requests strictly in order and sends exactly one response per
request. If the frame has an invalid magic or `len > 4160`, the device closes the
connection.

### 2.1 Status codes

| Code | Name              | Meaning                                              |
|------|-------------------|------------------------------------------------------|
| 0    | OK                |                                                      |
| 1    | ERR_UNKNOWN_CMD   | command id not recognised                            |
| 2    | ERR_BAD_LEN       | payload length wrong for the command                 |
| 3    | ERR_BAD_ARG       | argument out of range / unknown id                   |
| 4    | ERR_WRONG_MODE    | command not allowed in current device mode           |
| 5    | ERR_BUSY          | resource busy (e.g. capture already running)         |
| 6    | ERR_FLASH         | flash erase/write/read failed                        |
| 7    | ERR_CRC           | CRC / image validation failed                        |
| 8    | ERR_NOT_SUPPORTED | feature not compiled into this build                 |
| 9    | ERR_INTERNAL      | anything else                                        |

Error responses may carry an ASCII message as payload (no NUL), or be empty.

## 3. Enumerations

**Device mode** (`u8`): 0 = MAIN, 1 = CALIBRATION, 2 = FW_UPDATE

**Run mode** (`u8`): 0 = AUDIO (microphone + algorithm), 1 = PATTERN (pre-stored patterns, mic disabled)

**Build flags** (`u32` bitfield): bit0 = INTERNAL_TEST_BUILD, bit1 = ENABLE_DEBUG_CLI, bit2 = ENABLE_SELF_DIAGNOSTICS

**WiFi state** (`u8`): 0 = off, 1 = soft-AP, 2 = station connected, 3 = station connecting

**Param type** (`u8`): 0 = FLOAT, 1 = INT (value still sent as f32, integral), 2 = BOOL (0.0 / 1.0),
3 = ENUM (value = index as f32; the param's `unit` string holds the option names separated by `|`)

**Firmware target** (`u8`): 0 = APP (application image, written to inactive OTA slot),
1 = CALIB (calibration-parameter region image)

## 4. Commands

`req` = request payload, `resp` = response payload (when status is OK).

### 0x01 PING
req: empty. resp: empty.

### 0x02 GET_INFO
req: empty. resp (fixed 128 bytes):

| Off | Type     | Field              |
|-----|----------|--------------------|
| 0   | u32      | proto_version (=1) |
| 4   | u32      | build_flags        |
| 8   | u8       | device_mode        |
| 9   | u8       | run_mode           |
| 10  | u8       | active_algorithm   |
| 11  | u8       | num_algorithms     |
| 12  | u8       | active_pattern     |
| 13  | u8       | num_patterns       |
| 14  | u8       | brightness (0-255) |
| 15  | u8       | active_error_code (0 = none) |
| 16  | u32      | uptime_ms          |
| 20  | u32      | sample_rate_hz     |
| 24  | u16      | num_leds           |
| 26  | u16      | hop_size (samples per processing block) |
| 28  | u8       | reset_reason (ESP reset reason code) |
| 29  | u8       | wifi_state         |
| 30  | i8       | rssi_dbm (0 if not station) |
| 31  | u8       | calib_dirty (1 = unsaved parameter changes) |
| 32  | char[24] | fw_version         |
| 56  | char[24] | build_date         |
| 80  | char[32] | device_name        |
| 112 | char[16] | ip_address (dotted string) |

### 0x03 REBOOT
req: empty. resp: empty. Device reboots ~300 ms after responding.

### 0x04 SET_DEVICE_MODE
req: `u8 mode` (0 = MAIN, 1 = CALIBRATION). FW_UPDATE is entered only through FW_BEGIN.
Leaving CALIBRATION discards unsaved parameter changes (reloads from flash).
resp: empty.

### 0x05 SET_RUN_CONFIG
req: `u8 run_mode, u8 algorithm, u8 pattern, u8 brightness`. A field value of `0xFF`
means "unchanged" (for brightness, 255 is therefore not settable here; use SET_SETTING).
Takes effect immediately and is persisted. resp: empty.

### 0x06 GET_LIVE
Snapshot of the processing state for visualisation. req: empty. resp:

| Type              | Field |
|-------------------|-------|
| u16               | level (0..32767, smoothed amplitude after AGC) |
| u16               | bpm_x10 (estimated tempo * 10, 0 = unknown) |
| u16               | beat_count (free-running counter, increments on every detected beat) |
| u16               | centroid (0..32767, spectral centroid normalised to Nyquist) |
| u8                | num_bands (B) |
| u8                | reserved |
| u16               | num_leds (L) |
| u16[B]            | band levels (0..32767) |
| u8[L*3]           | LED colours R,G,B as sent to the strip (after brightness/power limiting) |

### 0x10 GET_ALGORITHMS
req: empty. resp: `u8 count`, then per algorithm:
`u8 id, u8 num_stages, u8 stage_ids[num_stages], name\0, description\0`

### 0x11 GET_STAGE_LIST
req: empty. resp: `u8 count, u8 stage_ids[count]` (every stage known to the firmware).

### 0x12 GET_STAGE_SCHEMA
req: `u8 stage_id`. resp:
`u8 stage_id, u8 num_params, name\0, description\0`, then per param:
`u8 param_id, u8 type, f32 min, f32 max, f32 default, f32 step, name\0, unit\0`

### 0x13 GET_PARAMS
req: `u8 stage_id`. resp: `u8 stage_id, u8 count, f32 values[count]` (current live values,
index = param_id).

### 0x14 SET_PARAM  (CALIBRATION mode only)
req: `u8 stage_id, u8 param_id, f32 value`. Value is clamped to [min,max] and applied live.
resp: `f32 applied_value`.

### 0x15 SAVE_PARAMS  (CALIBRATION mode only)
req: empty. Writes all live parameters to the calibration flash region. resp: empty.

### 0x16 RESET_PARAMS  (CALIBRATION mode only)
req: `u8 stage_id` (`0xFF` = all stages). Restores defaults live (not saved). resp: empty.

### 0x17 RELOAD_PARAMS  (CALIBRATION mode only)
req: empty. Discards unsaved changes (reload from flash). resp: empty.

### 0x20 GET_SETTINGS
req: empty. resp: sequence of TLV entries until end of payload:
`u8 id, u8 type (0 = unsigned int, 1 = string), u8 len, u8 data[len]`
(integers little-endian, `len` = 1, 2 or 4; strings without NUL).

| id | name           | type | notes |
|----|----------------|------|-------|
| 1  | device_name    | str  | <= 31 chars |
| 2  | wifi_mode      | u8   | 0 = auto (STA if SSID set, AP fallback), 1 = AP only, 2 = STA only |
| 3  | sta_ssid       | str  | <= 32 |
| 4  | sta_password   | str  | <= 64 |
| 5  | ap_ssid        | str  | <= 32 (empty = auto `LEDSTRIP-XXXX`) |
| 6  | ap_password    | str  | empty (open) or 8..64 |
| 7  | ap_channel     | u8   | 1..13 |
| 8  | max_current_ma | u16  | LED power limit, 100..3000 |
| 9  | num_leds       | u16  | active LEDs, 1..LED_MAX_COUNT |
| 10 | run_mode       | u8   | see enum |
| 11 | algorithm      | u8   | |
| 12 | pattern        | u8   | |
| 13 | brightness     | u8   | 0..255 |
| 14 | color_order    | u8   | 0 = GRB (WS2812B), 1 = RGB, 2 = BRG, 3 = RBG, 4 = GBR, 5 = BGR |

### 0x21 SET_SETTING
req: `u8 id, u8 len, u8 data[len]` (same encoding as above). Persisted immediately.
resp: `u8 reboot_required` (1 for network settings).

### 0x22 FACTORY_RESET_SETTINGS
req: empty. Restores default settings (network included) and persists. resp: empty.

### 0x30 FW_BEGIN
req: `u8 target, u32 total_size, u32 crc32` (CRC-32/ISO-HDLC, same as Python `zlib.crc32`).
Device enters FW_UPDATE mode (audio stopped, LEDs show progress). resp: `u16 max_chunk` (4096).

### 0x31 FW_DATA  (FW_UPDATE mode)
req: `u32 offset, u8 data[1..max_chunk]`. Offsets must be sequential.
resp: `u32 next_offset`.

### 0x32 FW_END  (FW_UPDATE mode)
req: empty. Device verifies size + CRC, finalises the image (APP: validates the ESP image and
selects it for next boot; CALIB: validates the region header and writes it).
resp: empty. On success for APP the host should send REBOOT. For CALIB the new parameters are
loaded immediately and the device returns to MAIN mode.

### 0x33 FW_ABORT
req: empty. Abort update, return to MAIN mode. resp: empty.

### 0x34 GET_CALIB_IMAGE
req: empty. resp: the raw calibration region image currently in effect (<= 4096 bytes; see
section 5). Can be written back later with FW_BEGIN target=CALIB.

### 0x40 CLI_EXEC  (only if ENABLE_DEBUG_CLI)
req: ASCII command line (no NUL). resp: ASCII output (<= 4096 bytes, may be truncated).
Type `help` for the command list.

### 0x50 GET_DIAG  (only if ENABLE_SELF_DIAGNOSTICS)
req: empty. resp: ASCII text, one metric per line: `group\tname\tvalue\tunit\n`.

### 0x51 RESET_DIAG  (only if ENABLE_SELF_DIAGNOSTICS)
req: empty. Resets max/min trackers. resp: empty.

### 0x60 GET_ERRORS
req: empty. resp: `u8 count`, then per entry:
`u8 code, u8 severity (0 info, 1 warning, 2 error, 3 fatal), u16 occurrences, u32 last_uptime_ms, name\0`

### 0x61 CLEAR_ERRORS
req: empty. resp: empty.

## 5. Calibration / settings region image format

Each region (partitions `calib` and `settings`) holds two 4 KiB slots (A/B). The valid slot
with the highest sequence number is active. A slot image is:

| Off | Type | Field |
|-----|------|-------|
| 0   | u32  | magic `0x4746434C` (`"LCFG"`) |
| 4   | u16  | format_version (=1) |
| 6   | u16  | store_id (0 = calib, 1 = settings) |
| 8   | u32  | sequence |
| 12  | u32  | data_len (bytes of records that follow) |
| 16  | u32  | crc32 of the record bytes |
| 20  | ...  | records |

Record: `u16 key, u8 type, u8 len, u8 data[len]`.
For the calib store `key = (stage_id << 8) | param_id`, `type = 0` (f32), `len = 4`.
Settings store keys are the setting ids of section 4 (0x20), types as in the TLV table.
Unknown keys are ignored by the firmware; missing keys fall back to defaults.
When a CALIB image is written via FW_BEGIN/FW_END, the firmware rewrites the sequence number.
