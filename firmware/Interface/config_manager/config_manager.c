/**
 * @file config_manager.c
 * @brief A/B slot key/value store on raw flash partitions.
 */
#include "config_manager.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "cfg";

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t store_id;
    uint32_t sequence;
    uint32_t data_len;
    uint32_t crc;
} cfg_header_t;

_Static_assert(sizeof(cfg_header_t) == CFG_HEADER_SIZE, "header size");

#define DATA_CAP    (CFG_SLOT_SIZE - CFG_HEADER_SIZE)

typedef struct {
    const esp_partition_t *part;
    uint8_t  img[CFG_SLOT_SIZE] __attribute__((aligned(4)));   /* header + records */
    uint8_t  active_slot;       /* slot the current flash data came from */
    bool     loaded;
    bool     dirty;
} cfg_ctx_t;

static cfg_ctx_t s_store[CFG_STORE_COUNT];
static SemaphoreHandle_t s_lock;

static const struct {
    const char *label;
    uint8_t subtype;
} s_part_info[CFG_STORE_COUNT] = {
    {CALIB_PARTITION_LABEL, CALIB_PARTITION_SUBTYPE},
    {SETTINGS_PARTITION_LABEL, SETTINGS_PARTITION_SUBTYPE},
};

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static cfg_header_t *hdr(cfg_ctx_t *c)
{
    return (cfg_header_t *)c->img;
}

static uint8_t *data(cfg_ctx_t *c)
{
    return c->img + CFG_HEADER_SIZE;
}

static void reset_image(cfg_ctx_t *c, cfg_store_t store, uint32_t seq)
{
    memset(c->img, 0xFF, sizeof(c->img));
    cfg_header_t *h = hdr(c);
    h->magic = CFG_MAGIC;
    h->version = CFG_FORMAT_VERSION;
    h->store_id = (uint16_t)store;
    h->sequence = seq;
    h->data_len = 0;
    h->crc = 0;
}

static int check_image(cfg_store_t store, const uint8_t *img, size_t len)
{
    if (len < CFG_HEADER_SIZE) return HW_ERR_ARG;
    cfg_header_t h;
    memcpy(&h, img, sizeof(h));
    if (h.magic != CFG_MAGIC || h.version != CFG_FORMAT_VERSION || h.store_id != store) return HW_ERR_CRC;
    if (h.data_len > DATA_CAP || CFG_HEADER_SIZE + h.data_len > len) return HW_ERR_CRC;
    if (hw_crc32(0, img + CFG_HEADER_SIZE, h.data_len) != h.crc) return HW_ERR_CRC;
    /* walk records to make sure they are well formed */
    uint32_t off = 0;
    while (off < h.data_len) {
        if (off + 4u > h.data_len) return HW_ERR_CRC;
        uint8_t l = img[CFG_HEADER_SIZE + off + 3];
        off += 4u + l;
    }
    return (off == h.data_len) ? HW_OK : HW_ERR_CRC;
}

/* Locate record @p key in the data area. Returns offset or -1. */
static int find_rec(cfg_ctx_t *c, uint16_t key)
{
    uint8_t *d = data(c);
    uint32_t len = hdr(c)->data_len;
    uint32_t off = 0;
    while (off + 4u <= len) {
        uint16_t k = (uint16_t)(d[off] | (d[off + 1] << 8));
        if (k == key) return (int)off;
        off += 4u + d[off + 3];
    }
    return -1;
}

static void remove_rec(cfg_ctx_t *c, int off)
{
    uint8_t *d = data(c);
    uint32_t rec_len = 4u + d[off + 3];
    uint32_t len = hdr(c)->data_len;
    memmove(d + off, d + off + rec_len, len - off - rec_len);
    hdr(c)->data_len = len - rec_len;
}

static int load_store(cfg_store_t store)
{
    cfg_ctx_t *c = &s_store[store];
    c->loaded = false;
    c->dirty = false;
    reset_image(c, store, 0);
    if (!c->part) return HW_ERR_NOT_FOUND;

    static uint8_t tmp[CFG_SLOT_SIZE] __attribute__((aligned(4)));
    int best = -1;
    uint32_t best_seq = 0;
    for (int slot = 0; slot < 2; slot++) {
        if (esp_partition_read(c->part, slot * CFG_SLOT_SIZE, tmp, CFG_SLOT_SIZE) != ESP_OK) continue;
        if (check_image(store, tmp, CFG_SLOT_SIZE) != HW_OK) continue;
        uint32_t seq = ((cfg_header_t *)tmp)->sequence;
        if (best < 0 || (int32_t)(seq - best_seq) > 0) {
            best = slot;
            best_seq = seq;
        }
    }
    if (best < 0) {
        ESP_LOGW(TAG, "store %d: no valid slot, using defaults", store);
        c->active_slot = 1;     /* first commit goes to slot 0 */
        return HW_ERR_NOT_FOUND;
    }
    esp_partition_read(c->part, best * CFG_SLOT_SIZE, c->img, CFG_SLOT_SIZE);
    c->active_slot = (uint8_t)best;
    c->loaded = true;
    ESP_LOGI(TAG, "store %d: slot %d seq %lu, %lu bytes", store, best, (unsigned long)best_seq,
             (unsigned long)hdr(c)->data_len);
    return HW_OK;
}

