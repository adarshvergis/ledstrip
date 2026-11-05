/**
 * @file test_modules.c
 * @brief Host (PC) tests of the firmware modules that are not DSP: the A/B flash store
 *        (config_manager), the CLI interpreter, LED patterns / indications and the stage +
 *        algorithm registry.
 *
 * config_manager.c is the real firmware code; the ESP-IDF pieces it needs (flash partition,
 * FreeRTOS mutex, logging) are replaced by the stand-ins in UnitTest/host/mocks. The flash is
 * RAM, so power loss during a commit can be simulated.
 */
#include <stdio.h>
#include <string.h>

#include "audio_types.h"
#include "cli.h"
#include "config_manager.h"
#include "hw_common.h"
#include "led_color.h"
#include "led_patterns.h"
#include "pipeline.h"
#include "spectrum_taps.h"
#include "stage_params.h"

#include "esp_partition.h"
#include "freertos/task.h"

#include <math.h>

TickType_t g_mock_ticks;

static int s_fails;
static int s_checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        s_checks++;                                         \
        if (cond) {                                         \
            printf("  ok    ");                             \
        } else {                                            \
            printf("  FAIL  ");                             \
            s_fails++;                                      \
        }                                                   \
        printf(__VA_ARGS__);                                \
        printf("\n");                                       \
    } while (0)

/* ========================================================================== */
/*  RAM flash behind the esp_partition mock                                   */
/* ========================================================================== */

#define PART_SIZE   (2u * CFG_SLOT_SIZE)

static uint8_t s_calib_mem[PART_SIZE];
static uint8_t s_settings_mem[PART_SIZE];
static esp_partition_t s_parts[2] = {
    {ESP_PARTITION_TYPE_DATA, CALIB_PARTITION_SUBTYPE, PART_SIZE, CALIB_PARTITION_LABEL, s_calib_mem},
    {ESP_PARTITION_TYPE_DATA, SETTINGS_PARTITION_SUBTYPE, PART_SIZE, SETTINGS_PARTITION_LABEL, s_settings_mem},
};
static int s_power_loss;        /* 1: the next write is torn (only 24 bytes written, then "power fails") */

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label)
{
    for (int i = 0; i < 2; i++) {
        if (s_parts[i].type == type && s_parts[i].subtype == subtype && strcmp(s_parts[i].label, label) == 0)
            return &s_parts[i];
    }
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    if (off + len > p->size) return ESP_FAIL;
    memcpy(dst, p->mem + off, len);
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len)
{
    if (off + len > p->size) return ESP_FAIL;
    memset(p->mem + off, 0xFF, len);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len)
{
    if (off + len > p->size) return ESP_FAIL;
    if (s_power_loss) {
        memcpy(p->mem + off, src, 24);          /* header + a few data bytes, the rest stays erased */
        s_power_loss = 0;
        return ESP_FAIL;
    }
    memcpy(p->mem + off, src, len);
    return ESP_OK;
}

static void flash_erase_all(void)
{
    memset(s_calib_mem, 0xFF, sizeof(s_calib_mem));
    memset(s_settings_mem, 0xFF, sizeof(s_settings_mem));
}

static uint32_t slot_seq(const uint8_t *mem, int slot)
{
    uint32_t seq;
    memcpy(&seq, mem + slot * CFG_SLOT_SIZE + 8, 4);    /* header: magic, version, store, sequence */
    return seq;
}

/* ========================================================================== */
/*  config_manager                                                            */
/* ========================================================================== */

