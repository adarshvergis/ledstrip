#!/usr/bin/env python3
"""Convert calibration parameters between JSON and a calibration region slot image.

Encode (JSON -> image)::

    python Scripts/firmware/make_calib_image.py params.json -o calib.bin [--sequence 1]

    params.json = {"params": [{"stage": 0, "param": 1, "value": 40.0}, ...]}
    (extra keys such as "name"/"stage_name" are ignored, so a file exported
    from the host app can be used directly)

Decode (image -> JSON)::

    python Scripts/firmware/make_calib_image.py --decode calib.bin [-o params.json]

The image format is described in Docs/protocol.md section 5 (store_id 0,
records keyed ``(stage_id << 8) | param_id``, f32 values, crc32 of records).
Upload the image from the host app: Firmware tab > "Calibration region image"
(the firmware rewrites the sequence number).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Host"))

from ledhost import protocol as P  # noqa: E402


def encode(src: Path, out: Path, sequence: int) -> None:
    doc = json.loads(src.read_text(encoding="utf-8"))
    params = doc["params"] if isinstance(doc, dict) else doc
    if not isinstance(params, list):
        raise SystemExit("error: JSON must be {\"params\": [...]} or a list")
    seen: set[tuple[int, int]] = set()
    for p in params:
        try:
            sid, pid = int(p["stage"]), int(p["param"])
            float(p["value"])
        except (KeyError, TypeError, ValueError):
            raise SystemExit(f"error: malformed entry {p!r} (need stage, param, value)") from None
        if not (0 <= sid <= 255 and 0 <= pid <= 255):
            raise SystemExit(f"error: stage/param out of range in {p!r}")
        if (sid, pid) in seen:
            raise SystemExit(f"error: duplicate entry for stage {sid} param {pid}")
        seen.add((sid, pid))
    img = P.encode_calib_image(params, sequence)
    out.write_bytes(img)
    print(f"wrote {out} ({len(img)} bytes, {len(params)} params, sequence {sequence})")


def decode(src: Path, out: Path | None) -> None:
    img = src.read_bytes()
    try:
        info = P.decode_region_image(img)
        params = P.decode_calib_image(img)
    except P.ProtocolError as e:
        raise SystemExit(f"error: {e}") from None
    doc = {"sequence": info["sequence"], "params": params}
    text = json.dumps(doc, indent=2) + "\n"
    if out is None:
        sys.stdout.write(text)
    else:
        out.write_text(text, encoding="utf-8")
        print(f"wrote {out} ({len(params)} params, sequence {info['sequence']})", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", type=Path, help="params JSON file to encode")
    ap.add_argument("--decode", type=Path, metavar="IMAGE", help="decode a calibration image to JSON")
    ap.add_argument("-o", "--output", type=Path, help="output file (encode default: calib_image.bin; decode default: stdout)")
    ap.add_argument("--sequence", type=int, default=1, help="slot sequence number (default 1)")
    args = ap.parse_args()
    if args.decode:
        decode(args.decode, args.output)
    elif args.input:
        encode(args.input, args.output or Path("calib_image.bin"), args.sequence)
    else:
        ap.error("give a JSON file to encode or --decode IMAGE")


if __name__ == "__main__":
    main()
