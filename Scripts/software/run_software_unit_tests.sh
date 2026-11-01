#!/usr/bin/env bash
# Host protocol tests against the device simulator.
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/unit_tests.ps1")" "$@"