int cfg_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    int ret = HW_OK;
    for (int s = 0; s < CFG_STORE_COUNT; s++) {
        s_store[s].part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                   (esp_partition_subtype_t)s_part_info[s].subtype,
                                                   s_part_info[s].label);
        if (!s_store[s].part) {
            ESP_LOGE(TAG, "partition '%s' missing", s_part_info[s].label);
            ret = HW_ERR_NOT_FOUND;
        }
        load_store((cfg_store_t)s);
    }
    return ret;
}

bool cfg_store_loaded(cfg_store_t store)
{
    return store < CFG_STORE_COUNT && s_store[store].loaded;
}

int cfg_read(cfg_store_t store, uint16_t key, void *out, uint8_t *len, uint8_t *type)
{
    if (store >= CFG_STORE_COUNT || !out || !len) return HW_ERR_ARG;
    cfg_ctx_t *c = &s_store[store];
    LOCK();
    int off = find_rec(c, key);
    int ret = HW_ERR_NOT_FOUND;
    if (off >= 0) {
        uint8_t *d = data(c);
        uint8_t l = d[off + 3];
        uint8_t n = l < *len ? l : *len;
        memcpy(out, d + off + 4, n);
        *len = l;
        if (type) *type = d[off + 2];
        ret = HW_OK;
    }
    UNLOCK();
    return ret;
}

int cfg_write(cfg_store_t store, uint16_t key, uint8_t type, const void *value, uint8_t len)
{
    if (store >= CFG_STORE_COUNT || (len && !value) || len > CFG_MAX_VALUE_LEN) return HW_ERR_ARG;
    cfg_ctx_t *c = &s_store[store];
    LOCK();
    uint8_t *d = data(c);
    int off = find_rec(c, key);
    int ret = HW_OK;
    if (off >= 0 && d[off + 3] == len && d[off + 2] == type) {
        if (memcmp(d + off + 4, value, len) != 0) {
            memcpy(d + off + 4, value, len);
            c->dirty = true;
        }
    } else {
        if (off >= 0) remove_rec(c, off);
        uint32_t dl = hdr(c)->data_len;
        if (dl + 4u + len > DATA_CAP) {
            ret = HW_ERR_NO_MEM;
        } else {
            d[dl] = (uint8_t)key;
            d[dl + 1] = (uint8_t)(key >> 8);
            d[dl + 2] = type;
            d[dl + 3] = len;
            memcpy(d + dl + 4, value, len);
            hdr(c)->data_len = dl + 4u + len;
            c->dirty = true;
        }
    }
    UNLOCK();
    return ret;
}

int cfg_remove(cfg_store_t store, uint16_t key)
{
    if (store >= CFG_STORE_COUNT) return HW_ERR_ARG;
    cfg_ctx_t *c = &s_store[store];
    LOCK();
    int off = find_rec(c, key);
    if (off >= 0) {
        remove_rec(c, off);
        c->dirty = true;
    }
    UNLOCK();
    return off >= 0 ? HW_OK : HW_ERR_NOT_FOUND;
}

void cfg_clear(cfg_store_t store)
{
    if (store >= CFG_STORE_COUNT) return;
    cfg_ctx_t *c = &s_store[store];
    LOCK();
    hdr(c)->data_len = 0;
    c->dirty = true;
    UNLOCK();
}

static int commit_locked(cfg_store_t store)
{
    cfg_ctx_t *c = &s_store[store];
    if (!c->part) return HW_ERR_NOT_FOUND;
    cfg_header_t *h = hdr(c);
    uint8_t slot = c->active_slot ^ 1u;
    h->magic = CFG_MAGIC;
    h->version = CFG_FORMAT_VERSION;
    h->store_id = (uint16_t)store;
    h->sequence = h->sequence + 1u;
    h->crc = hw_crc32(0, data(c), h->data_len);
    /* pad unused area with 0xFF (erased state) */
    memset(data(c) + h->data_len, 0xFF, DATA_CAP - h->data_len);

    if (esp_partition_erase_range(c->part, slot * CFG_SLOT_SIZE, CFG_SLOT_SIZE) != ESP_OK ||
        esp_partition_write(c->part, slot * CFG_SLOT_SIZE, c->img, CFG_SLOT_SIZE) != ESP_OK) {
        ESP_LOGE(TAG, "store %d: flash write failed", store);
        return HW_ERR_IO;
    }
    c->active_slot = slot;
    c->loaded = true;
    c->dirty = false;
    ESP_LOGI(TAG, "store %d: committed slot %d seq %lu", store, slot, (unsigned long)h->sequence);
    return HW_OK;
}

