# Firmware Architecture

Seeed Studio Xiao ESP32-C3 (RISC-V RV32IMC, 160 MHz, **no FPU**), WS2812B x60, INMP441.

## 1. Design decisions

| Topic | Decision | Why |
|---|---|---|
| Base | ESP-IDF v5.3 **only** for build system, FreeRTOS, WiFi/TCP-IP, 2nd-stage bootloader, OTA and flash access | The C3 radio needs Espressif's closed WiFi blobs, which need FreeRTOS + lwIP. Everything else is our own register-level C. |
| Own code | I2S mic driver, WS2812 driver, GDMA handling, watchdog, DSP library, pipeline, config store, protocol, CLI, diagnostics | No vendor driver layers (`driver/`, `esp_driver_*`) are used. Register maps come from the `soc/*_struct.h` headers. |
| Host link | WiFi (station, falling back to soft-AP), binary TCP protocol, UDP discovery | WiFi has much higher bandwidth than BLE for firmware upload, and works with any browser-based host. |
| RTOS | FreeRTOS (required by WiFi anyway) | The real-time task runs at priority 24, above WiFi (23). It blocks on the DMA interrupt, so it uses CPU only while it has a block to process. |
| Sample hand-off | Chunked: 64-sample DMA blocks (4 ms @ 16 kHz) | DMA fills the ring with no CPU cost. The ISR only wakes the task. Blocks amortise the per-block overhead, and the block size sets the latency. |
| Bootloader | IDF 2nd-stage bootloader + 2 OTA slots, **no custom bootloader** | Mode switching (main / calibration / fw-update) happens inside the app with no reboot. OTA A/B gives automatic rollback for free, even though no backup was required. |
| Arithmetic | Fixed point: Q31 audio, Q2.30 biquads, Q15 FFT with block-floating-point | Software float is ~50-100 cycles per operation on this chip. Float is only used when parameters change. |
| LED output | GPSPI2 + GDMA, 3 SPI bits per WS2812 bit at 2.5 MHz | The whole frame goes out by DMA. That is immune to WiFi interrupt latency, unlike RMT refilling or bit-banging. |

## 2. Latency budget (mic -> light)

| Step | Time |
|---|---|
| Sound -> INMP441 -> I2S, average sample age inside a 64-sample block | ~2.0 ms (hop/2) |
| DMA block complete -> ISR -> task wake | < 0.05 ms |
| Pipeline (spectrum + beat + render) | ~0.3-0.6 ms (measure with `latency` / diagnostics) |
| WS2812 frame on the wire (60 LEDs + latch) | 2.05 ms (the last LED gets its data ~1.7 ms after the first) |
| **Total** | **~4-5 ms** |

The beat detector fires one block (~4 ms) after the kick onset (host test: mean 3.6-3.9 ms).
To trade CPU for latency, change `AUDIO_HOP_SIZE` in `build_config.h` (32 = 2 ms blocks). The
hop must be longer than the LED frame time.

## 3. Tasks and data flow

```
 INMP441 --I2S--> GDMA ring (4 x 64 frames) --SUC_EOF ISR--> notify
                                                              |
 rt task (prio 24) <------------------------------------------+
   daq_read_block(): newest block -> mono Q31 (older blocks dropped if late)
   pipeline_process(): stage 0 .. stage N (audio_frame_t)
   light_show(): overlays -> brightness -> power limit -> GPSPI2/GDMA -> WS2812B

 comm task (prio 5)      TCP 4210: frames -> comm.c dispatcher -> calibration / mode / fwu / cli
 discovery task (prio 3) UDP 4211: discovery replies + beacons
 app_main (prio 1)       supervisor: watchdog feed, fatal reboot, health, OTA confirm
```

Other tasks never touch the microphone, the pipeline state or the strip. They post requests:

* algorithm change (`pipeline_select`)
* "parameters of stage X changed" (a dirty bit mask)
* sample capture

The real-time task applies these at the start of the next block. So the hot path takes no
locks, and stages never see half-applied parameters.

## 4. Processing pipeline

* `audio_frame_t` (audio_types.h) carries:
  * the samples, modified in place by the filter stages
  * the features: envelope, level, bands, centroid, beat, bpm, ...
  * the LED render target
* A **stage** (`stage_def_t`) is: an id, a name, a parameter descriptor table, a state
  pointer, and three callbacks (`reset` / `configure` / `process`).
  * `configure()` converts float parameters into fixed-point form (filter coefficients,
    time constants).
* An **algorithm** is an array of stage pointers (algorithms.c). Adding, removing or
  reordering stages means editing an array.
* **Parameters** are identified by `stage_id` (stage_params.h) and per-stage `param` enums.
  * They are stored as `key = stage << 8 | param` in the calib store.
  * Stages that appear in several algorithms share their parameters (e.g. mic gain is
    calibrated once).

| Id | Stage | Purpose |
|---|---|---|
| 0 | Front-end | mic channel (auto/left/right), DC blocker, input gain |
| 1, 2 | Filter A / B | configurable RBJ biquad: off / LP / HP / BP / notch / peak / shelves |
| 3 | Envelope | per-block RMS or peak, attack/release |
| 4 | AGC | level 0..1 against a decaying peak; noise gate, max gain, log or linear curve |
| 5 | Spectrum | sliding 256-pt FFT (75 % overlap), 12 log bands, per-band normalisation, centroid |
| 6 | Beat | bass onset detection (16 ms energy window), autocorrelation tempo with a 120 BPM prior, phase-locked prediction with flywheel and phase re-acquisition |
| 7 | Render level | all LEDs on/off (threshold, hysteresis, hold) or dimming |
| 8 | Render beat flash | all LEDs on with the beat (on/off or decaying flash, colour step per beat) |
| 9 | Render spectrum | one segment per band (*which* LEDs), bar length = band level (*how many*) |
| 10 | Render beat segments | pulse per beat: position = spectral centroid, width = beat strength |

