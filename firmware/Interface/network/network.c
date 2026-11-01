/**
 * @file network.c
 * @brief WiFi station, TCP frame server, UDP live stream and UDP discovery responder.
 */
#include "network.h"

#include <string.h>
#include <stdio.h>
#include "build_config.h"
#include "hw_common.h"
#include "error_handler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "net";

/* live stream datagram: header + payload (fixed fields, bands, LED colours) */
#define NET_STREAM_MAX_PAYLOAD  (64u + 2u * 32u + 3u * LED_MAX_COUNT)

static net_config_t     s_cfg;
static net_handler_t    s_handler;
static net_post_send_t  s_post_send;
static net_stream_fill_t s_stream_fill;
static volatile net_state_t s_state = NET_STATE_OFF;
static esp_netif_t     *s_netif_sta;
static char             s_ip[16];
static char             s_mac[18];
static volatile bool    s_client;
static esp_timer_handle_t s_retry_timer;
static uint32_t         s_retry_ms = NET_STA_RETRY_MIN_MS;

/* frame buffers (static: keep the task stack small) */
static uint8_t s_rx[NET_HEADER_SIZE + NET_MAX_PAYLOAD];
static uint8_t s_tx[NET_HEADER_SIZE + NET_MAX_PAYLOAD];

/* live stream state, owned by the server task */
static struct {
    bool               active;
    uint8_t            cmd;
    uint16_t           seq;
    int64_t            period_us;
    int64_t            next_us;
    struct sockaddr_in dst;
    struct in_addr     peer;            /* address of the connected host */
    int                sock;
    uint8_t            buf[NET_HEADER_SIZE + NET_STREAM_MAX_PAYLOAD];
} s_stream = {.sock = -1};

/* ========================================================================== */
/*  WiFi (station only)                                                       */
/* ========================================================================== */

static void retry_timer_cb(void *arg)
{
    if (s_state != NET_STATE_STA) esp_wifi_connect();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_state == NET_STATE_STA) ESP_LOGW(TAG, "station disconnected");
            s_state = NET_STATE_STA_CONNECTING;
            s_ip[0] = '\0';
            /* back-off 1 s -> 10 s; a persistent failure is shown as a warning */
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000u);
            if (s_retry_ms < NET_STA_RETRY_MAX_MS) {
                s_retry_ms *= 2u;
                if (s_retry_ms >= NET_STA_RETRY_MAX_MS) {
                    s_retry_ms = NET_STA_RETRY_MAX_MS;
                    err_report(ERR_WIFI_CONNECT);
                }
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state = NET_STATE_STA;
        s_retry_ms = NET_STA_RETRY_MIN_MS;
        esp_timer_stop(s_retry_timer);
        err_clear(ERR_WIFI_CONNECT);
        ESP_LOGI(TAG, "station connected, ip %s", s_ip);
    }
}

/* WiFi config strings are fixed-size fields that need not be NUL terminated. */
static void copy_field(uint8_t *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap);
    memcpy(dst, src, n);
}

static int wifi_start(void)
{
    esp_err_t e = nvs_flash_init();     /* PHY calibration data */
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs init: %s", esp_err_to_name(e));

    if (esp_netif_init() != ESP_OK) return HW_ERR_STATE;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return HW_ERR_STATE;

    s_netif_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif_sta, s_cfg.device_name);

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ic.nvs_enable = 0;
    if (esp_wifi_init(&ic) != ESP_OK) return HW_ERR_STATE;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    esp_timer_create_args_t ta = {.callback = retry_timer_cb, .name = "sta_retry"};
    esp_timer_create(&ta, &s_retry_timer);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    copy_field(sta.sta.ssid, sizeof(sta.sta.ssid), s_cfg.sta_ssid);
    copy_field(sta.sta.password, sizeof(sta.sta.password), s_cfg.sta_pass);
    sta.sta.threshold.authmode = s_cfg.sta_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    /* 20 MHz: ~3 dB more link margin than HT40; the host link needs < 20 kB/s */
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_state = NET_STATE_STA_CONNECTING;
    ESP_LOGI(TAG, "connecting to '%s'", s_cfg.sta_ssid);

    if (esp_wifi_start() != ESP_OK) return HW_ERR_STATE;
    esp_wifi_set_ps(WIFI_PS_NONE);          /* lowest latency for the host link */
    return HW_OK;
}

/* ========================================================================== */
/*  UDP live stream                                                           */
/* ========================================================================== */

void net_stream_set(uint16_t udp_port, uint8_t rate_hz, uint8_t stream_cmd)
{
    if (rate_hz == 0 || udp_port == 0) {
        s_stream.active = false;
        return;
    }
    if (rate_hz > NET_LIVE_MAX_HZ) rate_hz = NET_LIVE_MAX_HZ;
    s_stream.cmd = stream_cmd;
    s_stream.period_us = 1000000 / rate_hz;
    s_stream.next_us = esp_timer_get_time();
    s_stream.dst.sin_family = AF_INET;
    s_stream.dst.sin_port = htons(udp_port);
    s_stream.dst.sin_addr = s_stream.peer;
    s_stream.active = true;
}

