# Music Reactive LED Strip

Firmware and host software for a music-reactive WS2812B strip (60 LEDs) on a Seeed Studio
Xiao ESP32-C3 with an INMP441 I2S MEMS microphone. Typical delay from sound to light is
about 4-5 ms.

## Features

* **Low latency audio path**
  * DMA microphone capture in 4 ms blocks, processed in a real-time task above WiFi priority.
  * LED output by SPI + DMA, immune to WiFi interrupts.
* **Multi-stage processing pipeline**
  * Fixed-point DSP (the C3 has no FPU).
  * Stages are reusable building blocks with calibratable parameters; an algorithm is just a
    list of stages.
* **4 algorithms**:

  | # | Name | Behaviour |
  |---|---|---|
  | 0 | Amplitude (all LEDs) | all LEDs on/off (or dimmed) with loudness |
  | 1 | Beat (all LEDs), default | all LEDs on with the main beat (tempo locked, predicts missing beats) |
  | 2 | Spectrum bars | one segment per frequency band = *which* LEDs; bar length = band level = *how many* |
  | 3 | Beat segments | a pulse per beat, placed by the spectral centroid (bass left, treble right), width = beat strength |

* **10 pre-stored patterns** (rainbow, ocean, fire, comet, ...). The microphone is off in
  pattern mode.
* **Calibration mode**: tune any parameter live from the host app, then save to flash. The
  calibration region can be exported/imported as a binary image.
* **Firmware update over WiFi**: two OTA slots, automatic rollback if a new image fails to
  start.
* **WiFi station only**: the device joins your home WiFi, so the PC keeps its internet. Live
  data is streamed to the host over UDP (up to 100 Hz).
* **Robustness**
  * Watchdog with per-task check-ins.
  * Error codes shown as blink patterns on the strip.
  * LED current limiter (USB safe by default).
  * A/B flash storage that survives power loss during a save.
* **Debug tools** (internal test builds): text CLI over the network, self diagnostics
  (heap, stacks, CPU load, per-stage timing, latency), error log, device / WiFi settings from
  the host. Release builds compile all of these out.

## Hardware

| WS2812B | Xiao | INMP441 | Xiao |
|---|---|---|---|
| DIN | D0 (GPIO2) | SCK | D1 (GPIO3) |
| +5V | VBUS | WS | D2 (GPIO4) |
| GND | GND | SD | D3 (GPIO5) |
| | | VDD | 3V3 |
| | | GND, L/R | GND (left channel) |

* Power via the Xiao USB-C or an external USB-C breakout.
* The default LED current limit is 500 mA (setting `max_current_ma`). 60 LEDs at full white
  would need ~3.6 A, so raise the limit only with a suitable 5 V supply.

## Repository layout

```
App/                      application layer (ESP-IDF component "App")
  main.c                  start-up sequence + supervisor loop (watchdog, health, OTA confirm)
  build_config.h          ALL compile-time configuration: versions, build variants, pins,
                          audio, algorithms, LEDs, network, tasks, watchdog, flash addresses
  mode_manager/           device modes (main / calibration / fw-update), run modes, real-time task
  data_acquisition/       DMA block hand-off, mic channel auto-detect, signal supervision
  audio_processing/       pipeline framework (pipeline.c), algorithm table (algorithms.c),
    stage_params.h        stage + parameter id registry (persistent keys)
    stages/               one file per processing stage
  light_control/          frame buffer, overlays, brightness, power limiter
  calibration/            live parameter table, save/reload/reset, device + network settings
  software_communication/ protocol dispatcher (comm.c), debug CLI commands
Interface/                reusable submodules (ESP-IDF component "Interface")
  common/                 GPIO matrix, clock gating, DMA descriptors, cycle counter, CRC-32
  microphone/             INMP441 driver: I2S RX + GDMA ring (register level)
  led_strip/              WS2812B driver: GPSPI2 + GDMA (register level)
  signal_processing/      fixed-point DSP: biquad, FIR, DC blocker, envelope, FFT, log/exp,
                          autocorrelation, interpolation, baseline removal
  led_patterns/           pre-stored patterns, error/progress indication, colour helpers
  config_manager/         A/B-slot key/value flash store (calib + settings regions)
  network/                WiFi station, TCP frame server, UDP discovery
  firmware_updater/       OTA application update + calibration region update
  self_diagnostics/       heap/stack/CPU/flash reports, benchmarks (ENABLE_SELF_DIAGNOSTICS)
  cli/                    command interpreter (ENABLE_DEBUG_CLI)
  error_handler/          error registry, LED indication, TG0 hardware watchdog
Host/                     host software: desktop app (ledapp.py, tkinter), protocol module, simulator
UnitTest/host/            PC-side DSP / pipeline tests (native gcc)
Docs/                     architecture.md, protocol.md, extending.md
Testing/                  testing.md: test levels, checklist, security cases, /run-tests workflow, bug log
Future/                   future_plans.md: change requests, independence from ESP-IDF, features
Scripts/firmware/         build, flash, monitor, WiFi credentials, firmware unit tests, image tools
Scripts/software/         host build check, host run, host tests
build/                    all build output (git-ignored): build/firmware, build/software
CMakeLists.txt, sdkconfig.defaults, partitions.csv   ESP-IDF project files
```

