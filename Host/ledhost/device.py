"""Async TCP client for the LED strip device, plus UDP discovery.

The device accepts a single TCP client and answers requests strictly in order,
so :class:`DeviceClient` keeps one persistent connection and serialises every
request with an :class:`asyncio.Lock`. Any transport failure (timeout, reset,
bad frame) closes the connection; the next request reconnects automatically.
"""

from __future__ import annotations

import asyncio
import logging
import socket
import struct
import time
from typing import Any

from . import protocol as P
from .protocol import Cmd

log = logging.getLogger(__name__)

DEFAULT_TIMEOUT = 3.0
SLOW_TIMEOUT = 15.0      # FW_DATA / FW_END / SAVE_PARAMS (flash erase/write)
CONNECT_TIMEOUT = 3.0

SLOW_COMMANDS = {Cmd.FW_DATA, Cmd.FW_END, Cmd.SAVE_PARAMS, Cmd.FW_BEGIN}


class DeviceError(Exception):
    """A request failed.

    ``status`` is the device status code, or ``None`` for transport failures
    (not connected, timeout, connection lost).
    """

    def __init__(self, message: str, status: int | None = None, cmd: int | None = None) -> None:
        super().__init__(message)
        self.status = status
        self.cmd = cmd

    @property
    def status_name(self) -> str:
        return "TRANSPORT" if self.status is None else P.status_name(self.status)


def parse_address(addr: str, default_port: int = P.TCP_PORT) -> tuple[str, int]:
    """Split ``"host[:port]"`` into ``(host, port)``."""
    addr = addr.strip()
    if addr.count(":") == 1:
        host, port = addr.split(":")
        return host, int(port)
    return addr, default_port


class DeviceClient:
    """Serialized request/response client for one device."""

    def __init__(self, host: str, port: int = P.TCP_PORT) -> None:
        self.host = host
        self.port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._lock = asyncio.Lock()
        self._seq = 0
        self.last_error: str | None = None
        self.last_ok: float = 0.0

    # -- connection management ----------------------------------------------

    @property
    def connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    async def _connect(self) -> None:
        try:
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), CONNECT_TIMEOUT)
        except (OSError, asyncio.TimeoutError) as e:
            self._reader = self._writer = None
            msg = f"cannot connect to {self.host}:{self.port}: {e or 'timeout'}"
            self.last_error = msg
            raise DeviceError(msg) from None
        sock = self._writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log.info("connected to %s:%d", self.host, self.port)

    async def _drop(self) -> None:
        w, self._reader, self._writer = self._writer, None, None
        if w is not None:
            w.close()
            try:
                await asyncio.wait_for(w.wait_closed(), 1.0)
            except Exception:  # noqa: BLE001 - best effort close
                pass

    async def close(self) -> None:
        async with self._lock:
            await self._drop()

    # -- core request ---------------------------------------------------------

    async def request(self, cmd: int, payload: bytes = b"", timeout: float | None = None) -> bytes:
        """Send one request and return the response payload (status OK).

        Raises :class:`DeviceError` with the device status for error responses
        and with ``status=None`` for transport failures.
        """
        if timeout is None:
            timeout = SLOW_TIMEOUT if cmd in SLOW_COMMANDS else DEFAULT_TIMEOUT
        async with self._lock:
            if not self.connected:
                await self._connect()
            assert self._reader is not None and self._writer is not None
            self._seq = (self._seq + 1) & 0xFFFF
            seq = self._seq
            try:
                self._writer.write(P.encode_frame(cmd, seq, payload))
                await self._writer.drain()
                hdr, body = await asyncio.wait_for(self._read_response(seq), timeout)
            except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, P.ProtocolError) as e:
                await self._drop()
                name = _cmd_name(cmd)
                if isinstance(e, asyncio.TimeoutError):
                    msg = f"{name}: device did not answer within {timeout:g} s"
                elif isinstance(e, asyncio.IncompleteReadError):
                    msg = f"{name}: connection closed by device"
                else:
                    msg = f"{name}: {e}"
                self.last_error = msg
                raise DeviceError(msg, None, cmd) from None
            if hdr.cmd != (cmd | P.RESPONSE_FLAG):
                await self._drop()
                raise DeviceError(f"unexpected response cmd 0x{hdr.cmd:02X} to 0x{cmd:02X}", None, cmd)
            self.last_ok = time.time()
            self.last_error = None
            if hdr.status != P.Status.OK:
                text = body.decode("ascii", "replace").strip()
                base = P.STATUS_TEXT.get(hdr.status, P.status_name(hdr.status))
                raise DeviceError(f"{_cmd_name(cmd)}: {base}" + (f" ({text})" if text else ""),
                                  hdr.status, cmd)
            return body

    async def _read_response(self, seq: int) -> tuple[P.FrameHeader, bytes]:
        """Read frames until the one matching ``seq`` arrives (stale ones are skipped)."""
        assert self._reader is not None
        while True:
            hdr = P.decode_header(await self._reader.readexactly(P.HEADER_SIZE))
            body = await self._reader.readexactly(hdr.length) if hdr.length else b""
            if hdr.seq == seq:
                return hdr, body
            log.warning("discarding stale response seq=%d (want %d)", hdr.seq, seq)

    # -- typed commands -------------------------------------------------------

    async def ping(self) -> None:
        await self.request(Cmd.PING)

    async def get_info(self) -> P.DeviceInfo:
        return P.DeviceInfo.decode(await self.request(Cmd.GET_INFO))

    async def reboot(self) -> None:
        await self.request(Cmd.REBOOT)
        async with self._lock:
            await self._drop()

    async def set_device_mode(self, mode: int) -> None:
        await self.request(Cmd.SET_DEVICE_MODE, bytes([mode]))

    async def set_run_config(self, run_mode: int | None = None, algorithm: int | None = None,
                             pattern: int | None = None, brightness: int | None = None) -> None:
        await self.request(Cmd.SET_RUN_CONFIG,
                           P.build_set_run_config(run_mode, algorithm, pattern, brightness))

    async def get_live(self) -> P.LiveData:
        return P.LiveData.decode(await self.request(Cmd.GET_LIVE))

    async def get_algorithms(self) -> list[P.Algorithm]:
        return P.decode_algorithms(await self.request(Cmd.GET_ALGORITHMS))

    async def get_stage_list(self) -> list[int]:
        return P.decode_stage_list(await self.request(Cmd.GET_STAGE_LIST))

    async def get_stage_schema(self, stage_id: int) -> P.StageSchema:
        return P.decode_stage_schema(await self.request(Cmd.GET_STAGE_SCHEMA, bytes([stage_id])))

    async def get_params(self, stage_id: int) -> list[float]:
        _, values = P.decode_params(await self.request(Cmd.GET_PARAMS, bytes([stage_id])))
        return values

    async def set_param(self, stage_id: int, param_id: int, value: float) -> float:
        body = await self.request(Cmd.SET_PARAM, P.build_set_param(stage_id, param_id, value))
        return float(f"{struct.unpack('<f', body[:4])[0]:.7g}")

    async def save_params(self) -> None:
        await self.request(Cmd.SAVE_PARAMS)

    async def reset_params(self, stage_id: int = 0xFF) -> None:
        await self.request(Cmd.RESET_PARAMS, bytes([stage_id]))

    async def reload_params(self) -> None:
        await self.request(Cmd.RELOAD_PARAMS)

    async def get_settings(self) -> dict[int, int | str]:
        return P.decode_settings(await self.request(Cmd.GET_SETTINGS))

    async def set_setting(self, sid: int, data: bytes) -> bool:
        body = await self.request(Cmd.SET_SETTING, P.build_set_setting(sid, data))
        return bool(body[0]) if body else False

    async def factory_reset(self) -> None:
        await self.request(Cmd.FACTORY_RESET_SETTINGS)

    async def fw_begin(self, target: int, size: int, crc: int) -> int:
        body = await self.request(Cmd.FW_BEGIN, P.build_fw_begin(target, size, crc))
        return struct.unpack("<H", body[:2])[0] if len(body) >= 2 else 4096

    async def fw_data(self, offset: int, data: bytes) -> int:
        body = await self.request(Cmd.FW_DATA, P.build_fw_data(offset, data))
        return struct.unpack("<I", body[:4])[0]

    async def fw_end(self) -> None:
        await self.request(Cmd.FW_END)

    async def fw_abort(self) -> None:
        await self.request(Cmd.FW_ABORT)

    async def get_calib_image(self) -> bytes:
        return await self.request(Cmd.GET_CALIB_IMAGE)

    async def cli_exec(self, line: str) -> str:
        return (await self.request(Cmd.CLI_EXEC, line.encode("utf-8"))).decode("utf-8", "replace")

    async def get_diag(self) -> str:
        return (await self.request(Cmd.GET_DIAG)).decode("utf-8", "replace")

    async def reset_diag(self) -> None:
        await self.request(Cmd.RESET_DIAG)

    async def get_errors(self) -> list[P.ErrorEntry]:
        return P.decode_errors(await self.request(Cmd.GET_ERRORS))

    async def clear_errors(self) -> None:
        await self.request(Cmd.CLEAR_ERRORS)


def _cmd_name(cmd: int) -> str:
    try:
        return Cmd(cmd).name
    except ValueError:
        return f"CMD_0x{cmd:02X}"


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------


class _DiscoveryProtocol(asyncio.DatagramProtocol):
    def __init__(self, sink: dict[str, dict[str, Any]]) -> None:
        self.sink = sink

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        _record(self.sink, data, addr)


def _record(sink: dict[str, dict[str, Any]], data: bytes, addr: tuple[str, int]) -> None:
    d = P.parse_discovery_line(data.decode("utf-8", "replace"))
    if d is None:
        return
    # Prefer the advertised IP; fall back to the sender address if it is unusable.
    if d["ip"] in ("", "0.0.0.0"):
        d["ip"] = addr[0]
    d["source"] = addr[0]
    d["last_seen"] = time.time()
    sink[f"{d['ip']}:{d['port']}"] = d


async def discover(timeout: float = 1.5, extra_targets: list[str] | None = None) -> list[dict[str, Any]]:
    """Broadcast ``LEDSTRIP_DISCOVER`` and collect replies for ``timeout`` seconds.

    ``extra_targets`` are additional unicast addresses to probe (e.g. 127.0.0.1
    for the simulator, or the AP-mode address 192.168.4.1).
    """
    loop = asyncio.get_running_loop()
    found: dict[str, dict[str, Any]] = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("0.0.0.0", 0))
    transport, _ = await loop.create_datagram_endpoint(lambda: _DiscoveryProtocol(found), sock=sock)
    try:
        targets = ["255.255.255.255"] + list(extra_targets or [])
        for _ in range(2):  # UDP is lossy: send twice
            for t in targets:
                try:
                    transport.sendto(P.DISCOVER_REQUEST, (t, P.UDP_PORT))
                except OSError as e:
                    log.debug("discovery send to %s failed: %s", t, e)
            await asyncio.sleep(timeout / 2)
    finally:
        transport.close()
    return list(found.values())


class BeaconListener:
    """Listens on UDP 4211 for the unsolicited 3 s device beacons."""

    def __init__(self) -> None:
        self.devices: dict[str, dict[str, Any]] = {}
        self._transport: asyncio.DatagramTransport | None = None
        self.error: str | None = None

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.bind(("0.0.0.0", P.UDP_PORT))
            self._transport, _ = await loop.create_datagram_endpoint(
                lambda: _DiscoveryProtocol(self.devices), sock=sock)
            log.info("listening for device beacons on UDP %d", P.UDP_PORT)
        except OSError as e:
            self.error = f"beacon listener disabled: {e}"
            log.warning(self.error)

    def recent(self, max_age: float = 10.0) -> list[dict[str, Any]]:
        now = time.time()
        return [d for d in self.devices.values() if now - d["last_seen"] <= max_age]

    def stop(self) -> None:
        if self._transport is not None:
            self._transport.close()
            self._transport = None
