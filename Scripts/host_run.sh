#!/usr/bin/env bash
# Start the host software (web UI on http://127.0.0.1:8080 by default).
# Usage: Scripts/host_run.sh [--host 127.0.0.1] [--port 8080] [--device <ip[:port]>]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="$ROOT/Host"
if [ -x "$HOST/.venv/Scripts/python.exe" ]; then VPY="$HOST/.venv/Scripts/python.exe"
elif [ -x "$HOST/.venv/bin/python" ]; then VPY="$HOST/.venv/bin/python"
else echo "error: Host/.venv missing - run Scripts/host_setup.sh first" >&2; exit 1; fi
cd "$HOST"
exec "$VPY" -m ledhost "$@"
