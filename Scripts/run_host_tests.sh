#!/usr/bin/env bash
# Build and run the host-side DSP / pipeline sanity tests with the native gcc.
# (Pure C modules only: signal_processing, CRC, colour helpers, processing stages.)
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build_host_tests"
mkdir -p "$out"

stages=(
    stage_envelope stage_agc stage_spectrum stage_beat stage_filter
    stage_render_level stage_render_beat_flash stage_render_spectrum stage_render_beat_segments
)
srcs=("$root/UnitTest/host/test_host.c"
      "$root/Interface/signal_processing/dsp.c"
      "$root/Interface/signal_processing/dsp_fft.c"
      "$root/Interface/common/crc32.c"
      "$root/Interface/led_patterns/led_color.c")
for s in "${stages[@]}"; do srcs+=("$root/App/audio_processing/stages/$s.c"); done

gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
    -I"$root/App" -I"$root/App/audio_processing" -I"$root/Interface/common" \
    -I"$root/Interface/signal_processing" -I"$root/Interface/led_strip" -I"$root/Interface/led_patterns" \
    "${srcs[@]}" -lm -o "$out/test_host"
"$out/test_host"