static void test_config_store(void)
{
    printf("config store (A/B slots)\n");
    flash_erase_all();
    CHECK(cfg_init() == HW_OK, "init with an erased flash");
    CHECK(!cfg_store_loaded(CFG_STORE_CALIB) && !cfg_store_loaded(CFG_STORE_SETTINGS), "erased flash: nothing loaded");
    CHECK(cfg_get_uint(CFG_STORE_SETTINGS, 5, 77) == 77, "missing uint falls back to the default");
    char s[32];
    cfg_get_str(CFG_STORE_SETTINGS, 6, s, sizeof(s), "dflt");
    CHECK(strcmp(s, "dflt") == 0, "missing string falls back to the default");

    CHECK(cfg_set_uint(CFG_STORE_SETTINGS, 5, 0x1234, 2) == HW_OK, "write uint16");
    CHECK(cfg_set_str(CFG_STORE_SETTINGS, 6, "HomeWiFi") == HW_OK, "write string");
    CHECK(cfg_param_write(STAGE_AGC, 2, 0.25f) == HW_OK, "write float parameter to the calib store");
    CHECK(cfg_is_dirty(CFG_STORE_SETTINGS) && cfg_is_dirty(CFG_STORE_CALIB), "dirty after writes");
    CHECK(cfg_commit(CFG_STORE_SETTINGS) == HW_OK && cfg_commit(CFG_STORE_CALIB) == HW_OK, "commit both stores");
    CHECK(!cfg_is_dirty(CFG_STORE_SETTINGS), "clean after commit");
    CHECK(slot_seq(s_settings_mem, 0) == 1, "first commit goes to slot A with sequence 1");

    CHECK(cfg_init() == HW_OK, "re-init (simulated reboot)");
    CHECK(cfg_store_loaded(CFG_STORE_SETTINGS), "settings loaded after reboot");
    CHECK(cfg_get_uint(CFG_STORE_SETTINGS, 5, 0) == 0x1234, "uint persisted");
    cfg_get_str(CFG_STORE_SETTINGS, 6, s, sizeof(s), "");
    CHECK(strcmp(s, "HomeWiFi") == 0, "string persisted");
    float f = 0;
    CHECK(cfg_param_read(STAGE_AGC, 2, &f) == HW_OK && f == 0.25f, "float parameter persisted");
    CHECK(cfg_param_read(STAGE_AGC, 3, &f) == HW_ERR_NOT_FOUND, "unknown parameter: not found");

    cfg_set_str(CFG_STORE_SETTINGS, 6, "OtherNet");
    cfg_commit(CFG_STORE_SETTINGS);
    CHECK(slot_seq(s_settings_mem, 1) == 2, "second commit alternates to slot B with sequence 2");
    cfg_set_uint(CFG_STORE_SETTINGS, 5, 7, 1);
    cfg_commit(CFG_STORE_SETTINGS);
    CHECK(slot_seq(s_settings_mem, 0) == 3, "third commit goes back to slot A with sequence 3");
    cfg_init();
    CHECK(cfg_get_uint(CFG_STORE_SETTINGS, 5, 0) == 7, "overwriting with a different size replaces the record");

    int len_before = cfg_get_image(CFG_STORE_SETTINGS, (uint8_t[256]){0}, 256);
    cfg_set_uint(CFG_STORE_SETTINGS, 5, 9, 1);
    CHECK(cfg_get_image(CFG_STORE_SETTINGS, (uint8_t[256]){0}, 256) == len_before, "same key and size does not grow the image");

    CHECK(cfg_remove(CFG_STORE_SETTINGS, 5) == HW_OK && cfg_remove(CFG_STORE_SETTINGS, 5) == HW_ERR_NOT_FOUND,
          "remove a record, then it is gone");
    CHECK(cfg_set_uint(CFG_STORE_SETTINGS, 8, 1, 3) == HW_ERR_ARG, "uint size 3 rejected");
    CHECK(cfg_set_str(CFG_STORE_SETTINGS, 8, "0123456789012345678901234567890123456789012345678901234567890123456789") == HW_ERR_ARG,
          "string longer than CFG_MAX_VALUE_LEN rejected");
    cfg_clear(CFG_STORE_SETTINGS);
    CHECK(cfg_get_uint(CFG_STORE_SETTINGS, 6, 99) == 99, "clear removes all records");
    cfg_reload(CFG_STORE_SETTINGS);
    CHECK(cfg_get_uint(CFG_STORE_SETTINGS, 5, 0) == 7, "reload restores the flash contents (discards unsaved changes)");
}

