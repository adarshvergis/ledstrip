# Setup, Build and Flash

Everything needed to go from a clean Windows PC to a flashed board and a running host UI.
Commands are for Git Bash unless marked otherwise; CMD / PowerShell equivalents are given.

## 1. Toolchain to install on the host PC

| Tool | Version | Needed for | Notes |
|---|---|---|---|
| ESP-IDF | **v5.3.x** (tested 5.3.1) | firmware build, flash, monitor | Install to `%USERPROFILE%\esp\v5.3.1\esp-idf`, or set `IDF_PATH`. Use the ESP-IDF installer or `install.ps1 esp32c3`. It brings the RISC-V GCC toolchain, CMake, Ninja, OpenOCD and its own Python env (`%USERPROFILE%\.espressif`). |
| Python | 3.9+ with tkinter (tested 3.12) | host software, helper scripts | Must be on `PATH`. Separate from the ESP-IDF Python env. The python.org installer includes tkinter. |
| Git for Windows | any recent | Git Bash, running the `.sh` scripts | Provides `bash`, `curl`, `cygpath`. |
| PowerShell | 5.1+ (built in) | `Scripts/build.ps1` | The `.sh` / `.bat` build wrappers call it. |
| gcc (native) | any (tested MSYS2 ucrt64 13.2) | host unit tests only | Optional. Install MSYS2 and `pacman -S mingw-w64-ucrt-x86_64-gcc`; add `ucrt64\bin` to `PATH`. |
| USB driver | none normally | flashing | The Xiao ESP32-C3 uses native USB-Serial/JTAG, which Windows 10/11 handles itself. It shows up as a COM port (check Device Manager > Ports). |

The host software uses only the Python standard library (no venv, no pip install).
`flash_settings` uses the `esptool` module of the ESP-IDF Python env (through `build.ps1`).

## 2. Hardware

