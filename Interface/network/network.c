/**
 * @file network.c
 * @brief WiFi management, TCP frame server and UDP discovery responder.
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

static net_config_t     s_cfg;
static net_handler_t    s_handler;
static net_post_send_t  s_post_send;
static volatile net_state_t s_state = NET_STATE_OFF;
static esp_netif_t     *s_netif_sta;
static esp_netif_t     *s_netif_ap;
static char             s_ip[16];
static char             s_mac[18];
static volatile bool    s_client;
static bool             s_sta_wanted;
static bool             s_ap_fallback;
static esp_timer_handle_t s_sta_timer;

/* frame buffers (static: keep the task stack small) */
static uint8_t s_rx[NET_HEADER_SIZE + NET_MAX_PAYLOAD];
static uint8_t s_tx[NET_HEADER_SIZE + NET_MAX_PAYLOAD];

/* ========================================================================== */
/*  WiFi                                                                      */
/* ========================================================================== */

static void update_ip_from(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    }
}

static void build_ap_config(wifi_config_t *wc);

static void start_ap_fallback(void)
{
    if (s_ap_fallback) return;
    s_ap_fallback = true;
    ESP_LOGW(TAG, "station connect timeout -> enabling soft-AP");
    err_report(ERR_WIFI_CONNECT);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap;
    build_ap_config(&ap);                   /* AP config is only accepted once AP is enabled */
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_state = NET_STATE_AP;
    update_ip_from(s_netif_ap);
}

static void sta_timer_cb(void *arg)
{
    if (s_state == NET_STATE_STA) return;
    if (!s_ap_fallback && s_cfg.wifi_mode == NET_WIFI_MODE_AUTO) {
        start_ap_fallback();
    }
    /* keep retrying the station in the background */
    esp_wifi_connect();
    esp_timer_start_once(s_sta_timer, (uint64_t)NET_STA_RETRY_PERIOD_MS * 1000u);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_state == NET_STATE_STA) {
                ESP_LOGW(TAG, "station disconnected");
                s_state = s_ap_fallback ? NET_STATE_AP : NET_STATE_STA_CONNECTING;
                if (s_ap_fallback) update_ip_from(s_netif_ap); else s_ip[0] = '\0';
                esp_timer_stop(s_sta_timer);
                esp_timer_start_once(s_sta_timer, (uint64_t)NET_STA_CONNECT_TIMEOUT_MS * 1000u);
            }
            if (!s_ap_fallback) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_AP_START:
            if (s_state != NET_STATE_STA) {
                update_ip_from(s_netif_ap);
                if (!s_sta_wanted || s_ap_fallback) s_state = NET_STATE_AP;
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state = NET_STATE_STA;
        esp_timer_stop(s_sta_timer);
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

static void build_ap_config(wifi_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (s_cfg.ap_ssid[0]) {
        copy_field(wc->ap.ssid, sizeof(wc->ap.ssid), s_cfg.ap_ssid);
    } else {
        snprintf((char *)wc->ap.ssid, sizeof(wc->ap.ssid), NET_DEFAULT_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    }
    wc->ap.ssid_len = (uint8_t)strlen((char *)wc->ap.ssid);
    wc->ap.channel = (s_cfg.ap_channel >= 1 && s_cfg.ap_channel <= 13) ? s_cfg.ap_channel : NET_DEFAULT_AP_CHANNEL;
    wc->ap.max_connection = NET_AP_MAX_CLIENTS;
    if (strlen(s_cfg.ap_pass) >= 8) {
        copy_field(wc->ap.password, sizeof(wc->ap.password), s_cfg.ap_pass);
        wc->ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc->ap.authmode = WIFI_AUTH_OPEN;
    }
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
    s_netif_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_netif_sta, s_cfg.device_name);

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ic.nvs_enable = 0;
    if (esp_wifi_init(&ic) != ESP_OK) return HW_ERR_STATE;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    esp_timer_create_args_t ta = {.callback = sta_timer_cb, .name = "sta_to"};
    esp_timer_create(&ta, &s_sta_timer);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_sta_wanted = (s_cfg.wifi_mode == NET_WIFI_MODE_STA) || (s_cfg.wifi_mode == NET_WIFI_MODE_AUTO && s_cfg.sta_ssid[0]);

    if (s_sta_wanted) {
        wifi_config_t sta;
        memset(&sta, 0, sizeof(sta));
        copy_field(sta.sta.ssid, sizeof(sta.sta.ssid), s_cfg.sta_ssid);
        copy_field(sta.sta.password, sizeof(sta.sta.password), s_cfg.sta_pass);
        sta.sta.threshold.authmode = s_cfg.sta_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        s_state = NET_STATE_STA_CONNECTING;
        ESP_LOGI(TAG, "connecting to '%s'", s_cfg.sta_ssid);
    } else {
        wifi_config_t ap;
        build_ap_config(&ap);
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap);
        ESP_LOGI(TAG, "soft-AP '%s'", (char *)ap.ap.ssid);
    }
    if (esp_wifi_start() != ESP_OK) return HW_ERR_STATE;
    esp_wifi_set_ps(WIFI_PS_NONE);          /* lowest latency for the host link */

    if (s_sta_wanted) {
        esp_timer_start_once(s_sta_timer, (uint64_t)NET_STA_CONNECT_TIMEOUT_MS * 1000u);
    }
    return HW_OK;
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

    s_tx[0] = (uint8_t)NET_FRAME_MAGIC;
    s_tx[1] = (uint8_t)(NET_FRAME_MAGIC >> 8);
    s_tx[2] = cmd | NET_RESP_FLAG;
    s_tx[3] = status;
    s_tx[4] = (uint8_t)seq;
    s_tx[5] = (uint8_t)(seq >> 8);
    s_tx[6] = (uint8_t)resp_len;
    s_tx[7] = (uint8_t)(resp_len >> 8);
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

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        int maxfd = ls;
        if (c >= 0) {
            FD_SET(c, &rd);
            if (c > maxfd) maxfd = c;
        }
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

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
        if (now >= next_beacon && (s_state == NET_STATE_STA || s_state == NET_STATE_AP)) {
            next_beacon = now + (int64_t)NET_BEACON_PERIOD_MS * 1000;
            int n = build_beacon(line, sizeof(line));
            struct sockaddr_in bc = {.sin_family = AF_INET, .sin_port = htons(NET_UDP_DISCOVERY_PORT),
                                     .sin_addr.s_addr = htonl(INADDR_BROADCAST)};
            sendto(s, line, n, 0, (struct sockaddr *)&bc, sizeof(bc));
        }
    }
}

/* ========================================================================== */

int net_start(const net_config_t *cfg, net_handler_t handler, net_post_send_t post_send)
{
    if (!cfg || !handler) return HW_ERR_ARG;
    s_cfg = *cfg;
    s_handler = handler;
    s_post_send = post_send;
    if (!s_cfg.device_name[0]) {
        strncpy(s_cfg.device_name, NET_DEFAULT_DEVICE_NAME, sizeof(s_cfg.device_name) - 1);
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
