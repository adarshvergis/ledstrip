# Music Reactive LED Strip

Firmware and host software for a music-reactive WS2812B strip (60 LEDs) on a Seeed Studio
Xiao ESP32-C3 with an INMP441 I2S MEMS microphone. Typical delay from sound to light is
about 4-5 ms.

## Features

* **Low latency audio path**: DMA microphone capture in 4 ms blocks, processed in a
  real-time task above WiFi priority; LED output by SPI + DMA, immune to WiFi interrupts.
* **Multi-stage pipeline**: fixed-point DSP (the C3 has no FPU); stages are reusable
  building blocks with calibratable parameters, an algorithm is just a list of stages.
* **4 algorithms**:

  | # | Name | Behaviour |
  |---|---|---|
  | 0 | Music show, default | follows what the music does: clear beat = all or some LEDs per beat; lead guitar / vocal = note sparks placed by pitch; drum fills = per-drum flashes; build-ups and drops; genre profiles (auto, EDM, pop, rock, metal, R&B, hip-hop, trap, orchestral) |
  | 1 | Beat (all LEDs) | all LEDs on with the main beat (tempo locked, predicts missing beats) |
  | 2 | Spectrum bars | one segment per frequency band = *which* LEDs; bar length = band level = *how many* |
  | 3 | Beat segments | a pulse per beat, placed by the spectral centroid (bass left, treble right), width = beat strength |

* **10 pre-stored patterns** (rainbow, ocean, fire, comet, ...); the microphone is off in
  pattern mode.
* **Calibration mode**: tune any parameter live from the host app, then save to flash; the
  calibration region can be exported/imported as a binary image.
* **Live view**: LEDs, tempo, genre and a continuous spectrum streamed over UDP (up to
  100 Hz), with optional spectra after the filters / after the whole pipeline. Spectra are
  computed outside the real-time path, so they never add latency.
* **Firmware update over WiFi**: two OTA slots, automatic rollback if a new image fails.
* **WiFi station only**: the device joins your home WiFi, so the PC keeps its internet.
* **Robustness**: watchdog with per-task check-ins, error codes as blink patterns, LED
  current limiter (USB safe by default), A/B flash storage that survives power loss.
* **Debug tools** (internal test builds only): network CLI, self diagnostics (heap, stacks,
  CPU load, per-stage timing, latency), error log, device / WiFi settings from the host.

## Hardware

| WS2812B | Xiao | INMP441 | Xiao |
|---|---|---|---|
| DIN | D0 (GPIO2) | SCK | D1 (GPIO3) |
| +5V | VBUS | WS | D2 (GPIO4) |
| GND | GND | SD | D3 (GPIO5) |
| | | VDD | 3V3 |
| | | GND, L/R | GND (left channel) |

Power via the Xiao USB-C or an external USB-C breakout. The default LED current limit is
500 mA (setting `max_current_ma`); 60 LEDs at full white need ~3.6 A, so raise it only with a
suitable 5 V supply.

## Repository layout

```
firmware/                 ESP-IDF project: CMakeLists.txt, sdkconfig.defaults, partitions.csv
  App/                    application (ESP-IDF component "App")
    main.c                  start-up + supervisor loop (watchdog, health, OTA confirm)
    build_config.h          ALL compile-time configuration (versions, pins, audio, network, addresses)
    mode_manager/           device modes, run modes, real-time task
    data_acquisition/       DMA block hand-off, mic channel auto-detect, signal supervision
    audio_processing/       pipeline (pipeline.c), algorithms (algorithms.c), live spectrum taps
      stage_params.h        stage + parameter id registry (persistent keys)
      stages/               one file per processing stage
    light_control/          frame buffer, overlays, brightness, power limiter
    calibration/            live parameter table, save/reload/reset, device + network settings
    software_communication/ protocol dispatcher (comm.c), debug CLI commands
  Interface/              reusable modules (ESP-IDF component "Interface"): common (GPIO, DMA,
                          CRC-32), microphone + led_strip drivers (register level),
                          signal_processing (fixed-point DSP), led_patterns, config_manager,
                          network, firmware_updater, self_diagnostics, cli, error_handler
software/                 desktop host app (ledapp.py, tkinter), ledhost/ (protocol, DSP), tools/ (simulator, tests)
UnitTest/host/            native gcc tests of the firmware DSP, stages and modules
Docs/                     architecture, setup, protocol, extending, audio processing stages
Testing/                  testing.md: test levels, checklist, security cases, /run-tests workflow
Calibration/              saved calibration parameter sets (JSON)
Scripts/firmware/         build, flash, monitor, WiFi credentials, firmware unit tests, image tools
Scripts/software/         host build check, host run, host tests
build/                    all build output (git-ignored)
```

## Firmware: build and flash

Requirements: ESP-IDF **v5.3.x** at `%USERPROFILE%\esp\v5.3.1\esp-idf` (or set `IDF_PATH`).
The scripts load the IDF environment themselves through PowerShell (IDF refuses to run in
Git Bash / MSYS); do not call `idf.py` directly.

```sh
Scripts/firmware/build.sh -debug|-release [-clean] [-size] [-menuconfig]
Scripts/firmware/flash.sh [-COM11] [-release|-debug] [-nobuild]   # incremental build + flash over USB-C
Scripts/firmware/monitor.sh [-COM11]                              # console, Ctrl+] exits
```

