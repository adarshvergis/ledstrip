# Extending the firmware

How to add stages, parameters, algorithms, patterns, CLI commands, settings, protocol
commands, error codes and diagnostics. Background on the pipeline is in `architecture.md`,
section 4.

## 1. Concepts

```
algorithm (algorithms.c)       = ordered list of stages
stage (stages/stage_*.c)       = stage_def_t { id, name, desc, params[], state, reset, configure, process }
parameter (param_desc_t)       = { name, unit, type, min, max, default, step }, id = index in params[]
audio_frame_t (audio_types.h)  = the data passed from stage to stage, once per block (64 samples / 4 ms)
```

* **Stage id** (`stage_id_t` in `stage_params.h`) and **param id** (the per-stage
  `xxx_P_*` enum) are persistent keys:
  * the calibration store saves each value as `key = (stage << 8) | param`;
  * the host addresses parameters by (stage, param).
  * **Never renumber or reuse existing ids.** Always append new ones.
* All parameters are `float` on the wire and in storage. A stage converts them to
  fixed-point form in `configure()`. Soft float is fine there, because it only runs when a
  value changes.
* `process()` runs in the real-time task every 4 ms:
  * no floats, no blocking, no logging, no `malloc`;
  * keep it well under ~1 ms in total for all stages (`stages` CLI command and the
    diagnostics tab show per-stage timing).
* Stage state is a static struct inside the stage's `.c` file. Each stage id has exactly one
  state instance and one parameter set, shared by all algorithms that use the stage.
* The host UI builds parameter editors automatically from the schema (`GET_STAGE_SCHEMA`).
  **No host changes are needed** for new stages or parameters.
* Every stage is also a **live spectrum tap** point (tap bit n = after stage id n, see
  `spectrum_taps.h`); stage ids must stay below `TAP_END` (15), checked by a static assert.
