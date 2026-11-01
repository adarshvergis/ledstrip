"""Binary host <-> device protocol (v1), as specified in Docs/protocol.md.

This module is transport-free: it only converts between bytes and Python
objects. It contains

* the frame header encoder/decoder,
* all enumerations (commands, status codes, modes, ...),
* request payload builders and response payload parsers for every command,
* response payload builders used by the device simulator,
* the calibration/settings region slot image encoder/decoder (section 5).

All integers are little-endian; strings are NUL-terminated unless stated.
"""

from __future__ import annotations

import re
import struct
import zlib
from dataclasses import asdict, dataclass, field
from enum import IntEnum
from typing import Any

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

PROTO_VERSION = 1
TCP_PORT = 4210
UDP_PORT = 4211
AP_DEVICE_IP = "192.168.4.1"

MAGIC = 0x534C               # bytes 'L','S' on the wire (little-endian u16)
HEADER = struct.Struct("<HBBHH")
HEADER_SIZE = HEADER.size    # 8
MAX_PAYLOAD = 4160
RESPONSE_FLAG = 0x80

DISCOVER_REQUEST = b"LEDSTRIP_DISCOVER"

INFO_SIZE = 128
CFG_MAGIC = 0x4746434C       # "LCFG"
CFG_FORMAT_VERSION = 1
CFG_HEADER = struct.Struct("<IHHIII")
STORE_CALIB = 0
STORE_SETTINGS = 1
SLOT_SIZE = 4096


class Cmd(IntEnum):
    """Command identifiers."""

    PING = 0x01
    GET_INFO = 0x02
    REBOOT = 0x03
    SET_DEVICE_MODE = 0x04
    SET_RUN_CONFIG = 0x05
    GET_LIVE = 0x06
    GET_ALGORITHMS = 0x10
    GET_STAGE_LIST = 0x11
    GET_STAGE_SCHEMA = 0x12
    GET_PARAMS = 0x13
    SET_PARAM = 0x14
    SAVE_PARAMS = 0x15
    RESET_PARAMS = 0x16
    RELOAD_PARAMS = 0x17
    GET_SETTINGS = 0x20
    SET_SETTING = 0x21
    FACTORY_RESET_SETTINGS = 0x22
    FW_BEGIN = 0x30
    FW_DATA = 0x31
    FW_END = 0x32
    FW_ABORT = 0x33
    GET_CALIB_IMAGE = 0x34
    CLI_EXEC = 0x40
    GET_DIAG = 0x50
    RESET_DIAG = 0x51
    GET_ERRORS = 0x60
    CLEAR_ERRORS = 0x61


class Status(IntEnum):
    """Response status codes."""

    OK = 0
    ERR_UNKNOWN_CMD = 1
    ERR_BAD_LEN = 2
    ERR_BAD_ARG = 3
    ERR_WRONG_MODE = 4
    ERR_BUSY = 5
    ERR_FLASH = 6
    ERR_CRC = 7
    ERR_NOT_SUPPORTED = 8
    ERR_INTERNAL = 9


STATUS_TEXT: dict[int, str] = {
    Status.OK: "OK",
    Status.ERR_UNKNOWN_CMD: "Command not recognised by the device",
    Status.ERR_BAD_LEN: "Payload length wrong for the command",
    Status.ERR_BAD_ARG: "Argument out of range or unknown id",
    Status.ERR_WRONG_MODE: "Command not allowed in the current device mode",
    Status.ERR_BUSY: "Device resource busy",
    Status.ERR_FLASH: "Flash erase/write/read failed",
    Status.ERR_CRC: "CRC / image validation failed",
    Status.ERR_NOT_SUPPORTED: "Feature not compiled into this firmware build",
    Status.ERR_INTERNAL: "Internal device error",
}


def status_name(code: int) -> str:
    """Return the symbolic name of a status code (or ``STATUS_<n>``)."""
    try:
        return Status(code).name
    except ValueError:
        return f"STATUS_{code}"


class DeviceMode(IntEnum):
    MAIN = 0
    CALIBRATION = 1
    FW_UPDATE = 2


