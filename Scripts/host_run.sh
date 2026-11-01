#!/usr/bin/env bash
# Start the desktop host app (Python 3.9+ with tkinter, no other packages).
# Usage: Scripts/host_run.sh [--device <ip>] [--debug]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-}"
if [ -z "$PY" ]; then
  for cand in python3 python py; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import sys, tkinter; sys.exit(sys.version_info < (3, 9))' 2>/dev/null; then
      PY="$cand"; break
    fi
  done
fi
[ -n "$PY" ] || { echo "error: Python 3.9+ with tkinter not found (set PYTHON=/path/to/python)" >&2; exit 1; }
exec "$PY" "$ROOT/Host/ledapp.py" "$@"
