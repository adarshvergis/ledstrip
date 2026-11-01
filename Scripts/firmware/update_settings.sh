#!/usr/bin/env bash
# Write WiFi credentials over USB-C. Usage: update_settings.sh -ssid <name> -password <pw> [-name <device>] [-COM11]
#    or: update_settings.sh -file settings.json [-COM11]    (replaces all stored settings)
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/update_settings.ps1")" "$@"
