#!/usr/bin/env bash
# Build (incremental) and flash over USB-C. Usage: flash.sh [-COM11] [-release|-debug] [-nobuild]
# Without -COMx the only connected serial port is used.
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/flash.ps1")" "$@"