static void put_header(uint8_t *f, uint8_t cmd, uint8_t status, uint16_t seq, uint16_t len)
{
    f[0] = (uint8_t)NET_FRAME_MAGIC;
    f[1] = (uint8_t)(NET_FRAME_MAGIC >> 8);
    f[2] = cmd;
    f[3] = status;
    f[4] = (uint8_t)seq;
    f[5] = (uint8_t)(seq >> 8);
    f[6] = (uint8_t)len;
    f[7] = (uint8_t)(len >> 8);
}

static void stream_send(void)
{
    if (s_stream.sock < 0) {
        s_stream.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_stream.sock < 0) return;
    }
    uint16_t n = s_stream_fill(s_stream.buf + NET_HEADER_SIZE, NET_STREAM_MAX_PAYLOAD);
    put_header(s_stream.buf, s_stream.cmd, 0, s_stream.seq++, n);
    /* never wait: a frame that does not fit the WiFi queue is dropped, the next one is newer */
    sendto(s_stream.sock, s_stream.buf, NET_HEADER_SIZE + n, MSG_DONTWAIT,
           (struct sockaddr *)&s_stream.dst, sizeof(s_stream.dst));
}

/* Send due stream frames. Returns the time until the next frame [us] (or @p idle_us). */
static int64_t stream_service(int64_t last_rx_us, int64_t idle_us)
{
    if (!s_stream.active) return idle_us;
    int64_t now = esp_timer_get_time();
    if (now - last_rx_us > (int64_t)NET_LIVE_LEASE_MS * 1000) {
        ESP_LOGI(TAG, "live stream lease expired");
        s_stream.active = false;
        return idle_us;
    }
    if (now >= s_stream.next_us) {
        stream_send();
        s_stream.next_us += s_stream.period_us;
        if (s_stream.next_us <= now) s_stream.next_us = now + s_stream.period_us;   /* no burst after a stall */
    }
    return s_stream.next_us - now;
}

/* ========================================================================== */
/*  TCP frame server                                                          */
/* ========================================================================== */

static bool recv_all(int s, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = recv(s, buf + got, len - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

static bool send_all(int s, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return true;
}

/* Receive one request, dispatch, respond. Returns false if the client must be dropped. */
static bool serve_frame(int c)
{
    if (!recv_all(c, s_rx, NET_HEADER_SIZE)) return false;
    uint16_t magic = (uint16_t)(s_rx[0] | (s_rx[1] << 8));
    uint8_t cmd = s_rx[2];
    uint16_t seq = (uint16_t)(s_rx[4] | (s_rx[5] << 8));
    uint16_t len = (uint16_t)(s_rx[6] | (s_rx[7] << 8));
    if (magic != NET_FRAME_MAGIC || len > NET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "bad frame (magic %04x len %u) -> drop client", magic, len);
        return false;
    }
    if (len && !recv_all(c, s_rx + NET_HEADER_SIZE, len)) return false;

    uint16_t resp_len = 0;
    uint8_t status = s_handler(cmd, s_rx + NET_HEADER_SIZE, len, s_tx + NET_HEADER_SIZE, NET_MAX_PAYLOAD, &resp_len);
    if (resp_len > NET_MAX_PAYLOAD) resp_len = NET_MAX_PAYLOAD;

    put_header(s_tx, cmd | NET_RESP_FLAG, status, seq, resp_len);
    bool ok = send_all(c, s_tx, NET_HEADER_SIZE + resp_len);
    if (s_post_send) s_post_send(cmd);
    return ok;
}

static void close_client(int *c)
{
    if (*c >= 0) {
        shutdown(*c, SHUT_RDWR);
        close(*c);
        *c = -1;
    }
    s_client = false;
    s_stream.active = false;
}

static void server_task(void *arg)
{
    wdt_client_t wdt = wdt_register("comm", WDT_COMM_TASK_TIMEOUT_MS);
    int ls = -1, c = -1;
    int64_t last_rx_us = 0;

    for (;;) {
        wdt_checkin(wdt);
        if (ls < 0) {
            ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (ls >= 0) {
                int one = 1;
                setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(NET_TCP_PORT),
                                        .sin_addr.s_addr = htonl(INADDR_ANY)};
                if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
                    close(ls);
                    ls = -1;
                }
            }
            if (ls < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        /* select() wakes up for the next live stream frame or after 1 s */
        int64_t wait_us = stream_service(last_rx_us, 1000000);
        if (wait_us < 0) wait_us = 0;

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        int maxfd = ls;
        if (c >= 0) {
            FD_SET(c, &rd);
            if (c > maxfd) maxfd = c;
        }
        struct timeval tv = {.tv_sec = (time_t)(wait_us / 1000000), .tv_usec = (suseconds_t)(wait_us % 1000000)};
        int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (r == 0) continue;

        if (FD_ISSET(ls, &rd)) {
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            int nc = accept(ls, (struct sockaddr *)&peer, &pl);
            if (nc >= 0) {
                if (c >= 0) {
                    ESP_LOGI(TAG, "new host connection replaces the previous one");
                    close_client(&c);
                }
                c = nc;
                int one = 1;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                struct timeval rto = {.tv_sec = 5, .tv_usec = 0};
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
                setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &rto, sizeof(rto));
                s_client = true;
                s_stream.peer = peer.sin_addr;
                last_rx_us = esp_timer_get_time();
                ESP_LOGI(TAG, "host connected from %s", inet_ntoa(peer.sin_addr));
            }
        }

        if (c >= 0 && FD_ISSET(c, &rd)) {
            if (!serve_frame(c)) {
                ESP_LOGI(TAG, "host disconnected");
                close_client(&c);
            } else {
                last_rx_us = esp_timer_get_time();
            }
        } else if (c >= 0 && (esp_timer_get_time() - last_rx_us) > (int64_t)NET_RX_IDLE_TIMEOUT_S * 1000000) {
            ESP_LOGI(TAG, "host idle -> disconnect");
            close_client(&c);
        }
    }
}

