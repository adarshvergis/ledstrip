# Testing

## 1. Test levels

| Level | How | Needs hardware |
|---|---|---|
| Host DSP / pipeline tests | `Scripts/firmware/run_firmware_unit_tests.sh` (native gcc, 43 checks; see `UnitTest/README.md`) | no |
| Host protocol tests | `python Host/tools/proto_test.py` (starts the simulator itself on ports 14210/14211, debug then release build; 29 checks) | no (device simulator) |
| Firmware build check | `Scripts/firmware/build.sh -debug` and `-release` | no |
| On-device manual tests | flash, then use the host app / debug CLI (checklist in section 3) | yes |
| Automated test workflow (all of the above, plus issue reporting) | `/run-tests`, see section 5 | optional |

## 2. Debug test setup (hardware-in-the-loop) - future feature

A hardware-in-the-loop setup used for AI-assisted debugging:

* the Xiao ESP32-C3 board connected over USB (COM11 at the time of writing): flash, monitor,
  serial log;
* a camera pointed at the LED strip (laid out in a circle on the desk), to check visually
  what the strip shows;
* the host desktop app (`Host/ledapp.py`) on the PC.

**Status: not permanently connected.**

**Rule: before using the debug test setup (board or camera), ask the user
whether it is currently connected.** Do not assume it is available.

Notes from the first session (2026-11-01):

* **WiFi conflict (resolved):** joining the device soft-AP from the development PC
  disconnected the PC from the home WiFi. The firmware is now WiFi station only (no soft-AP,
  see `Future/future_plans.md`, item 1): write the home WiFi credentials once with
  `Scripts/firmware/update_settings.sh -ssid ... -password ... -COMx` (or Debug > Settings in the host
  app with debug firmware). The PC stays on the home network.
* No camera / browser automation tool was available to the AI agent in that session; only
  the serial port, `curl` against the (since removed) host REST API, and `ffmpeg` (installed at
  `C:\ffmpeg\bin`, usable for webcam snapshots) were.

Planned tooling:

* **Scripted host checks** through `Host/ledhost/protocol.py` (as `Host/tools/proto_test.py`
  does against the simulator): select algorithms, change parameters, run CLI commands, upload
  firmware. The host app is a tkinter desktop app, so browser tools (Playwright) do not apply.
* **Camera snapshots** (`ffmpeg -f dshow -i video="<camera>" -frames:v 1 shot.jpg`), plus a
  small script that samples the LED positions on the circle and reports colours/brightness,
  so an agent can check, for example, "all LEDs red after `set_led_strip ff0000 0 59`".
* **Serial capture script** (pyserial): reset, collect the boot log, grep for errors.
* **Audio stimulus:** play test files (click track at known BPM, sine sweeps) from the PC
  speaker and check BPM / beat timing through `GET_LIVE` and the camera.

## 3. Manual on-device checklist

Last synced commit: `08e70b2` (updated by the `/run-tests` workflow)

| # | Test | Expected |
|---|---|---|
| 1 | Power on | short blue wipe (boot animation), then the default algorithm |
| 2 | CLI `mic` | `channel left`, `signal ok`, `blocks` increasing, `timeouts` not increasing |
| 3 | CLI `print_mic_samples 64` | 64 values that change with sound |
| 4 | CLI `set_led_strip ff0000 0 59 5000` | whole strip red for 5 s |
| 5 | CLI `runmode pattern`, `pattern 0..9` | each pattern is visible; mic stopped (`mic`: blocks frozen) |
| 6 | Main view: algorithms 0..3 with music | 0 on/off with loudness, 1 flashes on beat, 2 spectrum bars, 3 beat pulses |
| 7 | CLI `latency` | processing well below 4000 us, no overruns |
| 8 | Calibration: change a parameter | visible effect immediately |
| 9 | Calibration: save, reboot, `params <stage>` | value persisted (**not yet tested**) |
| 10 | Calibration: exit without save | value reverts |
| 11 | Firmware update (Firmware view) with `build/firmware/<variant>/artefacts/led_strip_fw.bin` | progress bar on strip, reboot, new build date in `info` (**not yet tested**) |
| 12 | Calib region: download image, change params, upload image | old values restored (**not yet tested**) |
| 13 | CLI `raise_error 2` | first 2 LEDs blink red |
| 14 | `Scripts/firmware/update_settings.sh -ssid ... -password ... -COMx` (or Debug > Settings + reboot) | device joins the home network, found by Scan; calibration unchanged |
| 15 | No SSID stored (fresh flash / erased `settings` partition, or debug host Factory reset + reboot) | WiFi off, audio / patterns still work (**not yet tested**) |
| 16 | Switch the home AP off, then on | device reconnects by itself; `wifi_connect` warning once the back-off reaches 10 s (**not yet tested**) |
| 17 | Live view at 30 / 60 / 100 Hz (Live > Controls) | live header shows about that fps, few lost frames, LEDs match the strip (**not yet tested**) |
| 18 | Release build (`Scripts/firmware/flash.sh -release -COMx`) | Debug mode not offered in the host app; main view, calibration and firmware update work (**not yet tested**) |

