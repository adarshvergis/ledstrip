"""aiohttp web server: JSON REST API in front of the device + static web UI.

All API responses are JSON. Errors are returned as
``{"error": <message>, "status": <device status name>, "code": <int|null>}``
with an HTTP status derived from the device status code (see ``_http_status``).
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import zlib
from enum import IntEnum
from pathlib import Path
from typing import Any, Awaitable, Callable

from aiohttp import web

from . import protocol as P
from .device import BeaconListener, DeviceClient, DeviceError, discover, parse_address

log = logging.getLogger(__name__)

HOST_DIR = Path(__file__).resolve().parent.parent
STATIC_DIR = HOST_DIR / "static"
HOST_SETTINGS_FILE = HOST_DIR / "host_settings.json"

DEFAULT_HOST_SETTINGS: dict[str, Any] = {
    "poll_rate_hz": 15,
    "default_device_ip": "",
    "auto_connect": True,
}

Handler = Callable[[web.Request], Awaitable[web.StreamResponse]]


class ApiError(Exception):
    """Host-side validation error (HTTP 400 unless stated)."""

    def __init__(self, message: str, http_status: int = 400) -> None:
        super().__init__(message)
        self.http_status = http_status


# ---------------------------------------------------------------------------
# Application state
# ---------------------------------------------------------------------------


class FirmwareJob:
    """State of the backend-driven firmware upload (polled by the UI)."""

    def __init__(self) -> None:
        self.state = "idle"          # idle | running | done | error | aborted
        self.target = ""
        self.filename = ""
        self.total = 0
        self.sent = 0
        self.message = ""
        self.started = 0.0
        self.finished = 0.0
        self.abort_requested = False
        self.task: asyncio.Task[None] | None = None

    def to_json(self) -> dict[str, Any]:
        elapsed = (self.finished or time.time()) - self.started if self.started else 0.0
        return {
            "state": self.state, "target": self.target, "filename": self.filename,
            "total": self.total, "sent": self.sent,
            "percent": round(100.0 * self.sent / self.total, 1) if self.total else 0.0,
            "message": self.message, "elapsed_s": round(elapsed, 1),
            "rate_bps": round(self.sent / elapsed) if elapsed > 0 else 0,
        }


class HostState:
    """Everything the request handlers share."""

    def __init__(self) -> None:
        self.client: DeviceClient | None = None
        self.beacons = BeaconListener()
        self.fw = FirmwareJob()
        self.host_settings = load_host_settings()
        self.schema_cache: dict[int, P.StageSchema] = {}

    def device(self) -> DeviceClient:
        if self.client is None:
            raise ApiError("No device selected. Discover or enter an IP address and connect.", 503)
        return self.client

    async def set_target(self, address: str) -> DeviceClient:
        host, port = parse_address(address)
        if self.client is not None:
            await self.client.close()
        self.client = DeviceClient(host, port)
        self.schema_cache.clear()
        return self.client


def load_host_settings() -> dict[str, Any]:
    settings = dict(DEFAULT_HOST_SETTINGS)
    try:
        settings.update(json.loads(HOST_SETTINGS_FILE.read_text(encoding="utf-8")))
    except FileNotFoundError:
        pass
    except (OSError, ValueError) as e:
        log.warning("ignoring unreadable %s: %s", HOST_SETTINGS_FILE, e)
    return settings


def save_host_settings(settings: dict[str, Any]) -> None:
    HOST_SETTINGS_FILE.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")


STATE_KEY: web.AppKey[HostState] = web.AppKey("state", HostState)


def st(request: web.Request) -> HostState:
    return request.app[STATE_KEY]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_HTTP_FOR_STATUS = {
    P.Status.ERR_UNKNOWN_CMD: 501,
    P.Status.ERR_BAD_LEN: 400,
    P.Status.ERR_BAD_ARG: 400,
    P.Status.ERR_WRONG_MODE: 409,
    P.Status.ERR_BUSY: 409,
    P.Status.ERR_FLASH: 500,
    P.Status.ERR_CRC: 422,
    P.Status.ERR_NOT_SUPPORTED: 501,
    P.Status.ERR_INTERNAL: 500,
}


def _http_status(e: DeviceError) -> int:
    if e.status is None:
        return 504 if "did not answer" in str(e) else 502
    return _HTTP_FOR_STATUS.get(e.status, 500)


@web.middleware
async def error_middleware(request: web.Request, handler: Handler) -> web.StreamResponse:
    """Convert exceptions into JSON error responses."""
    try:
        return await handler(request)
    except DeviceError as e:
        return web.json_response({"error": str(e), "status": e.status_name, "code": e.status},
                                 status=_http_status(e))
    except ApiError as e:
        return web.json_response({"error": str(e), "status": "HOST", "code": None}, status=e.http_status)
    except web.HTTPException:
        raise
    except Exception as e:  # noqa: BLE001 - report unexpected failures to the UI
        log.exception("unhandled error in %s", request.path)
        return web.json_response({"error": f"host error: {e}", "status": "HOST", "code": None}, status=500)


async def body_json(request: web.Request) -> dict[str, Any]:
    if not request.can_read_body:
        return {}
    try:
        data = await request.json()
    except ValueError:
        raise ApiError("request body is not valid JSON") from None
    if not isinstance(data, dict):
        raise ApiError("JSON object expected")
    return data


def int_arg(data: dict[str, Any], key: str, lo: int, hi: int, required: bool = True) -> int | None:
    v = data.get(key)
    if v is None:
        if required:
            raise ApiError(f"missing '{key}'")
        return None
    try:
        iv = int(v)
    except (TypeError, ValueError):
        raise ApiError(f"'{key}' must be an integer") from None
    if not lo <= iv <= hi:
        raise ApiError(f"'{key}' must be in {lo}..{hi}")
    return iv


def ok(**extra: Any) -> web.Response:
    return web.json_response({"ok": True, **extra})


def download(data: bytes, filename: str, content_type: str) -> web.Response:
    return web.Response(body=data, content_type=content_type,
                        headers={"Content-Disposition": f'attachment; filename="{filename}"'})


def _mode_value(v: Any, enum: type[IntEnum]) -> int:
    if isinstance(v, str) and not v.isdigit():
        try:
            return int(enum[v.upper()])
        except KeyError:
            raise ApiError(f"unknown value '{v}'") from None
    try:
        return int(enum(int(v)))
    except (TypeError, ValueError):
        raise ApiError(f"unknown value '{v}'") from None


async def get_schema(state: HostState, stage_id: int) -> P.StageSchema:
    """Stage schemas never change while connected, so cache them."""
    if stage_id not in state.schema_cache:
        state.schema_cache[stage_id] = await state.device().get_stage_schema(stage_id)
    return state.schema_cache[stage_id]


# ---------------------------------------------------------------------------
# Connection / discovery
# ---------------------------------------------------------------------------


async def h_status(request: web.Request) -> web.Response:
    """Connection status; tries GET_INFO when a device is selected."""
    state = st(request)
    c = state.client
    out: dict[str, Any] = {"device": None, "connected": False, "info": None, "error": None,
                           "fw_job": state.fw.state}
    if c is not None:
        out["device"] = f"{c.host}:{c.port}"
        try:
            out["info"] = (await c.get_info()).to_json()
            out["connected"] = True
        except DeviceError as e:
            out["error"] = str(e)
    return web.json_response(out)


async def h_connect(request: web.Request) -> web.Response:
    data = await body_json(request)
    ip = str(data.get("ip", "")).strip()
    if not ip:
        raise ApiError("missing 'ip'")
    client = await st(request).set_target(ip)
    info = await client.get_info()
    if info.proto_version != P.PROTO_VERSION:
        log.warning("device reports protocol v%d (host speaks v%d)", info.proto_version, P.PROTO_VERSION)
    return ok(device=f"{client.host}:{client.port}", info=info.to_json())


async def h_disconnect(request: web.Request) -> web.Response:
    state = st(request)
    if state.client is not None:
        await state.client.close()
    state.client = None
    state.schema_cache.clear()
    return ok()


async def h_discover(request: web.Request) -> web.Response:
    state = st(request)
    timeout = min(max(float(request.query.get("timeout", 1.5)), 0.2), 10.0)
    extra = ["127.0.0.1"]
    if state.host_settings.get("default_device_ip"):
        extra.append(parse_address(state.host_settings["default_device_ip"])[0])
    found = {f"{d['ip']}:{d['port']}": d for d in state.beacons.recent()}
    for d in await discover(timeout, extra):
        found[f"{d['ip']}:{d['port']}"] = d
    return web.json_response({"devices": sorted(found.values(), key=lambda d: d["ip"]),
                              "beacon_listener": state.beacons.error or "active"})


# ---------------------------------------------------------------------------
# Main tab
# ---------------------------------------------------------------------------


async def h_info(request: web.Request) -> web.Response:
    return web.json_response((await st(request).device().get_info()).to_json())


async def h_reboot(request: web.Request) -> web.Response:
    await st(request).device().reboot()
    return ok()


async def h_device_mode(request: web.Request) -> web.Response:
    data = await body_json(request)
    mode = _mode_value(data.get("mode"), P.DeviceMode)
    if mode == P.DeviceMode.FW_UPDATE:
        raise ApiError("FW_UPDATE mode is entered only through a firmware upload")
    await st(request).device().set_device_mode(mode)
    return ok(mode=mode)


async def h_run_config(request: web.Request) -> web.Response:
    data = await body_json(request)
    run_mode = None if data.get("run_mode") is None else _mode_value(data["run_mode"], P.RunMode)
    algorithm = int_arg(data, "algorithm", 0, 254, required=False)
    pattern = int_arg(data, "pattern", 0, 254, required=False)
    brightness = int_arg(data, "brightness", 0, 255, required=False)
    dev = st(request).device()
    if brightness == 255:
        # 0xFF means "unchanged" in SET_RUN_CONFIG; full brightness goes via SET_SETTING.
        await dev.set_setting(13, bytes([255]))
        brightness = None
    if any(v is not None for v in (run_mode, algorithm, pattern, brightness)):
        await dev.set_run_config(run_mode, algorithm, pattern, brightness)
    return ok()


async def h_live(request: web.Request) -> web.Response:
    return web.json_response((await st(request).device().get_live()).to_json())


async def h_errors(request: web.Request) -> web.Response:
    errs = await st(request).device().get_errors()
    return web.json_response({"errors": [e.to_json() for e in errs]})


async def h_errors_clear(request: web.Request) -> web.Response:
    await st(request).device().clear_errors()
    return ok()


# ---------------------------------------------------------------------------
# Calibration
# ---------------------------------------------------------------------------


async def h_algorithms(request: web.Request) -> web.Response:
    algs = await st(request).device().get_algorithms()
    return web.json_response({"algorithms": [a.__dict__ for a in algs]})


async def h_stages(request: web.Request) -> web.Response:
    return web.json_response({"stages": await st(request).device().get_stage_list()})


async def h_stage_schema(request: web.Request) -> web.Response:
    sid = _stage_id(request)
    return web.json_response((await get_schema(st(request), sid)).to_json())


async def h_stage_params(request: web.Request) -> web.Response:
    sid = _stage_id(request)
    return web.json_response({"stage": sid, "values": await st(request).device().get_params(sid)})


def _stage_id(request: web.Request) -> int:
    try:
        sid = int(request.match_info["sid"])
    except ValueError:
        raise ApiError("stage id must be an integer") from None
    if not 0 <= sid <= 254:
        raise ApiError("stage id must be in 0..254")
    return sid


async def h_calib(request: web.Request) -> web.Response:
    """Everything the calibration tab needs in one call.

    Returns algorithms, every stage schema with current values, plus device mode
    and the calib_dirty flag.
    """
    state = st(request)
    dev = state.device()
    info = await dev.get_info()
    algs = await dev.get_algorithms()
    stages = []
    for sid in await dev.get_stage_list():
        schema = (await get_schema(state, sid)).to_json()
        schema["values"] = await dev.get_params(sid)
        stages.append(schema)
    return web.json_response({
        "device_mode": info.device_mode, "calib_dirty": info.calib_dirty,
        "active_algorithm": info.active_algorithm,
        "algorithms": [a.__dict__ for a in algs], "stages": stages,
    })


async def h_set_param(request: web.Request) -> web.Response:
    data = await body_json(request)
    sid = int_arg(data, "stage", 0, 254)
    pid = int_arg(data, "param", 0, 255)
    try:
        value = float(data["value"])
    except (KeyError, TypeError, ValueError):
        raise ApiError("'value' must be a number") from None
    applied = await st(request).device().set_param(sid, pid, value)  # type: ignore[arg-type]
    return ok(stage=sid, param=pid, requested=value, applied=applied)


async def h_set_params_bulk(request: web.Request) -> web.Response:
    """Apply a list of params (JSON import). Continues past individual failures."""
    data = await body_json(request)
    items = data.get("params")
    if not isinstance(items, list):
        raise ApiError("'params' list expected")
    dev = st(request).device()
    results = []
    for item in items:
        try:
            sid, pid, val = int(item["stage"]), int(item["param"]), float(item["value"])
        except (KeyError, TypeError, ValueError):
            results.append({"item": item, "error": "malformed entry"})
            continue
        try:
            results.append({"stage": sid, "param": pid, "requested": val,
                            "applied": await dev.set_param(sid, pid, val)})
        except DeviceError as e:
            if e.status is None or e.status == P.Status.ERR_WRONG_MODE:
                raise
            results.append({"stage": sid, "param": pid, "requested": val, "error": str(e)})
    failed = sum(1 for r in results if "error" in r)
    return ok(applied=len(results) - failed, failed=failed, results=results)


async def h_params_export(request: web.Request) -> web.Response:
    """Download all current param values as JSON (make_calib_image.py format + names)."""
    state = st(request)
    dev = state.device()
    params = []
    for sid in await dev.get_stage_list():
        schema = await get_schema(state, sid)
        values = await dev.get_params(sid)
        names = {p.id: p.name for p in schema.params}
        for pid, v in enumerate(values):
            params.append({"stage": sid, "param": pid, "value": v,
                           "stage_name": schema.name, "name": names.get(pid, "")})
    info = await dev.get_info()
    doc = {"device": info.device_name, "fw_version": info.fw_version,
           "exported": time.strftime("%Y-%m-%dT%H:%M:%S"), "params": params}
    fname = f"params_{info.device_name or 'device'}_{time.strftime('%Y%m%d_%H%M%S')}.json"
    return download(json.dumps(doc, indent=2).encode(), fname, "application/json")


async def h_params_save(request: web.Request) -> web.Response:
    await st(request).device().save_params()
    return ok()


async def h_params_reload(request: web.Request) -> web.Response:
    await st(request).device().reload_params()
    return ok()


async def h_params_reset(request: web.Request) -> web.Response:
    data = await body_json(request)
    sid = int_arg(data, "stage", 0, 254, required=False)
    await st(request).device().reset_params(0xFF if sid is None else sid)
    return ok()


# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------


def _settings_json(values: dict[int, int | str]) -> dict[str, Any]:
    named: dict[str, Any] = {}
    for sid, v in values.items():
        sdef = P.SETTINGS_BY_ID.get(sid)
        named[sdef.name if sdef else f"id_{sid}"] = v
    return named


async def h_settings(request: web.Request) -> web.Response:
    values = await st(request).device().get_settings()
    return web.json_response({
        "settings": _settings_json(values),
        "definitions": [{"id": s.id, "name": s.name, "type": "str" if s.type == P.SETTING_STR else "int",
                         "size": s.size, "min": s.minimum, "max": s.maximum} for s in P.SETTINGS],
    })


async def h_settings_set(request: web.Request) -> web.Response:
    """Set one or more settings: ``{"settings": {name: value, ...}}`` (or the bare dict).

    Every value is validated before anything is sent. Values unchanged from the
    device's current ones are skipped.
    """
    data = await body_json(request)
    wanted = data.get("settings", data)
    if not isinstance(wanted, dict) or not wanted:
        raise ApiError("settings object expected")
    encoded: list[tuple[P.SettingDef, bytes]] = []
    for name, value in wanted.items():
        sdef = P.SETTINGS_BY_NAME.get(name)
        if sdef is None:
            raise ApiError(f"unknown setting '{name}'")
        try:
            encoded.append((sdef, P.encode_setting_value(sdef, value)))
        except ValueError as e:
            raise ApiError(str(e)) from None
    dev = st(request).device()
    current = await dev.get_settings()
    reboot_required = False
    changed = []
    for sdef, payload in encoded:
        cur = current.get(sdef.id)
        if cur is not None and P.encode_setting_value_loose(sdef, cur) == payload:
            continue
        reboot_required |= await dev.set_setting(sdef.id, payload)
        changed.append(sdef.name)
    return ok(changed=changed, reboot_required=reboot_required)


async def h_settings_factory(request: web.Request) -> web.Response:
    await st(request).device().factory_reset()
    return ok(reboot_required=True)


async def h_settings_export(request: web.Request) -> web.Response:
    dev = st(request).device()
    info = await dev.get_info()
    doc = {"device": info.device_name, "fw_version": info.fw_version,
           "exported": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "settings": _settings_json(await dev.get_settings())}
    fname = f"settings_{info.device_name or 'device'}_{time.strftime('%Y%m%d_%H%M%S')}.json"
    return download(json.dumps(doc, indent=2).encode(), fname, "application/json")


async def h_host_settings(request: web.Request) -> web.Response:
    return web.json_response(st(request).host_settings)


async def h_host_settings_set(request: web.Request) -> web.Response:
    data = await body_json(request)
    state = st(request)
    new = dict(state.host_settings)
    if "poll_rate_hz" in data:
        try:
            new["poll_rate_hz"] = min(max(float(data["poll_rate_hz"]), 0.5), 30.0)
        except (TypeError, ValueError):
            raise ApiError("'poll_rate_hz' must be a number") from None
    if "default_device_ip" in data:
        new["default_device_ip"] = str(data["default_device_ip"]).strip()
    if "auto_connect" in data:
        new["auto_connect"] = bool(data["auto_connect"])
    state.host_settings = new
    save_host_settings(new)
    return web.json_response(new)


# ---------------------------------------------------------------------------
# Admin: firmware, CLI, diagnostics
# ---------------------------------------------------------------------------

ESP_IMAGE_MAGIC = 0xE9


async def h_fw_upload(request: web.Request) -> web.Response:
    """Start a firmware upload. Body = raw file bytes; query ``target=app|calib``, ``name``."""
    state = st(request)
    if state.fw.state == "running":
        raise ApiError("a firmware upload is already running", 409)
    target_s = request.query.get("target", "app").upper()
    if target_s not in P.FwTarget.__members__:
        raise ApiError("target must be 'app' or 'calib'")
    target = P.FwTarget[target_s]
    data = await request.read()
    if not data:
        raise ApiError("empty file")
    if target == P.FwTarget.APP and data[0] != ESP_IMAGE_MAGIC:
        raise ApiError("not an ESP application image (first byte must be 0xE9)")
    if target == P.FwTarget.CALIB:
        if len(data) > P.SLOT_SIZE:
            raise ApiError(f"calibration image larger than {P.SLOT_SIZE} bytes")
        try:
            P.decode_region_image(data)
        except P.ProtocolError as e:
            raise ApiError(f"invalid calibration image: {e}") from None
    dev = state.device()
    job = FirmwareJob()
    job.state, job.target, job.total = "running", target.name, len(data)
    job.filename = request.query.get("name", "")
    job.started = time.time()
    job.message = "starting"
    state.fw = job
    job.task = asyncio.create_task(_run_fw_upload(dev, job, target, data))
    return ok(job=job.to_json())


async def _run_fw_upload(dev: DeviceClient, job: FirmwareJob, target: P.FwTarget, data: bytes) -> None:
    """Background task: FW_BEGIN, FW_DATA..., FW_END, then REBOOT for APP images."""
    begun = False
    try:
        crc = zlib.crc32(data) & 0xFFFFFFFF
        job.message = "erasing / preparing"
        max_chunk = await dev.fw_begin(target, len(data), crc)
        begun = True
        max_chunk = max(1, min(max_chunk or 4096, P.MAX_PAYLOAD - 4))
        offset = 0
        job.message = "sending"
        while offset < len(data):
            if job.abort_requested:
                await dev.fw_abort()
                begun = False
                job.state, job.message = "aborted", "aborted by user"
                return
            chunk = data[offset:offset + max_chunk]
            nxt = await dev.fw_data(offset, chunk)
            if nxt != offset + len(chunk):
                raise DeviceError(f"device expects offset {nxt}, host sent up to {offset + len(chunk)}")
            offset = nxt
            job.sent = offset
        job.message = "verifying"
        await dev.fw_end()
        begun = False
        if target == P.FwTarget.APP:
            job.message = "rebooting into new firmware"
            await dev.reboot()
            job.message = "done - device is rebooting"
        else:
            job.message = "done - calibration parameters loaded"
        job.state = "done"
    except Exception as e:  # noqa: BLE001 - surface any failure in the job status
        job.state, job.message = "error", str(e)
        log.warning("firmware upload failed: %s", e)
        if begun:
            try:
                await dev.fw_abort()
            except DeviceError:
                pass
    finally:
        job.finished = time.time()


async def h_fw_status(request: web.Request) -> web.Response:
    return web.json_response(st(request).fw.to_json())


async def h_fw_abort(request: web.Request) -> web.Response:
    state = st(request)
    if state.fw.state == "running":
        state.fw.abort_requested = True
        return ok(message="abort requested")
    # Not uploading from here: still tell the device to leave FW_UPDATE mode.
    await state.device().fw_abort()
    return ok(message="FW_ABORT sent")


async def h_calib_image(request: web.Request) -> web.Response:
    img = await st(request).device().get_calib_image()
    return download(img, f"calib_image_{time.strftime('%Y%m%d_%H%M%S')}.bin", "application/octet-stream")


async def h_cli(request: web.Request) -> web.Response:
    data = await body_json(request)
    line = str(data.get("cmd", "")).strip()
    if not line:
        raise ApiError("missing 'cmd'")
    return web.json_response({"output": await st(request).device().cli_exec(line)})


async def h_diag(request: web.Request) -> web.Response:
    text = await st(request).device().get_diag()
    return web.json_response({"rows": P.decode_diag(text), "raw": text})


async def h_diag_reset(request: web.Request) -> web.Response:
    await st(request).device().reset_diag()
    return ok()


# ---------------------------------------------------------------------------
# App factory
# ---------------------------------------------------------------------------


async def h_index(request: web.Request) -> web.FileResponse:
    return web.FileResponse(STATIC_DIR / "index.html")


@web.middleware
async def no_cache_middleware(request: web.Request, handler: Handler) -> web.StreamResponse:
    resp = await handler(request)
    resp.headers.setdefault("Cache-Control", "no-store")
    return resp


def create_app(device: str | None = None) -> web.Application:
    """Build the aiohttp application. ``device`` = initial target ``ip[:port]``."""
    app = web.Application(middlewares=[no_cache_middleware, error_middleware],
                          client_max_size=16 * 1024 * 1024)
    state = HostState()
    app[STATE_KEY] = state

    async def on_startup(app: web.Application) -> None:
        await state.beacons.start()
        target = device or (state.host_settings.get("default_device_ip")
                            if state.host_settings.get("auto_connect") else None)
        if target:
            await state.set_target(target)
            log.info("device target: %s", target)

    async def on_cleanup(app: web.Application) -> None:
        state.beacons.stop()
        if state.fw.task is not None and not state.fw.task.done():
            state.fw.task.cancel()
        if state.client is not None:
            await state.client.close()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)

    r = app.router
    r.add_get("/", h_index)
    r.add_get("/api/status", h_status)
    r.add_post("/api/connect", h_connect)
    r.add_post("/api/disconnect", h_disconnect)
    r.add_get("/api/discover", h_discover)
    r.add_get("/api/info", h_info)
    r.add_post("/api/reboot", h_reboot)
    r.add_post("/api/device_mode", h_device_mode)
    r.add_post("/api/run_config", h_run_config)
    r.add_get("/api/live", h_live)
    r.add_get("/api/errors", h_errors)
    r.add_post("/api/errors/clear", h_errors_clear)
    r.add_get("/api/algorithms", h_algorithms)
    r.add_get("/api/stages", h_stages)
    r.add_get("/api/stages/{sid}/schema", h_stage_schema)
    r.add_get("/api/stages/{sid}/params", h_stage_params)
    r.add_get("/api/calib", h_calib)
    r.add_post("/api/params", h_set_param)
    r.add_post("/api/params/bulk", h_set_params_bulk)
    r.add_get("/api/params/export", h_params_export)
    r.add_post("/api/params/save", h_params_save)
    r.add_post("/api/params/reload", h_params_reload)
    r.add_post("/api/params/reset", h_params_reset)
    r.add_get("/api/settings", h_settings)
    r.add_post("/api/settings", h_settings_set)
    r.add_post("/api/settings/factory_reset", h_settings_factory)
    r.add_get("/api/settings/export", h_settings_export)
    r.add_get("/api/host_settings", h_host_settings)
    r.add_post("/api/host_settings", h_host_settings_set)
    r.add_post("/api/fw/upload", h_fw_upload)
    r.add_get("/api/fw/status", h_fw_status)
    r.add_post("/api/fw/abort", h_fw_abort)
    r.add_get("/api/calib_image", h_calib_image)
    r.add_post("/api/cli", h_cli)
    r.add_get("/api/diag", h_diag)
    r.add_post("/api/diag/reset", h_diag_reset)
    r.add_static("/static/", STATIC_DIR)
    return app