int cfg_commit(cfg_store_t store)
{
    if (store >= CFG_STORE_COUNT) return HW_ERR_ARG;
    LOCK();
    int ret = commit_locked(store);
    UNLOCK();
    return ret;
}

bool cfg_is_dirty(cfg_store_t store)
{
    return store < CFG_STORE_COUNT && s_store[store].dirty;
}

int cfg_reload(cfg_store_t store)
{
    if (store >= CFG_STORE_COUNT) return HW_ERR_ARG;
    LOCK();
    int ret = load_store(store);
    UNLOCK();
    return ret;
}

int cfg_get_image(cfg_store_t store, uint8_t *out, size_t cap)
{
    if (store >= CFG_STORE_COUNT || !out) return HW_ERR_ARG;
    cfg_ctx_t *c = &s_store[store];
    LOCK();
    cfg_header_t *h = hdr(c);
    h->crc = hw_crc32(0, data(c), h->data_len);     /* make RAM image self-consistent */
    size_t len = CFG_HEADER_SIZE + h->data_len;
    int ret = (len <= cap) ? (int)len : HW_ERR_NO_MEM;
    if (ret > 0) memcpy(out, c->img, len);
    UNLOCK();
    return ret;
}

int cfg_validate_image(cfg_store_t store, const uint8_t *img, size_t len)
{
    if (store >= CFG_STORE_COUNT || !img) return HW_ERR_ARG;
    return check_image(store, img, len);
}

int cfg_load_image(cfg_store_t store, const uint8_t *img, size_t len)
{
    int ret = cfg_validate_image(store, img, len);
    if (ret != HW_OK) return ret;
    cfg_ctx_t *c = &s_store[store];
    LOCK();
    uint32_t seq = hdr(c)->sequence;
    cfg_header_t h;
    memcpy(&h, img, sizeof(h));
    memset(c->img, 0xFF, sizeof(c->img));
    memcpy(c->img, img, CFG_HEADER_SIZE + h.data_len);
    hdr(c)->sequence = seq;             /* commit_locked() increments it */
    ret = commit_locked(store);
    UNLOCK();
    return ret;
}

/* ---- Typed helpers ------------------------------------------------------- */

int cfg_param_read(uint8_t stage, uint8_t param, float *value)
{
    uint8_t len = sizeof(float);
    uint8_t type;
    float v;
    int ret = cfg_read(CFG_STORE_CALIB, CFG_PARAM_KEY(stage, param), &v, &len, &type);
    if (ret == HW_OK && (len != sizeof(float) || type != CFG_TYPE_F32)) ret = HW_ERR_NOT_FOUND;
    if (ret == HW_OK) *value = v;
    return ret;
}

int cfg_param_write(uint8_t stage, uint8_t param, float value)
{
    return cfg_write(CFG_STORE_CALIB, CFG_PARAM_KEY(stage, param), CFG_TYPE_F32, &value, sizeof(float));
}

uint32_t cfg_get_uint(cfg_store_t store, uint16_t key, uint32_t def)
{
    uint8_t buf[4] = {0};
    uint8_t len = sizeof(buf);
    uint8_t type;
    if (cfg_read(store, key, buf, &len, &type) != HW_OK || type != CFG_TYPE_UINT || len == 0 || len > 4) {
        return def;
    }
    uint32_t v = 0;
    for (int i = len - 1; i >= 0; i--) v = (v << 8) | buf[i];
    return v;
}

int cfg_set_uint(cfg_store_t store, uint16_t key, uint32_t value, uint8_t size)
{
    if (size != 1 && size != 2 && size != 4) return HW_ERR_ARG;
    uint8_t buf[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    return cfg_write(store, key, CFG_TYPE_UINT, buf, size);
}

void cfg_get_str(cfg_store_t store, uint16_t key, char *out, size_t cap, const char *def)
{
    if (!out || cap == 0) return;
    uint8_t buf[CFG_MAX_VALUE_LEN];
    uint8_t len = sizeof(buf);
    uint8_t type;
    if (cfg_read(store, key, buf, &len, &type) != HW_OK || type != CFG_TYPE_STR) {
        strncpy(out, def ? def : "", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(out, buf, n);
    out[n] = '\0';
}

int cfg_set_str(cfg_store_t store, uint16_t key, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > CFG_MAX_VALUE_LEN) return HW_ERR_ARG;
    return cfg_write(store, key, CFG_TYPE_STR, s, (uint8_t)n);
}
