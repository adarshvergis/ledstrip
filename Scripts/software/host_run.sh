#!/usr/bin/env bash
# Start the desktop host app. Usage: host_run.sh [-device <ip>] [-debug]
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/host_run.ps1")" "$@"
