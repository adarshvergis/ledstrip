<#
  Build and run the native (PC) DSP / pipeline unit tests with gcc (pure C modules only).
  Usage (via Scripts\firmware\run_firmware_unit_tests.bat / .sh)
  Output: build\firmware\unit_tests\test_host.exe
  Needs gcc on PATH (e.g. MSYS2 ucrt64).
#>
. "$PSScriptRoot\common.ps1"

if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) { throw "gcc not found on PATH (install MSYS2 ucrt64 gcc, see Docs/setup.md)" }
$out = Join-Path $BuildRoot "unit_tests"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$stages = "stage_envelope", "stage_agc", "stage_spectrum", "stage_beat", "stage_filter",
          "stage_render_level", "stage_render_beat_flash", "stage_render_spectrum", "stage_render_beat_segments"
$srcs = @("UnitTest/host/test_host.c",
          "Interface/signal_processing/dsp.c", "Interface/signal_processing/dsp_fft.c",
          "Interface/common/crc32.c", "Interface/led_patterns/led_color.c")
$srcs += $stages | ForEach-Object { "App/audio_processing/stages/$_.c" }

Set-Location $Root
$exe = "$out\test_host.exe"
& gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare `
    -IApp -IApp/audio_processing -IInterface/common -IInterface/signal_processing -IInterface/led_strip -IInterface/led_patterns `
    @srcs -lm -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD OF UNIT TESTS FAILED"; exit $LASTEXITCODE }
& $exe
exit $LASTEXITCODE
