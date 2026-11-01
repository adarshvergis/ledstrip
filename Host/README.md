# LED Strip Host Software

Calibration, settings, live view, CLI, diagnostics and firmware update for the
music-reactive LED strip. A small Python backend (asyncio + aiohttp) talks to the
device over the binary protocol in [`Docs/protocol.md`](../Docs/protocol.md)
(TCP 4210 / UDP 4211) and serves a single-page web UI at <http://127.0.0.1:8080>.
The UI is plain HTML/CSS/JS with no build step and no CDN, so it works offline.

```
Host/
  ledhost/            Python package
    protocol.py       frame encode/decode, enums, all command payloads, region image format
    device.py         async TCP client (one persistent connection, lock-serialised, auto-reconnect), UDP discovery
    server.py         aiohttp REST/JSON API + static file serving, firmware upload job
    __main__.py       `python -m ledhost`
  static/             index.html, app.js, style.css (the web UI)
  tools/
    sim_device.py     device simulator (protocol server side) for testing without hardware
    api_test.py       end-to-end REST test against the simulator
  requirements.txt    aiohttp
```

## Setup and run

```sh
Scripts/host_setup.sh                  # or Scripts\host_setup.bat  - creates Host/.venv, installs aiohttp
Scripts/host_run.sh --device 192.168.4.1   # or Scripts\host_run.bat
```

Then open <http://127.0.0.1:8080>. Options for `python -m ledhost` (run from `Host/`):

| Option | Default | Meaning |
|---|---|---|
| `--host` | `127.0.0.1` | interface for the web UI (use `0.0.0.0` to open it from a phone on the LAN) |
| `--port` | `8080` | web UI port |
| `--device ip[:port]` | host setting `default_device_ip` | device to use at start-up |
| `-v` | | debug logging |

Without `--device`, use the Main tab: **Scan** (UDP broadcast `LEDSTRIP_DISCOVER` +
listening for the 3 s beacons) or type an IP. In soft-AP mode the device is
`192.168.4.1`.

Host-side settings (live poll rate, default device IP, auto-connect) are stored in
`Host/host_settings.json` (git-ignored).

## Testing without hardware

```sh
python Host/tools/sim_device.py            # TCP 127.0.0.1:4210 + UDP discovery on 127.0.0.1:4211
cd Host && python -m ledhost --device 127.0.0.1
python Host/tools/api_test.py              # 71 end-to-end checks; expects a freshly started simulator
```

The simulator has 4 algorithms built from 5 stages (FLOAT/INT/BOOL/ENUM params),
5 patterns, an animated 60-LED strip, settings, errors, a CLI (`help`, `echo`, ...),
diagnostics and a fake firmware update with real CRC/offset checks. `--flags 1`
hides the CLI and diagnostics tabs; `--fw-delay` slows uploads down.

## UI tabs

* **Main** - discovery and manual connect, device info, run mode / algorithm /
  pattern / brightness, live view (LED strip in real colours, level, BPM with beat
  flash, band bars, spectral centroid; polled at the host poll rate, default 15 Hz),
  error list with clear.
* **Calibration** - enter/exit calibration mode; per-algorithm pipeline view (or all
  stages); editors generated from the stage schema (slider + number for FLOAT/INT,
  toggle for BOOL, dropdown for ENUM); changes are applied live (100 ms debounce) and
  the value returned by the device is shown (clamping is highlighted); save to flash,
  reload, reset stage / all, export/import JSON. "Unsaved changes" follows `calib_dirty`.
* **Settings** - device settings (name, Wi-Fi, AP, current limit, LED count, colour
  order) with reboot-required notice, reboot, factory reset (in-page confirmation),
  export/import of all device settings, host settings.
* **Admin** - CLI (only if build flag `ENABLE_DEBUG_CLI`), firmware update (APP or
  CALIB target, progress, abort) and calibration image download, self diagnostics
  (only if `ENABLE_SELF_DIAGNOSTICS`; auto refresh 1 s, reset).

## Calibration image tool

```sh
python Scripts/make_calib_image.py params.json -o calib.bin [--sequence 1]
python Scripts/make_calib_image.py --decode calib.bin [-o params.json]
```

`params.json` is `{"params": [{"stage": 0, "param": 1, "value": 40.0}, ...]}`; a
file from **Calibration > Export JSON** works as-is. Upload the image in
**Admin > Firmware update** with target *Calibration region*.

## REST API

