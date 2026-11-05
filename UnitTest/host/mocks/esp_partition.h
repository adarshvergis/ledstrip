/* Host-test stand-in for esp_partition.h: the flash is RAM owned by the test (test_modules.c). */
#ifndef MOCK_ESP_PARTITION_H
#define MOCK_ESP_PARTITION_H
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)

typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef int esp_partition_subtype_t;

typedef struct {
    esp_partition_type_t    type;
    esp_partition_subtype_t subtype;
    uint32_t                size;
    char                    label[17];
    uint8_t                *mem;        /* mock only */
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len);
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len);
#endif