class RunMode(IntEnum):
    AUDIO = 0
    PATTERN = 1


class BuildFlag(IntEnum):
    """Bit numbers in ``build_flags``."""

    INTERNAL_TEST_BUILD = 0
    ENABLE_DEBUG_CLI = 1
    ENABLE_SELF_DIAGNOSTICS = 2


class WifiState(IntEnum):
    OFF = 0
    SOFT_AP = 1
    STA_CONNECTED = 2
    STA_CONNECTING = 3


class ParamType(IntEnum):
    FLOAT = 0
    INT = 1
    BOOL = 2
    ENUM = 3


class FwTarget(IntEnum):
    APP = 0
    CALIB = 1


class ProtocolError(Exception):
    """Raised when a frame or payload cannot be decoded."""


# ---------------------------------------------------------------------------
# Framing
# ---------------------------------------------------------------------------


def encode_frame(cmd: int, seq: int, payload: bytes = b"", status: int = 0) -> bytes:
    """Build a complete frame (header + payload)."""
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError(f"payload too long ({len(payload)} > {MAX_PAYLOAD})")
    return HEADER.pack(MAGIC, cmd & 0xFF, status & 0xFF, seq & 0xFFFF, len(payload)) + payload


@dataclass
class FrameHeader:
    cmd: int
    status: int
    seq: int
    length: int


def decode_header(data: bytes) -> FrameHeader:
    """Decode and validate an 8-byte frame header."""
    if len(data) != HEADER_SIZE:
        raise ProtocolError("short header")
    magic, cmd, status, seq, length = HEADER.unpack(data)
    if magic != MAGIC:
        raise ProtocolError(f"bad magic 0x{magic:04X}")
    if length > MAX_PAYLOAD:
        raise ProtocolError(f"payload length {length} exceeds {MAX_PAYLOAD}")
    return FrameHeader(cmd, status, seq, length)


# ---------------------------------------------------------------------------
# Payload reader helper
# ---------------------------------------------------------------------------


class Reader:
    """Sequential little-endian reader over a payload."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def _take(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise ProtocolError(f"payload truncated at offset {self.pos} (need {n} bytes)")
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return chunk

    def u8(self) -> int:
        return self._take(1)[0]

    def i8(self) -> int:
        return struct.unpack("<b", self._take(1))[0]

    def u16(self) -> int:
        return struct.unpack("<H", self._take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self._take(4))[0]

    def f32(self) -> float:
        return struct.unpack("<f", self._take(4))[0]

    def raw(self, n: int) -> bytes:
        return self._take(n)

    def cstr(self) -> str:
        end = self.data.find(b"\0", self.pos)
        if end < 0:
            raise ProtocolError("unterminated string")
        s = self.data[self.pos:end].decode("utf-8", "replace")
        self.pos = end + 1
        return s

    def fixed_str(self, n: int) -> str:
        return self._take(n).split(b"\0", 1)[0].decode("utf-8", "replace")

    def remaining(self) -> int:
        return len(self.data) - self.pos


def _cstr(s: str) -> bytes:
    return s.encode("utf-8") + b"\0"


def _fixed(s: str, n: int) -> bytes:
    b = s.encode("utf-8")[: n - 1]
    return b + b"\0" * (n - len(b))


def round_f32(v: float) -> float:
    """Round a Python float to the nearest f32 value (as the device stores it)."""
    return struct.unpack("<f", struct.pack("<f", v))[0]


def _clean_float(v: float) -> float:
    """Present an f32 as a short decimal (0.1f -> 0.1 instead of 0.10000000149)."""
    return float(f"{v:.7g}")


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------


@dataclass
class DeviceInfo:
    proto_version: int
    build_flags: int
    device_mode: int
    run_mode: int
    active_algorithm: int
    num_algorithms: int
    active_pattern: int
    num_patterns: int
    brightness: int
    active_error_code: int
    uptime_ms: int
    sample_rate_hz: int
    num_leds: int
    hop_size: int
    reset_reason: int
    wifi_state: int
    rssi_dbm: int
    calib_dirty: int
    fw_version: str
    build_date: str
    device_name: str
    ip_address: str

    _FMT = struct.Struct("<IIBBBBBBBBIIHHBBbB24s24s32s16s")

    @classmethod
    def decode(cls, payload: bytes) -> "DeviceInfo":
        if len(payload) < INFO_SIZE:
            raise ProtocolError(f"GET_INFO payload is {len(payload)} bytes, expected {INFO_SIZE}")
        v = list(cls._FMT.unpack_from(payload))
        for i in range(18, 22):
            v[i] = v[i].split(b"\0", 1)[0].decode("utf-8", "replace")
        return cls(*v)

    def encode(self) -> bytes:
        d = asdict(self)
        vals = list(d.values())
        sizes = (24, 24, 32, 16)
        for k, i in enumerate(range(18, 22)):
            vals[i] = _fixed(vals[i], sizes[k])
        return self._FMT.pack(*vals)

    def to_json(self) -> dict[str, Any]:
        d = asdict(self)
        d["has_cli"] = bool(self.build_flags & (1 << BuildFlag.ENABLE_DEBUG_CLI))
        d["has_diag"] = bool(self.build_flags & (1 << BuildFlag.ENABLE_SELF_DIAGNOSTICS))
        d["internal_test_build"] = bool(self.build_flags & (1 << BuildFlag.INTERNAL_TEST_BUILD))
        d["device_mode_name"] = _enum_name(DeviceMode, self.device_mode)
        d["run_mode_name"] = _enum_name(RunMode, self.run_mode)
        d["wifi_state_name"] = _enum_name(WifiState, self.wifi_state)
        return d


def _enum_name(enum: type[IntEnum], value: int) -> str:
    try:
        return enum(value).name
    except ValueError:
        return f"UNKNOWN_{value}"


@dataclass
class LiveData:
    level: int
    bpm_x10: int
    beat_count: int
    centroid: int
    num_leds: int
    bands: list[int]
    leds: bytes  # R,G,B triplets

    @classmethod
    def decode(cls, payload: bytes) -> "LiveData":
        r = Reader(payload)
        level, bpm, beats, centroid = r.u16(), r.u16(), r.u16(), r.u16()
        nb = r.u8()
        r.u8()  # reserved
        nl = r.u16()
        bands = [r.u16() for _ in range(nb)]
        leds = r.raw(nl * 3)
        return cls(level, bpm, beats, centroid, nl, bands, leds)

    def encode(self) -> bytes:
        out = struct.pack("<HHHHBBH", self.level, self.bpm_x10, self.beat_count,
                          self.centroid, len(self.bands), 0, self.num_leds)
        out += struct.pack(f"<{len(self.bands)}H", *self.bands)
        return out + bytes(self.leds[: self.num_leds * 3])

    def to_json(self) -> dict[str, Any]:
        return {
            "level": self.level,
            "bpm": self.bpm_x10 / 10.0,
            "beat_count": self.beat_count,
            "centroid": self.centroid,
            "num_leds": self.num_leds,
            "bands": self.bands,
            "leds": self.leds.hex(),  # compact: 6 hex chars per LED
        }


@dataclass
class Algorithm:
    id: int
    stage_ids: list[int]
    name: str
    description: str


def decode_algorithms(payload: bytes) -> list[Algorithm]:
    r = Reader(payload)
    out = []
    for _ in range(r.u8()):
        aid = r.u8()
        n = r.u8()
        stages = list(r.raw(n))
        out.append(Algorithm(aid, stages, r.cstr(), r.cstr()))
    return out


def encode_algorithms(algs: list[Algorithm]) -> bytes:
    out = bytes([len(algs)])
    for a in algs:
        out += bytes([a.id, len(a.stage_ids)]) + bytes(a.stage_ids) + _cstr(a.name) + _cstr(a.description)
    return out


def decode_stage_list(payload: bytes) -> list[int]:
    r = Reader(payload)
    return list(r.raw(r.u8()))


def encode_stage_list(ids: list[int]) -> bytes:
    return bytes([len(ids)]) + bytes(ids)


@dataclass
class ParamSchema:
    id: int
    type: int
    min: float
    max: float
    default: float
    step: float
    name: str
    unit: str

    def to_json(self) -> dict[str, Any]:
        d = asdict(self)
        for k in ("min", "max", "default", "step"):
            d[k] = _clean_float(d[k])
        d["type_name"] = _enum_name(ParamType, self.type)
        d["options"] = self.unit.split("|") if self.type == ParamType.ENUM else None
        return d


@dataclass
class StageSchema:
    id: int
    name: str
    description: str
    params: list[ParamSchema] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        return {"id": self.id, "name": self.name, "description": self.description,
                "params": [p.to_json() for p in self.params]}


_PARAM_HDR = struct.Struct("<BBffff")


def decode_stage_schema(payload: bytes) -> StageSchema:
    r = Reader(payload)
    sid, n = r.u8(), r.u8()
    st = StageSchema(sid, r.cstr(), r.cstr())
    for _ in range(n):
        pid, ptype, mn, mx, df, step = _PARAM_HDR.unpack(r.raw(_PARAM_HDR.size))
        st.params.append(ParamSchema(pid, ptype, mn, mx, df, step, r.cstr(), r.cstr()))
    return st


def encode_stage_schema(st: StageSchema) -> bytes:
    out = bytes([st.id, len(st.params)]) + _cstr(st.name) + _cstr(st.description)
    for p in st.params:
        out += _PARAM_HDR.pack(p.id, p.type, p.min, p.max, p.default, p.step) + _cstr(p.name) + _cstr(p.unit)
    return out


def decode_params(payload: bytes) -> tuple[int, list[float]]:
    """Decode GET_PARAMS response -> (stage_id, values)."""
    r = Reader(payload)
    sid, n = r.u8(), r.u8()
    return sid, [_clean_float(r.f32()) for _ in range(n)]


def encode_params(stage_id: int, values: list[float]) -> bytes:
    return bytes([stage_id, len(values)]) + struct.pack(f"<{len(values)}f", *values)


def build_set_param(stage_id: int, param_id: int, value: float) -> bytes:
    return struct.pack("<BBf", stage_id, param_id, value)


def build_set_run_config(run_mode: int | None = None, algorithm: int | None = None,
                         pattern: int | None = None, brightness: int | None = None) -> bytes:
    """SET_RUN_CONFIG payload; ``None`` fields become 0xFF ("unchanged")."""
    def v(x: int | None) -> int:
        return 0xFF if x is None else int(x) & 0xFF
    return bytes([v(run_mode), v(algorithm), v(pattern), v(brightness)])


@dataclass
class ErrorEntry:
    code: int
    severity: int
    occurrences: int
    last_uptime_ms: int
    name: str

    SEVERITY_NAMES = ("info", "warning", "error", "fatal")

    def to_json(self) -> dict[str, Any]:
        d = asdict(self)
        d["severity_name"] = (self.SEVERITY_NAMES[self.severity]
                              if self.severity < len(self.SEVERITY_NAMES) else str(self.severity))
        return d


def decode_errors(payload: bytes) -> list[ErrorEntry]:
    r = Reader(payload)
    out = []
    for _ in range(r.u8()):
        code, sev, occ, last = r.u8(), r.u8(), r.u16(), r.u32()
        out.append(ErrorEntry(code, sev, occ, last, r.cstr()))
    return out


def encode_errors(errors: list[ErrorEntry]) -> bytes:
    out = bytes([len(errors)])
    for e in errors:
        out += struct.pack("<BBHI", e.code, e.severity, e.occurrences, e.last_uptime_ms) + _cstr(e.name)
    return out


def decode_diag(text: str) -> list[dict[str, str]]:
    """Split GET_DIAG text into rows of group/name/value/unit."""
    rows = []
    for line in text.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        parts += [""] * (4 - len(parts))
        rows.append({"group": parts[0], "name": parts[1], "value": parts[2], "unit": "\t".join(parts[3:])})
    return rows


# ---------------------------------------------------------------------------
# Settings (TLV)
# ---------------------------------------------------------------------------

SETTING_INT = 0
SETTING_STR = 1


@dataclass(frozen=True)
class SettingDef:
    id: int
    name: str
    type: int        # SETTING_INT / SETTING_STR
    size: int        # int: byte width; str: max length
    minimum: int = 0
    maximum: int = 0


SETTINGS: list[SettingDef] = [
    SettingDef(1, "device_name", SETTING_STR, 31),
    SettingDef(2, "wifi_mode", SETTING_INT, 1, 0, 2),
    SettingDef(3, "sta_ssid", SETTING_STR, 32),
    SettingDef(4, "sta_password", SETTING_STR, 64),
    SettingDef(5, "ap_ssid", SETTING_STR, 32),
    SettingDef(6, "ap_password", SETTING_STR, 64),
    SettingDef(7, "ap_channel", SETTING_INT, 1, 1, 13),
    SettingDef(8, "max_current_ma", SETTING_INT, 2, 100, 3000),
    SettingDef(9, "num_leds", SETTING_INT, 2, 1, 65535),
    SettingDef(10, "run_mode", SETTING_INT, 1, 0, 1),
    SettingDef(11, "algorithm", SETTING_INT, 1, 0, 255),
    SettingDef(12, "pattern", SETTING_INT, 1, 0, 255),
    SettingDef(13, "brightness", SETTING_INT, 1, 0, 255),
    SettingDef(14, "color_order", SETTING_INT, 1, 0, 5),
]
SETTINGS_BY_ID = {s.id: s for s in SETTINGS}
SETTINGS_BY_NAME = {s.name: s for s in SETTINGS}


def decode_settings(payload: bytes) -> dict[int, int | str]:
    """Decode GET_SETTINGS TLV list -> {id: value}."""
    r = Reader(payload)
    out: dict[int, int | str] = {}
    while r.remaining() > 0:
        sid, stype, ln = r.u8(), r.u8(), r.u8()
        data = r.raw(ln)
        if stype == SETTING_STR:
            out[sid] = data.decode("utf-8", "replace")
        else:
            out[sid] = int.from_bytes(data, "little") if data else 0
    return out


def encode_setting_value(sdef: SettingDef, value: int | str) -> bytes:
    """Encode one setting value (data bytes only), validating it."""
    if sdef.type == SETTING_STR:
        data = str(value).encode("utf-8")
        if len(data) > sdef.size:
            raise ValueError(f"{sdef.name}: at most {sdef.size} bytes")
        if sdef.name == "ap_password" and 0 < len(data) < 8:
            raise ValueError("ap_password must be empty (open) or 8..64 characters")
        return data
    try:
        iv = int(value)
    except (TypeError, ValueError):
        raise ValueError(f"{sdef.name}: integer expected") from None
    if not sdef.minimum <= iv <= sdef.maximum:
        raise ValueError(f"{sdef.name}: must be in {sdef.minimum}..{sdef.maximum}")
    return iv.to_bytes(sdef.size, "little")


def encode_setting_value_loose(sdef: SettingDef, value: int | str) -> bytes:
    """Encode a setting value exactly as the device would report it (no validation)."""
    if sdef.type == SETTING_STR:
        return str(value).encode("utf-8")
    return int(value).to_bytes(sdef.size, "little")


def encode_settings_tlv(values: dict[int, int | str]) -> bytes:
    """Encode a GET_SETTINGS response (used by the simulator)."""
    out = b""
    for sid, val in values.items():
        sdef = SETTINGS_BY_ID[sid]
        data = encode_setting_value_loose(sdef, val)
        out += bytes([sid, sdef.type, len(data)]) + data
    return out


def build_set_setting(sid: int, data: bytes) -> bytes:
    return bytes([sid, len(data)]) + data


def build_fw_begin(target: int, total_size: int, crc32: int) -> bytes:
    return struct.pack("<BII", target, total_size, crc32)


def build_fw_data(offset: int, data: bytes) -> bytes:
    return struct.pack("<I", offset) + data


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

_DISCOVERY_RE = re.compile(
    r"^LEDSTRIP proto=(?P<proto>\d+) name=(?P<name>.*?) ip=(?P<ip>\S+) port=(?P<port>\d+)"
    r" fw=(?P<fw>\S*) mac=(?P<mac>\S+)\s*$"
)


def parse_discovery_line(line: str) -> dict[str, Any] | None:
    """Parse a discovery reply/beacon line; returns None if it is not one.

    The device name may contain spaces, so it is matched lazily up to ``" ip="``.
    """
    m = _DISCOVERY_RE.match(line.strip())
    if not m:
        return None
    d: dict[str, Any] = m.groupdict()
    d["proto"] = int(d["proto"])
    d["port"] = int(d["port"])
    return d


def format_discovery_line(name: str, ip: str, fw: str, mac: str, port: int = TCP_PORT) -> str:
    return f"LEDSTRIP proto={PROTO_VERSION} name={name} ip={ip} port={port} fw={fw} mac={mac}"


# ---------------------------------------------------------------------------
# Calibration / settings region slot image (section 5)
# ---------------------------------------------------------------------------


def calib_key(stage_id: int, param_id: int) -> int:
    return ((stage_id & 0xFF) << 8) | (param_id & 0xFF)


def encode_region_image(records: list[tuple[int, int, bytes]], store_id: int = STORE_CALIB,
                        sequence: int = 1) -> bytes:
    """Build a slot image from ``(key, type, data)`` records."""
    body = b"".join(struct.pack("<HBB", k, t, len(d)) + d for k, t, d in records)
    img = CFG_HEADER.pack(CFG_MAGIC, CFG_FORMAT_VERSION, store_id, sequence, len(body),
                          zlib.crc32(body) & 0xFFFFFFFF) + body
    if len(img) > SLOT_SIZE:
        raise ValueError(f"image is {len(img)} bytes, slot is {SLOT_SIZE}")
    return img


def decode_region_image(img: bytes, verify_crc: bool = True) -> dict[str, Any]:
    """Parse a slot image -> header fields + list of (key, type, data) records."""
    if len(img) < CFG_HEADER.size:
        raise ProtocolError("image shorter than header")
    magic, ver, store, seq, dlen, crc = CFG_HEADER.unpack_from(img)
    if magic != CFG_MAGIC:
        raise ProtocolError(f"bad image magic 0x{magic:08X} (expected 0x{CFG_MAGIC:08X})")
    body = img[CFG_HEADER.size:CFG_HEADER.size + dlen]
    if len(body) != dlen:
        raise ProtocolError("image data truncated")
    if verify_crc and (zlib.crc32(body) & 0xFFFFFFFF) != crc:
        raise ProtocolError("image CRC mismatch")
    r = Reader(body)
    records = []
    while r.remaining() > 0:
        key, t, ln = struct.unpack("<HBB", r.raw(4))
        records.append((key, t, r.raw(ln)))
    return {"format_version": ver, "store_id": store, "sequence": seq, "crc32": crc, "records": records}


def encode_calib_image(params: list[dict[str, Any]], sequence: int = 1) -> bytes:
    """Build a calib store image from ``[{"stage":s,"param":p,"value":v}, ...]``."""
    recs = [(calib_key(int(p["stage"]), int(p["param"])), 0, struct.pack("<f", float(p["value"])))
            for p in params]
    return encode_region_image(recs, STORE_CALIB, sequence)


def decode_calib_image(img: bytes) -> list[dict[str, Any]]:
    """Reverse of :func:`encode_calib_image` (non-f32 records are skipped)."""
    info = decode_region_image(img)
    if info["store_id"] != STORE_CALIB:
        raise ProtocolError(f"not a calib image (store_id={info['store_id']})")
    out = []
    for key, t, data in info["records"]:
        if t == 0 and len(data) == 4:
            out.append({"stage": key >> 8, "param": key & 0xFF,
                        "value": _clean_float(struct.unpack("<f", data)[0])})
    return out