## Firmware: build and flash

Requirements: ESP-IDF **v5.3.x**, installed at `%USERPROFILE%\esp\v5.3.1\esp-idf` (or set
`IDF_PATH`). The scripts load the IDF environment themselves (through PowerShell, because IDF
refuses to run in Git-Bash/MSYS shells).

```sh
Scripts/firmware/build.sh -debug             # debug build (INTERNAL_TEST_BUILD: CLI + diagnostics)
Scripts/firmware/build.sh -release           # release build
Scripts/firmware/flash.sh -COM11 -release    # build (incremental) + flash over the Xiao USB-C port
Scripts/firmware/flash.sh -COM11 -debug
Scripts/firmware/monitor.sh -COM11           # boot log / console (Ctrl+] to exit)
Scripts/firmware/build.sh -debug -size | -clean | -menuconfig
```

CMD / PowerShell: the same names with `.bat`, e.g. `Scripts\firmware\flash.bat -COM11 -release`.
`-debug` is the default if no variant is given. Without `-COMx` the only connected serial
port is used. Scripts: `Scripts/firmware/` (`build`, `flash`, `monitor`, `update_settings`,
`run_firmware_unit_tests`; helpers in `sub/`).

| | Debug (default) | Release |
|---|---|---|
| Host features | everything: main view, calibration, firmware update, settings / WiFi, errors, CLI, diagnostics | main view, calibration, firmware update only; other commands answer "unknown command" |
| Size | ~906 KB | ~884 KB |

Outputs, per variant (`debug` / `release`):

* `build/firmware/<variant>/artefacts/`:
  * `led_strip_fw.bin`: application image. Use this file for over-the-air updates.
  * `led_strip_fw.elf`, `led_strip_fw.map`: debugging.
  * `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`: only needed for USB
    flashing, which `flash` handles.
  * `led_strip_fw_full.bin` / `led_strip_fw_full.hex`: bootloader + partition table + otadata +
    app merged, to be written at flash offset 0x0 (first-flash image for other tools).
* `build/firmware/<variant>/build_files/`: CMake / Ninja files and the generated `sdkconfig`.

**When you add new source files**, run `Scripts/firmware/build.sh -clean` once (add `-release` for the release tree). Sources are found
by a glob that is evaluated at CMake configure time.

## First-time setup

1. Flash over USB. On boot you see a short blue wipe on the strip, then the default algorithm
   (Beat). Without stored WiFi credentials, WiFi stays off and the device runs stand-alone.
2. Store your home WiFi credentials (the device is a WiFi station only; it has no own AP):
   * any firmware, over USB:
     `Scripts/firmware/update_settings.sh -ssid HomeWiFi -password secret [-name ledstrip] [-COM11]`
     (or `update_settings.bat`, or `-file settings.json` with setting names). This rewrites the whole
     `settings` partition (other settings revert to defaults); firmware and calibration are
     kept. Release firmware can only be provisioned this way.
   * debug firmware: host app, Debug toggle > Debug > Settings, then reboot. This needs a
     connection, so it works only after the device is already on a network.
   * OTA never touches the settings partition, so you can also flash debug, set WiFi from the
     host, then OTA the release image.
3. The device joins the home network and reconnects forever (back-off 1 s -> 10 s; warning
   `wifi_connect` once the back-off reaches 10 s).
4. Start the host software (next section) and find the device with **Scan**: UDP discovery,
   plus beacons the device sends every 3 s while connected.

The device runs stand-alone with the last saved settings. The host is only needed for
tuning, settings and updates.

## Host software

A desktop app (`Host/ledapp.py`, tkinter) using only the Python standard library: Python
3.9+, no venv, no pip install.