## 4. Security test cases

Run only on the user's own device or the simulator (the workflow asks first). Keep them
non-destructive and bounded.

| # | Test | Expected |
|---|---|---|
| S1 | Protocol: bad magic, bad CRC, wrong length field, length larger than the maximum frame | frame rejected or connection closed; device keeps running (no reboot, `info` uptime continues) |
| S2 | Protocol: unknown command id, truncated and oversized payloads for every command | error response, no crash |
| S3 | Fuzz (bounded, a few thousand random frames) | no reboot, no hang, no error codes in the serial log |
| S4 | Open several TCP connections, then a short burst of commands | device stays responsive; LED output and mic keep running |
| S5 | Unauthenticated actions: `SET_SETTING` (debug builds), `FW_BEGIN`, reboot, CLI from any LAN client | document the exposure (no authentication) as Info / Medium; invalid images must be rejected and rolled back |
| S6 | Firmware update with a corrupt image, a truncated image, a wrong-target image | rejected, old firmware still boots |
| S7 | WiFi password stored in clear in flash and returned by `GET_SETTINGS` (debug builds) | report as finding; password must not appear in the serial log |
| S8 | `LIVE_SUBSCRIBE`: rate > 100, rate 0, port 0, then the client goes silent | rate clamped to 100, 0 / port 0 stop the stream, stream stops after the 3 s lease |
| S9 | Live stream target: the stream only goes to the IP of the connected TCP client | no datagrams to other hosts |
| S10 | Release build: settings, errors, CLI and diagnostics commands (0x20-0x22, 0x40, 0x50/0x51, 0x60/0x61) | `ERR_UNKNOWN_CMD` |

## 5. Automated test workflow (`/run-tests`)

Defined as a project skill in `.claude/skills/run-tests/SKILL.md`. Start it in Claude Code
with `/run-tests`. It is only started by the user (never automatically).

**Questions asked on every run**

1. Scope: firmware, host software, or all.
2. Include security tests (section 4): yes / no.
3. Workbench: none / board only / board + camera. If a board is used, which COM port.

**Steps**

| Step | What happens |
|---|---|
| 1 | Ask the questions above. Hardware is never assumed to be connected. |
| 2 | Sync test cases: compare the code with the `Last synced commit` marker (section 3). New CLI commands, protocol commands, algorithms, stages, parameters, settings, patterns, error codes or host app views get new rows in this file and new native / `proto_test.py` checks. The marker is updated. |
| 3 | Unit tests: `Scripts/firmware/run_firmware_unit_tests.sh`. Each failure gets a root-cause analysis in `UnitTest/failures/<date>_<name>.md`. |
| 4 | Build checks: debug and release builds. |
| 5 | On-device tests (board connected): flash, boot log scan, checklist (section 3). Cases that need a camera, sound or a human are marked SKIPPED when the workbench lacks them. |
| 6 | Host software tests: `proto_test.py` (simulator, debug + release); protocol checks against the board (live stream, settings, calibration) when connected. |
| 7 | Security tests (section 4), only if requested. |
| 8 | Issues: every bug, crash or finding is appended to `Testing/issues_found.md` with expected result, actual result, exact steps and reproducibility (re-run at least twice). |
| 9 | Wrap-up: stop everything the workflow started, update the checklist markers, summarise PASS / FAIL / SKIPPED. |

**Rules**

* The workflow never commits, stages or pushes. All changes stay in the working tree.
* It reports issues; it does not fix source code to make tests pass.
* It does not erase flash, and asks before destructive checklist rows (calibration save, WiFi
  firmware update, calib region upload).
* It never changes the PC's network; the device must already be on the home WiFi (section 2).

**Outputs**

| File | Content |
|---|---|
| `UnitTest/failures/*.md` | one file per unit test failure, with cause analysis |
| `Testing/issues_found.md` | open issues from device / host / security testing (the Bug log below is for fixed bugs) |
| this file | new test case rows, `Last synced commit`, "not yet tested" markers |

## 6. Bug log

| Date | Symptom | Root cause | Fix |
|---|---|---|---|
| 2026-11-01 | No LED output in any mode; `mic`: blocks 0, timeouts rising; error 3 mic_timeout | The GDMA controller is held in reset after boot (`SYSTEM_DMA_RST` defaults to 1); the driver only enabled its clock | `hw_periph_enable(HW_PERIPH_GDMA)` also releases the reset (`Interface/common/hw_common.c`) |
| 2026-11-01 (host test) | Beat locked to the off-beat at 95 BPM; double triggers | ODF from a single 4 ms block wobbles within one kick; no phase re-acquisition | 16 ms energy window, re-arm hysteresis, off-phase re-acquisition (`stage_beat.c`) |

Known minor issue: one `audio_overrun` warning right after audio starts. The first block
includes the stage reconfiguration and is longer than the hop. It is harmless; the warning
clears after 4 s.