/* ========================================================================== */
/*  UDP discovery                                                             */
/* ========================================================================== */

static int build_beacon(char *out, size_t cap)
{
    return snprintf(out, cap, "LEDSTRIP proto=%d name=%s ip=%s port=%d fw=%s mac=%s", PROTOCOL_VERSION,
                    s_cfg.device_name, s_ip[0] ? s_ip : "0.0.0.0", NET_TCP_PORT, FW_VERSION_STRING, s_mac);
}

static void discovery_task(void *arg)
{
    int s = -1;
    int64_t next_beacon = 0;
    char buf[96];
    char line[160];

    for (;;) {
        if (s < 0) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s >= 0) {
                int one = 1;
                setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
                setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                struct timeval rto = {.tv_sec = 1, .tv_usec = 0};
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
                struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(NET_UDP_DISCOVERY_PORT),
                                        .sin_addr.s_addr = htonl(INADDR_ANY)};
                if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
                    close(s);
                    s = -1;
                }
            }
            if (s < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int r = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (r > 0) {
            buf[r] = '\0';
            if (strncmp(buf, NET_DISCOVERY_REQUEST, strlen(NET_DISCOVERY_REQUEST)) == 0) {
                int n = build_beacon(line, sizeof(line));
                sendto(s, line, n, 0, (struct sockaddr *)&from, fl);
            }
        }

        int64_t now = esp_timer_get_time();
        if (now >= next_beacon && s_state == NET_STATE_STA) {
            next_beacon = now + (int64_t)NET_BEACON_PERIOD_MS * 1000;
            int n = build_beacon(line, sizeof(line));
            struct sockaddr_in bc = {.sin_family = AF_INET, .sin_port = htons(NET_UDP_DISCOVERY_PORT),
                                     .sin_addr.s_addr = htonl(INADDR_BROADCAST)};
            sendto(s, line, n, 0, (struct sockaddr *)&bc, sizeof(bc));
        }
    }
}

/* ========================================================================== */

int net_start(const net_config_t *cfg, net_handler_t handler, net_post_send_t post_send,
              net_stream_fill_t stream_fill)
{
    if (!cfg || !handler || !stream_fill) return HW_ERR_ARG;
    s_cfg = *cfg;
    s_handler = handler;
    s_post_send = post_send;
    s_stream_fill = stream_fill;
    if (!s_cfg.device_name[0]) {
        strncpy(s_cfg.device_name, NET_DEFAULT_DEVICE_NAME, sizeof(s_cfg.device_name) - 1);
    }
    if (!s_cfg.sta_ssid[0]) {
        ESP_LOGW(TAG, "no WiFi credentials stored - network off");
        return HW_ERR_STATE;
    }

    if (wifi_start() != HW_OK) {
        err_report(ERR_NET_INIT);
        return HW_ERR_STATE;
    }
    if (xTaskCreate(server_task, "comm", TASK_STACK_COMM, NULL, TASK_PRIO_COMM, NULL) != pdPASS ||
        xTaskCreate(discovery_task, "discovery", TASK_STACK_DISCOVERY, NULL, TASK_PRIO_DISCOVERY, NULL) != pdPASS) {
        err_report(ERR_NET_INIT);
        return HW_ERR_NO_MEM;
    }
    return HW_OK;
}

net_state_t net_state(void)
{
    return s_state;
}

const char *net_ip_str(void)
{
    return s_ip;
}

int8_t net_rssi(void)
{
    if (s_state != NET_STATE_STA) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

const char *net_mac_str(void)
{
    return s_mac;
}

bool net_client_connected(void)
{
    return s_client;
}