static void test_config_power_loss(void)
{
    printf("config store: power loss and corruption\n");
    flash_erase_all();
    cfg_init();
    cfg_set_uint(CFG_STORE_SETTINGS, 1, 111, 4);
    cfg_commit(CFG_STORE_SETTINGS);                  /* slot A, seq 1 */
    cfg_set_uint(CFG_STORE_SETTINGS, 1, 222, 4);
    cfg_commit(CFG_STORE_SETTINGS);                  /* slot B, seq 2 */

    cfg_set_uint(CFG_STORE_SETTINGS, 1, 333, 4);
    s_power_loss = 1;
    CHECK(cfg_commit(CFG_STORE_SETTINGS) == HW_ERR_IO, "torn write reports an I/O error");
    cfg_init();
    CHECK(cfg_get_uint(CFG_STORE_SETTINGS, 1, 0) == 222, "after power loss the previous value survives");

    cfg_set_uint(CFG_STORE_SETTINGS, 1, 444, 4);
    CHECK(cfg_commit(CFG_STORE_SETTINGS) == HW_OK, "commit works again after the failure");
    cfg_init();
    CHECK(cfg_get_uint(CFG_STORE_SETTINGS, 1, 0) == 444, "new value stored");

    /* corrupt the newest slot: the older one must be used */
    uint32_t sa = slot_seq(s_settings_mem, 0), sb = slot_seq(s_settings_mem, 1);
    int newest = ((int32_t)(sa - sb) > 0) ? 0 : 1;
    s_settings_mem[newest * CFG_SLOT_SIZE + CFG_HEADER_SIZE + 4] ^= 0x55;
    cfg_init();
    CHECK(cfg_store_loaded(CFG_STORE_SETTINGS) && cfg_get_uint(CFG_STORE_SETTINGS, 1, 0) == 222,
          "corrupt newest slot: falls back to the older valid slot (222)");
    memset(s_settings_mem, 0x00, sizeof(s_settings_mem));
    cfg_init();
    CHECK(!cfg_store_loaded(CFG_STORE_SETTINGS) && cfg_get_uint(CFG_STORE_SETTINGS, 1, 5) == 5,
          "both slots corrupt: defaults are used");
}

static void test_config_images(void)
{
    printf("config store: region images\n");
    flash_erase_all();
    cfg_init();
    cfg_param_write(STAGE_ENVELOPE, 1, 12.5f);
    cfg_param_write(STAGE_BEAT, 2, 3.0f);
    uint8_t img[512];
    int n = cfg_get_image(CFG_STORE_CALIB, img, sizeof(img));
    CHECK(n > (int)CFG_HEADER_SIZE, "get_image returns header + records (%d bytes)", n);
    CHECK(cfg_validate_image(CFG_STORE_CALIB, img, (size_t)n) == HW_OK, "image validates for its own store");
    CHECK(cfg_validate_image(CFG_STORE_SETTINGS, img, (size_t)n) != HW_OK, "image of another store is rejected");
    CHECK(cfg_get_image(CFG_STORE_CALIB, img, 10) == HW_ERR_NO_MEM, "too small buffer: error");

    uint8_t bad[512];
    memcpy(bad, img, (size_t)n);
    bad[CFG_HEADER_SIZE + 5] ^= 1;
    CHECK(cfg_validate_image(CFG_STORE_CALIB, bad, (size_t)n) == HW_ERR_CRC, "flipped data bit: CRC error");
    memcpy(bad, img, (size_t)n);
    bad[0] ^= 1;
    CHECK(cfg_validate_image(CFG_STORE_CALIB, bad, (size_t)n) == HW_ERR_CRC, "bad magic rejected");
    CHECK(cfg_validate_image(CFG_STORE_CALIB, img, 5) == HW_ERR_ARG, "image shorter than the header rejected");
    CHECK(cfg_validate_image(CFG_STORE_CALIB, img, (size_t)n - 1) == HW_ERR_CRC, "truncated image rejected");

    flash_erase_all();
    cfg_init();
    CHECK(cfg_load_image(CFG_STORE_CALIB, img, (size_t)n) == HW_OK, "load_image commits a valid image");
    cfg_init();
    float f = 0;
    CHECK(cfg_param_read(STAGE_ENVELOPE, 1, &f) == HW_OK && f == 12.5f, "loaded image survives a reboot");
    CHECK(cfg_load_image(CFG_STORE_CALIB, bad, (size_t)n) != HW_OK, "invalid image is not loaded");
    CHECK(cfg_param_read(STAGE_BEAT, 2, &f) == HW_OK && f == 3.0f, "store untouched after a rejected image");
}

