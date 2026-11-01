# Unit tests

A full on-target unit test suite is not part of the initial version.

`host/test_host.c` is a quick regression net for the signal processing. It is built with
the native PC compiler (`Scripts/firmware/run_firmware_unit_tests.sh`, needs `gcc` in PATH, e.g. MSYS2 ucrt64)
from the pure-C modules:

* `firmware/Interface/signal_processing`
* `firmware/Interface/common/crc32.c`
* `firmware/Interface/led_patterns/led_color.c`
* all `firmware/App/audio_processing/stages` except the front-end, which needs the microphone driver

It feeds synthetic audio through the real stage code and checks:

* CRC-32, fixed-point log/exp/sqrt/RMS, baseline removal, interpolation
* FFT peak position, magnitude and leakage
* Biquad responses (LP -3 dB point, HP, peaking EQ)
* Envelope dBFS accuracy, AGC levelling and noise gate
* Spectrum band selection and spectral centroid for a 1 kHz tone
* Beat detector on synthetic kick patterns (95 / 120 / 128 / 140 BPM, and missing kicks):
  tempo estimate, beat count, alignment with the kick (mean detection delay is printed)
* Render stages: which and how many LEDs light up
