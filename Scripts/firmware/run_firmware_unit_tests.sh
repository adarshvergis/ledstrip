#!/usr/bin/env bash
# Native DSP / pipeline unit tests (gcc on PATH). Output: build\firmware\unit_tests.
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/unit_tests.ps1")" "$@"
