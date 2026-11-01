# Setup, Build and Flash

Everything needed to go from a clean Windows PC to a flashed board and a running host UI.
Commands are for Git Bash unless marked otherwise; CMD / PowerShell equivalents are given.

## 1. Toolchain to install on the host PC

| Tool | Version | Needed for | Notes |
|---|---|---|---|
| ESP-IDF | **v5.3.x** (tested 5.3.1) | firmware build, flash, monitor | Install to `%USERPROFILE%\esp\v5.3.1\esp-idf`, or set `IDF_PATH`. Use the ESP-IDF installer or `install.ps1 esp32c3`. It brings the RISC-V GCC toolchain, CMake, Ninja, OpenOCD and its own Python env (`%USERPROFILE%\.espressif`). |
| Python | 3.9+ with tkinter (tested 3.12) | host software, helper scripts | Must be on `PATH`. Separate from the ESP-IDF Python env. The python.org installer includes tkinter. |
| Git for Windows | any recent | Git Bash, running the `.sh` scripts | Provides `bash`, `curl`, `cygpath`. |
| PowerShell | 5.1+ (built in) | `Scripts/*/sub/*.ps1` | All `.sh` / `.bat` scripts are thin wrappers around PowerShell scripts in `Scripts/firmware/sub` and `Scripts/software/sub`. |
| gcc (native) | any (tested MSYS2 ucrt64 13.2) | firmware unit tests only | Optional. Install MSYS2 and `pacman -S mingw-w64-ucrt-x86_64-gcc`; add `ucrt64\bin` to `PATH`. |
| USB driver | none normally | flashing | The Xiao ESP32-C3 uses native USB-Serial/JTAG, which Windows 10/11 handles itself. It shows up as a COM port (check Device Manager > Ports). |

The host software uses only the Python standard library (no venv, no pip install).
`flash` and `update_settings` use the `esptool` module of the ESP-IDF Python env (the scripts load the IDF environment themselves).

## 2. Hardware

Wiring and pin table: see [README.md](../README.md#wiring). Power the board over USB-C.

Flash over the Xiao USB-C port. If the port is not detected, hold **BOOT**, tap **RESET**, release
**BOOT** to enter the ROM bootloader, then flash again.

## 3. Scripts overview

All scripts take dash-style options (`-release`, `-COM11`, ...). Every `.sh` (Git Bash) has a `.bat`
twin (CMD / PowerShell) with the same name and options. `-debug` is the default variant.

| Script | Purpose |
|---|---|
| `Scripts/firmware/build.sh -release\|-debug [-clean] [-size] [-menuconfig]` | build the firmware |
| `Scripts/firmware/flash.sh [-COM11] [-release\|-debug] [-nobuild]` | build (incremental) and flash over USB-C |
| `Scripts/firmware/monitor.sh [-COM11] [-release\|-debug]` | serial console (Ctrl+] exits) |
| `Scripts/firmware/update_settings.sh -ssid <name> -password <pw> [-name <dev>] [-COM11]` | write WiFi credentials over USB-C |
| `Scripts/firmware/run_firmware_unit_tests.sh` | native DSP / pipeline tests (gcc) |
| `Scripts/firmware/make_calib_image.py` | calibration JSON <-> region image |
| `Scripts/software/host_run.sh [-device <ip>] [-debug]` | start the desktop host app |
| `Scripts/software/build.sh [-clean]` | byte-compile check of the host sources (no packaging needed) |
| `Scripts/software/run_software_unit_tests.sh` | protocol tests against the device simulator |

Helper scripts live in `Scripts/firmware/sub/` and `Scripts/software/sub/` (PowerShell scripts that
do the work, `make_settings_image.py`). Without `-COMx`, `flash`, `monitor` and `update_settings` use
the only serial port that is connected, and stop with an error if there are none or several.

### Build output

Everything goes below `build/` (git-ignored):

```
build/
  firmware/
    debug/    artefacts/   led_strip_fw.bin .elf .map, bootloader.bin, partition-table.bin,
                           ota_data_initial.bin, led_strip_fw_full.bin / .hex (merged, flash offset 0x0)
              build_files/ CMake / Ninja files, sdkconfig
    release/  artefacts/ + build_files/   (same layout)
    unit_tests/            native test executable
    settings_image/        settings.bin written by update_settings (contains the WiFi password)
  software/
    build_files/pycache    byte code (the host is plain Python: no artefacts)
```

## 4. Build, flash and monitor firmware

```sh
Scripts/firmware/build.sh -debug              # CLI, self diagnostics, settings from the host
Scripts/firmware/build.sh -release            # main view, calibration, firmware update only
Scripts/firmware/flash.sh -COM11 -release     # build + flash the release image
Scripts/firmware/flash.sh -COM11 -debug       # build + flash the debug image
Scripts/firmware/monitor.sh -COM11            # serial log (exit with Ctrl+])
```

CMD / PowerShell: `Scripts\firmware\build.bat -release`, `Scripts\firmware\flash.bat -COM11 -release`, ...

* Do **not** run `idf.py` from Git Bash: ESP-IDF refuses to run in MSYS shells. The scripts call
  PowerShell, source `export.ps1` and clear `MSYSTEM` for you.
* Each variant has its own build tree and its own `sdkconfig` (created from `sdkconfig.defaults` on
  the first build, target esp32c3). After editing `sdkconfig.defaults` or adding source files, run
  `build.sh -clean` (add `-release` for the release tree) and build again.
