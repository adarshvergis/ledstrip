"""Entry point: ``python -m ledhost [--host H] [--port P] [--device IP[:PORT]]``."""

from __future__ import annotations

import argparse
import logging

from aiohttp import web

from .server import create_app


def main() -> None:
    ap = argparse.ArgumentParser(prog="ledhost", description="LED strip host software (web UI + device API)")
    ap.add_argument("--host", default="127.0.0.1", help="address for the web UI to listen on (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=8080, help="web UI port (default 8080)")
    ap.add_argument("--device", default=None, help="device address ip[:port] to use at start-up")
    ap.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    logging.getLogger("aiohttp.access").setLevel(logging.WARNING)
    print(f"LED strip host UI: http://{args.host}:{args.port}/")
    web.run_app(create_app(args.device), host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
