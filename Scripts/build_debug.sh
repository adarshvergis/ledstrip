#!/usr/bin/env bash
# Debug firmware (INTERNAL_TEST_BUILD: CLI, self diagnostics, settings/WiFi from the host).
# Output: build/ and dist/debug/.   Usage: Scripts/build_debug.sh [build|flash|flash-monitor] [COM5]
exec "$(dirname "$0")/build.sh" "$@"
