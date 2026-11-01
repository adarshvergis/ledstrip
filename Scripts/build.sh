#!/usr/bin/env bash
# Firmware build wrapper for Git Bash / MSYS on Windows.
# ESP-IDF does not support MSYS shells directly, so this forwards to build.ps1.
#
#   Scripts/build.sh                      # debug build (CLI, diagnostics, settings) -> build/, dist/debug/
#   Scripts/build.sh --release            # release build -> build_release/, dist/release/
#   Scripts/build.sh flash COM5           # build + flash
#   Scripts/build.sh flash-monitor COM5
#   Scripts/build.sh monitor COM5
#   Scripts/build.sh size|clean|fullclean|menuconfig
# Shortcuts: Scripts/build_debug.sh, Scripts/build_release.sh. WiFi credentials: Scripts/flash_settings.sh
set -e
here="$(cd "$(dirname "$0")" && pwd)"
action="build"
port=""
release=""
for a in "$@"; do
    case "$a" in
        --release) release="-Release" ;;
        COM*|/dev/*) port="-Port $a" ;;
        *) action="$a" ;;
    esac
done
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/build.ps1")" $action $port $release