| Algo | Name | Stages |
|---|---|---|
| 0 | Amplitude (all LEDs) | 0 1 2 3 4 7 |
| 1 | Beat (all LEDs) - default | 0 1 3 4 6 8 |
| 2 | Spectrum bars | 0 1 2 3 4 5 9 |
| 3 | Beat segments | 0 1 3 4 5 6 10 |

### Adding a stage

1. Append `STAGE_xxx` before `STAGE_COUNT` in `stage_params.h`, and add a `xxx_P_*` parameter enum.
2. Implement `App/audio_processing/stages/stage_xxx.c`. Copy any existing stage as a template.
3. Declare it with `extern` in `audio_types.h`, and add it to `s_all_stages` in `pipeline.c`.
4. Insert it into one or more algorithms in `algorithms.c`.
5. Run `Scripts/build.sh`. The host UI builds the parameter editors from the schema automatically.

## 5. Device modes and run modes

* **MAIN**: normal operation.
* **CALIBRATION**: same output, but `SET_PARAM` / `SAVE` / `RESET` are allowed. Changes are
  audible and visible immediately. Leaving without saving reloads the values from flash.
* **FW_UPDATE**: entered with `FW_BEGIN`. The mic and the algorithm stop, and the strip
  shows a progress bar. The update aborts if no data arrives for 30 s. After a successful
  APP update the device reboots after 5 s (or earlier on `REBOOT`).
* **Run mode** AUDIO / PATTERN: in PATTERN mode the I2S clocks are stopped (mic disabled),
  and a const pattern from `led_patterns.c` is rendered at 100 Hz.

## 6. Persistent storage

| Region | Address | Content |
|---|---|---|
| bootloader | 0x0000 | IDF 2nd-stage bootloader |
| partition table | 0x8000 | `partitions.csv` |
| nvs / otadata / phy | 0x9000-0xFFFF | WiFi PHY calibration, OTA boot selection |
| ota_0 / ota_1 | 0x10000 / 0x1F0000 | 1.875 MiB each (image is ~0.9 MiB) |
| calib | 0x3D0000 | algorithm parameters, A/B 4 KiB slots |
| settings | 0x3D8000 | device / network / run settings, A/B 4 KiB slots |

* Slot format: header (magic, version, store id, sequence, length, CRC-32), then
  `key/type/len/value` records. The full format is in Docs/protocol.md section 5.
* A commit always writes the *other* slot with sequence + 1, so a power loss during a save
  keeps the previous data.
* The calibration region can be read (`GET_CALIB_IMAGE`) and written as a whole
  (`FW_BEGIN target=1`). Use `Scripts/make_calib_image.py` to build an image from JSON.

## 7. Error handling

* `err_report(code)` records the occurrence count and the time.
* The most severe active error is shown on the strip:
  * the first *code* LEDs blink (red = error, amber = warning, 1 Hz);
  * FATAL errors also make the rest of the strip glow dim red, and the device reboots after 8 s.
* Warnings disappear from the strip after 4 s, but stay in the log (`GET_ERRORS`, CLI `errors`).
* Watchdog:
  * Our own TG0 MWDT driver resets the chip after 3 s without a feed.
  * The supervisor feeds it only while every registered task has checked in: `rt` within
    1 s, `comm` within 10 s.
  * The task that hung is stored in RTC memory and reported after the reboot.
  * The IDF interrupt watchdog (TG1) stays enabled.
* A previous reset caused by a panic, watchdog or brown-out becomes an error entry at boot.

| Code | Name | Severity |
|---|---|---|
| 1 | mic_init | fatal |
| 2 | mic_no_signal | error |
| 3 | mic_timeout | error |
| 4 | audio_overrun | warning |
| 5 | led_init | fatal |
| 6 | led_timeout | warning |
| 7 | cfg_corrupt | warning |
| 8 | cfg_write | error |
| 9 | net_init | error |
| 10 | wifi_connect | warning |
| 11 | fw_update | error |
| 12 | reset_watchdog | warning |
| 13 | reset_panic | warning |
| 14 | reset_brownout | warning |
| 15 | heap_low | warning |
| 16 | stack_low | warning |
| 17 | partition | error |
| 18 | internal | error |

## 8. Power

All 60 LEDs at full white would draw about 3.6 A, which is far more than USB can supply.
`light_control` estimates the current of every frame and scales the frame down to the
`max_current_ma` setting (default 500 mA). The estimate is 20 mA per colour channel at 255,
plus 1 mA idle per LED. Raise the limit only with a suitable 5 V supply.

## 9. Hardware notes

* GPIO2 (LED data) is a strapping pin. The WS2812B input is high impedance, so boot is not
  affected.
* The INMP441 with L/R tied to GND outputs on the left slot. The front-end's `mic_channel = auto`
  picks the slot that carries signal during the first 256 ms. CLI `mic` shows the result.
* For long LED runs, add a 300-500 ohm series resistor on DIN and a 1000 uF capacitor across
  5 V/GND.