Every script has a `.bat` twin for CMD / PowerShell. `-debug` is the default; without
`-COMx` the only connected serial port is used. After adding source files (found by a glob
at configure time) or changing `sdkconfig.defaults`, run `build.sh -clean` once per variant.

| | Debug (default) | Release |
|---|---|---|
| Host features | everything: main view, calibration, firmware update, settings / WiFi, errors, CLI, diagnostics | main view, calibration, firmware update; other commands answer "unknown command" |
| Size | ~906 KB | ~884 KB |

Output in `build/firmware/<variant>/artefacts/`: `led_strip_fw.bin` (the OTA image), `.elf` /
`.map`, `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` (USB flashing, done by
`flash`) and `led_strip_fw_full.bin/.hex` (everything merged, for offset 0x0). CMake / Ninja
files and the variant's `sdkconfig` are in `build_files/`.

## First-time setup

1. Flash over USB. The strip shows a short blue wipe, then the default algorithm (Music
   show). Without stored WiFi credentials WiFi stays off and the device runs stand-alone.
2. Store your WiFi credentials (station only, no own AP):
   * any firmware, over USB:
     `Scripts/firmware/update_settings.sh -ssid HomeWiFi -password secret [-name ledstrip] [-COM11]`
     (or `-file settings.json`). This rewrites the whole `settings` partition (other settings
     revert to defaults); firmware and calibration are kept. The only way for release firmware.
   * debug firmware: host app, Debug > Settings, then reboot (needs a connection already).
     OTA never touches the settings partition, so "flash debug, set WiFi, OTA release" works.
3. The device joins the network and reconnects forever (back-off 1 s -> 10 s; warning
   `wifi_connect` once the back-off reaches 10 s).
4. Start the host app and find the device with **Scan** (UDP discovery, plus beacons every
   3 s while connected).

The host is only needed for tuning, settings and updates.

## Host software

A desktop app (`software/ledapp.py`, tkinter), Python 3.9+ standard library only (no venv,
no pip).

```sh
Scripts/software/host_run.sh [-device 192.168.1.50] [-debug]   # or host_run.bat, or python software/ledapp.py
python software/tools/sim_device.py                            # device simulator, no hardware needed
```

| View | Content |
|---|---|
| Live | LED strip in the colours sent to it, LED count, WiFi strength, live fps / jitter / lost frames, tempo with beat pulse, genre, level, tone; continuous spectrum (dBFS, log frequency) with peak hold; tick boxes "After processing" and "Filter overlay" (per-filter measured output + theoretical response); controls: run mode, algorithm, pattern, brightness, live view on/off and rate 30/60/100 Hz |
| Calibration | enter/exit calibration mode; parameter editors generated from the device schema, applied live; save, reload, reset stage / all; JSON export/import |
| Firmware | device details; upload an app image (`led_strip_fw.bin`, reboots after) or a calibration image; download the calibration image |
| Debug (debug firmware only) | Settings (name, WiFi, current limit, LED count, colour order; reboot, factory reset), Errors, Diagnostics, CLI, request Log |

Data is fetched once per (re)connect; Refresh buttons reload it. Allow Python through the
Windows firewall on private networks, or the UDP live stream and discovery replies are
blocked. Details: [software/README.md](software/README.md).

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

## Tests

```sh
Scripts/firmware/run_firmware_unit_tests.sh    # firmware DSP, stages and modules, native gcc (147 checks)
Scripts/software/run_software_unit_tests.sh    # protocol end-to-end against the simulator (starts it itself)
Scripts/software/build.sh                      # byte-compile check of the host sources
```

The on-device checklist and the bug log are in [Testing/testing.md](Testing/testing.md).

## Troubleshooting

| Symptom | Check |
|---|---|
| First N LEDs blink red / amber | error code N; table in `Docs/architecture.md` section 7, or CLI `errors` |
| No reaction to sound | CLI `mic`: `signal ok`, `blocks` increasing? Raise the Front-end `input_gain` or lower the AGC `noise_gate` |
| Wrong colours | Debug > Settings -> colour order (WS2812B = GRB) |
| Strip dim at full white | power limiter: raise `max_current_ma` (only with an adequate supply) |
| Device not found by Scan | same network? WiFi credentials stored? Python allowed through the firewall? |
| WiFi wrong / unreachable | warning 10 `wifi_connect`; rewrite the credentials with `update_settings` over USB |
| Live view frozen, fps 0 | UDP blocked by the firewall; allow Python on private networks |

## Documentation

* [Docs/setup.md](Docs/setup.md): toolchain, build, flash, WiFi provisioning, host app.
* [Docs/architecture.md](Docs/architecture.md): design decisions, latency budget, tasks and
  data flow, pipeline, modes, flash layout, error codes, power, hardware notes.
* [Docs/audio_processing_stages.md](Docs/audio_processing_stages.md): every pipeline stage in
  detail.
* [Docs/extending.md](Docs/extending.md): adding stages, parameters, algorithms, patterns, CLI
  commands, settings, protocol commands, error codes, diagnostics.
* [Docs/protocol.md](Docs/protocol.md): host <-> device protocol and region image format.
* [Testing/testing.md](Testing/testing.md): test levels, checklist, debug test setup, bug log.
* [software/README.md](software/README.md): host software details.