/* ========================================================================== */
/*  CLI interpreter                                                           */
/* ========================================================================== */

static int s_last_argc;
static char s_last_arg1[32];

static int cmd_echo(int argc, char **argv, cli_out_t *o)
{
    s_last_argc = argc;
    snprintf(s_last_arg1, sizeof(s_last_arg1), "%s", argc > 1 ? argv[1] : "");
    cli_printf(o, "echo:%d", argc);
    return 0;
}

static int cmd_fail(int argc, char **argv, cli_out_t *o)
{
    (void)argc; (void)argv; (void)o;
    return 3;
}

static void test_cli(void)
{
    printf("cli interpreter\n");
#ifdef ENABLE_DEBUG_CLI
    static const cli_cmd_t cmds[] = {
        {"echo", "[a] [b]", "print the argument count", cmd_echo},
        {"fail", "", "always fails", cmd_fail},
    };
    cli_register(cmds, 2);
    char out[256];

    size_t n = cli_execute("echo a b", out, sizeof(out));
    CHECK(strcmp(out, "echo:3") == 0 && n == strlen(out), "runs a registered command (%s)", out);
    cli_execute("   echo   \t x  ", out, sizeof(out));
    CHECK(s_last_argc == 2 && strcmp(s_last_arg1, "x") == 0, "extra white space is ignored");
    cli_execute("echo \"two words\" y", out, sizeof(out));
    CHECK(s_last_argc == 3 && strcmp(s_last_arg1, "two words") == 0, "quoted argument stays together");
    cli_execute("echo 1 2 3 4 5 6 7 8 9 10 11 12", out, sizeof(out));
    CHECK(s_last_argc == CLI_MAX_ARGS, "arguments beyond CLI_MAX_ARGS are dropped (%d)", s_last_argc);

    cli_execute("nosuchcmd", out, sizeof(out));
    CHECK(strstr(out, "unknown command 'nosuchcmd'") != NULL, "unknown command is reported");
    cli_execute("fail", out, sizeof(out));
    CHECK(strstr(out, "error 3") != NULL && strstr(out, "usage: fail") != NULL, "non-zero result prints the usage");
    n = cli_execute("help", out, sizeof(out));
    CHECK(strstr(out, "echo") != NULL && strstr(out, "always fails") != NULL && n > 0, "help lists the commands");
    CHECK(cli_execute("", out, sizeof(out)) == 0 && cli_execute("   ", out, sizeof(out)) == 0, "empty line: no output");

    char tiny[16];
    n = cli_execute("help", tiny, sizeof(tiny));
    CHECK(n < sizeof(tiny) && strlen(tiny) == n, "tiny output buffer is never overrun (%zu bytes)", n);
    CHECK(cli_execute("echo", out, 0) == 0, "zero-size output buffer is safe");
#else
    CHECK(1, "CLI compiled out (release configuration): nothing to test");
#endif
}

/* ========================================================================== */
/*  LED patterns and indications                                              */
/* ========================================================================== */

static int lit(const led_rgb_t *fb, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) if (fb[i].r | fb[i].g | fb[i].b) c++;
    return c;
}

