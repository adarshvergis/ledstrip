# LED Strip Host Software

Desktop app for live view, calibration and firmware update of the music-reactive LED
strip, plus settings, errors, diagnostics and CLI for debug firmware. It is one Python
file using only the standard library (tkinter, socket, threading), and it talks to the
device over the binary protocol in [`Docs/protocol.md`](../Docs/protocol.md): TCP 4210
for commands, a UDP live stream, and UDP 4211 for discovery.

The device joins your home WiFi in station mode, so the PC stays on its normal network
and keeps internet access. There is no device access point to join.

```
software/
  ledapp.py           the desktop app
  ledhost/
    protocol.py       frame encode/decode, enums, all command payloads, region image formats
  tools/
    sim_device.py     device simulator (protocol server side) for testing without hardware
    proto_test.py     end-to-end protocol test against the simulator
```

## Run

You need Python 3.9 or newer with tkinter. The python.org installer for Windows
includes it. There is no virtual environment and nothing to install.

```sh
Scripts/software/host_run.sh [-device 192.168.1.50] [-debug]     # or Scripts\software\host_run.bat, or python software/ledapp.py
```

* **Device:** without `--device` the app reconnects to the last device. Otherwise
  **Scan** broadcasts `LEDSTRIP_DISCOVER` and connects to the first device that
  answers; you can also type the device IP.
* **Host settings:** the last device, live rate and debug mode are saved in
  `software/ledapp_settings.json`, which is git-ignored.
* **Windows firewall:** on first start Windows may ask whether Python may use the
  network. Allow it for private networks, otherwise the UDP live stream and discovery
  replies are blocked.

### First-time WiFi setup

The device needs your WiFi credentials once:

* **Release firmware:** write them over the USB-C cable. This replaces all stored
  settings; calibration and firmware are kept.
  `Scripts/firmware/update_settings.sh -ssid HomeWiFi -password secret -COM5`
* **Debug firmware:** you can also set them in **Debug > Settings**, then reboot the
  device. The settings partition survives OTA updates, so you can flash a debug
  build, set the WiFi from the host, and then OTA the release image.

## Views

The UI is dark only. The top bar shows the connection state, the device address,
Scan / Connect and the **Debug** toggle.

* **Live**
  * The LED strip as glowing dots in real colours. On narrow windows the strip wraps
    into several rows. Dim LEDs are brightened on screen so they stay visible; the
    strip itself is not affected.
  * The header shows the LED count, WiFi strength (bars and dBm, read at connect),
    live frames per second, jitter and lost frames, and the device mode when it is
    not MAIN.
  * Tempo (BPM with a beat pulse), level and tone (spectral centroid) meters, and
    spectrum bars with peak hold.
  * **Controls:** source (Audio / Pattern), algorithm, pattern, brightness (0-254),
    live view on/off and live rate (30, 60 or 100 Hz). Below about 700 px window
    width the controls move under the live view.
* **Calibration**
  * Enter or exit calibration mode, and filter the stages by algorithm.
  * Parameter editors are generated from the stage schema: slider and spin box for
    FLOAT/INT, check box for BOOL, drop-down for ENUM.
  * Changes are sent live. Slider moves are coalesced so that only the newest value
    per parameter goes out, about every 60 ms. The value the device applied, which
    may be clamped, is shown.
  * Save to flash, reload, reset a stage or all stages, and export or import JSON.
* **Firmware**
  * Device details: name, firmware version and build type, build date, address,
    WiFi, audio setup and last reset reason.
  * Upload an application image (must start with `0xE9`) or a calibration region
    image, with progress and an abort button.
  * After an application image the app sends REBOOT and reconnects on its own.
  * Download the current calibration image.
* **Debug** (Debug toggle in the top bar)
  * Shown only when the connected device is debug firmware (build flag
    `ENABLE_DEBUG_HOST`).
  * **Settings:** device name, WiFi SSID and password, current limit, LED count and
    colour order, with reboot and factory reset.
  * **Errors**, **Diagnostics**, **CLI** with history, and a request **Log** with
    per-request timing.

## How it works (latency)

* **Live view is pushed, not polled.** After connecting, the app sends `LIVE_SUBSCRIBE`
  with its UDP port, and the firmware's comm task sends one datagram per period with
  the newest snapshot. There are no request round trips, and no HTTP, JSON or hex
  encoding.
  * Lost datagrams are not resent. A late datagram is dropped by sequence number, so
    the view never steps backwards.
  * The UI thread draws only the newest frame every ~15 ms and changes only the LEDs
    whose colour changed.
* **Threads:**
  * The **UI thread** never blocks on I/O.
  * A **worker thread** owns the TCP link. It sends requests in order, sends a PING
    once a second (which also renews the 3 s live-stream lease) and reconnects every
    2 s after a failure.
  * A **live thread** receives the UDP stream.
* **Diagnostics are read once per connection.** On every connect or reconnect (and
  so after every device reboot) the app reads info, algorithms, stage schemas and
  parameters. On debug firmware it also reads errors, settings and self
  diagnostics. Nothing is polled after that; the Debug tab has Refresh buttons.
* **Jitter figure:** the device timestamps each frame (`t_ms`). The clocks are not
  synchronised, so the "jitter" figure is the average delay above the fastest frame
  seen.

## Testing without hardware

```sh
python software/tools/sim_device.py               # debug build (--flags 15); TCP 127.0.0.1:4210, UDP 4211
python software/tools/sim_device.py --flags 0     # release build: debug-only commands -> ERR_UNKNOWN_CMD
python software/ledapp.py --device 127.0.0.1
python software/tools/proto_test.py               # starts its own simulator on 14210/14211; 29 checks
```

The simulator has:

* 4 algorithms built from 5 stages, with FLOAT/INT/BOOL/ENUM parameters, and 5 patterns.
* An animated 60-LED strip, served over the UDP live stream.
* Settings, errors, a CLI (`help`, `echo`, ...) and diagnostics.
* A fake firmware update with real CRC and offset checks. `--fw-delay` slows uploads
  down.

## Image tools

```sh
python Scripts/firmware/make_calib_image.py params.json -o calib.bin [--sequence 1]   # Calibration > Export works as input
python Scripts/firmware/make_calib_image.py --decode calib.bin [-o params.json]
python Scripts/firmware/sub/make_settings_image.py --ssid HomeWiFi --password secret -o settings.bin   # used by update_settings
```

Upload a calibration image in **Firmware**, choosing *Calibration region image*. The
settings image is written over USB at `0x3D8000` by `Scripts/firmware/update_settings.sh`.
