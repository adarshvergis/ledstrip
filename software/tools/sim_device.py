"""LED strip device simulator (server side of Docs/protocol.md).

Lets the host software be developed and tested without hardware:

* TCP command server (default 127.0.0.1:4210), one client at a time
  (a new connection replaces the old one),
* UDP live stream (LIVE_SUBSCRIBE) pushed to the client's address,
* UDP discovery responder (default 127.0.0.1:4211),
* four fake algorithms built from five stages with FLOAT/INT/BOOL/ENUM params,
* animated fake live data (beat at ~122 BPM, spectrum bands, LED strip),
* debug build (default, --flags 15): settings TLV store, errors, CLI echo, diagnostics;
  release build (--flags 0): those commands answer ERR_UNKNOWN_CMD like the firmware,
* fake firmware update (APP and CALIB targets, real CRC/offset checks).

Usage (from the repository root)::

    python software/tools/sim_device.py [--bind 127.0.0.1] [--flags 15|0] [--leds 60] [--fw-delay 0.005]
"""

from __future__ import annotations

import argparse
import asyncio
import colorsys
import logging
import math
import random
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ledhost import protocol as P  # noqa: E402
from ledhost.protocol import Cmd, ParamType, Status  # noqa: E402

log = logging.getLogger("sim")

FW_VERSION = "1.0.0-sim"
BUILD_DATE = time.strftime("%Y-%m-%d %H:%M")
MAC = "aa:bb:cc:00:11:22"
NUM_PATTERNS = 5
NUM_BANDS = 8


def _p(pid: int, ptype: int, mn: float, mx: float, df: float, step: float, name: str, unit: str) -> P.ParamSchema:
    return P.ParamSchema(pid, ptype, mn, mx, df, step, name, unit)


STAGES: dict[int, P.StageSchema] = {
    0: P.StageSchema(0, "dc_block", "DC removal / high-pass input filter", [
        _p(0, ParamType.BOOL, 0, 1, 1, 1, "enable", ""),
        _p(1, ParamType.FLOAT, 5, 300, 40, 1, "cutoff", "Hz"),
    ]),
    1: P.StageSchema(1, "agc", "Automatic gain control", [
        _p(0, ParamType.INT, 1000, 30000, 12000, 100, "target_level", ""),
        _p(1, ParamType.FLOAT, 0.1, 500, 10, 0.1, "attack", "ms"),
        _p(2, ParamType.FLOAT, 10, 5000, 800, 10, "release", "ms"),
        _p(3, ParamType.FLOAT, 0, 40, 24, 0.5, "max_gain", "dB"),
    ]),
    2: P.StageSchema(2, "envelope", "Amplitude envelope follower and threshold", [
        _p(0, ParamType.FLOAT, 0, 1, 0.3, 0.01, "threshold", ""),
        _p(1, ParamType.ENUM, 0, 2, 1, 1, "response", "Hard|Soft|Fade"),
        _p(2, ParamType.FLOAT, 1, 1000, 120, 1, "decay", "ms"),
    ]),
    3: P.StageSchema(3, "beat", "Spectral-flux beat / tempo detector", [
        _p(0, ParamType.FLOAT, 0.5, 5, 1.5, 0.05, "sensitivity", ""),
        _p(1, ParamType.INT, 40, 120, 70, 1, "min_bpm", "BPM"),
        _p(2, ParamType.INT, 100, 220, 180, 1, "max_bpm", "BPM"),
        _p(3, ParamType.INT, 50, 500, 150, 10, "hold_off", "ms"),
    ]),
    4: P.StageSchema(4, "bands", "Band splitter and LED segment mapper", [
        _p(0, ParamType.INT, 2, 16, 8, 1, "num_bands", ""),
        _p(1, ParamType.ENUM, 0, 3, 0, 1, "palette", "Rainbow|Fire|Ocean|Mono"),
        _p(2, ParamType.BOOL, 0, 1, 0, 1, "mirror", ""),
        _p(3, ParamType.FLOAT, 0, 1, 0.6, 0.01, "smoothing", ""),
    ]),
}

