# Future plans

## 1. Host connection without losing internet (change request)

**Problem:** with the default soft-AP mode, the PC running the host software has to join
`LEDSTRIP-XXXX`. On a PC with a single WiFi adapter, that disconnects the PC from the home
network, so it loses internet.

**Already possible today (workaround):** the firmware supports station mode. Connect to the
soft-AP once, then in the Settings tab set wifi_mode = auto (or station) and enter the home
WiFi SSID and password, and reboot. The device then joins the home network: the host finds it
with Scan (UDP discovery / beacons) and the PC keeps its internet. If the home network is
unreachable, the device falls back to the soft-AP after 15 s.

**Planned fixes:**

1. **Provisioning over USB**: the host sends the WiFi credentials over the Xiao USB-C
   (USB-Serial/JTAG) port, so the PC never has to join the device AP. This is also useful as
   a recovery path when WiFi is misconfigured.
2. **Host over USB as an alternative transport**: the same binary protocol over
   USB-Serial/JTAG. The host could then work with no WiFi at all (no internet loss, no
   pairing).
3. **Captive-portal style setup page** served by the device on the soft-AP (from a phone),
   to enter home WiFi credentials once.
4. Optional **mDNS** name (`ledstrip.local`) so the host can reach the device in station mode
   without scanning.
5. Host UI: show clearly whether the device is in AP or station mode, and warn before
   suggesting a switch to the device AP.

## 2. Remove Espressif libraries - fully independent firmware

Today ESP-IDF provides the build system, FreeRTOS, the WiFi/TCP-IP stack (closed WiFi blobs +
lwIP), the 2nd-stage bootloader, OTA/partition handling, flash access, interrupt allocation
and logging. Our own code already covers the I2S, SPI/WS2812, GDMA, watchdog, DSP, storage
format, protocol and CLI. Plan to become independent:

| Step | Replace | With |
|---|---|---|
| 1 | IDF CMake / idf.py | own Makefile/CMake with `riscv32-esp-elf-gcc`, own linker script (IRAM/DRAM/flash-mapped sections), esptool or own flasher for images |
| 2 | IDF startup, interrupt allocator, logging | own `_start` / vector table (RISC-V `mtvec`, interrupt matrix), ROM `esp_rom_printf` or own USB-Serial/JTAG console |
| 3 | FreeRTOS | bare-metal super-loop + interrupts (mic DMA ISR drives the pipeline), or a tiny own scheduler for the comm task |
| 4 | WiFi / lwIP | **host transport over USB-Serial/JTAG** (fully own code; this also solves item 1). Wireless without Espressif code is not possible on the ESP32-C3: the radio needs Espressif's closed binary blobs. Options: keep WiFi as an optional, isolated module, or use an external radio |
| 5 | IDF 2nd-stage bootloader + OTA | own minimal bootloader: check an image header + CRC in slot A/B, map flash cache (MMU), jump. Update over USB first |
| 6 | esp_partition / spi_flash | own flash driver using the ROM SPI flash functions (`esp_rom_spiflash_*`) with cache disable/enable |
| 7 | MMU / cache / clock setup (done by the IDF bootloader) | own clock tree setup (PLL 160 MHz, APB 80 MHz) and cache/MMU configuration |

Expected result: smaller image (today ~0.9 MB, mostly WiFi/lwIP/mbedTLS), faster boot,
deterministic timing, no dependency on IDF releases. Trade-off: no wireless unless the WiFi
module stays (as an optional build flavour).

## 3. Future features

### Audio / algorithms

* Play music via the website (original spec): stream audio from the browser to a speaker, or
  play a reference track while calibrating so the response can be measured.
* Feedback / feed-forward support in the pipeline (original spec): let stages read the
  previous block's outputs (e.g. render brightness -> AGC), or a feature from a later stage.
  Planned via an `audio_frame_t` history / "previous frame" pointer and explicit stage
  dependencies.
* More stages: spectral flux onset across all bands, chroma / key detection, vocal-band
  emphasis, silence detection with auto-switch to a pattern, peak limiter / compressor.
* More render stages: VU meter from the centre, BPM-synced colour palettes, comets launched on
  beats, ripple from the beat position on the circular layout, palette per frequency band.
* Algorithm switch by music character (auto mode).
* Multiple parameter sets (presets) per algorithm, selectable from the host.
* Stage instances with their own parameters (today a stage id has one parameter set shared by
  all algorithms using it).
* Run-time algorithm composition from the host (choose stages without rebuilding).

### Host software

* Live charts: level, onset function, band history, BPM over time.
* Parameter A/B comparison and undo history.
* Firmware update progress shown with the device log.
* Playwright-based automated UI tests; camera-based LED verification (see `Testing/testing.md`).
* Packaging as a single executable (PyInstaller).
* Optional authentication for the device protocol (currently open on the local network).

### Firmware / system

* On-target unit tests (Unity) for drivers and the config store.
* Protocol: event push (errors, beats) instead of polling.
* Power: brightness auto-limit by supply detection; idle dimming after silence.
* Brown-out handling for USB supplies when many LEDs are on.
* Support for longer strips (> 60 LEDs, multiple DMA descriptors) and RGBW (SK6812).
* Use the second GDMA-capable peripheral for a second strip.
* Reduce the start-up `audio_overrun` warning (skip the overrun check on the first block
  after a restart).
