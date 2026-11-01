#!/usr/bin/env bash
# Write the WiFi credentials (settings partition) over the Xiao USB-C port.
# Needed once per device for release firmware; debug firmware can also set them from the host.
#
#   Scripts/flash_settings.sh COM5 --ssid HomeWiFi --password secret [--name ledstrip]
#   Scripts/flash_settings.sh COM5 settings.json
#
# Replaces all stored settings (others fall back to defaults); calibration and firmware are kept.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ $# -ge 2 ] || { sed -n '2,8p' "$0"; exit 1; }
port="$1"; shift
img="$here/../build/settings.bin"
mkdir -p "$here/../build"
PY="${PYTHON:-python}"
command -v "$PY" >/dev/null 2>&1 || PY=py
"$PY" "$here/make_settings_image.py" "$@" -o "$img"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/build.ps1")" \
    flash-settings -Port "$port" -File "$(cygpath -w "$img")"
