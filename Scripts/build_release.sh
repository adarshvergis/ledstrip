#!/usr/bin/env bash
# Release firmware (main view, calibration and firmware update only).
# Output: build_release/ and dist/release/.   Usage: Scripts/build_release.sh [build|flash|flash-monitor] [COM5]
exec "$(dirname "$0")/build.sh" --release "$@"
