#!/usr/bin/env bash
# Serial console (Ctrl+] exits). Usage: monitor.sh [-COM11] [-release|-debug]
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/monitor.ps1")" "$@"
