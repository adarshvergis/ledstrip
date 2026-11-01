#!/usr/bin/env bash
# Build the firmware (ESP-IDF). Usage: build.sh [-release|-debug] [-clean] [-size] [-menuconfig]
# Output: build\firmware\<variant>\artefacts (.bin .hex .elf) and build_files.
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/build.ps1")" "$@"
