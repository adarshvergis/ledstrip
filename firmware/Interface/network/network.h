/**
 * @file network.h
 * @brief WiFi station, TCP command server, UDP live stream and UDP discovery.
 *
 * The device works without any network: everything here runs in low priority tasks
 * and never blocks the real-time audio path. The device only joins an existing WiFi
 * network (no soft-AP), so the host PC keeps its own connection. Framing is described
 * in Docs/protocol.md; this module only moves frames, the command semantics live in the
 * application (software_communication) which installs a frame handler.
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
    NET_STATE_OFF = 0,                  /**< no SSID configured or WiFi init failed */
    NET_STATE_STA = 2,                  /**< (value 1 was the removed soft-AP state) */
    NET_STATE_STA_CONNECTING = 3,
} net_state_t;

typedef struct {
    char    sta_ssid[33];               /**< empty -> WiFi stays off */
    char    sta_pass[65];
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

/**
 * Live stream payload producer, called in the network task once per stream period.
 * @return payload length written to @p buf.
 */
typedef uint16_t (*net_stream_fill_t)(uint8_t *buf, uint16_t cap);

/** Start WiFi and the server/discovery tasks. */
int net_start(const net_config_t *cfg, net_handler_t handler, net_post_send_t post_send,
              net_stream_fill_t stream_fill);

/**
 * Start (rate_hz > 0) or stop (0) the UDP live stream to @p udp_port on the address of
 * the connected host. Call from the frame handler only (network task context).
 * Datagrams are frames with cmd @p stream_cmd and seq = stream counter. The stream
 * stops on its own when the host sends nothing for NET_LIVE_LEASE_MS or disconnects.
 */
void net_stream_set(uint16_t udp_port, uint8_t rate_hz, uint8_t stream_cmd);

/** True while the UDP live stream is running. */
bool net_stream_active(void);

net_state_t net_state(void);

/** Current IP address as dotted string ("" if none). */
const char *net_ip_str(void);

/** Station RSSI [dBm] (0 if not connected). */
int8_t net_rssi(void);

/** MAC address "aa:bb:cc:dd:ee:ff". */
const char *net_mac_str(void);

/** True while a host is connected to the command port. */
bool net_client_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */
