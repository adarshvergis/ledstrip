# Setup, Build and Flash

Everything needed to go from a clean Windows PC to a flashed board and a running host UI.
Commands are for Git Bash unless marked otherwise; CMD / PowerShell equivalents are given.

## 1. Toolchain to install on the host PC

| Tool | Version | Needed for | Notes |
|---|---|---|---|
| ESP-IDF | **v5.3.x** (tested 5.3.1) | firmware build, flash, monitor | Install to `%USERPROFILE%\esp\v5.3.1\esp-idf`, or set `IDF_PATH`. Use the ESP-IDF installer or `install.ps1 esp32c3`. It brings the RISC-V GCC toolchain, CMake, Ninja, OpenOCD and its own Python env (`%USERPROFILE%\.espressif`). |
| Python | 3.9+ (tested 3.12) | host software, helper scripts | Must be on `PATH`. Separate from the ESP-IDF Python env. |
| Git for Windows | any recent | Git Bash, running the `.sh` scripts | Provides `bash`, `curl`, `cygpath`. |
| PowerShell | 5.1+ (built in) | `Scripts/build.ps1` | The `.sh` / `.bat` build wrappers call it. |
| gcc (native) | any (tested MSYS2 ucrt64 13.2) | host unit tests only | Optional. Install MSYS2 and `pacman -S mingw-w64-ucrt-x86_64-gcc`; add `ucrt64\bin` to `PATH`. |
| USB driver | none normally | flashing | The Xiao ESP32-C3 uses native USB-Serial/JTAG, which Windows 10/11 handles itself. It shows up as a COM port (check Device Manager > Ports). |

The host software's only Python dependency is `aiohttp` (installed by `host_setup`).

## 2. Hardware

Wiring and pin table: see [README.md](../README.md#wiring). Power the board over USB-C.

Flash over the Xiao USB-C port. If the port is not detected, hold **BOOT**, tap **RESET**, release
**BOOT** to enter the ROM bootloader, then flash again.

## 3. Build firmware

| Goal | Git Bash | CMD / PowerShell |
|---|---|---|
| Debug build (CLI + self diagnostics) -> `build/` | `Scripts/build.sh` | `Scripts\build.bat` |
| Release build -> `build_release/` | `Scripts/build.sh --release` | `Scripts\build.bat -Release` |
| Size report | `Scripts/build.sh size` | `Scripts\build.bat size` |
| Clean / full clean | `Scripts/build.sh clean` / `fullclean` | `Scripts\build.bat clean` / `fullclean` |
| menuconfig | `Scripts/build.sh menuconfig` | `Scripts\build.bat menuconfig` |

Notes:

* Do **not** run `idf.py` from Git Bash: ESP-IDF refuses to run in MSYS shells. `build.sh` forwards
  to `build.ps1`, which sources `export.ps1` and clears `MSYSTEM`.
* The first build creates `sdkconfig` (`idf.py set-target esp32c3`) from `sdkconfig.defaults`. After
  editing `sdkconfig.defaults`, delete `sdkconfig` (or run `fullclean`) so it is regenerated.
* Outputs: `build/led_strip_fw.bin` (app image, also the file for WiFi OTA),
  `build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`.
* Last measured size: 0xDD550 bytes (about 0.9 MiB) in a 1.875 MiB OTA slot.

## 4. Flash and monitor

Replace `COM11` with your port.

```sh
Scripts/build.sh flash COM11            # build + flash bootloader, partition table, app
Scripts/build.sh monitor COM11          # serial log (exit with Ctrl+])
Scripts/build.sh flash-monitor COM11    # both
Scripts/build.sh flash COM11 --release  # flash the release image
```

CMD / PowerShell: `Scripts\build.bat flash -Port COM11` (add `-Release` for release).

* The first flash must be over USB. Later updates can go over WiFi (section 6).
* `idf.py flash` does not touch the `calib` and `settings` partitions, so saved calibration and WiFi
  credentials survive a re-flash. Use `idf.py erase-flash` (from an ESP-IDF shell) to wipe them.
* Release builds have no debug CLI and no self-diagnostics tab.

## 5. First boot and network

1. The device starts the soft-AP `LEDSTRIP-XXXX` (password `ledstrip123`, IP `192.168.4.1`).
2. Join that WiFi from the PC, start the host (section 6) and connect to `192.168.4.1`.
3. In the **Settings** tab enter your WiFi SSID and password, then reboot the device.
4. If the station connection fails, the device falls back to the soft-AP.
5. On your LAN, use **Scan** in the Main tab (UDP discovery on port 4211) or type the device IP.
   The command channel is TCP 4210.

## 6. Host software

```sh
Scripts/host_setup.sh                    # once: creates Host/.venv, installs aiohttp
Scripts/host_run.sh                      # UI at http://127.0.0.1:8080
Scripts/host_run.sh --device 192.168.4.1 # connect to a device on start
```

CMD: `Scripts\host_setup.bat`, `Scripts\host_run.bat`.

Options (`python -m ledhost`, run from `Host/`): `--host` (use `0.0.0.0` to reach the UI from a phone),
`--port`, `--device ip[:port]`, `-v` for debug logs. Settings persist in `Host/host_settings.json`.

Tabs: Main (discovery, live view, algorithm, brightness), Calibration, Settings, Admin (CLI,
Firmware Update, Self Diagnostics; CLI and diagnostics in debug builds only).

**WiFi firmware update:** build, then Admin > Firmware Update and upload `build/led_strip_fw.bin`.
The device reboots after about 5 s. A new image must confirm itself or the bootloader rolls back to
the previous slot.

**Calibration image:** `python Scripts/make_calib_image.py params.json -o calib.bin` builds an image
for Admin > Firmware Update > "Calibration region". `--decode calib.bin` converts it back to JSON.

## 7. Testing

```sh
Scripts/run_host_tests.sh               # DSP + pipeline stage tests, native gcc (needs gcc on PATH)
python Host/tools/sim_device.py         # simulated device on 127.0.0.1:4210 / :4211, no hardware
python Host/tools/api_test.py           # REST API test against the simulator
```

To try the UI without hardware: start `sim_device.py`, then `Scripts/host_run.sh --device 127.0.0.1`.

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `ESP-IDF not found at ...` | Install v5.3.1 to `%USERPROFILE%\esp\v5.3.1\esp-idf` or set `IDF_PATH`. |
| IDF complains about MSYS / MinGW | Use `Scripts/build.sh` or `build.bat`, not `idf.py` in Git Bash. |
| Port busy / access denied on flash | Close the serial monitor and any other program using the COM port. |
| Flash cannot connect | Enter the ROM bootloader: hold BOOT, tap RESET, release BOOT. |
| `Host/.venv missing` | Run `Scripts/host_setup.sh`. |
| Host cannot find the device | Check the PC is on the same network (or on `LEDSTRIP-XXXX`); try the IP directly. |
| `run_host_tests.sh`: `gcc: command not found` | Add the MSYS2 `ucrt64\bin` directory to `PATH`. |