All endpoints return JSON. Errors are `{"error": "...", "status": "<NAME>", "code": <int|null>}`;
`status` is the device status name (`ERR_WRONG_MODE`, ...), `TRANSPORT` for
connection problems or `HOST` for host-side validation.

HTTP mapping: 400 bad argument / length / validation, 409 wrong mode or busy,
422 CRC, 501 unknown command / not supported, 500 flash/internal, 502 device
unreachable, 503 no device selected, 504 device timeout.

| Method | Path | Body / query | Result |
|---|---|---|---|
| GET | `/api/status` | | `{device, connected, info, error, fw_job}` (does GET_INFO) |
| POST | `/api/connect` | `{ip: "a.b.c.d[:port]"}` | `{device, info}` |
| POST | `/api/disconnect` | | |
| GET | `/api/discover` | `?timeout=1.5` | `{devices: [{name, ip, port, fw, mac, ...}], beacon_listener}` |
| GET | `/api/info` | | decoded GET_INFO + `has_cli`, `has_diag`, mode names |
| POST | `/api/reboot` | | |
| POST | `/api/device_mode` | `{mode: 0\|1\|"main"\|"calibration"}` | |
| POST | `/api/run_config` | `{run_mode?, algorithm?, pattern?, brightness?}` | omitted = unchanged |
| GET | `/api/live` | | `{level, bpm, beat_count, centroid, num_leds, bands[], leds: "rrggbb..."}` |
| GET | `/api/errors` | | `{errors: [{code, name, severity, severity_name, occurrences, last_uptime_ms}]}` |
| POST | `/api/errors/clear` | | |
| GET | `/api/algorithms` | | `{algorithms: [{id, name, description, stage_ids}]}` |
| GET | `/api/stages` | | `{stages: [ids]}` |
| GET | `/api/stages/{id}/schema` | | `{id, name, description, params: [{id, type, type_name, min, max, default, step, name, unit, options}]}` |
| GET | `/api/stages/{id}/params` | | `{stage, values: []}` |
| GET | `/api/calib` | | algorithms + every stage schema with `values` + `device_mode`, `calib_dirty`, `active_algorithm` |
| POST | `/api/params` | `{stage, param, value}` | `{requested, applied}` |
| POST | `/api/params/bulk` | `{params: [{stage, param, value}]}` | `{applied, failed, results}` |
| GET | `/api/params/export` | | JSON file download |
| POST | `/api/params/save` / `reload` | | |
| POST | `/api/params/reset` | `{stage?}` (omit = all) | |
| GET | `/api/settings` | | `{settings: {name: value}, definitions}` |
| POST | `/api/settings` | `{settings: {name: value, ...}}` | `{changed: [names], reboot_required}` (unchanged values skipped) |
| POST | `/api/settings/factory_reset` | | |
| GET | `/api/settings/export` | | JSON file download |
| GET/POST | `/api/host_settings` | `{poll_rate_hz?, default_device_ip?, auto_connect?}` | host settings |
| POST | `/api/fw/upload` | raw file body; `?target=app\|calib&name=file.bin` | starts background job |
| GET | `/api/fw/status` | | `{state, target, total, sent, percent, message, elapsed_s, rate_bps}` |
| POST | `/api/fw/abort` | | aborts the job (or sends FW_ABORT if none running) |
| GET | `/api/calib_image` | | binary download (GET_CALIB_IMAGE) |
| POST | `/api/cli` | `{cmd}` | `{output}` |
| GET | `/api/diag` | | `{rows: [{group, name, value, unit}], raw}` |
| POST | `/api/diag/reset` | | |

## Implementation notes

* The device serves one TCP client: the backend keeps a single connection and
  serialises requests with an `asyncio.Lock`. Timeout is 3 s per request, 15 s for
  FW_BEGIN / FW_DATA / FW_END / SAVE_PARAMS. Any transport error drops the
  connection; the next request reconnects. Responses with a stale `seq` are skipped.
* Firmware upload runs in the backend: FW_BEGIN (size + `zlib.crc32`), FW_DATA
  chunks of `max_chunk`, FW_END, then REBOOT for APP images. On failure FW_ABORT is
  sent. APP files must start with the ESP image magic `0xE9`; CALIB files are
  checked against the region image format before anything is sent.
* `brightness = 255` cannot be sent with SET_RUN_CONFIG (0xFF = unchanged), so
  `/api/run_config` sends it as SET_SETTING id 13 instead.
