#!/usr/bin/env python3
"""Build the settings partition image (WiFi credentials etc.) for a first flash over USB.

Release firmware cannot change WiFi credentials from the host, so they are written once
with esptool (Scripts/flash_settings.sh does both steps)::

    python Scripts/make_settings_image.py --ssid HomeWiFi --password secret [--name ledstrip] -o settings.bin
    python Scripts/make_settings_image.py settings.json -o settings.bin

    settings.json = {"sta_ssid": "HomeWiFi", "sta_password": "secret", "device_name": "ledstrip",
                     "num_leds": 60, "max_current_ma": 500, "brightness": 160, ...}

Names are the setting names of Docs/protocol.md (cmd 0x20). Settings not given use the
firmware defaults. The output is the whole 8 KiB A/B area: slot A holds the image, slot B
is left erased so slot A is the one loaded. Flashing it replaces all stored settings.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "Host"))

from ledhost import protocol as P  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", type=Path, help="settings JSON file")
    ap.add_argument("--ssid", help="home WiFi SSID (sta_ssid)")
    ap.add_argument("--password", help="home WiFi password (sta_password, empty = open network)")
    ap.add_argument("--name", help="device name (device_name)")
    ap.add_argument("-o", "--output", type=Path, default=Path("settings.bin"), help="output file (settings.bin)")
    args = ap.parse_args()

    values: dict[str, int | str] = {}
    if args.input:
        values.update(json.loads(args.input.read_text(encoding="utf-8")))
    for name, v in (("sta_ssid", args.ssid), ("sta_password", args.password), ("device_name", args.name)):
        if v is not None:
            values[name] = v
    if not values.get("sta_ssid"):
        ap.error("no sta_ssid given (--ssid or JSON): the device would not join any network")
    try:
        img = P.encode_settings_region(values)
    except ValueError as e:
        raise SystemExit(f"error: {e}") from None
    args.output.write_bytes(img)
    print(f"wrote {args.output} ({len(img)} bytes) for address 0x{P.SETTINGS_REGION_ADDR:X}: "
          + ", ".join(k for k in values))


if __name__ == "__main__":
    main()