static void test_led_patterns(void)
{
    printf("led patterns\n");
    enum { N = 60 };
    led_rgb_t fb[N + 4];
    int count = led_pattern_count();
    CHECK(count > 0, "pattern table is not empty (%d patterns)", count);

    int ok_names = 1, ok_output = 1, ok_guard = 1;
    for (int p = 0; p < count; p++) {
        if (!led_pattern_name((uint8_t)p) || !led_pattern_name((uint8_t)p)[0]) ok_names = 0;
        int any = 0;
        for (uint32_t t = 0; t < 8000; t += 250) {
            memset(fb, 0xA5, sizeof(fb));
            led_pattern_render((uint8_t)p, t, fb, N);
            for (int i = N; i < N + 4; i++) {
                if (fb[i].r != 0xA5 || fb[i].g != 0xA5 || fb[i].b != 0xA5) ok_guard = 0;
            }
            if (lit(fb, N) > 0) any = 1;
        }
        if (!any) ok_output = 0;
    }
    CHECK(ok_names, "every pattern has a name");
    CHECK(ok_output, "every pattern lights at least one LED over time");
    CHECK(ok_guard, "patterns never write beyond the frame buffer");
    CHECK(led_pattern_name(200) != NULL, "out-of-range pattern name does not crash");
    led_pattern_render(200, 0, fb, N);
    CHECK(1, "out-of-range pattern render does not crash");

    /* error indication: code N lights N red LEDs, warnings are amber */
    memset(fb, 0, sizeof(fb));
    led_pattern_error(3, false, false, 0, fb, N);
    CHECK(lit(fb, N) == 3 && fb[0].r == 255 && fb[0].g == 0, "error 3: three red LEDs");
    memset(fb, 0, sizeof(fb));
    led_pattern_error(10, true, false, 0, fb, N);
    CHECK(lit(fb, N) == 10 && fb[0].r == 255 && fb[0].g > 0, "warning 10: ten amber LEDs");
    memset(fb, 0, sizeof(fb));
    led_pattern_error(3, false, false, 700, fb, N);
    CHECK(lit(fb, N) == 0, "error pattern blinks (off phase at 700 ms)");
    memset(fb, 0, sizeof(fb));
    led_pattern_error(99, false, false, 0, fb, N);
    CHECK(lit(fb, N) == 16, "error code is capped at 16 LEDs");
    memset(fb, 0, sizeof(fb));
    led_pattern_error(3, false, true, 0, fb, N);
    CHECK(lit(fb, N) == N - 1 && fb[10].r == 24 && fb[10].g == 0 && fb[3].r == 0,
          "fatal error: dim red background, separator LED after the code stays dark");

    /* progress bar */
    memset(fb, 0, sizeof(fb));
    led_pattern_progress(50, 0, fb, N);
    CHECK(fb[10].g == 120 && fb[29].g == 120 && fb[45].g == 0, "progress 50%%: first half is green");
    led_pattern_progress(250, 0, fb, N);
    CHECK(fb[N - 1].g == 120, "progress above 100%% is clamped");

    /* boot animation ends */
    CHECK(led_pattern_boot(100, fb, N) == true && lit(fb, N) > 0, "boot animation runs at 100 ms");
    CHECK(led_pattern_boot(600, fb, N) == false && lit(fb, N) == 0, "boot animation finished after 600 ms, strip dark");
}

/* ========================================================================== */
/*  Stage and algorithm registry                                              */
/* ========================================================================== */

/* The real front-end needs the microphone driver; the registry tests only need its descriptor. */
static void stub_reset(void *s) { (void)s; }
static void stub_configure(void *s, const float *p) { (void)s; (void)p; }
static void stub_process(void *s, audio_frame_t *f) { (void)s; (void)f; }
const stage_def_t g_stage_frontend = {STAGE_FRONTEND, "Front-end (stub)", "host test stub", NULL, 0, NULL,
                                      stub_reset, stub_configure, stub_process};

static int is_render(int id)
{
    return id == STAGE_RENDER_BEAT_FLASH || id == STAGE_RENDER_SPECTRUM || id == STAGE_RENDER_BEAT_SEGMENTS ||
           id == STAGE_RENDER_SHOW;
}