```sh
Scripts/software/host_run.sh                 # or host_run.bat, or python Host/ledapp.py
Scripts/software/host_run.sh -device 192.168.1.50 [-debug]
python Host/tools/sim_device.py     # device simulator for testing without hardware
```

| View | Content |
|---|---|
| Live | LED strip (glowing dots, wraps on narrow windows), LED count, WiFi strength, live fps / jitter / lost frames, tempo with beat pulse, level, tone, spectrum with peak hold; controls: run mode (audio / pattern), algorithm, pattern, brightness, live view on/off and rate 30/60/100 Hz |
| Calibration | enter/exit calibration mode; per-stage parameter editors generated from the device schema, applied live; save to flash, reload, reset stage / all; JSON export/import |
| Firmware | device details (name, firmware version, build, address, WiFi); upload an app image (`led_strip_fw.bin`, reboots after) or a calibration region image; download the current calibration image |
| Debug (debug firmware only, Debug toggle in the top bar) | Settings (device name, WiFi SSID / password, current limit, LED count, colour order; reboot, factory reset), Errors, Diagnostics, CLI, request Log |

Data is fetched once per (re)connect; Refresh buttons reload it. On first run Windows may ask
for firewall access for Python: allow private networks, or the UDP live stream and discovery
replies are blocked. More detail: `Host/README.md`.

## Debug CLI (internal test builds)

| Command | Purpose |
|---|---|
| `help` | list all commands |
| `info`, `version` | system overview |
| `mic` | sample rate, channel, signal status, block / drop / timeout counters |
| `print_mic_samples [n]` | raw microphone samples |
| `print_processed_samples <stage_no> [n]` | samples after stage index (see `stages`) |
| `stages` | stages of the active algorithm with timing |
| `features` | level, BPM, beat count, band levels |
| `latency` | processing time, block->LED latency, estimated mic->light |
| `set_led_strip <rrggbb> <start> <end> [ms]`, `led_release` | test colour overlay |
| `algo [n]`, `runmode [audio\|pattern]`, `pattern [n]`, `brightness [n]` | run configuration |
| `mode [main\|calib]`, `params [stage]`, `param <stage> <param> [value]`, `save` | calibration |
| `errors`, `clear_errors`, `raise_error <code>` | error log / LED indication test |
| `diag`, `heap`, `bench_dsp`, `reboot` | diagnostics |

## Troubleshooting

| Symptom | Check |
|---|---|
| First N LEDs blink red / amber | error code N; see the table in `Docs/architecture.md` section 7, or CLI `errors` |
| No reaction to sound | CLI `mic`: `signal ok`? `blocks` increasing? Raise the Front-end `input_gain` or lower the AGC `noise_gate` in Calibration |
| Wrong colours | Debug > Settings -> colour order (WS2812B = GRB) |
| Strip dim at full white | power limiter: raise `max_current_ma` (only with an adequate supply) |
| Device not found by Scan | PC and device on the same network? WiFi credentials stored? Python allowed through the Windows firewall (private networks)? |
| WiFi settings wrong / unreachable | warning 10 `wifi_connect` (10 amber LEDs for 4 s, then in the error log); the device keeps retrying. Rewrite the credentials with `Scripts/firmware/update_settings.sh` over USB |
| Live view frozen, fps 0 | UDP blocked by the firewall; allow Python on private networks |

## Tests

```sh
Scripts/firmware/run_firmware_unit_tests.sh    # DSP + stage tests with native gcc (43 checks)
Scripts/software/run_software_unit_tests.sh    # protocol end-to-end against the simulator (starts it itself)
Scripts/software/build.sh                      # byte-compile check of the host sources
```

The manual on-device checklist and the bug log are in `Testing/testing.md`.

## Documentation

* [Docs/architecture.md](Docs/architecture.md): design decisions, latency budget, tasks and
  data flow, pipeline and stage list, modes, flash layout, error codes, power, hardware notes.
* [Docs/extending.md](Docs/extending.md): how to add stages, parameters, algorithms,
  patterns, CLI commands, settings, protocol commands, error codes, diagnostics.
* [Docs/protocol.md](Docs/protocol.md): host <-> device protocol and region image format.
* [Testing/testing.md](Testing/testing.md): testing levels, checklist, debug test setup, bug log.
* [Future/future_plans.md](Future/future_plans.md): planned changes and features.
* [Docs/setup.md](Docs/setup.md): toolchain, build, flash, WiFi provisioning, host app.
* [Host/README.md](Host/README.md): host software details.