* The OTA file for the host's Firmware view is `build/firmware/<variant>/artefacts/led_strip_fw.bin`.
* Last measured size: debug ~906 KB, release ~884 KB, in a 1.875 MiB OTA slot.
* The first flash must be over USB. Later updates can go over WiFi (section 6).
* `flash` writes bootloader, partition table, otadata and app. It does not touch the `calib` and
  `settings` partitions, so saved calibration and WiFi credentials survive a re-flash. To wipe the
  whole flash use `python -m esptool --chip esp32c3 -p COMx erase_flash` from an ESP-IDF shell.
* `flash.sh -nobuild` flashes the existing artefacts without building.
* Release builds serve only the main view, calibration and firmware update. Settings, errors,
  CLI and self diagnostics are compiled out (the device answers "unknown command").

## 5. First boot and network

The device is a WiFi station only (no soft-AP). It joins the home WiFi from the `sta_ssid` /
`sta_password` settings, so the PC stays on its own network and keeps internet.

1. Without stored credentials WiFi stays off; the device runs stand-alone (audio and patterns
   work).
2. Write the credentials once over USB (works for debug and release firmware):

   ```sh
   Scripts/firmware/update_settings.sh -ssid HomeWiFi -password secret [-name ledstrip] [-COM11]
   Scripts/firmware/update_settings.sh -file settings.json [-COM11]   # JSON of setting names (Docs/protocol.md, cmd 0x20)
   ```

   CMD: `Scripts\firmware\update_settings.bat -ssid HomeWiFi -password secret -COM11`. Do not type
   brackets (they only mark optional options). Quote values with spaces or shell characters.
   The script builds the 8 KiB settings A/B area with `Scripts/firmware/sub/make_settings_image.py`
   (slot A = image, slot B erased, written to `build/firmware/settings_image/settings.bin`) and
   writes it at 0x3D8000 with esptool. All stored settings are replaced (others revert to
   defaults); firmware and calibration are kept.
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
Scripts/software/host_run.sh                         # or: python Host/ledapp.py
Scripts/software/host_run.sh -device 192.168.1.50    # connect to a device on start
Scripts/software/host_run.sh -debug                  # start in debug mode
Scripts/software/build.sh                            # optional: byte-compile check of the host sources
```

CMD: `Scripts\software\host_run.bat`. Host settings (last device, live rate, debug mode) persist in
`Host/ledapp_settings.json` (git-ignored).

On first run Windows may show a firewall prompt for Python: allow it on private networks,
otherwise the UDP live stream and discovery replies are blocked.

The UI is dark only. Scan / Connect and the Debug toggle are in the top bar. Views: Live
(LED strip, LED count, WiFi strength, tempo, level, tone, spectrum; controls for run mode,
algorithm, pattern, brightness and live rate 30/60/100 Hz), Calibration, Firmware (device
details, update). The Debug toggle (shown only when the firmware is a debug build) adds a
Debug tab: Settings, Errors, Diagnostics, CLI, Log. The app reads
everything once per (re)connect; use the Refresh buttons to reload.

**WiFi firmware update:** build, then Firmware view, upload `build/firmware/<variant>/artefacts/led_strip_fw.bin` (or the
release image). The app reboots the device afterwards. A new image must confirm itself or the
bootloader rolls back to the previous slot.

**Calibration image:** `python Scripts/firmware/make_calib_image.py params.json -o calib.bin` builds an image
for Firmware > "Calibration region image". `--decode calib.bin` converts it back to JSON. The
Calibration view's JSON export/import uses the same format.

## 7. Testing

```sh
Scripts/firmware/run_firmware_unit_tests.sh   # DSP + pipeline stage tests, native gcc (needs gcc on PATH)
Scripts/software/run_software_unit_tests.sh   # protocol test; starts the simulator itself (ports 14210/14211)
python Host/tools/sim_device.py               # simulated device on 127.0.0.1:4210 / :4211, no hardware
python Host/tools/sim_device.py --flags 0     # simulate a release build (default --flags 15 = debug)
```

To try the UI without hardware: start `sim_device.py`, then `Scripts/software/host_run.sh -device 127.0.0.1`.

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `ESP-IDF not found at ...` | Install v5.3.1 to `%USERPROFILE%\esp\v5.3.1\esp-idf` or set `IDF_PATH`. |
| IDF complains about MSYS / MinGW | Use `Scripts/firmware/build.sh` or `build.bat`, not `idf.py` in Git Bash. |
| `no serial port found` / `several serial ports found` | Connect the board, or pass the port explicitly (`-COM11`). |
| Port busy / access denied on flash | Close the serial monitor and any other program using the COM port. |
| Flash cannot connect | Enter the ROM bootloader: hold BOOT, tap RESET, release BOOT. |
| `Python 3.9+ with tkinter not found` | Install Python from python.org (includes tkinter) and make sure `py` or `python` is on `PATH`. |
| Host cannot find the device | Check the PC is on the same network and WiFi credentials are stored (`update_settings`); allow Python through the firewall; try the IP directly. |
| Live view shows no frames | Windows firewall blocks UDP for Python; allow it on private networks. |
| `gcc not found on PATH` (firmware unit tests) | Add the MSYS2 `ucrt64\bin` directory to `PATH`. |