static void test_registry(void)
{
    printf("stage and algorithm registry\n");
    const stage_def_t *all[STAGE_COUNT] = {
        &g_stage_frontend, &g_stage_filter_a, &g_stage_filter_b, &g_stage_envelope, &g_stage_agc,
        &g_stage_spectrum, &g_stage_beat, NULL /* retired id 7 */, &g_stage_render_beat_flash,
        &g_stage_render_spectrum, &g_stage_render_beat_segments, &g_stage_melody, &g_stage_music,
        &g_stage_render_show,
    };
    int ids_ok = 1, names_ok = 1, cb_ok = 1, range_ok = 1, uniq_ok = 1, step_ok = 1, enum_ok = 1;
    for (int i = 0; i < STAGE_COUNT; i++) {
        const stage_def_t *s = all[i];
        if (!s) continue;
        if (s->num_params > STAGE_MAX_PARAMS) range_ok = 0;
        if (s->id != i) { ids_ok = 0; printf("        stage %d has id %d\n", i, s->id); }
        if (!s->name || !s->name[0] || !s->desc) names_ok = 0;
        if (!s->reset || !s->configure || !s->process) cb_ok = 0;
        for (int p = 0; p < s->num_params; p++) {
            const param_desc_t *d = &s->params[p];
            if (!d->name || !d->name[0] || d->min >= d->max || d->def < d->min || d->def > d->max) {
                range_ok = 0;
                printf("        stage %s param %d (%s): min %g max %g def %g\n", s->name, p, d->name ? d->name : "?",
                       d->min, d->max, d->def);
            }
            if (d->type == PARAM_FLOAT && d->step <= 0.0f) step_ok = 0;
            if (d->type == PARAM_ENUM && (!d->unit || !strchr(d->unit, '|'))) enum_ok = 0;
            for (int q = p + 1; q < s->num_params; q++) {
                if (strcmp(d->name, s->params[q].name) == 0) uniq_ok = 0;
            }
        }
    }
    CHECK(ids_ok, "stage ids equal their position (persistent calibration keys)");
    CHECK(names_ok, "all stages have a name and a description");
    CHECK(cb_ok, "all stages implement reset / configure / process");
    CHECK(range_ok, "every parameter has a name, min < max and a default inside the range (<= STAGE_MAX_PARAMS)");
    CHECK(step_ok, "float parameters have a positive step");
    CHECK(enum_ok, "enum parameters list their options in the unit string");
    CHECK(uniq_ok, "parameter names are unique within a stage");

    int algo_ok = 1, order_ok = 1, last_ok = 1, dup_ok = 1, first_ok = 1;
    for (int a = 0; a < NUM_ALGORITHMS; a++) {
        const algorithm_def_t *al = &g_algorithms[a];
        if (al->id != a || !al->name || !al->desc || al->num_stages < 2) algo_ok = 0;
        if (al->stages[0]->id != STAGE_FRONTEND) first_ok = 0;
        const stage_def_t *lastst = al->stages[al->num_stages - 1];
        if (!is_render(lastst->id)) last_ok = 0;
        int seen[STAGE_COUNT] = {0};
        for (int i = 0; i < al->num_stages; i++) {
            int id = al->stages[i]->id;
            if (seen[id]) dup_ok = 0;
            seen[id] = 1;
            /* a render stage needs the analysis stage that produces its input, earlier in the list */
            if (id == STAGE_MUSIC && !(seen[STAGE_SPECTRUM] && seen[STAGE_BEAT] && seen[STAGE_MELODY] && seen[STAGE_AGC]))
                order_ok = 0;
            if (id == STAGE_RENDER_SHOW && !seen[STAGE_MUSIC]) order_ok = 0;
            if (is_render(id) && i != al->num_stages - 1) order_ok = 0;
            if ((id == STAGE_RENDER_BEAT_FLASH || id == STAGE_RENDER_BEAT_SEGMENTS) && !seen[STAGE_BEAT]) order_ok = 0;
            if ((id == STAGE_RENDER_SPECTRUM || id == STAGE_RENDER_BEAT_SEGMENTS) && !seen[STAGE_SPECTRUM]) order_ok = 0;
            if (id == STAGE_BEAT && !seen[STAGE_ENVELOPE]) order_ok = 0;
        }
    }
    CHECK(algo_ok, "algorithm table: ids, names, descriptions, at least two stages");
    CHECK(first_ok, "every algorithm starts with the front-end");
    CHECK(last_ok, "every algorithm ends with a render stage");
    CHECK(dup_ok, "no stage appears twice in one algorithm");
    CHECK(order_ok, "each stage's inputs are produced earlier in the same algorithm");
    CHECK(DEFAULT_ALGORITHM < NUM_ALGORITHMS, "default algorithm exists");
}

