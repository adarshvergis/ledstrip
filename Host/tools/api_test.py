"""End-to-end REST API test against a running host server + device simulator.

Start the simulator and the host first::

    python Host/tools/sim_device.py
    cd Host && python -m ledhost --device 127.0.0.1

then run ``python Host/tools/api_test.py [--base http://127.0.0.1:8080]``.
Only the standard library is used. Note: the test changes simulator state
(and restores host_settings.json afterwards).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from ledhost import protocol as P  # noqa: E402

BASE = "http://127.0.0.1:8080"
passed = 0
failed: list[str] = []


def call(method: str, path: str, body: Any = None, raw: bytes | None = None,
         expect: int = 200) -> Any:
    data, headers = None, {}
    if raw is not None:
        data, headers = raw, {"Content-Type": "application/octet-stream"}
    elif body is not None:
        data, headers = json.dumps(body).encode(), {"Content-Type": "application/json"}
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            status, payload, hdrs = r.status, r.read(), r.headers
    except urllib.error.HTTPError as e:
        status, payload, hdrs = e.code, e.read(), e.headers
    if status != expect:
        raise AssertionError(f"{method} {path}: HTTP {status} (want {expect}): {payload[:300]!r}")
    if "json" in hdrs.get("Content-Type", "") and "attachment" not in hdrs.get("Content-Disposition", ""):
        return json.loads(payload)
    return payload


def check(name: str, cond: bool, detail: Any = "") -> None:
    global passed
    if cond:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed.append(name)
        print(f"  FAIL {name} {detail}")


def step(name: str, fn) -> None:  # type: ignore[no-untyped-def]
    print(f"[{name}]")
    try:
        fn()
    except Exception as e:  # noqa: BLE001
        failed.append(f"{name}: {e}")
        print(f"  FAIL {name}: {e}")


def wait_fw(timeout: float = 60) -> dict[str, Any]:
    t0 = time.time()
    while time.time() - t0 < timeout:
        j = call("GET", "/api/fw/status")
        if j["state"] != "running":
            return j
        time.sleep(0.2)
    raise AssertionError("firmware job did not finish")


def wait_connected(timeout: float = 10) -> dict[str, Any]:
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = call("GET", "/api/status")
        if s["connected"] and s["info"]["uptime_ms"] < 5000:
            return s
        time.sleep(0.3)
    raise AssertionError("device did not come back")


def t_status() -> None:
    s = call("GET", "/api/status")
    check("connected", s["connected"], s)
    check("info has build flags", s["info"]["build_flags"] == 7)
    check("has_cli/has_diag", s["info"]["has_cli"] and s["info"]["has_diag"])


def t_discover() -> None:
    d = call("GET", "/api/discover?timeout=1")
    check("sim discovered", any(x["ip"] == "127.0.0.1" and x["port"] == 4210 for x in d["devices"]), d)


def t_static() -> None:
    html = call("GET", "/")
    check("index served", b"LED Strip Host" in html)
    check("app.js served", b"liveLoop" in call("GET", "/static/app.js"))
    check("style.css served", b"--accent" in call("GET", "/static/style.css"))


def t_schema() -> None:
    i = call("GET", "/api/info")
    check("info num_leds", i["num_leds"] == 60)
    a = call("GET", "/api/algorithms")["algorithms"]
    check("4 algorithms", len(a) == 4 and a[0]["name"] == "Amplitude Flash", a)
    stages = call("GET", "/api/stages")["stages"]
    check("stage list", stages == [0, 1, 2, 3, 4], stages)
    sc = call("GET", "/api/stages/2/schema")
    check("schema enum options", sc["params"][1]["options"] == ["Hard", "Soft", "Fade"], sc)
    check("schema float clean", sc["params"][0]["step"] == 0.01, sc["params"][0])
    pv = call("GET", "/api/stages/2/params")
    check("params values", pv["values"] == [0.3, 1.0, 120.0], pv)
    call("GET", "/api/stages/99/schema", expect=400)
    check("unknown stage -> 400", True)
    c = call("GET", "/api/calib")
    check("calib bundle", len(c["stages"]) == 5 and "values" in c["stages"][0] and c["device_mode"] == 0)


def t_calibration() -> None:
    r = call("POST", "/api/params", {"stage": 1, "param": 1, "value": 5}, expect=409)
    check("SET_PARAM in MAIN -> 409 wrong mode", r["status"] == "ERR_WRONG_MODE", r)
    call("POST", "/api/device_mode", {"mode": 1})
    check("calibration mode", call("GET", "/api/info")["device_mode"] == 1)
    r = call("POST", "/api/params", {"stage": 1, "param": 1, "value": 5.5})
    check("set float", r["applied"] == 5.5, r)
    r = call("POST", "/api/params", {"stage": 1, "param": 1, "value": 9999})
    check("clamped to max", r["applied"] == 500.0, r)
    r = call("POST", "/api/params", {"stage": 0, "param": 0, "value": 0})
    check("bool off", r["applied"] == 0.0, r)
    r = call("POST", "/api/params", {"stage": 2, "param": 1, "value": 2})
    check("enum", r["applied"] == 2.0, r)
    check("calib_dirty set", call("GET", "/api/info")["calib_dirty"] == 1)
    r = call("POST", "/api/params", {"stage": 9, "param": 0, "value": 1}, expect=400)
    check("bad stage -> 400 ERR_BAD_ARG", r["status"] == "ERR_BAD_ARG", r)
    b = call("POST", "/api/params/bulk", {"params": [
        {"stage": 3, "param": 0, "value": 2.0}, {"stage": 3, "param": 1, "value": 60},
        {"stage": 42, "param": 0, "value": 1}, {"bogus": 1}]})
    check("bulk applied 2 failed 2", b["applied"] == 2 and b["failed"] == 2, b)
    exp = json.loads(call("GET", "/api/params/export"))
    check("export has all params", len(exp["params"]) == 17, len(exp["params"]))
    check("export value", any(p["stage"] == 3 and p["param"] == 1 and p["value"] == 60 for p in exp["params"]))
    call("POST", "/api/params/save")
    check("save clears dirty", call("GET", "/api/info")["calib_dirty"] == 0)
    call("POST", "/api/params", {"stage": 4, "param": 0, "value": 12})
    call("POST", "/api/params/reload")
    check("reload discards", call("GET", "/api/stages/4/params")["values"][0] == 8.0)
    call("POST", "/api/params/reset", {"stage": 1})
    check("reset stage", call("GET", "/api/stages/1/params")["values"][1] == 10.0)
    call("POST", "/api/params/reset", {})
    check("reset all", call("GET", "/api/stages/3/params")["values"][1] == 70.0)
    call("POST", "/api/params/reload")
    call("POST", "/api/device_mode", {"mode": "main"})
    check("back to main", call("GET", "/api/info")["device_mode"] == 0)
    call("POST", "/api/device_mode", {"mode": 2}, expect=400)
    check("FW_UPDATE mode refused", True)


def t_run_config() -> None:
    call("POST", "/api/run_config", {"run_mode": 1, "pattern": 2, "algorithm": 1})
    i = call("GET", "/api/info")
    check("run config", (i["run_mode"], i["active_pattern"], i["active_algorithm"]) == (1, 2, 1), i)
    call("POST", "/api/run_config", {"brightness": 255})
    check("brightness 255 via SET_SETTING", call("GET", "/api/info")["brightness"] == 255)
    call("POST", "/api/run_config", {"brightness": 100})
    check("brightness 100", call("GET", "/api/info")["brightness"] == 100)
    r = call("POST", "/api/run_config", {"algorithm": 9}, expect=400)
    check("bad algorithm -> 400", r["status"] == "ERR_BAD_ARG", r)
    call("POST", "/api/run_config", {"run_mode": "audio", "algorithm": 0})


def t_live() -> None:
    lv = call("GET", "/api/live")
    check("live leds", lv["num_leds"] == 60 and len(lv["leds"]) == 360, lv.get("num_leds"))
    check("live bands", len(lv["bands"]) == 8)
    check("live bpm", lv["bpm"] == 122.0)
    t0 = time.time()
    for _ in range(30):
        call("GET", "/api/live")
    rate = 30 / (time.time() - t0)
    check(f"live poll rate {rate:.0f}/s >= 15", rate >= 15)


def t_errors() -> None:
    e = call("GET", "/api/errors")["errors"]
    check("errors listed", len(e) == 2 and e[0]["name"] == "MIC_DMA_OVERRUN", e)
    call("POST", "/api/errors/clear")
    check("errors cleared", call("GET", "/api/errors")["errors"] == [])


def t_settings() -> None:
    s = call("GET", "/api/settings")
    check("settings", s["settings"]["device_name"] == "LED Strip Sim" and s["settings"]["max_current_ma"] == 1500, s)
    r = call("POST", "/api/settings", {"settings": {"device_name": "Desk Strip", "max_current_ma": 2000}})
    check("set settings", sorted(r["changed"]) == ["device_name", "max_current_ma"] and r["reboot_required"], r)
    r = call("POST", "/api/settings", {"settings": {"max_current_ma": 2000}})
    check("unchanged skipped", r["changed"] == [] and not r["reboot_required"], r)
    r = call("POST", "/api/settings", {"settings": {"max_current_ma": 1800}})
    check("non-network setting -> no reboot", r["changed"] == ["max_current_ma"] and not r["reboot_required"], r)
    r = call("POST", "/api/settings", {"settings": {"ap_password": "short"}}, expect=400)
    check("ap_password validation", "8..64" in r["error"], r)
    call("POST", "/api/settings", {"settings": {"nope": 1}}, expect=400)
    r = call("POST", "/api/settings", {"settings": {"num_leds": 500}}, expect=400)
    check("device rejects num_leds > max", r["status"] == "ERR_BAD_ARG", r)
    exp = json.loads(call("GET", "/api/settings/export"))
    check("settings export", exp["settings"]["device_name"] == "Desk Strip")
    call("POST", "/api/settings/factory_reset")
    check("factory reset", call("GET", "/api/settings")["settings"]["device_name"] == "LED Strip Sim")
    r = call("POST", "/api/settings", {"settings": exp["settings"]})
    check("settings import", "device_name" in r["changed"], r)
    call("POST", "/api/settings/factory_reset")


def t_host_settings() -> None:
    orig = call("GET", "/api/host_settings")
    r = call("POST", "/api/host_settings", {"poll_rate_hz": 99, "default_device_ip": " 10.0.0.5 "})
    check("host settings clamp/strip", r["poll_rate_hz"] == 30.0 and r["default_device_ip"] == "10.0.0.5", r)
    call("POST", "/api/host_settings", orig)


def t_cli_diag() -> None:
    out = call("POST", "/api/cli", {"cmd": "help"})["output"]
    check("cli help", "get_fw_version" in out, out)
    check("cli echo", call("POST", "/api/cli", {"cmd": "echo hi there"})["output"] == "hi there\n")
    d = call("GET", "/api/diag")
    check("diag rows", len(d["rows"]) >= 5 and set(d["rows"][0]) == {"group", "name", "value", "unit"}, d["rows"][:1])
    call("POST", "/api/diag/reset")
    check("diag reset", True)


def t_firmware() -> None:
    img = call("GET", "/api/calib_image")
    params = P.decode_calib_image(img)
    check("calib image decodes", len(params) == 17, len(params))
    for p in params:
        if p["stage"] == 1 and p["param"] == 0:
            p["value"] = 20000
    new_img = P.encode_calib_image(params, 1)
    call("POST", "/api/fw/upload?target=calib&name=calib.bin", raw=new_img)
    j = wait_fw()
    check("calib upload done", j["state"] == "done" and j["sent"] == len(new_img), j)
    check("calib values loaded", call("GET", "/api/stages/1/params")["values"][0] == 20000.0)
    check("back in MAIN after calib", call("GET", "/api/info")["device_mode"] == 0)

    r = call("POST", "/api/fw/upload?target=calib", raw=b"garbage" * 10, expect=400)
    check("bad calib image rejected", "invalid calibration image" in r["error"], r)
    r = call("POST", "/api/fw/upload?target=app", raw=b"\x00" * 100, expect=400)
    check("non-ESP app rejected", "0xE9" in r["error"], r)

    app = b"\xE9" + os.urandom(200 * 1024)
    call("POST", "/api/fw/upload?target=app&name=fw.bin", raw=app)
    time.sleep(0.3)
    call("POST", "/api/fw/upload?target=app", raw=app, expect=409)
    check("second upload refused while running", True)
    j = wait_fw()
    check("app upload done", j["state"] == "done" and j["percent"] == 100.0, j)
    s = wait_connected()
    check("device back after reboot", s["info"]["uptime_ms"] < 5000)

    call("POST", "/api/fw/upload?target=app", raw=app)
    time.sleep(0.5)
    call("POST", "/api/fw/abort")
    j = wait_fw()
    check("abort", j["state"] == "aborted" and j["sent"] < len(app), j)
    check("main mode after abort", call("GET", "/api/info")["device_mode"] == 0)


def t_reconnect() -> None:
    call("POST", "/api/disconnect")
    s = call("GET", "/api/status")
    check("disconnected", s["device"] is None and not s["connected"])
    r = call("GET", "/api/info", expect=503)
    check("no device -> 503", "No device" in r["error"])
    r = call("POST", "/api/connect", {"ip": "127.0.0.1:4299"}, expect=502)
    check("connect refused -> 502", "cannot connect" in r["error"], r)
    r = call("POST", "/api/connect", {"ip": "127.0.0.1"})
    check("reconnect", r["info"]["device_name"] == "LED Strip Sim", r)


def main() -> None:
    global BASE
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=BASE)
    BASE = ap.parse_args().base.rstrip("/")
    for name, fn in [("status", t_status), ("discover", t_discover), ("static", t_static),
                     ("schema", t_schema), ("calibration", t_calibration), ("run_config", t_run_config),
                     ("live", t_live), ("errors", t_errors), ("settings", t_settings),
                     ("host_settings", t_host_settings), ("cli/diag", t_cli_diag),
                     ("firmware", t_firmware), ("reconnect", t_reconnect)]:
        step(name, fn)
    print(f"\n{passed} checks passed, {len(failed)} failed")
    for f in failed:
        print("  -", f)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
