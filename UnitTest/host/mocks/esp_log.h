/* Host-test stand-in for ESP-IDF esp_log.h: logging is discarded. */
#ifndef MOCK_ESP_LOG_H
#define MOCK_ESP_LOG_H
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#define ESP_LOGV(tag, fmt, ...) ((void)(tag))
#endif