ALGORITHMS = [
    P.Algorithm(0, [0, 1, 2], "Amplitude Flash", "All LEDs follow the amplitude envelope"),
    P.Algorithm(1, [0, 1, 3], "Beat Pulse", "All LEDs flash on every detected beat"),
    P.Algorithm(2, [0, 1, 2, 4], "Spectrum Bars", "Band levels shown as LED segments"),
    P.Algorithm(3, [0, 1, 3, 4], "Beat Segments", "Beat lights a segment chosen by spectral centroid"),
]

DEFAULT_SETTINGS: dict[int, int | str] = {
    1: "LED Strip Sim", 3: "HomeWiFi", 4: "", 8: 1500, 9: 60,
    10: 0, 11: 0, 12: 0, 13: 160, 14: 0,
}
REBOOT_SETTINGS = {1, 3, 4}
DEBUG_HOST_CMDS = {Cmd.GET_SETTINGS, Cmd.SET_SETTING, Cmd.FACTORY_RESET_SETTINGS,
                   Cmd.GET_ERRORS, Cmd.CLEAR_ERRORS}
LIVE_LEASE_S = 3.0
LIVE_MAX_HZ = 100


class SimDevice:
    """All mutable device state plus the command dispatcher."""

    def __init__(self, flags: int, max_leds: int, ip: str, fw_delay: float) -> None:
        self.flags = flags
        self.max_leds = max_leds
        self.ip = ip
        self.fw_delay = fw_delay
        self.boot_time = time.monotonic()
        self.device_mode = P.DeviceMode.MAIN
        self.settings: dict[int, int | str] = dict(DEFAULT_SETTINGS)
        self.saved = {sid: [p.default for p in st.params] for sid, st in STAGES.items()}
        self.live = {sid: list(v) for sid, v in self.saved.items()}
        self.errors = [P.ErrorEntry(3, 1, 2, 1500, "MIC_DMA_OVERRUN"), P.ErrorEntry(7, 0, 1, 10, "WIFI_RECONNECT")]
        self.fw: dict | None = None
        self.calib_sequence = 1
        self.diag_max_loop_us = 0.0
        self.requests = 0
        self.offline_until = 0.0          # simulated boot time: refuse clients until then
        self.on_reboot = lambda: None     # set by TcpServer: drops the client connection
        self.stream: tuple[int, int] | None = None   # (udp_port, rate_hz) set by LIVE_SUBSCRIBE

    # -- helpers ---------------------------------------------------------------

    @property
    def uptime_ms(self) -> int:
        return int((time.monotonic() - self.boot_time) * 1000) & 0xFFFFFFFF

    @property
    def calib_dirty(self) -> bool:
        return any(P.round_f32(a) != P.round_f32(b)
                   for sid in self.live for a, b in zip(self.live[sid], self.saved[sid]))

    def num_leds(self) -> int:
        return min(int(self.settings[9]), self.max_leds)

    def info(self) -> P.DeviceInfo:
        active_err = max((e.code for e in self.errors if e.severity >= 2), default=0)
        return P.DeviceInfo(
            proto_version=P.PROTO_VERSION, build_flags=self.flags, device_mode=int(self.device_mode),
            run_mode=int(self.settings[10]), active_algorithm=int(self.settings[11]),
            num_algorithms=len(ALGORITHMS), active_pattern=int(self.settings[12]),
            num_patterns=NUM_PATTERNS, brightness=int(self.settings[13]),
            active_error_code=active_err, uptime_ms=self.uptime_ms, sample_rate_hz=16000,
            num_leds=self.num_leds(), hop_size=256, reset_reason=1, wifi_state=2, rssi_dbm=-58,
            calib_dirty=int(self.calib_dirty), fw_version=FW_VERSION, build_date=BUILD_DATE,
            device_name=str(self.settings[1]), ip_address=self.ip)

    def calib_image(self) -> bytes:
        params = [{"stage": sid, "param": pid, "value": v}
                  for sid, vals in self.saved.items() for pid, v in enumerate(vals)]
        return P.encode_calib_image(params, self.calib_sequence)

    # -- live data -------------------------------------------------------------

    def live_data(self) -> P.LiveData:
        t = time.monotonic() - self.boot_time
        n = self.num_leds()
        bri = int(self.settings[13]) / 255.0
        bpm = 122.0
        phase = (t * bpm / 60.0) % 1.0
        beat_count = int(t * bpm / 60.0) & 0xFFFF
        env = math.exp(-phase * 5.0)
        level = int(min(32767, 6000 + 22000 * env + random.uniform(-1500, 1500)))
        bands = []
        for b in range(NUM_BANDS):
            base = 0.8 * env if b < 2 else 0.35 + 0.3 * math.sin(t * (1.3 + b * 0.7) + b)
            bands.append(int(max(0.0, min(1.0, base + random.uniform(-0.05, 0.05))) * 32767))
        centroid = int((0.3 + 0.2 * math.sin(t * 0.4)) * 32767)
        leds = bytearray(n * 3)
        run_mode = int(self.settings[10])
        alg = int(self.settings[11])

        def put(i: int, rgb: tuple[float, float, float]) -> None:
            leds[i * 3:i * 3 + 3] = bytes(int(max(0, min(1, c)) * 255 * bri) for c in rgb)

        if self.device_mode == P.DeviceMode.FW_UPDATE:
            done = self.fw["next"] / max(1, self.fw["size"]) if self.fw else 0
            for i in range(n):
                put(i, (0, 0.3, 1) if i < done * n else (0, 0, 0))
        elif run_mode == P.RunMode.PATTERN:
            pat = int(self.settings[12])
            for i in range(n):
                if pat == 0:      # rainbow
                    rgb = colorsys.hsv_to_rgb((i / n + t * 0.2) % 1, 1, 1)
                elif pat == 1:    # comet
                    d = (t * 30 - i) % n
                    rgb = (1, 0.4, 0.1) if d < 1 else tuple(c * math.exp(-d / 4) for c in (1, 0.4, 0.1))
                elif pat == 2:    # breathing
                    rgb = (0.2, 0.2, 1 * (0.5 + 0.5 * math.sin(t * 2)))
                elif pat == 3:    # theater chase
                    rgb = (1, 1, 1) if (i + int(t * 8)) % 3 == 0 else (0, 0, 0)
                else:             # static warm white
                    rgb = (1, 0.75, 0.45)
                put(i, rgb)
        else:
            hue = (t * 0.05) % 1
            if alg == 0:
                for i in range(n):
                    put(i, colorsys.hsv_to_rgb(hue, 1, level / 32767))
            elif alg == 1:
                for i in range(n):
                    put(i, colorsys.hsv_to_rgb(hue, 0.8, env if env > 0.3 else 0))
            elif alg == 2:
                seg = max(1, n // NUM_BANDS)
                for i in range(n):
                    b = min(NUM_BANDS - 1, i // seg)
                    lit = (i % seg) / seg < bands[b] / 32767
                    put(i, colorsys.hsv_to_rgb(b / NUM_BANDS, 1, 1) if lit else (0, 0, 0))
            else:
                centre = int(centroid / 32767 * n)
                width = int(4 + 10 * env)
                for i in range(n):
                    put(i, colorsys.hsv_to_rgb(centroid / 32767, 1, 1) if abs(i - centre) < width else (0, 0, 0))
        return P.LiveData(self.uptime_ms, level, int(bpm * 10), beat_count, centroid, int(self.device_mode),
                          bands, bytes(leds))

    # -- dispatcher ------------------------------------------------------------

    async def handle(self, cmd: int, payload: bytes) -> tuple[int, bytes, bool]:
        """Process one request -> (status, payload, close_connection_after)."""
        self.requests += 1
        t0 = time.perf_counter()
        try:
            status, body, close = await self._dispatch(cmd, payload)
        except (P.ProtocolError, struct.error, IndexError):
            status, body, close = Status.ERR_BAD_LEN, b"malformed payload", False
        self.diag_max_loop_us = max(self.diag_max_loop_us, (time.perf_counter() - t0) * 1e6)
        return status, body, close

    def _err(self, status: int, msg: str = "") -> tuple[int, bytes, bool]:
        return status, msg.encode("ascii"), False

    async def _dispatch(self, cmd: int, pl: bytes) -> tuple[int, bytes, bool]:
        OK = Status.OK
        mode = self.device_mode
        if (cmd in DEBUG_HOST_CMDS and not self.flags & 8) or (cmd == Cmd.CLI_EXEC and not self.flags & 2) \
                or (cmd in (Cmd.GET_DIAG, Cmd.RESET_DIAG) and not self.flags & 4):
            return self._err(Status.ERR_UNKNOWN_CMD)    # not compiled into a release build
        fw_cmds = {Cmd.FW_DATA, Cmd.FW_END, Cmd.FW_ABORT}
        if mode == P.DeviceMode.FW_UPDATE and cmd not in fw_cmds | {Cmd.PING, Cmd.GET_INFO, Cmd.REBOOT}:
            return self._err(Status.ERR_WRONG_MODE, "firmware update in progress")
        calib_only = {Cmd.SET_PARAM, Cmd.SAVE_PARAMS, Cmd.RESET_PARAMS, Cmd.RELOAD_PARAMS}
        if cmd in calib_only and mode != P.DeviceMode.CALIBRATION:
            return self._err(Status.ERR_WRONG_MODE, "enter calibration mode first")

        if cmd == Cmd.PING:
            return OK, b"", False
        if cmd == Cmd.GET_INFO:
            return OK, self.info().encode(), False
        if cmd == Cmd.REBOOT:
            asyncio.get_running_loop().call_later(0.3, self._reboot)
            return OK, b"", True
        if cmd == Cmd.SET_DEVICE_MODE:
            if len(pl) != 1:
                return self._err(Status.ERR_BAD_LEN)
            if pl[0] not in (0, 1):
                return self._err(Status.ERR_BAD_ARG, "mode must be 0 or 1")
            if mode == P.DeviceMode.CALIBRATION and pl[0] == 0:
                self.live = {sid: list(v) for sid, v in self.saved.items()}
            self.device_mode = P.DeviceMode(pl[0])
            return OK, b"", False
        if cmd == Cmd.SET_RUN_CONFIG:
            if len(pl) != 4:
                return self._err(Status.ERR_BAD_LEN)
            rm, alg, pat, bri = pl
            if (rm != 0xFF and rm > 1) or (alg != 0xFF and alg >= len(ALGORITHMS)) \
                    or (pat != 0xFF and pat >= NUM_PATTERNS):
                return self._err(Status.ERR_BAD_ARG)
            for sid, v in ((10, rm), (11, alg), (12, pat), (13, bri)):
                if v != 0xFF:
                    self.settings[sid] = v
            return OK, b"", False
        if cmd == Cmd.GET_LIVE:
            return OK, self.live_data().encode(), False
        if cmd == Cmd.LIVE_SUBSCRIBE:
            if len(pl) != 4:
                return self._err(Status.ERR_BAD_LEN)
            port, hz, _ = struct.unpack("<HBB", pl)
            hz = min(hz, LIVE_MAX_HZ)
            self.stream = (port, hz) if port and hz else None
            return OK, bytes([hz if port else 0]), False
        if cmd == Cmd.GET_ALGORITHMS:
            return OK, P.encode_algorithms(ALGORITHMS), False
        if cmd == Cmd.GET_STAGE_LIST:
            return OK, P.encode_stage_list(sorted(STAGES)), False
        if cmd == Cmd.GET_STAGE_SCHEMA:
            if len(pl) != 1:
                return self._err(Status.ERR_BAD_LEN)
            if pl[0] not in STAGES:
                return self._err(Status.ERR_BAD_ARG, "unknown stage")
            return OK, P.encode_stage_schema(STAGES[pl[0]]), False
        if cmd == Cmd.GET_PARAMS:
            if len(pl) != 1:
                return self._err(Status.ERR_BAD_LEN)
            if pl[0] not in STAGES:
                return self._err(Status.ERR_BAD_ARG, "unknown stage")
            return OK, P.encode_params(pl[0], self.live[pl[0]]), False
        if cmd == Cmd.SET_PARAM:
            if len(pl) != 6:
                return self._err(Status.ERR_BAD_LEN)
            sid, pid, val = struct.unpack("<BBf", pl)
            if sid not in STAGES or pid >= len(STAGES[sid].params):
                return self._err(Status.ERR_BAD_ARG, "unknown stage/param")
            sch = STAGES[sid].params[pid]
            val = min(max(val, sch.min), sch.max)
            if sch.type in (ParamType.INT, ParamType.ENUM):
                val = float(round(val))
            elif sch.type == ParamType.BOOL:
                val = 1.0 if val >= 0.5 else 0.0
            self.live[sid][pid] = P.round_f32(val)
            return OK, struct.pack("<f", val), False
        if cmd == Cmd.SAVE_PARAMS:
            await asyncio.sleep(0.2)  # pretend flash write
            self.saved = {sid: list(v) for sid, v in self.live.items()}
            self.calib_sequence += 1
            return OK, b"", False
        if cmd == Cmd.RESET_PARAMS:
            if len(pl) != 1:
                return self._err(Status.ERR_BAD_LEN)
            targets = list(STAGES) if pl[0] == 0xFF else [pl[0]]
            if any(s not in STAGES for s in targets):
                return self._err(Status.ERR_BAD_ARG, "unknown stage")
            for s in targets:
                self.live[s] = [p.default for p in STAGES[s].params]
            return OK, b"", False
        if cmd == Cmd.RELOAD_PARAMS:
            self.live = {sid: list(v) for sid, v in self.saved.items()}
            return OK, b"", False
        if cmd == Cmd.GET_SETTINGS:
            return OK, P.encode_settings_tlv(self.settings), False
        if cmd == Cmd.SET_SETTING:
            if len(pl) < 2 or len(pl) != 2 + pl[1]:
                return self._err(Status.ERR_BAD_LEN)
            sid, data = pl[0], pl[2:]
            sdef = P.SETTINGS_BY_ID.get(sid)
            if sdef is None:
                return self._err(Status.ERR_BAD_ARG, "unknown setting")
            if sdef.type == P.SETTING_STR:
                value: int | str = data.decode("utf-8", "replace")
                if len(data) > sdef.size:
                    return self._err(Status.ERR_BAD_ARG, "string too long")
            else:
                if len(data) not in (1, 2, 4):
                    return self._err(Status.ERR_BAD_LEN)
                value = int.from_bytes(data, "little")
                hi = self.max_leds if sid == 9 else sdef.maximum
                if not sdef.minimum <= value <= hi:
                    return self._err(Status.ERR_BAD_ARG, f"out of range {sdef.minimum}..{hi}")
            self.settings[sid] = value
            return OK, bytes([1 if sid in REBOOT_SETTINGS else 0]), False
        if cmd == Cmd.FACTORY_RESET_SETTINGS:
            self.settings = dict(DEFAULT_SETTINGS)
            return OK, b"", False
        if cmd == Cmd.FW_BEGIN:
            if len(pl) != 9:
                return self._err(Status.ERR_BAD_LEN)
            target, size, crc = struct.unpack("<BII", pl)
            if target > 1 or size == 0 or (target == 1 and size > P.SLOT_SIZE):
                return self._err(Status.ERR_BAD_ARG)
            await asyncio.sleep(0.3)  # pretend partition erase
            self.fw = {"target": target, "size": size, "crc": crc, "next": 0, "buf": bytearray()}
            self.device_mode = P.DeviceMode.FW_UPDATE
            return OK, struct.pack("<H", 4096), False
        if cmd in fw_cmds and self.fw is None:
            return self._err(Status.ERR_WRONG_MODE, "no update in progress")
        if cmd == Cmd.FW_DATA:
            assert self.fw is not None
            if len(pl) < 5 or len(pl) > 4 + 4096:
                return self._err(Status.ERR_BAD_LEN)
            off = struct.unpack("<I", pl[:4])[0]
            if off != self.fw["next"] or off + len(pl) - 4 > self.fw["size"]:
                return self._err(Status.ERR_BAD_ARG, f"expected offset {self.fw['next']}")
            if self.fw_delay:
                await asyncio.sleep(self.fw_delay)
            self.fw["buf"] += pl[4:]
            self.fw["next"] = len(self.fw["buf"])
            return OK, struct.pack("<I", self.fw["next"]), False
        if cmd == Cmd.FW_END:
            assert self.fw is not None
            fw, self.fw = self.fw, None
            self.device_mode = P.DeviceMode.MAIN
            buf = bytes(fw["buf"])
            if len(buf) != fw["size"] or (zlib.crc32(buf) & 0xFFFFFFFF) != fw["crc"]:
                return self._err(Status.ERR_CRC, "size/CRC mismatch")
            if fw["target"] == P.FwTarget.APP:
                if buf[0] != 0xE9:
                    return self._err(Status.ERR_CRC, "invalid ESP image")
                log.info("APP image of %d bytes accepted", len(buf))
                return OK, b"", False
            try:
                params = P.decode_calib_image(buf)
            except P.ProtocolError as e:
                return self._err(Status.ERR_CRC, str(e))
            for p in params:
                if p["stage"] in STAGES and p["param"] < len(self.saved[p["stage"]]):
                    self.saved[p["stage"]][p["param"]] = P.round_f32(p["value"])
            self.live = {sid: list(v) for sid, v in self.saved.items()}
            self.calib_sequence += 1
            return OK, b"", False
        if cmd == Cmd.FW_ABORT:
            self.fw = None
            self.device_mode = P.DeviceMode.MAIN
            return OK, b"", False
        if cmd == Cmd.GET_CALIB_IMAGE:
            return OK, self.calib_image(), False
        if cmd == Cmd.CLI_EXEC:
            return OK, self._cli(pl.decode("ascii", "replace")).encode()[:4096], False
        if cmd in (Cmd.GET_DIAG, Cmd.RESET_DIAG):
            if cmd == Cmd.RESET_DIAG:
                self.diag_max_loop_us = 0.0
                return OK, b"", False
            return OK, self._diag().encode(), False
        if cmd == Cmd.GET_ERRORS:
            return OK, P.encode_errors(self.errors), False
        if cmd == Cmd.CLEAR_ERRORS:
            self.errors = []
            return OK, b"", False
        return self._err(Status.ERR_UNKNOWN_CMD)

    def _reboot(self) -> None:
        log.info("simulated reboot")
        self.on_reboot()
        self.offline_until = time.monotonic() + 1.5
        self.boot_time = time.monotonic()
        self.device_mode = P.DeviceMode.MAIN
        self.fw = None
        self.stream = None
        self.live = {sid: list(v) for sid, v in self.saved.items()}

    def _cli(self, line: str) -> str:
        parts = line.split()
        if not parts:
            return ""
        if parts[0] == "help":
            return ("commands:\n  help              this list\n  get_fw_version    firmware version\n"
                    "  uptime            uptime in ms\n  echo <text>       echo text back\n"
                    "  set_led_strip <colour> <start> <end>\n")
        if parts[0] == "get_fw_version":
            return FW_VERSION + "\n"
        if parts[0] == "uptime":
            return f"{self.uptime_ms} ms\n"
        if parts[0] == "echo":
            return " ".join(parts[1:]) + "\n"
        return f"sim: '{line}' (unknown command, try 'help')\n"

    def _diag(self) -> str:
        rows = [
            ("memory", "heap_free", f"{180000 + random.randint(-2000, 2000)}", "bytes"),
            ("memory", "heap_min_free", "171204", "bytes"),
            ("stack", "audio_task_hwm", "1320", "bytes"),
            ("stack", "net_task_hwm", "2410", "bytes"),
            ("timing", "audio_block", f"{2100 + random.randint(-150, 150)}", "us"),
            ("timing", "audio_block_max", "3480", "us"),
            ("timing", "led_refresh", "1810", "us"),
            ("timing", "cmd_handler_max", f"{self.diag_max_loop_us:.0f}", "us"),
            ("net", "requests", str(self.requests), ""),
            ("flash", "calib_sequence", str(self.calib_sequence), ""),
        ]
        return "".join("\t".join(r) + "\n" for r in rows)


class TcpServer:
    """Accepts one client at a time; a new connection replaces the previous one."""

    def __init__(self, dev: SimDevice) -> None:
        self.dev = dev
        self.current: asyncio.StreamWriter | None = None
        self.last_rx = 0.0
        dev.on_reboot = self._drop_current

    def _drop_current(self) -> None:
        if self.current is not None:
            self.current.close()

    async def client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        if time.monotonic() < self.dev.offline_until:
            writer.close()  # still "booting"
            return
        if self.current is not None and not self.current.is_closing():
            log.info("new client %s replaces previous one", peer)
            self.current.close()
        self.current = writer
        self.dev.stream = None
        log.info("client connected: %s", peer)
        self.last_rx = time.monotonic()
        streamer = asyncio.create_task(self._stream(writer, peer[0]))
        try:
            while True:
                hdr_b = await reader.readexactly(P.HEADER_SIZE)
                try:
                    hdr = P.decode_header(hdr_b)
                except P.ProtocolError as e:
                    log.warning("closing connection: %s", e)
                    break
                payload = await reader.readexactly(hdr.length) if hdr.length else b""
                self.last_rx = time.monotonic()
                status, body, close = await self.dev.handle(hdr.cmd, payload)
                writer.write(P.encode_frame(hdr.cmd | P.RESPONSE_FLAG, hdr.seq, body, status))
                await writer.drain()
                if close:
                    await asyncio.sleep(0.05)
                    break
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            streamer.cancel()
            if self.current is writer:
                self.dev.stream = None
            writer.close()
            log.info("client disconnected: %s", peer)

    async def _stream(self, writer: asyncio.StreamWriter, host: str) -> None:
        """Push LIVE_FRAME datagrams to the client while it is subscribed (like the firmware)."""
        loop = asyncio.get_running_loop()
        transport, _ = await loop.create_datagram_endpoint(asyncio.DatagramProtocol, family=socket.AF_INET)
        seq = 0
        due = time.monotonic()
        try:
            while not writer.is_closing():
                sub = self.dev.stream if self.current is writer else None
                now = time.monotonic()
                if sub is None:
                    due = now
                    await asyncio.sleep(0.02)
                    continue
                if now - self.last_rx > LIVE_LEASE_S:
                    log.info("live stream lease expired")
                    self.dev.stream = None
                    continue
                port, hz = sub
                if now >= due:
                    transport.sendto(P.encode_frame(P.LIVE_FRAME, seq, self.dev.live_data().encode()), (host, port))
                    seq = (seq + 1) & 0xFFFF
                    due = max(due + 1.0 / hz, now)    # absolute schedule: coarse OS timers do not lower the rate
                await asyncio.sleep(max(0.0, due - time.monotonic()))
        finally:
            transport.close()


class UdpDiscovery(asyncio.DatagramProtocol):
    def __init__(self, dev: SimDevice) -> None:
        self.dev = dev
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        if data.strip() == P.DISCOVER_REQUEST and self.transport is not None:
            line = P.format_discovery_line(str(self.dev.settings[1]), self.dev.ip, FW_VERSION, MAC)
            self.transport.sendto(line.encode(), addr)


async def main() -> None:
    ap = argparse.ArgumentParser(description="LED strip device simulator")
    ap.add_argument("--bind", default="127.0.0.1", help="address to bind TCP/UDP (default 127.0.0.1)")
    ap.add_argument("--tcp-port", type=int, default=P.TCP_PORT)
    ap.add_argument("--udp-port", type=int, default=P.UDP_PORT)
    ap.add_argument("--flags", type=int, default=15,
                    help="build_flags: 15 = debug build (CLI, diag, settings), 0 = release build (default 15)")
    ap.add_argument("--leds", type=int, default=60, help="LED_MAX_COUNT (default 60)")
    ap.add_argument("--fw-delay", type=float, default=0.005, help="artificial delay per FW_DATA chunk (s)")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s sim: %(message)s")

    ip = "127.0.0.1" if args.bind in ("127.0.0.1", "0.0.0.0") else args.bind
    dev = SimDevice(args.flags, args.leds, ip, args.fw_delay)
    srv = TcpServer(dev)
    server = await asyncio.start_server(srv.client, args.bind, args.tcp_port)
    loop = asyncio.get_running_loop()
    try:
        await loop.create_datagram_endpoint(lambda: UdpDiscovery(dev), local_addr=(args.bind, args.udp_port))
        log.info("discovery responder on UDP %s:%d", args.bind, args.udp_port)
    except OSError as e:
        log.warning("UDP discovery disabled: %s", e)
    log.info("simulated device listening on TCP %s:%d (flags=%d)", args.bind, args.tcp_port, args.flags)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
