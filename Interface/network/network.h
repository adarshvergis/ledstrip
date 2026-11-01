/**
 * @file network.h
 * @brief WiFi (station with soft-AP fallback), TCP command server and UDP discovery.
 *
 * The device works without any network: everything here runs in low priority tasks
 * and never blocks the real-time audio path. Framing is described in Docs/protocol.md;
 * this module only moves frames, the command semantics live in the application
 * (software_communication) which installs a frame handler.
 */
#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_FRAME_MAGIC     0x534Cu     /* bytes 'L','S' */
#define NET_HEADER_SIZE     8u
#define NET_RESP_FLAG       0x80u

typedef enum {
    NET_STATE_OFF = 0,
    NET_STATE_AP = 1,
    NET_STATE_STA = 2,
    NET_STATE_STA_CONNECTING = 3,
} net_state_t;

typedef struct {
    uint8_t wifi_mode;          /**< NET_WIFI_MODE_AUTO / NET_WIFI_MODE_AP / NET_WIFI_MODE_STA */
    char    sta_ssid[33];
    char    sta_pass[65];
    char    ap_ssid[33];        /**< empty -> NET_DEFAULT_AP_SSID_PREFIX + MAC */
    char    ap_pass[65];
    uint8_t ap_channel;
    char    device_name[32];
} net_config_t;

/**
 * Request handler, called in the network task for each received frame.
 * @return status code for the response frame (0 = OK).
 */
typedef uint8_t (*net_handler_t)(uint8_t cmd, const uint8_t *req, uint16_t req_len,
                                 uint8_t *resp, uint16_t resp_cap, uint16_t *resp_len);

/** Optional hook called after a response has been sent (e.g. to reboot). */
typedef void (*net_post_send_t)(uint8_t cmd);

/** Start WiFi and the server/discovery tasks. */
int net_start(const net_config_t *cfg, net_handler_t handler, net_post_send_t post_send);

net_state_t net_state(void);

/** Current IP address as dotted string ("" if none). */
const char *net_ip_str(void);

/** Station RSSI [dBm] (0 if not connected as station). */
int8_t net_rssi(void);

/** MAC address "aa:bb:cc:dd:ee:ff". */
const char *net_mac_str(void);

/** True while a host is connected to the command port. */
bool net_client_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */
