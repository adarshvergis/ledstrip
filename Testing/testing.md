# Testing

## 1. Test levels

| Level | How | Needs hardware |
|---|---|---|
| Host DSP / pipeline tests | `Scripts/run_host_tests.sh` (native gcc, 43 checks; see `UnitTest/README.md`) | no |
| Host software API tests | `python Host/tools/sim_device.py`, then `python Host/tools/api_test.py` | no (device simulator) |
| Firmware build check | `Scripts/build.sh` and `Scripts/build.sh --release` | no |
| On-device manual tests | flash, then use the host UI / debug CLI (checklist in section 3) | yes |
| Automated on-device tests (debug test setup) | planned, see section 2 | yes |

## 2. Debug test setup (hardware-in-the-loop) - future feature

A hardware-in-the-loop setup used for AI-assisted debugging:

* the Xiao ESP32-C3 board connected over USB (COM11 at the time of writing): flash, monitor,
  serial log;
* a camera pointed at the LED strip (laid out in a circle on the desk), to check visually
  what the strip shows;
* browser access to the host software (http://127.0.0.1:8080).

**Status: not permanently connected.**

**Rule: before using the debug test setup (board, camera, or browser automation), ask the user
whether it is currently connected.** Do not assume it is available.

Notes from the first session (2026-11-01):

* **WiFi conflict:** joining the device soft-AP (`LEDSTRIP-XXXX`, 192.168.4.1) from the
  development PC disconnects the PC from the home WiFi, so it loses internet access. Avoid this
  (see `Future/future_plans.md`, item 1). Until that is fixed, prefer:
  * putting the device in station mode on the home network (Settings tab: wifi_mode =
    station/auto, STA SSID + password, reboot); or
  * a second WiFi adapter for the device network; or
  * USB serial only (flash / monitor) when no host connection is needed.
* No camera / browser automation tool was available to the AI agent in that session; only
  the serial port, `curl` against the host REST API, and `ffmpeg` (installed at
  `C:\ffmpeg\bin`, usable for webcam snapshots) were.

Planned tooling:

* **Playwright** (Python) to drive the host web UI: select algorithms, change parameters, run
  CLI commands, upload firmware, and screenshot each tab.
* **Camera snapshots** (`ffmpeg -f dshow -i video="<camera>" -frames:v 1 shot.jpg`), plus a
  small script that samples the LED positions on the circle and reports colours/brightness,
  so an agent can check, for example, "all LEDs red after `set_led_strip ff0000 0 59`".
* **Serial capture script** (pyserial): reset, collect the boot log, grep for errors.
* **Audio stimulus:** play test files (click track at known BPM, sine sweeps) from the PC
  speaker and check BPM / beat timing through `GET_LIVE` and the camera.

## 3. Manual on-device checklist

| # | Test | Expected |
|---|---|---|
| 1 | Power on | short blue wipe (boot animation), then the default algorithm |
| 2 | CLI `mic` | `channel left`, `signal ok`, `blocks` increasing, `timeouts` not increasing |
| 3 | CLI `print_mic_samples 64` | 64 values that change with sound |
| 4 | CLI `set_led_strip ff0000 0 59 5000` | whole strip red for 5 s |
| 5 | CLI `runmode pattern`, `pattern 0..9` | each pattern is visible; mic stopped (`mic`: blocks frozen) |
| 6 | Main tab: algorithms 0..3 with music | 0 on/off with loudness, 1 flashes on beat, 2 spectrum bars, 3 beat pulses |
| 7 | CLI `latency` | processing well below 4000 us, no overruns |
| 8 | Calibration: change a parameter | visible effect immediately |
| 9 | Calibration: save, reboot, `params <stage>` | value persisted (**not yet tested**) |
| 10 | Calibration: exit without save | value reverts |
| 11 | Firmware update (Admin tab) with `build/led_strip_fw.bin` | progress bar on strip, reboot, new build date in `info` (**not yet tested**) |
| 12 | Calib region: download image, change params, upload image | old values restored (**not yet tested**) |
| 13 | CLI `raise_error 2` | first 2 LEDs blink red |
| 14 | Settings: station credentials, reboot | device joins the home network, found by Scan |

## 4. Bug log

| Date | Symptom | Root cause | Fix |
|---|---|---|---|
| 2026-11-01 | No LED output in any mode; `mic`: blocks 0, timeouts rising; error 3 mic_timeout | The GDMA controller is held in reset after boot (`SYSTEM_DMA_RST` defaults to 1); the driver only enabled its clock | `hw_periph_enable(HW_PERIPH_GDMA)` also releases the reset (`Interface/common/hw_common.c`) |
| 2026-11-01 (host test) | Beat locked to the off-beat at 95 BPM; double triggers | ODF from a single 4 ms block wobbles within one kick; no phase re-acquisition | 16 ms energy window, re-arm hysteresis, off-phase re-acquisition (`stage_beat.c`) |

Known minor issue: one `audio_overrun` warning right after audio starts. The first block
includes the stage reconfiguration and is longer than the hop. It is harmless; the warning
clears after 4 s.
