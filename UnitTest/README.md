# Unit tests

A full on-target unit test suite is not part of the initial version.

Two test files are built together with the native PC compiler
(`Scripts/firmware/run_firmware_unit_tests.sh`, needs `gcc` in PATH, e.g. MSYS2 ucrt64;
output in `build/firmware/unit_tests/`). The source list is in `Scripts/firmware/sub/unit_tests.ps1`.
Real firmware sources under test:

* `firmware/Interface/signal_processing`, `common/crc32.c`, `led_patterns/` (`led_color.c`, `led_patterns.c`)
* `firmware/Interface/config_manager/config_manager.c` and `cli/cli.c`
* `firmware/App/audio_processing/algorithms.c`, `music_profiles.c`, `spectrum_taps.c` and all `stages/` except the front-end, which needs the
  microphone driver (the registry test uses a stub descriptor instead)

`host/test_host.c` is the DSP / pipeline regression net (and `main`). `host/test_modules.c`
covers the non-DSP modules. The ESP-IDF parts that `config_manager.c` needs (flash partition,
FreeRTOS mutex and tick, logging) are replaced by stand-ins in `host/mocks/`; the flash is RAM, so power
loss can be simulated, and the tick is set by the test.

It feeds synthetic audio through the real stage code and checks:

* CRC-32, fixed-point log/exp/sqrt/RMS, baseline removal, interpolation
* FFT peak position, magnitude and leakage
* Biquad responses (LP -3 dB point, HP, peaking EQ)
* Envelope dBFS accuracy, AGC levelling and noise gate
* Spectrum band selection and spectral centroid for a 1 kHz tone
* Beat detector on synthetic kick patterns (95 / 120 / 128 / 140 BPM, and missing kicks):
  tempo estimate, beat count, alignment with the kick (mean detection delay is printed)
* Render stages: which and how many LEDs light up
* Melody tracker: pitch of a harmonic lead line (within 0.5 semitone), one note start per
  note, no "voice" on drums only
* Music show (all analysis + render stages chained): groove -> GROOVE with both all-LED and
  some-LED beats; lead line -> LEAD with only a few LEDs lit; tom fill -> FILL within 0.3 s and
  back to GROOVE; 8 s build-up -> BUILD, then DROP when the bass returns; silence -> dark; auto
  genre (four-on-the-floor -> EDM, 808 + hi-hat rolls -> trap); a fixed genre parameter wins.
  `SHOW_DEBUG=1` / `HIT_DEBUG=1` print the analysis and every detected hit / beat

`test_modules.c` checks:

* Config store: write / read / persist across a simulated reboot, A/B slot alternation and
  sequence numbers, record replace / remove / clear / reload, value size limits
* Power loss: a torn commit keeps the previous value; a corrupt newest slot falls back to the
  older slot; two corrupt slots give defaults
* Region images (`get_image`, `validate_image`, `load_image`): CRC, magic, store id, truncation
* CLI: argument parsing (white space, quotes, argument limit), unknown command, usage on error,
  `help`, tiny / zero-size output buffers
* LED patterns: names, every pattern lights LEDs and stays inside the buffer, error code
  indication (colour, blinking, cap, fatal background), progress bar, boot animation end
* Registry: stage ids equal their position (persistent calibration keys), parameter ranges and
  defaults, unique parameter names, algorithms start with the front-end, end with a render stage,
  and have each stage's inputs earlier in the list
* Spectrum taps (live view): at most 4 taps, 72-point layout covering FFT bins 1..128, no
  spectrum before a full 256-sample window, a half-scale 1 kHz sine at the right point and
  -6 dBFS (+-1 dB), unselected and stale taps give nothing, mask 0 stops capturing

Not covered (needs hardware): drivers (I2S, WS2812, GDMA), WiFi / network, OTA, the real-time
task, calibration and mode manager (depend on the above), `stage_frontend`, the comm task's
protocol handling (covered against the simulator by `software/tools/proto_test.py`). See `Testing/testing.md`.

Adding a test: put it in `test_modules.c` (modules) or `test_host.c` (DSP / stages). A new
source file under test must be added to `Scripts/firmware/sub/unit_tests.ps1`; if it needs more
ESP-IDF headers, add a stand-in to `host/mocks/`.