* The host's **Filter overlay** treats a stage as a filter when its first four parameters are
  `type`, `frequency`, `q`, `gain`, and draws its response with `software/ledhost/dsp.py`
  (`biquad_db`, a copy of the firmware's `dsp_biquad_design`). Keep the two in sync when
  adding or changing filter types.

### audio_frame_t fields (inputs / outputs)

| Field | Written by | Meaning |
|---|---|---|
| `samples[64]`, `n` | data acquisition; modified by Front-end, Filter A/B | mono audio, Q31 (+/-1.0 = full scale) |
| `envelope`, `envelope_db` | Envelope | linear amplitude (Q31), dBFS in Q8 (1/256 dB) |
| `level` | AGC | 0..32767 normalised loudness |
| `bands[12]`, `centroid`, `dominant_band`, `spectrum_valid` | Spectrum | band levels 0..32767; centroid / Nyquist in Q15 |
| `beat`, `beat_strength`, `beat_phase`, `bpm_x10`, `beat_count`, `onset` | Beat | beat event this block; strength / phase in Q15; tempo x10; onset function in Q8 dB |
| `leds`, `num_leds` | (frame setup) | render target. Render stages must write all `num_leds` entries |

Before each block `beat`, `beat_strength` and `spectrum_valid` are cleared. Other features
keep their last value if the producing stage is not in the algorithm. So a render stage must
only rely on features produced by stages listed **before** it.

Fixed-point quick reference: Q31 = int32 / 2^31; Q15 = int16 / 2^15. Helpers in `dsp.h`:

* `dsp_mul_q31`, `dsp_mul_q15`, `dsp_sat_q31`, `dsp_smooth(y, x, alpha)`
* `dsp_tau_to_alpha(ms, BLOCK_RATE_HZ)` (time constant -> one-pole coefficient)
* `dsp_db_q8_to_lin_q16`, `dsp_power_to_db_q8`

Colour helpers are in `led_color.h`: `led_hsv`, `led_scale`, `led_add`, `led_gamma8`,
`led_fill`.

## 2. Add a new stage (worked example: VU meter render stage)

### Step 1: id and parameter enum (`firmware/App/audio_processing/stage_params.h`)

```c
typedef enum {
    ...
    STAGE_RENDER_BEAT_SEGMENTS = 10,
    STAGE_RENDER_VU            = 11,    /* NEW: append, never renumber */
    STAGE_COUNT
} stage_id_t;

/* ---- STAGE_RENDER_VU ---- */
typedef enum {
    RVU_P_HUE_LOW = 0,
    RVU_P_HUE_HIGH,
    RVU_P_FROM_CENTER,                  /**< bool */
    RVU_P_COUNT
} rvu_param_t;
```

`STAGE_COUNT` must stay <= 32 (dirty-mask width). Each stage may have at most
`STAGE_MAX_PARAMS` (12, `build_config.h`) parameters.

### Step 2: implementation (`firmware/App/audio_processing/stages/stage_render_vu.c`)

```c
#include "audio_types.h"
#include "led_color.h"

typedef struct {
    uint16_t hue_lo, hue_hi;
    bool     center;
} rvu_state_t;

static rvu_state_t s_st;

static const param_desc_t s_params[RVU_P_COUNT] = {
    /*                     name          unit   min    max     default step */
    [RVU_P_HUE_LOW]     = PDESC_I("hue_low",  "deg", 0.0f, 359.0f, 120.0f),
    [RVU_P_HUE_HIGH]    = PDESC_I("hue_high", "deg", 0.0f, 359.0f, 0.0f),
    [RVU_P_FROM_CENTER] = PDESC_B("from_center", 1.0f),
};

static void reset(void *st) { (void)st; }

static void configure(void *st, const float *p)
{
    rvu_state_t *s = st;
    s->hue_lo = (uint16_t)p[RVU_P_HUE_LOW];
    s->hue_hi = (uint16_t)p[RVU_P_HUE_HIGH];
    s->center = p[RVU_P_FROM_CENTER] > 0.5f;
}

static void process(void *st, audio_frame_t *f)
{
    rvu_state_t *s = st;
    uint16_t n = f->num_leds;
    uint16_t lit = (uint16_t)(((uint32_t)f->level * n) >> 15);    /* level 0..32767 */
    for (uint16_t i = 0; i < n; i++) {
        uint16_t pos = s->center ? (uint16_t)(2 * (i > n / 2 ? i - n / 2 : n / 2 - i)) : i;
        int32_t hue = s->hue_lo + ((int32_t)(s->hue_hi - s->hue_lo) * pos) / n;
        f->leds[i] = (pos < lit) ? led_hsv((uint16_t)((hue + 360) % 360), 255, 255) : LED_BLACK;
    }
}

const stage_def_t g_stage_render_vu = {
    .id = STAGE_RENDER_VU,
    .name = "Render: VU meter",
    .desc = "Bar length follows the level",
    .params = s_params,
    .num_params = RVU_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
```

Parameter descriptor macros (`audio_types.h`):

| Macro | Type | Host editor |
|---|---|---|
| `PDESC_F(name, unit, min, max, def, step)` | float | slider + number |
| `PDESC_I(name, unit, min, max, def)` | integer (rounded) | slider + number, step 1 |
| `PDESC_B(name, def)` | bool (0/1) | toggle |
| `PDESC_E(name, "a\|b\|c", count, def)` | enum (index) | dropdown, options from the string |

Values are clamped to [min, max] (and rounded for INT/BOOL/ENUM) before `configure()` sees
them. So `configure()` can trust the ranges.

### Step 3: register the stage

* `firmware/App/audio_processing/audio_types.h`:
  `extern const stage_def_t g_stage_render_vu;`
* `firmware/App/audio_processing/pipeline.c`, in `s_all_stages[]`:
  `[STAGE_RENDER_VU] = &g_stage_render_vu,`

### Step 4: use it in an algorithm (`algorithms.c`)

Either insert it into an existing list, or create a new algorithm (section 4).

### Step 5: build and tune

* The new `.c` file is picked up by the glob in `firmware/App/CMakeLists.txt`. Because it is a new
  file, re-run CMake (`Scripts/firmware/build.sh -clean`, or touch `firmware/App/CMakeLists.txt`), then build.
* Add the file to the source list in `Scripts/firmware/sub/unit_tests.ps1` if it should be tested on the PC, and add a check in
  `UnitTest/host/test_host.c`.
* In the host: Calibration tab -> enter calibration mode -> tune -> **Save to flash**.

## 3. Add / change parameters of an existing stage

* **Add:** append a new enum entry before `xxx_P_COUNT` and a descriptor at that index.
  Old saved calibrations don't contain the new key, so the default is used.
* **Change range/default:** edit the descriptor. Stored values outside the new range are
  clamped when loaded.
* **Remove:** do not delete the enum entry; rename it to `xxx_P_RESERVED_n` and keep a dummy
  descriptor, so later ids keep their numbers. Alternatively accept that ids shift and
  reset the calibration (Calibration -> Reset all -> Save).
* `calib_save()` rewrites the whole store from the current stage tables, so keys of stages
  that no longer exist disappear from flash at the next save.
* Reading a parameter from other code: `calib_get_param(stage, param, &v)` or
  `calib_stage_values(stage)[param]`. Flash-level access by enum:
  `cfg_param_read(stage, param, &v)` / `cfg_param_write()` (`config_manager.h`).

## 4. Add an algorithm

1. `build_config.h`: add `#define ALGO_MY_ALGO 4` and increase `NUM_ALGORITHMS`. Optionally
   change `DEFAULT_ALGORITHM`.
2. `algorithms.c`: add a stage list and an `ALGO(...)` entry. The static assert checks that
   the table size matches `NUM_ALGORITHMS`.

```c
static const stage_def_t *const s_algo_vu[] = {
    &g_stage_frontend, &g_stage_filter_a, &g_stage_envelope, &g_stage_agc, &g_stage_render_vu,
};
...
    ALGO(ALGO_MY_ALGO, "VU meter", "Bar follows the level", s_algo_vu),
```

* At most `PIPELINE_MAX_STAGES` (12) stages per algorithm.
* The last stage must render into `f->leds`.
* The host lists the algorithm automatically (`GET_ALGORITHMS`).
* A stored algorithm index that is out of range (e.g. after removing one) falls back to
  `DEFAULT_ALGORITHM`.

## 5. Add a pre-stored LED pattern (`firmware/Interface/led_patterns/led_patterns.c`)

* Add a palette (`static const led_rgb_t PAL_X[] = {...}`) if needed.
* Append a row to `s_patterns[]`: `{"Name", PAT_TYPE, PAL(PAL_X), period_ms, width}`.
* For a new animation kind, add a `PAT_*` value to `led_pattern_type_t` (`led_patterns.h`)
  and a `case` in `led_pattern_render()`.
  * The renderer must be a pure function of (pattern, time): no RAM state.

The pattern count is reported to the host automatically.

## 6. Add a debug CLI command (`firmware/App/software_communication/cli_commands.c`)

```c
static int c_hello(int argc, char **argv, cli_out_t *o)
{
    if (argc < 2) return -1;                 /* non-zero -> "error N. usage: ..." */
    cli_printf(o, "hello %s\n", argv[1]);
    return 0;
}
...
    {"hello", "<name>", "example command", c_hello},     /* in s_cmds[] */
```

* Output is limited to ~4 KB per command.
* The handler runs in the network task, so it must not touch real-time state directly.
  Use the request APIs instead, for example:
  * `pipeline_select`
  * `settings_set*`
  * `pipeline_capture_start`
* All of this is compiled only with `ENABLE_DEBUG_CLI` (internal test builds).

## 7. Add a device setting (`firmware/App/calibration/calibration.[ch]`)

1. Append an id to `setting_id_t` (before `SET_ID_MAX`) and add a field to `settings_t`.
2. Add a default in `settings_defaults()`, loading and range repair in `settings_load()`.
3. Add a `case` in `settings_set()` (validation, `cfg_set_uint/str`, `reboot = true` if it
   only takes effect after a reboot).
4. Serialise it in `settings_serialize()`.
5. If it should apply live, apply it in `apply_live()`.
6. Document it in `Docs/protocol.md` (table at command 0x20).
7. Host: add a `SettingDef` to `SETTINGS` in `software/ledhost/protocol.py` (this also makes it
   usable in `Scripts/firmware/sub/make_settings_image.py` / `update_settings -file` JSON files), add its id to
   the Debug > Settings field list in `software/ledapp.py` if it should be editable there, and
   add a default to `DEFAULT_SETTINGS` in `software/tools/sim_device.py`.

Settings commands exist only in debug builds (`ENABLE_DEBUG_HOST`). For release firmware a
setting can only be written over USB with `Scripts/firmware/update_settings.sh`.

## 8. Add a protocol command

1. `firmware/App/software_communication/protocol.h`: `#define CMD_X 0x08` (use a free id in the
   right group).
2. `comm.c`: add a `case CMD_X:` in `handle()`.
   * Parse the request with `r_u32` / `r_f32`; write the response with `w_u8/u16/u32/f32/str`.
   * Return an `ST_*` status.
   * Debug-only commands go inside `#ifdef ENABLE_DEBUG_HOST` (or `ENABLE_DEBUG_CLI` /
     `ENABLE_SELF_DIAGNOSTICS`), so release builds answer them with `ERR_UNKNOWN_CMD`.
3. `Docs/protocol.md`: describe the request and response payloads. Bump `PROTOCOL_VERSION`
   (`build_config.h`) for incompatible changes.
4. Host: add the id and a request builder / parser in `software/ledhost/protocol.py`, use it in
   `software/ledapp.py`, implement it in the simulator `software/tools/sim_device.py` (and in its
   debug-only set if applicable), and add a check to `software/tools/proto_test.py`.

## 9. Add an error code (`firmware/Interface/error_handler/error_handler.[ch]`)

* Append to `err_code_t` (before `ERR_COUNT`). The value is the LED blink count, so keep it
  stable.
* Add `{name, severity}` to `s_info[]`, then report with `err_report(ERR_X)`.
* Severities:
  * WARNING: shown for 4 s;
  * ERROR: shown until cleared;
  * FATAL: shown, then reboot after 8 s.
* Add it to the table in `architecture.md`, section 7.

## 10. Add diagnostics / benchmarks (`firmware/Interface/self_diagnostics`)

```c
#ifdef ENABLE_SELF_DIAGNOSTICS
static diag_bench_t s_bench = DIAG_BENCH_INIT("group", "name");
/* once: */ diag_bench_register(&s_bench);
/* around code: */ diag_bench_begin(&s_bench); ...; diag_bench_end(&s_bench);
#endif
```

* Extra metrics: `diag_register_provider(fn)`; in `fn`, call
  `diag_write(w, group, name, unit, fmt, ...)`. See `diag_provider()` in
  `mode_manager.c`.
* The host Debug > Diagnostics tab shows all lines automatically.

## 11. Change compile-time configuration (`firmware/App/build_config.h`)

| Need | Change |
|---|---|
| Lower latency | `AUDIO_HOP_SIZE` 32 (2 ms blocks; LED frame 2.05 ms must fit) |
| Finer spectrum | `AUDIO_FFT_SIZE` 512 + `AUDIO_FFT_LOG2` 9 (more CPU, slower time response; the live spectrum taps use the same size and their `u8` bin edges only reach bin 255, so `spectrum_taps.c` / the protocol would need changing too) |
| More bands | `AUDIO_NUM_BANDS` (render spectrum divides the strip by it; protocol sends the count) |
| Other pins / LED count | `PIN_*`, `LED_MAX_COUNT` (<= ~450 with one DMA descriptor) |
| Release build | `Scripts/firmware/build.sh -release` (no settings / errors from the host, no CLI, no diagnostics) |
