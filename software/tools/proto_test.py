"""Protocol v2 end-to-end test: host link + UDP live stream against the device simulator.

Starts sim_device.py twice (debug build flags, then release flags) on private ports and
checks framing, the live stream (rate, sequence, unsubscribe, lease expiry), calibration,
settings, firmware upload and that release builds refuse the debug-only commands.

    python software/tools/proto_test.py
"""

from __future__ import annotations

import socket
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

HOST_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST_DIR))

from ledapp import DeviceError, Link, LiveReceiver  # noqa: E402
from ledhost import protocol as P  # noqa: E402
from ledhost.dsp import biquad_db, spectrum_edges  # noqa: E402
from ledhost.protocol import BuildFlag, Cmd, Status  # noqa: E402

TCP_PORT, UDP_PORT = 14210, 14211
passed = failed = 0


def check(name: str, ok: bool, detail: object = "") -> None:
    global passed, failed
    if ok:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed += 1
        print(f"  FAIL {name} {detail}")


def expect_status(name: str, fn, status: int) -> None:
    try:
        fn()
        check(name, False, "no error")
    except DeviceError as e:
        check(name, e.status == status, f"status {e.status}")


def start_sim(flags: int) -> subprocess.Popen:
    p = subprocess.Popen([sys.executable, str(HOST_DIR / "tools" / "sim_device.py"), "--flags", str(flags),
                          "--tcp-port", str(TCP_PORT), "--udp-port", str(UDP_PORT), "--fw-delay", "0"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", TCP_PORT), timeout=0.2).close()
            time.sleep(0.2)
            return p
        except OSError:
            time.sleep(0.1)
    p.kill()
    raise SystemExit("simulator did not start")


def frames_in(rx: LiveReceiver, seconds: float) -> int:
    rx.reset()
    time.sleep(seconds)
    return rx.frames


def t_offline() -> None:
    print("offline encoders")
    f = P.encode_frame(Cmd.GET_INFO, 7, b"abc")
    h = P.decode_header(f[:8])
    check("frame round trip", (h.cmd, h.seq, h.length) == (Cmd.GET_INFO, 7, 3))
    live = P.LiveData(1234, 100, 1205, 7, 9000, 1, [1, 2, 3], bytes(range(9)))
    check("live round trip", P.LiveData.decode(live.encode()) == live)
    live3 = P.LiveData(1234, 100, 1205, 7, 9000, 1, [1, 2, 3], bytes(range(9)), 4,
                       {0: bytes([0, 80, 255]), P.TAP_END: bytes([10, 20, 30])})
    check("live v3 round trip (genre, tagged spectra)", P.LiveData.decode(live3.encode()) == live3)
    check("spectrum dB scale", live3.spectrum_db(0) == [-120.0, -80.0, 7.5] and live3.spectrum_db(7) == [])
    lay = P.SpectrumLayout(60, 0x8001, spectrum_edges())
    lay2 = P.SpectrumLayout.decode(lay.encode())
    fq = lay2.freqs(16000)
    check("subscribe layout: 72 points, 62.5 Hz .. 8 kHz", lay2 == lay and len(fq) == 72
          and abs(fq[0] - 62.5) < 0.1 and 7000 < fq[-1] < 8000, (len(fq), fq[0], fq[-1]))
    hp = biquad_db(2, 45.0, 0.707, 0.0, 16000.0, [45.0, 1000.0, 10.0])
    check("filter response (45 Hz high-pass)", abs(hp[0] + 3.0) < 0.1 and abs(hp[1]) < 0.05 and hp[2] < -25, hp)
    img = P.encode_settings_region({"sta_ssid": "Home", "sta_password": "pw12345678", "num_leds": 30})
    info = P.decode_region_image(img[:P.SLOT_SIZE])
    recs = {k: d for k, _, d in info["records"]}
    check("settings image", len(img) == 2 * P.SLOT_SIZE and info["store_id"] == P.STORE_SETTINGS
          and recs[3] == b"Home" and recs[9] == (30).to_bytes(2, "little") and img[P.SLOT_SIZE:] == b"\xFF" * 4096)
    try:
        P.encode_settings_region({"ap_ssid": "x"})
        check("removed soft-AP setting rejected", False)
    except ValueError:
        check("removed soft-AP setting rejected", True)


def t_debug_build() -> None:
    print("debug build (flags 15)")
    sim = start_sim(15)
    try:
        link = Link("127.0.0.1", TCP_PORT)
        info = P.DeviceInfo.decode(link.request(Cmd.GET_INFO))
        check("protocol version", info.proto_version == P.PROTO_VERSION)
        check("debug host flag", info.has(BuildFlag.ENABLE_DEBUG_HOST))

        rx = LiveReceiver()
        rx.device_ip = "127.0.0.1"
        rx.start()
        taps = (1 << 0) | (1 << 5) | (1 << P.TAP_END)          # input, after Filter A, after all stages
        lay = P.SpectrumLayout.decode(link.request(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(rx.port, 50, taps)))
        check("subscribe ack (rate, taps, layout)", lay.rate_hz == 50 and lay.taps == taps and len(lay.edges) == 73)
        n = frames_in(rx, 1.0)
        check("live stream ~50 fps", 35 <= n <= 55, n)
        check("no lost frames", rx.dropped == 0, rx.dropped)
        live = rx.latest[0] if rx.latest else None
        check("live frame content", live is not None and len(live.leds) == 180 and len(live.bands) == 8)
        check("live genre + requested taps, 72 points each", live is not None and 1 <= live.genre <= 8
              and sorted(live.spectra) == [0, 5, P.TAP_END] and all(len(v) == 72 for v in live.spectra.values()),
              live and {k: len(v) for k, v in live.spectra.items()})
        if live:
            inp, hp = live.spectrum_db(0), live.spectrum_db(5)
            want = biquad_db(2, 45.0, 0.707, 0.0, 16000.0, [62.5])[0]       # Filter A default at point 0
            check("filter tap = input x 45 Hz high-pass response (low end cut, mids kept)",
                  abs((hp[0] - inp[0]) - want) < 0.6 and abs(hp[20] - inp[20]) < 0.6,
                  (round(hp[0] - inp[0], 2), round(want, 2), hp[20] - inp[20]))
        link.request(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(rx.port, 50, 1 << 0))
        g = P.LiveData.decode(link.request(Cmd.GET_LIVE))
        check("unticked overlays are not sent", sorted(g.spectra) == [0], sorted(g.spectra))
        link.request(Cmd.SET_RUN_CONFIG, P.build_set_run_config(algorithm=1))
        g = P.LiveData.decode(link.request(Cmd.GET_LIVE))
        check("no genre without the music stage", g.genre == 0)
        link.request(Cmd.SET_RUN_CONFIG, P.build_set_run_config(algorithm=0))
        link.request(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(rx.port, 0))
        time.sleep(0.1)
        check("unsubscribe stops stream", frames_in(rx, 0.5) == 0)
        link.request(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(rx.port, 50))
        time.sleep(3.5)                     # no host traffic -> lease expires
        check("lease expiry stops stream", frames_in(rx, 0.5) == 0)
        link.request(Cmd.PING)
        link.request(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(rx.port, 50))
        check("re-subscribe after lease", frames_in(rx, 0.5) > 10)

        expect_status("SET_PARAM outside calibration", lambda: link.request(
            Cmd.SET_PARAM, P.build_set_param(0, 1, 50.0)), Status.ERR_WRONG_MODE)
        link.request(Cmd.SET_DEVICE_MODE, b"\x01")
        v = struct.unpack("<f", link.request(Cmd.SET_PARAM, P.build_set_param(0, 1, 9999.0)))[0]
        check("SET_PARAM clamps", v == 300.0, v)
        link.request(Cmd.SET_DEVICE_MODE, b"\x00")

        s = P.decode_settings(link.request(Cmd.GET_SETTINGS))
        check("settings have sta_ssid, no soft-AP", 3 in s and not {2, 5, 6, 7} & set(s), s)
        r = link.request(Cmd.SET_SETTING, P.build_set_setting(3, b"OtherWiFi"))
        check("SSID change needs reboot", r == b"\x01")
        expect_status("removed setting id 5", lambda: link.request(Cmd.SET_SETTING, P.build_set_setting(5, b"x")),
                      Status.ERR_BAD_ARG)
        check("errors readable", isinstance(P.decode_errors(link.request(Cmd.GET_ERRORS)), list))
        check("diag readable", len(P.decode_diag(link.request(Cmd.GET_DIAG).decode())) > 3)

        img = b"\xE9" + bytes(10000)
        crc = zlib.crc32(img) & 0xFFFFFFFF
        chunk = struct.unpack("<H", link.request(Cmd.FW_BEGIN, P.build_fw_begin(0, len(img), crc)))[0]
        off = 0
        while off < len(img):
            off = struct.unpack("<I", link.request(Cmd.FW_DATA, P.build_fw_data(off, img[off:off + chunk])))[0]
        link.request(Cmd.FW_END)
        check("firmware upload", off == len(img))
        link.close()
    finally:
        sim.kill()


def t_release_build() -> None:
    print("release build (flags 0)")
    sim = start_sim(0)
    try:
        link = Link("127.0.0.1", TCP_PORT)
        info = P.DeviceInfo.decode(link.request(Cmd.GET_INFO))
        check("no debug flags", info.build_flags == 0)
        for cmd in (Cmd.GET_SETTINGS, Cmd.CLI_EXEC, Cmd.GET_DIAG, Cmd.GET_ERRORS, Cmd.FACTORY_RESET_SETTINGS):
            expect_status(f"{cmd.name} refused", lambda c=cmd: link.request(c), Status.ERR_UNKNOWN_CMD)
        check("main view still works", len(P.LiveData.decode(link.request(Cmd.GET_LIVE)).leds) == 180)
        check("calibration still works", len(P.decode_algorithms(link.request(Cmd.GET_ALGORITHMS))) == 4)
        link.close()
    finally:
        sim.kill()


if __name__ == "__main__":
    t_offline()
    t_debug_build()
    t_release_build()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
