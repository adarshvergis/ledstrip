#!/usr/bin/env bash
# Create the host software virtual environment (Host/.venv) and install dependencies.
# Usage: Scripts/host_setup.sh            (works in Git Bash on Windows, Linux and macOS)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="$ROOT/Host"

PY="${PYTHON:-}"
if [ -z "$PY" ]; then
  for cand in python3 python py; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import sys; sys.exit(sys.version_info < (3, 9))' 2>/dev/null; then
      PY="$cand"; break
    fi
  done
fi
[ -n "$PY" ] || { echo "error: Python 3.9+ not found (set PYTHON=/path/to/python)" >&2; exit 1; }

echo "Using $("$PY" --version 2>&1) ($PY)"
"$PY" -m venv "$HOST/.venv"
if [ -x "$HOST/.venv/Scripts/python.exe" ]; then VPY="$HOST/.venv/Scripts/python.exe"; else VPY="$HOST/.venv/bin/python"; fi
"$VPY" -m pip install --upgrade pip >/dev/null
"$VPY" -m pip install -r "$HOST/requirements.txt"
echo "Host environment ready. Start it with: Scripts/host_run.sh [--device <ip>]"
