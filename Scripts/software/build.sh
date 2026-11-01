#!/usr/bin/env bash
# Check the host software (byte-compile, no packaging needed). Output: build\software\build_files.
here="$(cd "$(dirname "$0")" && pwd)"
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$here/sub/build.ps1")" "$@"