/* ========================================================================== */

/* ========================================================================== */
/*  Live spectrum taps                                                        */
/* ========================================================================== */

static void test_spectrum_taps(void)
{
    printf("\n[spectrum taps]\n");
    uint16_t kept = taps_set((1u << STAGE_FRONTEND) | (1u << STAGE_FILTER_A) | (1u << STAGE_FILTER_B) |
                             (1u << STAGE_AGC) | (1u << TAP_END));
    CHECK(kept == ((1u << STAGE_FRONTEND) | (1u << STAGE_FILTER_A) | (1u << STAGE_FILTER_B) | (1u << STAGE_AGC)),
          "at most %u taps are kept, lowest ids first (mask 0x%04x)", TAP_MAX_SLOTS, kept);

    const uint8_t *e = taps_edges();
    bool mono = true;
    for (unsigned i = 0; i < TAP_POINTS; i++) mono &= e[i + 1] > e[i];
    CHECK(e[0] == 1 && e[TAP_POINTS] == AUDIO_FFT_SIZE / 2 + 1 && mono,
          "%u points cover FFT bins 1..%u, every point >= 1 bin", TAP_POINTS, AUDIO_FFT_SIZE / 2);

    taps_set(1u << STAGE_FILTER_A);
    uint8_t out[TAP_POINTS];
    static q31_t blk[AUDIO_HOP_SIZE];
    g_mock_ticks = 1000;
    double ph = 0;
    for (int b = 0; b < 2; b++) {           /* 128 samples: not a full FFT window yet */
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, ph += 2 * M_PI * 1000.0 / SAMPLE_RATE) blk[i] = (q31_t)(0.5 * sin(ph) * 2147483647.0);
        taps_capture(STAGE_FILTER_A, blk, AUDIO_HOP_SIZE);
    }
    CHECK(!taps_spectrum(STAGE_FILTER_A, out), "no spectrum before %u samples", AUDIO_FFT_SIZE);
    for (int b = 0; b < 6; b++) {
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, ph += 2 * M_PI * 1000.0 / SAMPLE_RATE) blk[i] = (q31_t)(0.5 * sin(ph) * 2147483647.0);
        taps_capture(STAGE_FILTER_A, blk, AUDIO_HOP_SIZE);
    }
    bool got = taps_spectrum(STAGE_FILTER_A, out);
    unsigned best = 0;
    for (unsigned i = 1; i < TAP_POINTS; i++) if (out[i] > out[best]) best = i;
    unsigned bin = (unsigned)(1000.0 / ((double)SAMPLE_RATE / AUDIO_FFT_SIZE) + 0.5);
    double db = -120.0 + out[best] / 2.0;
    CHECK(got && e[best] <= bin && bin < e[best + 1], "1 kHz sine peaks at the point of bin %u (point %u)", bin, best);
    CHECK(got && fabs(db + 6.0) < 1.0, "half-scale sine reads -6 dBFS (%.1f)", db);
    CHECK(!taps_spectrum(STAGE_FILTER_B, out), "unselected tap: no spectrum");
    g_mock_ticks += 200;
    CHECK(!taps_spectrum(STAGE_FILTER_A, out), "stale tap (stage no longer running): no spectrum");
    taps_set(0);
    CHECK(taps_mask() == 0, "tap mask 0 stops all capturing");
}

int test_modules_run(int *checks, int *fails)
{
    test_spectrum_taps();
    test_config_store();
    test_config_power_loss();
    test_config_images();
    test_cli();
    test_led_patterns();
    test_registry();
    *checks += s_checks;
    *fails += s_fails;
    return s_fails;
}