Wiring and pin table: see [README.md](../README.md#wiring). Power the board over USB-C.

Flash over the Xiao USB-C port. If the port is not detected, hold **BOOT**, tap **RESET**, release
**BOOT** to enter the ROM bootloader, then flash again.

## 3. Build firmware

| Goal | Git Bash | CMD / PowerShell |
|---|---|---|
| Debug build (CLI, self diagnostics, settings from host) -> `build/` | `Scripts/build.sh` or `Scripts/build_debug.sh` | `Scripts\build.bat` or `Scripts\build_debug.bat` |
| Release build -> `build_release/` | `Scripts/build.sh --release` or `Scripts/build_release.sh` | `Scripts\build.bat -Release` or `Scripts\build_release.bat` |
| Size report | `Scripts/build.sh size` | `Scripts\build.bat size` |
| Clean / full clean | `Scripts/build.sh clean` / `fullclean` | `Scripts\build.bat clean` / `fullclean` |
| menuconfig | `Scripts/build.sh menuconfig` | `Scripts\build.bat menuconfig` |

Notes:

* Do **not** run `idf.py` from Git Bash: ESP-IDF refuses to run in MSYS shells. `build.sh` forwards
  to `build.ps1`, which sources `export.ps1` and clears `MSYSTEM`.
* The first build creates `sdkconfig` (`idf.py set-target esp32c3`) from `sdkconfig.defaults`. After
  editing `sdkconfig.defaults`, delete `sdkconfig` (or run `fullclean`) so it is regenerated.
* Outputs: `build/led_strip_fw.bin` (app image, also the file for WiFi OTA),
  `build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`
  (release: same under `build_release/`).
* Every successful build or flash copies `led_strip_fw.bin`, `bootloader.bin`,
  `partition-table.bin` and `ota_data_initial.bin` to `dist/debug/` or `dist/release/`
  (git-ignored).
* Last measured size: debug ~906 KB, release ~884 KB, in a 1.875 MiB OTA slot.

## 4. Flash and monitor

Replace `COM11` with your port.

```sh
Scripts/build.sh flash COM11            # build + flash bootloader, partition table, app
Scripts/build.sh monitor COM11          # serial log (exit with Ctrl+])
Scripts/build.sh flash-monitor COM11    # both
Scripts/build.sh flash COM11 --release  # flash the release image
Scripts/build_release.sh flash COM11    # same
```

CMD / PowerShell: `Scripts\build.bat flash -Port COM11` (add `-Release` for release), or
`Scripts\build_debug.bat flash COM11` / `Scripts\build_release.bat flash COM11`.

* The first flash must be over USB. Later updates can go over WiFi (section 6).
* `idf.py flash` does not touch the `calib` and `settings` partitions, so saved calibration and WiFi
  credentials survive a re-flash. Use `idf.py erase-flash` (from an ESP-IDF shell) to wipe them.
* Release builds serve only the main view, calibration and firmware update. Settings, errors,
  CLI and self diagnostics are compiled out (the device answers "unknown command").

## 5. First boot and network

The device is a WiFi station only (no soft-AP). It joins the home WiFi from the `sta_ssid` /
`sta_password` settings, so the PC stays on its own network and keeps internet.

1. Without stored credentials WiFi stays off; the device runs stand-alone (audio and patterns
   work).
2. Write the credentials once over USB (works for debug and release firmware):

   ```sh
   Scripts/flash_settings.sh COM11 --ssid HomeWiFi --password secret [--name ledstrip]
   Scripts/flash_settings.sh COM11 settings.json   # JSON of setting names (Docs/protocol.md, cmd 0x20)
   ```

   CMD: `Scripts\flash_settings.bat COM11 --ssid HomeWiFi --password secret`.
   It builds the 8 KiB settings A/B area with `Scripts/make_settings_image.py` (slot A = image,
   slot B erased) and writes it at 0x3D8000 with esptool
   (`build.ps1 flash-settings -Port COMx -File <img>`). All stored settings are replaced (others
   revert to defaults); firmware and calibration are kept.
3. Debug firmware can also change them from the host app (Debug > Settings, then reboot), once it
   is reachable. OTA never touches the settings partition, so "flash debug, set WiFi from the host,
   OTA the release image" also works.
4. After a disconnect the device retries forever with a back-off of 1 s up to 10 s; warning 10
   `wifi_connect` is reported once the back-off reaches 10 s.
5. On your LAN, use **Scan** (UDP discovery on port 4211, plus a beacon every 3 s while connected)
   or type the device IP. The command channel is TCP 4210; live data comes as a UDP stream to the
   host (`LIVE_SUBSCRIBE`, see `Docs/protocol.md`).

## 6. Host software

Desktop app `Host/ledapp.py` (tkinter, Python standard library only).

```sh
Scripts/host_run.sh                         # or: python Host/ledapp.py
Scripts/host_run.sh --device 192.168.1.50   # connect to a device on start
Scripts/host_run.sh --debug                 # start in debug mode
```

CMD: `Scripts\host_run.bat`. Host settings (last device, live rate, debug mode) persist in
`Host/ledapp_settings.json` (git-ignored).

On first run Windows may show a firewall prompt for Python: allow it on private networks,
otherwise the UDP live stream and discovery replies are blocked.

Views: Main (scan / connect, info, run mode, algorithm, pattern, brightness, live view; live rate
30/60/100 Hz in the View menu), Calibration, Firmware. View > Debug mode (shown only when the
firmware is a debug build) adds a Debug tab: Settings, Errors, Diagnostics, CLI, Log. The app reads
everything once per (re)connect; use the Refresh buttons to reload.

**WiFi firmware update:** build, then Firmware view, upload `build/led_strip_fw.bin` (or the
release image). The app reboots the device afterwards. A new image must confirm itself or the
bootloader rolls back to the previous slot.

**Calibration image:** `python Scripts/make_calib_image.py params.json -o calib.bin` builds an image
for Firmware > "Calibration region image". `--decode calib.bin` converts it back to JSON. The
Calibration view's JSON export/import uses the same format.

## 7. Testing

```sh
Scripts/run_host_tests.sh               # DSP + pipeline stage tests, native gcc (needs gcc on PATH)
python Host/tools/sim_device.py         # simulated device on 127.0.0.1:4210 / :4211, no hardware
python Host/tools/sim_device.py --flags 0   # simulate a release build (default --flags 15 = debug)
python Host/tools/proto_test.py         # protocol test; starts the simulator itself (ports 14210/14211)
```

To try the UI without hardware: start `sim_device.py`, then `Scripts/host_run.sh --device 127.0.0.1`.

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `ESP-IDF not found at ...` | Install v5.3.1 to `%USERPROFILE%\esp\v5.3.1\esp-idf` or set `IDF_PATH`. |
| IDF complains about MSYS / MinGW | Use `Scripts/build.sh` or `build.bat`, not `idf.py` in Git Bash. |
| Port busy / access denied on flash | Close the serial monitor and any other program using the COM port. |
| Flash cannot connect | Enter the ROM bootloader: hold BOOT, tap RESET, release BOOT. |
| `Python 3.9+ with tkinter not found` | Install Python from python.org (includes tkinter) or set `PYTHON`. |
| Host cannot find the device | Check the PC is on the same network and WiFi credentials are stored (`flash_settings`); allow Python through the firewall; try the IP directly. |
| Live view shows no frames | Windows firewall blocks UDP for Python; allow it on private networks. |
| `run_host_tests.sh`: `gcc: command not found` | Add the MSYS2 `ucrt64\bin` directory to `PATH`. |
