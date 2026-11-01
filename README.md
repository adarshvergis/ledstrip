# Music Reactive LED Strip

Firmware and host software for a music-reactive WS2812B strip (60 LEDs) on a Seeed Studio
Xiao ESP32-C3 with an INMP441 I2S microphone. The light reacts about 4-5 ms after the sound.

```
App/                    application layer (ESP-IDF component "App")
  main.c                start-up + supervisor
  build_config.h        all compile-time configuration (versions, pins, audio, network, ...)
  mode_manager/         device modes + the real-time task
  data_acquisition/     DMA block hand-off, mic channel detection, signal supervision
  audio_processing/     pipeline framework, algorithms.c, stages/ (one file per stage)
  light_control/        frame buffer, overlays, brightness, current limiter
  calibration/          live parameter table, settings (device / network / run config)
  software_communication/  protocol dispatcher, debug CLI commands
Interface/              reusable submodules (ESP-IDF component "Interface")
  common/               GPIO matrix, clock gating, DMA descriptors, CRC, cycle counter
  microphone/           INMP441 driver: I2S RX + GDMA ring (register level)
  led_strip/            WS2812B driver: GPSPI2 + GDMA (register level)
  signal_processing/    fixed-point DSP: biquad/FIR/DC/envelope, FFT, log/exp, autocorr, ...
  led_patterns/         const patterns, error / progress indication, colour helpers
  config_manager/       A/B-slot key/value flash store
  network/              WiFi STA/AP, TCP frame server, UDP discovery
  firmware_updater/     OTA app update + calibration-region update
  self_diagnostics/     heap/stack/CPU/flash reports, benchmarks (ENABLE_SELF_DIAGNOSTICS)
  cli/                  command interpreter (ENABLE_DEBUG_CLI)
  error_handler/        error registry, LED indication, TG0 watchdog
Host/                   host software (Python + browser UI), see Host/README.md
UnitTest/host/          host-side DSP / pipeline tests (native gcc)
Docs/                   architecture.md, protocol.md
Scripts/                build / flash / test / host setup scripts
```

## Wiring

| WS2812B | Xiao | INMP441 | Xiao |
|---|---|---|---|
| DIN | D0 (GPIO2) | SCK | D1 (GPIO3) |
| +5V | VBUS | WS | D2 (GPIO4) |
| GND | GND | SD | D3 (GPIO5) |
| | | VDD / GND / L/R | 3V3 / GND / GND |

## Firmware

Requirements: ESP-IDF v5.3.x installed at `%USERPROFILE%\esp\v5.3.1\esp-idf`, or `IDF_PATH` set.

```sh
Scripts/build.sh                    # debug build (INTERNAL_TEST_BUILD: CLI + diagnostics)
Scripts/build.sh --release          # release build -> build_release/
Scripts/build.sh flash COM5         # first flash over the Xiao USB-C port
Scripts/build.sh monitor COM5       # boot log / console
```

From CMD or PowerShell use `Scripts\build.bat` / `Scripts\build.ps1` (same options:
`flash -Port COM5`, `-Release`).

* After the first USB flash, updates can go over WiFi: upload `build/led_strip_fw.bin` in
  the host's *Admin -> Firmware Update* tab.
* On first boot the device opens the soft-AP `LEDSTRIP-XXXX` (password `ledstrip123`,
  IP 192.168.4.1). Enter your WiFi credentials in the *Settings* tab and reboot. If the
  station connection fails, the device falls back to the soft-AP.
* The device runs stand-alone; the host is needed only for tuning and updates.

## Host software

```sh
Scripts/host_setup.sh               # once: creates Host/.venv and installs aiohttp
Scripts/host_run.sh                 # http://127.0.0.1:8080
python Host/tools/sim_device.py     # optional: device simulator for testing without hardware
```

Tabs:

* **Main**: discovery, live view, algorithm / pattern / brightness.
* **Calibration**: parameter editors generated from the device schema, applied live; save,
  reset, import/export.
* **Settings**: WiFi and LED setup.
* **Admin**:
  * CLI (debug builds only)
  * Firmware Update (app image or calibration region)
  * Self Diagnostics (debug builds only)

## Tests

```sh
Scripts/run_host_tests.sh           # DSP + stage tests with native gcc (43 checks)
python Host/tools/api_test.py       # host REST API against the simulator
```

## Documentation

* [Docs/architecture.md](Docs/architecture.md): design decisions, latency budget, tasks,
  pipeline, how to add a stage, storage, error codes.
* [Docs/protocol.md](Docs/protocol.md): host <-> device protocol and region image format.
