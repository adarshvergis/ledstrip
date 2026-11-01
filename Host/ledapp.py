"""LED strip host - desktop app (Python standard library only: tkinter, socket, threading).

The device joins the home WiFi (station mode), so this PC keeps its own network.

Threads:
  * UI thread (tkinter): draws the newest live frame every ~15 ms, never blocks on I/O.
  * worker thread: owns the TCP command link (4210); runs requests in order, keeps the
    link alive (PING every second, which also renews the live-stream lease) and reconnects.
    On every (re)connect it fetches info, algorithms, parameter schemas and - in debug
    builds - errors and self diagnostics once; nothing is polled afterwards.
  * live thread: receives the UDP live stream (LIVE_SUBSCRIBE) and keeps only the newest
    frame, so a late frame is never drawn after a newer one.

Views: Main (live view + run controls), Calibration, Firmware. Debug mode (menu View, only for
debug firmware) adds Settings (WiFi credentials), Errors, Diagnostics, CLI and a request log.

    python Host/ledapp.py [--device IP] [--debug]
"""

from __future__ import annotations

import argparse
import collections
import json
import queue
import select
import socket
import sys
import threading
import time
import tkinter as tk
import zlib
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ledhost import protocol as P  # noqa: E402
from ledhost.protocol import BuildFlag, Cmd, DeviceMode, ParamType  # noqa: E402

SETTINGS_FILE = Path(__file__).resolve().parent / "ledapp_settings.json"
DEFAULT_HOST_SETTINGS = {"device": "", "live_hz": 60, "debug_mode": False}
KEEPALIVE_S = 1.0           # < firmware NET_LIVE_LEASE_MS (3 s)
RECONNECT_S = 2.0
PARAM_DEBOUNCE_MS = 60
FRAME_MS = 15


# ---------------------------------------------------------------------------
# Device link (worker thread only)
# ---------------------------------------------------------------------------


class DeviceError(Exception):
    def __init__(self, status: int, msg: str = "") -> None:
        self.status = status
        super().__init__(f"{P.STATUS_TEXT.get(status, f'status {status}')}{': ' + msg if msg else ''}")


class Link:
    """Blocking request/response over one TCP connection."""

    def __init__(self, host: str, port: int = P.TCP_PORT) -> None:
        self.host = host
        self.sock = socket.create_connection((host, port), timeout=5.0)   # allows 2 SYN retries
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.seq = 0
        self.last_tx = time.monotonic()

    def request(self, cmd: int, payload: bytes = b"", timeout: float = 5.0) -> bytes:
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.settimeout(timeout)
        self.sock.sendall(P.encode_frame(cmd, self.seq, payload))
        self.last_tx = time.monotonic()
        while True:
            hdr = P.decode_header(self._recv(P.HEADER_SIZE))
            body = self._recv(hdr.length) if hdr.length else b""
            if hdr.seq == self.seq and hdr.cmd == cmd | P.RESPONSE_FLAG:
                break
        if hdr.status != P.Status.OK:
            raise DeviceError(hdr.status, body.decode("ascii", "replace"))
        return body

    def _recv(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("device closed the connection")
            buf += chunk
        return bytes(buf)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class Worker(threading.Thread):
    """Runs jobs ``fn(link)`` in order; results go back to the UI through ``results``."""

    def __init__(self, results: queue.Queue, live_port: int) -> None:
        super().__init__(daemon=True, name="worker")
        self.jobs: queue.Queue = queue.Queue()
        self.results = results
        self.live_port = live_port
        self.live_hz = 60
        self.link: Link | None = None
        self.target = ""
        self.next_try = 0.0
        self.on_connect: Callable[[Link], Any] | None = None   # runs in this thread
        self.log_requests = False

    # -- called from the UI thread --------------------------------------------

    def submit(self, fn: Callable[[Link], Any], done: Callable[[Any], None] | None = None,
               fail: Callable[[Exception], None] | None = None) -> None:
        self.jobs.put((fn, done, fail))

    def connect(self, host: str) -> None:
        self.jobs.put(("connect", host))

    def disconnect(self) -> None:
        self.jobs.put(("connect", ""))

    # -- worker thread ------------------------------------------------------------

    def post(self, cb: Callable[..., None], *args: Any) -> None:
        self.results.put((cb, args))

    def run(self) -> None:
        while True:
            try:
                job = self.jobs.get(timeout=0.25)
            except queue.Empty:
                job = None
            if job and job[0] == "connect":
                self._drop("disconnected" if not job[1] else "connecting")
                self.target, self.next_try = job[1], 0.0
                job = None
            self._maintain()
            if job:
                self._run_job(*job)

    def _run_job(self, fn: Callable, done: Callable | None, fail: Callable | None) -> None:
        if self.link is None:
            if fail:
                self.post(fail, ConnectionError("not connected"))
            return
        try:
            value = fn(self.link)
        except DeviceError as e:
            if fail:
                self.post(fail, e)
            return
        except (OSError, P.ProtocolError) as e:
            self._drop(f"link lost: {e}")
            if fail:
                self.post(fail, e)
            return
        if done:
            self.post(done, value)

    def _maintain(self) -> None:
        now = time.monotonic()
        if self.link is None:
            if self.target and now >= self.next_try:
                self.next_try = now + RECONNECT_S
                try:
                    self.link = Link(self.target)
                    if self.on_connect:
                        self.on_connect(self.link)
                    self.request_live(self.link)
                except (OSError, P.ProtocolError, DeviceError) as e:
                    self._drop(f"cannot connect to {self.target}: {e}")
            return
        if now - self.link.last_tx >= KEEPALIVE_S:
            try:
                # also renews the live-stream lease; 5 s rides out TCP retransmit back-off on a weak link
                self.link.request(Cmd.PING, timeout=5.0)
            except (OSError, P.ProtocolError, DeviceError) as e:
                self._drop(f"link lost: {e}")

    def request_live(self, link: Link) -> None:
        link.request(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(self.live_port, self.live_hz))

    def _drop(self, why: str) -> None:
        if self.link is not None:
            self.link.close()
            self.link = None
        self.post(self._status_cb, why)

    _status_cb: Callable[[str], None] = staticmethod(lambda why: None)


# ---------------------------------------------------------------------------
# Live stream receiver
# ---------------------------------------------------------------------------


class LiveReceiver(threading.Thread):
    """UDP live frames -> ``latest`` (newest only) plus simple link statistics."""

    def __init__(self) -> None:
        super().__init__(daemon=True, name="live")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        self.port = self.sock.getsockname()[1]
        self.device_ip = ""
        self.latest: tuple[P.LiveData, float] | None = None
        self.reset()

    def reset(self) -> None:
        self.last_seq: int | None = None
        self.last_time = 0.0
        self.frames = 0
        self.dropped = 0
        self.min_offset = float("inf")      # host_ms - device_ms of the fastest frame seen
        self.age_sum = 0.0

    def run(self) -> None:
        while True:
            try:
                data, addr = self.sock.recvfrom(4096)
                now = time.perf_counter()
                if addr[0] != self.device_ip or len(data) < P.HEADER_SIZE:
                    continue
                hdr = P.decode_header(data[:P.HEADER_SIZE])
                if hdr.cmd != P.LIVE_FRAME:
                    continue
                live = P.LiveData.decode(data[P.HEADER_SIZE:P.HEADER_SIZE + hdr.length])
            except (OSError, P.ProtocolError):
                continue
            if self.last_seq is not None:
                d = (hdr.seq - self.last_seq) & 0xFFFF
                if (d == 0 or d > 0x8000) and now - self.last_time < 0.5:
                    continue                    # duplicate or late frame (a reboot restarts seq)
                if d < 0x8000:
                    self.dropped += d - 1
            self.last_seq, self.last_time = hdr.seq, now
            # Clocks are not synchronised: the age shown is the delay above the fastest frame.
            offset = now * 1000.0 - live.t_ms
            self.min_offset = min(self.min_offset, offset)
            self.age_sum += offset - self.min_offset
            self.frames += 1
            self.latest = (live, now)


def _local_ipv4() -> list[str]:
    ips = {"127.0.0.1"}
    try:
        ips.update(a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET))
    except OSError:
        pass
    return sorted(ips)


def discover(timeout: float = 1.0) -> list[dict[str, Any]]:
    """Broadcast LEDSTRIP_DISCOVER on every local interface and collect the replies.

    Windows sends 255.255.255.255 out of one adapter only (often a virtual one such as
    WSL/Hyper-V), so one socket is bound to each local IPv4 address. The request is repeated
    a few times because a single datagram is easily lost on a weak WiFi link.
    """
    socks: dict[socket.socket, str] = {}
    for ip in _local_ipv4():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.bind((ip, 0))
            socks[s] = "127.0.0.1" if ip.startswith("127.") else "255.255.255.255"
        except OSError:
            s.close()
    found: dict[str, dict[str, Any]] = {}
    end = time.monotonic() + timeout
    next_send = 0.0
    try:
        while socks and time.monotonic() < end:
            now = time.monotonic()
            if now >= next_send:
                next_send = now + timeout / 4
                for s, dst in socks.items():
                    try:
                        s.sendto(P.DISCOVER_REQUEST, (dst, P.UDP_PORT))
                    except OSError:
                        pass
            ready, _, _ = select.select(list(socks), [], [], max(0.0, min(end, next_send) - time.monotonic()))
            for s in ready:
                try:
                    data, addr = s.recvfrom(512)
                except OSError:
                    continue
                d = P.parse_discovery_line(data.decode("utf-8", "replace"))
                if d:
                    if d["ip"] in ("0.0.0.0", ""):
                        d["ip"] = addr[0]
                    found[d["ip"]] = d
    finally:
        for s in socks:
            s.close()
    return list(found.values())


# ---------------------------------------------------------------------------
# UI
# ---------------------------------------------------------------------------


class ScrollFrame(ttk.Frame):
    """Vertically scrollable container (``.body`` holds the content)."""

    def __init__(self, parent: tk.Widget) -> None:
        super().__init__(parent)
        self.canvas = tk.Canvas(self, highlightthickness=0)
        sb = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.body = ttk.Frame(self.canvas)
        self.body.bind("<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        win = self.canvas.create_window((0, 0), window=self.body, anchor="nw")
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(win, width=e.width))
        self.canvas.configure(yscrollcommand=sb.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.canvas.bind_all("<MouseWheel>", self._wheel, add="+")

    def _wheel(self, e: tk.Event) -> None:
        if str(e.widget).startswith(str(self)):
            self.canvas.yview_scroll(int(-e.delta / 120), "units")


class App:
    def __init__(self, root: tk.Tk, args: argparse.Namespace) -> None:
        self.root = root
        self.cfg = self._load_cfg()
        if args.device:
            self.cfg["device"] = args.device
        if args.debug:
            self.cfg["debug_mode"] = True

        self.results: queue.Queue = queue.Queue()
        self.live = LiveReceiver()
        self.worker = Worker(self.results, self.live.port)
        self.worker.live_hz = int(self.cfg["live_hz"])
        self.worker.on_connect = self._fetch_on_connect
        self.worker._status_cb = self._on_link_status
        self.live.start()
        self.worker.start()

        self.info: P.DeviceInfo | None = None
        self.algorithms: list[P.Algorithm] = []
        self.schemas: dict[int, P.StageSchema] = {}
        self.values: dict[int, list[float]] = {}
        self.connected = False
        self.calib_dirty = False
        self.device_mode = DeviceMode.MAIN
        self.pending_params: dict[tuple[int, int], float] = {}
        self.pending_lock = threading.Lock()
        self.flush_scheduled = False
        self.fw_abort = threading.Event()
        self.drawn_frame: P.LiveData | None = None
        self.last_beat = -1
        self.beat_until = 0.0
        self.loaded_settings: dict[int, int | str] = {}

        root.title("LED Strip Host")
        root.geometry("1000x700")
        root.minsize(720, 520)
        self._build_menu()
        self._build_connection_bar()
        self.nb = ttk.Notebook(root)
        self.nb.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self._build_main_tab()
        self._build_calib_tab()
        self._build_fw_tab()
        self._build_debug_tab()
        self._update_debug_visibility()

        root.protocol("WM_DELETE_WINDOW", self._quit)
        root.after(FRAME_MS, self._tick)
        root.after(500, self._stats_tick)
        if self.cfg["device"]:
            self._connect()

    # -- settings file ------------------------------------------------------------

    def _load_cfg(self) -> dict[str, Any]:
        cfg = dict(DEFAULT_HOST_SETTINGS)
        try:
            cfg.update(json.loads(SETTINGS_FILE.read_text(encoding="utf-8")))
        except (OSError, ValueError):
            pass
        return cfg

    def _save_cfg(self) -> None:
        try:
            SETTINGS_FILE.write_text(json.dumps(self.cfg, indent=2) + "\n", encoding="utf-8")
        except OSError:
            pass

    def _quit(self) -> None:
        self._save_cfg()
        self.root.destroy()

    # -- layout ---------------------------------------------------------------------

    def _build_menu(self) -> None:
        bar = tk.Menu(self.root)
        view = tk.Menu(bar, tearoff=False)
        self.debug_var = tk.BooleanVar(value=bool(self.cfg["debug_mode"]))
        view.add_checkbutton(label="Debug mode", variable=self.debug_var, command=self._toggle_debug)
        rate = tk.Menu(view, tearoff=False)
        self.rate_var = tk.IntVar(value=int(self.cfg["live_hz"]))
        for hz in (30, 60, 100):
            rate.add_radiobutton(label=f"{hz} Hz", value=hz, variable=self.rate_var, command=self._set_rate)
        view.add_cascade(label="Live rate", menu=rate)
        bar.add_cascade(label="View", menu=view)
        self.root.config(menu=bar)

    def _build_connection_bar(self) -> None:
        f = ttk.Frame(self.root, padding=8)
        f.pack(fill="x")
        ttk.Label(f, text="Device").pack(side="left")
        self.dev_var = tk.StringVar(value=self.cfg["device"])
        self.dev_combo = ttk.Combobox(f, textvariable=self.dev_var, width=22)
        self.dev_combo.pack(side="left", padx=4)
        self.dev_combo.bind("<Return>", lambda e: self._connect())
        ttk.Button(f, text="Scan", command=self._scan).pack(side="left")
        self.conn_btn = ttk.Button(f, text="Connect", command=self._connect_toggle)
        self.conn_btn.pack(side="left", padx=4)
        self.status_var = tk.StringVar(value="not connected")
        ttk.Label(f, textvariable=self.status_var).pack(side="left", padx=8)
        self.stats_var = tk.StringVar()
        ttk.Label(f, textvariable=self.stats_var, foreground="#666").pack(side="right")

    def _build_main_tab(self) -> None:
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="Main")

        top = ttk.Frame(tab)
        top.pack(fill="x")
        self.info_var = tk.StringVar(value="-")
        ttk.Label(top, textvariable=self.info_var, justify="left").pack(side="left", anchor="n")

        ctl = ttk.LabelFrame(top, text="Run", padding=8)
        ctl.pack(side="right")
        self.run_mode_var = tk.IntVar(value=0)
        ttk.Radiobutton(ctl, text="Audio", value=0, variable=self.run_mode_var,
                        command=lambda: self._run_config(run_mode=self.run_mode_var.get())).grid(row=0, column=0)
        ttk.Radiobutton(ctl, text="Pattern", value=1, variable=self.run_mode_var,
                        command=lambda: self._run_config(run_mode=self.run_mode_var.get())).grid(row=0, column=1)
        ttk.Label(ctl, text="Algorithm").grid(row=1, column=0, sticky="w")
        self.algo_combo = ttk.Combobox(ctl, state="readonly", width=24)
        self.algo_combo.grid(row=1, column=1, sticky="ew", pady=2)
        self.algo_combo.bind("<<ComboboxSelected>>",
                             lambda e: self._run_config(algorithm=self.algo_combo.current()))
        ttk.Label(ctl, text="Pattern").grid(row=2, column=0, sticky="w")
        self.pattern_var = tk.IntVar(value=0)
        self.pattern_spin = ttk.Spinbox(ctl, from_=0, to=0, textvariable=self.pattern_var, width=5,
                                        command=lambda: self._run_config(pattern=self.pattern_var.get()))
        self.pattern_spin.grid(row=2, column=1, sticky="w", pady=2)
        ttk.Label(ctl, text="Brightness").grid(row=3, column=0, sticky="w")
        self.bri_var = tk.IntVar(value=0)
        bri = ttk.Scale(ctl, from_=0, to=254, variable=self.bri_var, length=180)   # 255 = "unchanged"
        bri.grid(row=3, column=1, sticky="ew", pady=2)
        bri.bind("<ButtonRelease-1>", lambda e: self._run_config(brightness=int(self.bri_var.get())))

        self.live_on = tk.BooleanVar(value=True)
        ttk.Checkbutton(tab, text="Live view", variable=self.live_on, command=self._toggle_live).pack(anchor="w",
                                                                                                    pady=(8, 0))
        self.cv = tk.Canvas(tab, background="#111", highlightthickness=0)
        self.cv.pack(fill="both", expand=True, pady=(4, 0))
        self.cv.bind("<Configure>", lambda e: self._layout_live())
        self.led_items: list[int] = []
        self.led_colors: list[str] = []
        self.band_items: list[int] = []

    def _build_calib_tab(self) -> None:
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="Calibration")
        bar = ttk.Frame(tab)
        bar.pack(fill="x")
        self.calib_btn = ttk.Button(bar, text="Enter calibration mode", command=self._toggle_calib)
        self.calib_btn.pack(side="left")
        ttk.Label(bar, text="Show").pack(side="left", padx=(12, 2))
        self.calib_filter = ttk.Combobox(bar, state="readonly", width=26)
        self.calib_filter.pack(side="left")
        self.calib_filter.bind("<<ComboboxSelected>>", lambda e: self._build_param_editors())
        for text, cmd in (("Save", self._calib_save), ("Reload", self._calib_reload),
                          ("Reset all", self._calib_reset), ("Export...", self._calib_export),
                          ("Import...", self._calib_import)):
            ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=2)
        self.dirty_var = tk.StringVar()
        ttk.Label(bar, textvariable=self.dirty_var, foreground="#c60").pack(side="left", padx=8)
        self.param_area = ScrollFrame(tab)
        self.param_area.pack(fill="both", expand=True, pady=(8, 0))
        self.param_vars: dict[tuple[int, int], tk.Variable] = {}
        self.param_widgets: list[tk.Widget] = []

    def _build_fw_tab(self) -> None:
        tab = ttk.Frame(self.nb, padding=8)
        self.nb.add(tab, text="Firmware")
        self.fw_target = tk.IntVar(value=P.FwTarget.APP)
        ttk.Radiobutton(tab, text="Application image (.bin)", value=P.FwTarget.APP,
                        variable=self.fw_target).grid(row=0, column=0, sticky="w")
        ttk.Radiobutton(tab, text="Calibration region image", value=P.FwTarget.CALIB,
                        variable=self.fw_target).grid(row=1, column=0, sticky="w")
        self.fw_file = tk.StringVar()
        ttk.Entry(tab, textvariable=self.fw_file, width=60).grid(row=2, column=0, sticky="ew", pady=6)
        ttk.Button(tab, text="Browse...", command=self._fw_browse).grid(row=2, column=1, padx=4)
        btns = ttk.Frame(tab)
        btns.grid(row=3, column=0, sticky="w")
        self.fw_btn = ttk.Button(btns, text="Upload", command=self._fw_upload)
        self.fw_btn.pack(side="left")
        ttk.Button(btns, text="Abort", command=self.fw_abort.set).pack(side="left", padx=4)
        ttk.Button(btns, text="Download calibration image...", command=self._calib_download).pack(side="left",
                                                                                                  padx=12)
        self.fw_prog = ttk.Progressbar(tab, maximum=1.0, length=400)
        self.fw_prog.grid(row=4, column=0, sticky="ew", pady=6)
        self.fw_msg = tk.StringVar()
        ttk.Label(tab, textvariable=self.fw_msg).grid(row=5, column=0, sticky="w")
        tab.columnconfigure(0, weight=1)

    def _build_debug_tab(self) -> None:
        self.dbg_tab = ttk.Frame(self.nb, padding=8)
        sub = ttk.Notebook(self.dbg_tab)
        sub.pack(fill="both", expand=True)

        # settings (WiFi credentials etc.)
        st = ttk.Frame(sub, padding=8)
        sub.add(st, text="Settings")
        self.set_vars: dict[str, tk.StringVar] = {}
        for row, sdef in enumerate(s for s in P.SETTINGS if s.id in (1, 3, 4, 8, 9, 14)):
            ttk.Label(st, text=sdef.name).grid(row=row, column=0, sticky="w", pady=2)
            var = tk.StringVar()
            self.set_vars[sdef.name] = var
            if sdef.name == "color_order":
                w: tk.Widget = ttk.Combobox(st, textvariable=var, values=P.COLOR_ORDERS, state="readonly", width=8)
            else:
                w = ttk.Entry(st, textvariable=var, width=36, show="*" if sdef.name == "sta_password" else "")
                if sdef.name == "sta_password":
                    self.pw_entry = w
            w.grid(row=row, column=1, sticky="w", pady=2)
        self.pw_show = tk.BooleanVar()
        ttk.Checkbutton(st, text="show", variable=self.pw_show,
                        command=lambda: self.pw_entry.configure(show="" if self.pw_show.get() else "*")).grid(
            row=2, column=2, sticky="w")
        b = ttk.Frame(st)
        b.grid(row=10, column=0, columnspan=3, sticky="w", pady=8)
        ttk.Button(b, text="Load", command=self._settings_load).pack(side="left")
        ttk.Button(b, text="Apply", command=self._settings_apply).pack(side="left", padx=4)
        ttk.Button(b, text="Reboot device", command=self._reboot).pack(side="left", padx=4)
        ttk.Button(b, text="Factory reset...", command=self._factory_reset).pack(side="left", padx=4)
        self.set_msg = tk.StringVar(value="Network changes take effect after a reboot.")
        ttk.Label(st, textvariable=self.set_msg).grid(row=11, column=0, columnspan=3, sticky="w")

        # errors
        er = ttk.Frame(sub, padding=8)
        sub.add(er, text="Errors")
        self.err_tree = self._tree(er, ("code", "name", "severity", "count", "last [ms]"))
        b = ttk.Frame(er)
        b.pack(fill="x", pady=4)
        ttk.Button(b, text="Refresh", command=self._errors_load).pack(side="left")
        ttk.Button(b, text="Clear", command=self._errors_clear).pack(side="left", padx=4)

        # diagnostics
        dg = ttk.Frame(sub, padding=8)
        sub.add(dg, text="Diagnostics")
        self.diag_tree = self._tree(dg, ("group", "name", "value", "unit"))
        b = ttk.Frame(dg)
        b.pack(fill="x", pady=4)
        ttk.Button(b, text="Refresh", command=self._diag_load).pack(side="left")
        ttk.Button(b, text="Reset max/min", command=self._diag_reset).pack(side="left", padx=4)

        # CLI
        cl = ttk.Frame(sub, padding=8)
        sub.add(cl, text="CLI")
        self.cli_out = tk.Text(cl, height=20, font=("Consolas", 10), state="disabled")
        self.cli_out.pack(fill="both", expand=True)
        self.cli_in = ttk.Entry(cl, font=("Consolas", 10))
        self.cli_in.pack(fill="x", pady=(4, 0))
        self.cli_in.bind("<Return>", self._cli_send)
        self.cli_in.bind("<Up>", lambda e: self._cli_hist(-1))
        self.cli_in.bind("<Down>", lambda e: self._cli_hist(1))
        self.cli_history: list[str] = []
        self.cli_pos = 0

        # request log
        lg = ttk.Frame(sub, padding=8)
        sub.add(lg, text="Log")
        self.log_list = tk.Listbox(lg, font=("Consolas", 9))
        self.log_list.pack(fill="both", expand=True)

    def _tree(self, parent: tk.Widget, cols: tuple[str, ...]) -> ttk.Treeview:
        t = ttk.Treeview(parent, columns=cols, show="headings", height=14)
        for c in cols:
            t.heading(c, text=c)
            t.column(c, width=120, anchor="w")
        t.pack(fill="both", expand=True)
        return t

    # -- debug mode -----------------------------------------------------------------

    def _device_debug(self) -> bool:
        return bool(self.info and self.info.has(BuildFlag.ENABLE_DEBUG_HOST))

    def _update_debug_visibility(self) -> None:
        show = self.debug_var.get() and (self._device_debug() or not self.connected)
        tabs = self.nb.tabs()
        if show and str(self.dbg_tab) not in tabs:
            self.nb.add(self.dbg_tab, text="Debug")
        elif not show and str(self.dbg_tab) in tabs:
            self.nb.forget(self.dbg_tab)
        self.worker.log_requests = bool(show)

    def _toggle_debug(self) -> None:
        self.cfg["debug_mode"] = self.debug_var.get()
        self._update_debug_visibility()
        if self.debug_var.get() and self._device_debug():
            self._settings_load()
            self._errors_load()
            self._diag_load()

    # -- connection -------------------------------------------------------------------

    def _connect_toggle(self) -> None:
        if self.worker.target:
            self.worker.disconnect()
            self.conn_btn.configure(text="Connect")
        else:
            self._connect()

    def _connect(self) -> None:
        host = self.dev_var.get().strip().split(" ")[0]
        try:
            socket.inet_aton(host)
        except OSError:
            self.status_var.set("enter the device IP address or use Scan")
            return
        self.cfg["device"] = host
        self.status_var.set(f"connecting to {host}...")
        self.conn_btn.configure(text="Disconnect")
        self.worker.connect(host)

    def _scan(self) -> None:
        self.status_var.set("scanning...")

        def run() -> None:
            found = discover()
            self.results.put((self._scan_done, (found,)))
        threading.Thread(target=run, daemon=True).start()

    def _scan_done(self, found: list[dict[str, Any]]) -> None:
        entries = [f"{d['ip']}  ({d['name']}, fw {d['fw']})" for d in found]
        self.dev_combo.configure(values=entries)
        if not found:
            self.status_var.set("no device found (is it on the same network?)")
            return
        self.status_var.set(f"found {len(found)} device(s)")
        if not self.worker.target:
            self.dev_var.set(found[0]["ip"])
            self._connect()

    def _on_link_status(self, why: str) -> None:
        if self.connected:
            self.connected = False
            self.live.latest = None
        self.status_var.set(why if self.worker.target else "not connected")

    def _fetch_on_connect(self, link: Link) -> None:
        """Worker thread: everything the UI needs, once per (re)connect."""
        self.live.device_ip = link.host         # live datagrams are accepted from this address only
        info =P.DeviceInfo.decode(link.request(Cmd.GET_INFO))
        algs = P.decode_algorithms(link.request(Cmd.GET_ALGORITHMS))
        schemas, values = {}, {}
        for sid in P.decode_stage_list(link.request(Cmd.GET_STAGE_LIST)):
            schemas[sid] = P.decode_stage_schema(link.request(Cmd.GET_STAGE_SCHEMA, bytes([sid])))
            values[sid] = P.decode_params(link.request(Cmd.GET_PARAMS, bytes([sid])))[1]
        dbg: dict[str, Any] = {}
        if info.has(BuildFlag.ENABLE_DEBUG_HOST):
            dbg["errors"] = P.decode_errors(link.request(Cmd.GET_ERRORS))
            dbg["settings"] = P.decode_settings(link.request(Cmd.GET_SETTINGS))
        if info.has(BuildFlag.ENABLE_SELF_DIAGNOSTICS):
            dbg["diag"] = P.decode_diag(link.request(Cmd.GET_DIAG).decode("utf-8", "replace"))
        self.live.reset()
        self.worker.post(self._connected, info, algs, schemas, values, dbg)

    def _connected(self, info: P.DeviceInfo, algs: list[P.Algorithm], schemas: dict[int, P.StageSchema],
                   values: dict[int, list[float]], dbg: dict[str, Any]) -> None:
        self.connected = True
        self.info, self.algorithms, self.schemas, self.values = info, algs, schemas, values
        self.device_mode = DeviceMode(info.device_mode) if info.device_mode in (0, 1, 2) else DeviceMode.MAIN
        self.calib_dirty = bool(info.calib_dirty)
        build = "debug" if info.has(BuildFlag.INTERNAL_TEST_BUILD) else "release"
        if info.proto_version != P.PROTO_VERSION:
            build += f", PROTOCOL v{info.proto_version} (host v{P.PROTO_VERSION})"
        self.status_var.set(f"connected to {info.device_name} ({info.ip_address or self.live.device_ip})")
        self.info_var.set(f"{info.device_name}\nfirmware {info.fw_version} ({build})\nbuilt {info.build_date}\n"
                          f"{info.num_leds} LEDs, {info.sample_rate_hz} Hz, hop {info.hop_size}\n"
                          f"WiFi {info.rssi_dbm} dBm, reset reason {info.reset_reason}")
        self.run_mode_var.set(info.run_mode)
        self.algo_combo.configure(values=[a.name for a in algs])
        if info.active_algorithm < len(algs):
            self.algo_combo.current(info.active_algorithm)
        self.pattern_spin.configure(to=max(0, info.num_patterns - 1))
        self.pattern_var.set(info.active_pattern)
        self.bri_var.set(min(info.brightness, 254))
        self.calib_filter.configure(values=["All stages"] + [f"Algorithm: {a.name}" for a in algs])
        if not self.calib_filter.get():
            self.calib_filter.current(0)
        self._build_param_editors()
        self._update_mode_ui()
        self._update_debug_visibility()
        if "errors" in dbg:
            self._show_errors(dbg["errors"])
        if "settings" in dbg:
            self._show_settings(dbg["settings"])
        if "diag" in dbg:
            self._show_diag(dbg["diag"])
        self._layout_live()

    # -- generic request helper ------------------------------------------------------

    def call(self, cmd: int, payload: bytes = b"", done: Callable[[bytes], None] | None = None,
             timeout: float = 5.0, what: str = "") -> None:
        log = self.worker.log_requests

        def fn(link: Link) -> bytes:
            t0 = time.perf_counter()
            try:
                return link.request(cmd, payload, timeout)
            finally:
                if log:
                    self.worker.post(self._log, f"{P.enum_name(Cmd, cmd):<22} {len(payload):5d} B "
                                                f"{(time.perf_counter() - t0) * 1000:7.1f} ms")
        self.worker.submit(fn, done, lambda e: self._error(what or P.enum_name(Cmd, cmd), e))

    def _error(self, what: str, e: Exception) -> None:
        self.status_var.set(f"{what}: {e}")

    def _log(self, line: str) -> None:
        self.log_list.insert("end", time.strftime("%H:%M:%S ") + line)
        if self.log_list.size() > 500:
            self.log_list.delete(0)
        self.log_list.see("end")

    # -- main tab ---------------------------------------------------------------------

    def _run_config(self, **kw: int) -> None:
        self.call(Cmd.SET_RUN_CONFIG, P.build_set_run_config(**kw))

    def _set_rate(self) -> None:
        self.cfg["live_hz"] = self.worker.live_hz = self.rate_var.get()
        self._toggle_live()

    def _toggle_live(self) -> None:
        hz = self.worker.live_hz if self.live_on.get() else 0
        self.call(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(self.live.port, hz))
        self.live.reset()

    def _layout_live(self) -> None:
        cv = self.cv
        cv.delete("all")
        w, h = max(cv.winfo_width(), 200), max(cv.winfo_height(), 200)
        n = self.info.num_leds if self.info else 60
        pad = 12
        size = (w - 2 * pad) / max(n, 1)
        y0 = pad
        self.led_items = [cv.create_rectangle(pad + i * size + 1, y0, pad + (i + 1) * size - 1, y0 + max(size, 14),
                                              fill="#000", outline="#333") for i in range(n)]
        self.led_colors = ["#000"] * n
        y = y0 + max(size, 14) + 16
        cv.create_text(pad, y, text="level", fill="#888", anchor="w")
        self.level_bar = cv.create_rectangle(pad + 50, y - 6, pad + 50, y + 6, fill="#4c4", width=0)
        self.level_w = w - 2 * pad - 50
        y += 24
        cv.create_text(pad, y, text="centroid", fill="#888", anchor="w")
        cv.create_line(pad + 50, y, pad + 50 + self.level_w, y, fill="#333")
        self.centroid_mark = cv.create_line(pad + 50, y - 7, pad + 50, y + 7, fill="#fc3", width=3)
        y += 24
        self.bpm_text = cv.create_text(pad, y, text="BPM -", fill="#ddd", anchor="w", font=("Segoe UI", 12, "bold"))
        self.beat_dot = cv.create_oval(pad + 110, y - 8, pad + 126, y + 8, fill="#222", width=0)
        self.mode_text = cv.create_text(w - pad, y, text="", fill="#888", anchor="e")
        y += 20
        self.band_top, self.band_bottom = y, h - pad
        self.band_area = (pad, w - pad)
        nb = len(self.drawn_frame.bands) if self.drawn_frame else 12
        self.band_items = []
        bw = (w - 2 * pad) / max(nb, 1)
        for b in range(nb):
            self.band_items.append(cv.create_rectangle(pad + b * bw + 2, h - pad, pad + (b + 1) * bw - 2, h - pad,
                                                       fill="#38f", width=0))
        self.drawn_frame = None

    def _draw(self, f: P.LiveData) -> None:
        cv = self.cv
        n = min(len(self.led_items), len(f.leds) // 3)
        leds = f.leds
        for i in range(n):
            c = "#%02x%02x%02x" % (leds[3 * i], leds[3 * i + 1], leds[3 * i + 2])
            if c != self.led_colors[i]:
                self.led_colors[i] = c
                cv.itemconfigure(self.led_items[i], fill=c)
        x0 = self.cv.coords(self.level_bar)
        if x0:
            cv.coords(self.level_bar, x0[0], x0[1], x0[0] + self.level_w * f.level / 32767, x0[3])
            cx = x0[0] + self.level_w * f.centroid / 32767
            c = cv.coords(self.centroid_mark)
            cv.coords(self.centroid_mark, cx, c[1], cx, c[3])
        cv.itemconfigure(self.bpm_text, text=f"BPM {f.bpm_x10 / 10:.1f}" if f.bpm_x10 else "BPM -")
        now = time.perf_counter()
        if f.beat_count != self.last_beat:
            if self.last_beat >= 0:
                self.beat_until = now + 0.08
            self.last_beat = f.beat_count
        cv.itemconfigure(self.beat_dot, fill="#f44" if now < self.beat_until else "#222")
        cv.itemconfigure(self.mode_text, text=P.enum_name(DeviceMode, f.device_mode))
        if len(f.bands) != len(self.band_items):
            self.drawn_frame = f
            self._layout_live()
            return
        span = self.band_bottom - self.band_top
        for item, v in zip(self.band_items, f.bands):
            c = cv.coords(item)
            cv.coords(item, c[0], self.band_bottom - span * v / 32767, c[2], self.band_bottom)
        if f.device_mode != self.device_mode and f.device_mode in (0, 1, 2):
            self.device_mode = DeviceMode(f.device_mode)
            self._update_mode_ui()

    def _tick(self) -> None:
        """UI loop: worker results, then the newest live frame (if any)."""
        try:
            while True:
                cb, args = self.results.get_nowait()
                cb(*args)
        except queue.Empty:
            pass
        latest = self.live.latest
        if latest and latest[0] is not self.drawn_frame and self.led_items:
            self.drawn_frame = latest[0]
            self._draw(latest[0])
        self.root.after(FRAME_MS, self._tick)

    def _stats_tick(self) -> None:
        lv = self.live
        if self.connected and lv.frames:
            self.stats_var.set(f"live {lv.frames * 2} fps, jitter {lv.age_sum / lv.frames:.1f} ms, "
                               f"lost {lv.dropped}")
        else:
            self.stats_var.set("")
        lv.frames, lv.age_sum = 0, 0.0
        self.root.after(500, self._stats_tick)

    # -- calibration ---------------------------------------------------------------

    def _update_mode_ui(self) -> None:
        cal = self.device_mode == DeviceMode.CALIBRATION
        self.calib_btn.configure(text="Exit calibration mode" if cal else "Enter calibration mode")
        for w in self.param_widgets:
            w.state(["!disabled"] if cal else ["disabled"])
        self.dirty_var.set("unsaved changes" if self.calib_dirty else "")

    def _toggle_calib(self) -> None:
        enter = self.device_mode != DeviceMode.CALIBRATION
        self.call(Cmd.SET_DEVICE_MODE, bytes([1 if enter else 0]), lambda _: self._mode_set(enter))

    def _mode_set(self, calib: bool) -> None:
        self.device_mode = DeviceMode.CALIBRATION if calib else DeviceMode.MAIN
        if not calib:                   # leaving discards unsaved values on the device
            self.calib_dirty = False
            self._reload_values()
        self._update_mode_ui()

    def _visible_stages(self) -> list[int]:
        i = self.calib_filter.current()
        if i <= 0 or i - 1 >= len(self.algorithms):
            return sorted(self.schemas)
        return [s for s in self.algorithms[i - 1].stage_ids if s in self.schemas]

    def _build_param_editors(self) -> None:
        body = self.param_area.body
        for w in body.winfo_children():
            w.destroy()
        self.param_vars.clear()
        self.param_widgets = []
        for sid in self._visible_stages():
            st = self.schemas[sid]
            box = ttk.LabelFrame(body, text=f"{st.name}  -  {st.description}", padding=6)
            box.pack(fill="x", pady=4, padx=2)
            box.columnconfigure(1, weight=1)
            for p in st.params:
                self._param_row(box, sid, p)
            ttk.Button(box, text="Reset stage", command=lambda s=sid: self._calib_reset(s)).grid(
                row=len(st.params), column=0, sticky="w", pady=(4, 0))
        self._update_mode_ui()

    def _param_row(self, box: ttk.LabelFrame, sid: int, p: P.ParamSchema) -> None:
        row = p.id
        unit = "" if p.type == ParamType.ENUM else p.unit
        ttk.Label(box, text=p.name + (f" [{unit}]" if unit else "")).grid(row=row, column=0, sticky="w", padx=(0, 8))
        vals = self.values.get(sid, [])
        cur = vals[p.id] if p.id < len(vals) else p.default
        key = (sid, p.id)
        if p.type == ParamType.BOOL:
            var: tk.Variable = tk.BooleanVar(value=cur >= 0.5)
            w = ttk.Checkbutton(box, variable=var, command=lambda: self._param_changed(key, float(var.get())))
            w.grid(row=row, column=1, sticky="w")
            self.param_widgets.append(w)
        elif p.type == ParamType.ENUM:
            var = tk.StringVar()
            opts = p.options
            w = ttk.Combobox(box, values=opts, state="readonly", width=20, textvariable=var)
            if 0 <= int(cur) < len(opts):
                w.current(int(cur))
            w.bind("<<ComboboxSelected>>", lambda e, c=w: self._param_changed(key, float(c.current())))
            w.grid(row=row, column=1, sticky="w")
            self.param_widgets.append(w)
        else:
            var = tk.DoubleVar(value=cur)
            is_int = p.type == ParamType.INT

            def from_scale(v: str) -> None:
                x = float(v)
                if p.step > 0:
                    x = p.min + round((x - p.min) / p.step) * p.step
                x = round(x) if is_int else round(x, 6)
                var.set(x)
                self._param_changed(key, float(x))
            sc = ttk.Scale(box, from_=p.min, to=p.max, variable=var, command=from_scale)
            sc.grid(row=row, column=1, sticky="ew")
            def from_spin(*_: Any) -> None:
                try:
                    self._param_changed(key, float(var.get()))
                except (tk.TclError, ValueError):
                    pass                # half-typed number
            sp = ttk.Spinbox(box, from_=p.min, to=p.max, increment=p.step or 1, textvariable=var, width=10,
                             command=from_spin)
            sp.bind("<Return>", from_spin)
            sp.grid(row=row, column=2, padx=4)
            self.param_widgets += [sc, sp]
        self.param_vars[key] = var

    def _param_changed(self, key: tuple[int, int], value: float) -> None:
        """Coalesce slider moves: only the newest value per parameter is sent."""
        if self.device_mode != DeviceMode.CALIBRATION:
            return
        with self.pending_lock:
            self.pending_params[key] = value
        if not self.flush_scheduled:
            self.flush_scheduled = True
            self.root.after(PARAM_DEBOUNCE_MS, self._flush_params)

    def _flush_params(self) -> None:
        self.flush_scheduled = False

        def fn(link: Link) -> list[tuple[tuple[int, int], float]]:
            with self.pending_lock:
                todo, self.pending_params = self.pending_params, {}
            applied = []
            for (sid, pid), v in todo.items():
                body = link.request(Cmd.SET_PARAM, P.build_set_param(sid, pid, v))
                applied.append(((sid, pid), P.clean_float(P.Reader(body).f32())))
            return applied
        self.worker.submit(fn, self._params_applied, lambda e: self._error("set parameter", e))

    def _params_applied(self, applied: list[tuple[tuple[int, int], float]]) -> None:
        for (sid, pid), v in applied:
            if sid in self.values and pid < len(self.values[sid]):
                self.values[sid][pid] = v
            var = self.param_vars.get((sid, pid))
            if isinstance(var, tk.DoubleVar) and abs(var.get() - v) > 1e-6:
                var.set(v)              # clamped / quantised by the device
        self.calib_dirty = True
        self._update_mode_ui()

    def _reload_values(self) -> None:
        def fn(link: Link) -> dict[int, list[float]]:
            return {sid: P.decode_params(link.request(Cmd.GET_PARAMS, bytes([sid])))[1] for sid in self.schemas}

        def done(values: dict[int, list[float]]) -> None:
            self.values = values
            self._build_param_editors()
        self.worker.submit(fn, done, lambda e: self._error("read parameters", e))

    def _calib_save(self) -> None:
        def done(_: bytes) -> None:
            self.calib_dirty = False
            self._update_mode_ui()
            self.status_var.set("parameters saved to flash")
        self.call(Cmd.SAVE_PARAMS, done=done, timeout=10.0)

    def _calib_reload(self) -> None:
        def done(_: bytes) -> None:
            self.calib_dirty = False
            self._reload_values()
        self.call(Cmd.RELOAD_PARAMS, done=done)

    def _calib_reset(self, stage: int = 0xFF) -> None:
        def done(_: bytes) -> None:
            self.calib_dirty = True
            self._reload_values()
        self.call(Cmd.RESET_PARAMS, bytes([stage]), done)

    def _calib_export(self) -> None:
        path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("JSON", "*.json")])
        if not path:
            return
        params = [{"stage": sid, "param": p.id, "value": self.values[sid][p.id], "stage_name": st.name,
                   "name": p.name} for sid, st in sorted(self.schemas.items()) for p in st.params
                  if p.id < len(self.values.get(sid, []))]
        Path(path).write_text(json.dumps({"params": params}, indent=2) + "\n", encoding="utf-8")

    def _calib_import(self) -> None:
        if self.device_mode != DeviceMode.CALIBRATION:
            messagebox.showinfo("Import", "Enter calibration mode first.")
            return
        path = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if not path:
            return
        try:
            doc = json.loads(Path(path).read_text(encoding="utf-8"))
            items = [(int(p["stage"]), int(p["param"]), float(p["value"])) for p in doc["params"]]
        except (OSError, ValueError, KeyError, TypeError) as e:
            messagebox.showerror("Import", f"Cannot read {path}: {e}")
            return
        with self.pending_lock:
            for sid, pid, v in items:
                if sid in self.schemas:
                    self.pending_params[(sid, pid)] = v
        self._flush_params()
        self.worker.submit(lambda link: None, lambda _: self._reload_values())

    # -- firmware ------------------------------------------------------------------

    def _fw_browse(self) -> None:
        path = filedialog.askopenfilename(filetypes=[("Binary", "*.bin"), ("All files", "*.*")])
        if path:
            self.fw_file.set(path)

    def _fw_upload(self) -> None:
        try:
            data = Path(self.fw_file.get()).read_bytes()
        except OSError as e:
            messagebox.showerror("Firmware", str(e))
            return
        target = self.fw_target.get()
        if target == P.FwTarget.APP and (not data or data[0] != 0xE9):
            messagebox.showerror("Firmware", "Not an ESP application image (first byte must be 0xE9).")
            return
        if target == P.FwTarget.CALIB:
            try:
                P.decode_region_image(data)
            except P.ProtocolError as e:
                messagebox.showerror("Firmware", f"Not a calibration image: {e}")
                return
        self.fw_abort.clear()
        self.fw_btn.configure(state="disabled")
        self.fw_msg.set("starting...")

        def fn(link: Link) -> str:
            crc = zlib.crc32(data) & 0xFFFFFFFF
            body = link.request(Cmd.FW_BEGIN, P.build_fw_begin(target, len(data), crc), timeout=30.0)
            chunk = P.Reader(body).u16()
            t0, off = time.monotonic(), 0
            try:
                while off < len(data):
                    if self.fw_abort.is_set():
                        raise DeviceError(P.Status.ERR_INTERNAL, "aborted by user")
                    part = data[off:off + chunk]
                    off = P.Reader(link.request(Cmd.FW_DATA, P.build_fw_data(off, part), timeout=15.0)).u32()
                    rate = off / max(time.monotonic() - t0, 1e-3) / 1024
                    self.worker.post(self._fw_progress, off / len(data), f"{off}/{len(data)} bytes, {rate:.0f} KiB/s")
                link.request(Cmd.FW_END, timeout=30.0)
            except Exception:
                try:
                    link.request(Cmd.FW_ABORT)
                except (OSError, P.ProtocolError, DeviceError):
                    pass
                raise
            if target == P.FwTarget.APP:
                link.request(Cmd.REBOOT)
                return "installed - device reboots, reconnecting..."
            return "calibration image installed"

        def done(msg: str) -> None:
            self.fw_btn.configure(state="normal")
            self.fw_msg.set(msg)
            if target == P.FwTarget.CALIB:
                self._reload_values()

        def fail(e: Exception) -> None:
            self.fw_btn.configure(state="normal")
            self.fw_msg.set(f"failed: {e}")
        self.worker.submit(fn, done, fail)

    def _fw_progress(self, frac: float, msg: str) -> None:
        self.fw_prog.configure(value=frac)
        self.fw_msg.set(msg)

    def _calib_download(self) -> None:
        def done(img: bytes) -> None:
            path = filedialog.asksaveasfilename(defaultextension=".bin", initialfile="calib_image.bin")
            if path:
                Path(path).write_bytes(img)
        self.call(Cmd.GET_CALIB_IMAGE, done=done)

    # -- debug: settings, errors, diagnostics, CLI ----------------------------------

    def _settings_load(self) -> None:
        self.call(Cmd.GET_SETTINGS, done=lambda b: self._show_settings(P.decode_settings(b)))

    def _show_settings(self, values: dict[int, int | str]) -> None:
        self.loaded_settings = values
        for name, var in self.set_vars.items():
            v = values.get(P.SETTINGS_BY_NAME[name].id, "")
            if name == "color_order" and isinstance(v, int) and v < len(P.COLOR_ORDERS):
                v = P.COLOR_ORDERS[v]
            var.set(str(v))

    def _settings_apply(self) -> None:
        changes = []
        try:
            for name, var in self.set_vars.items():
                sdef = P.SETTINGS_BY_NAME[name]
                raw: int | str = var.get()
                if name == "color_order":
                    raw = P.COLOR_ORDERS.index(str(raw)) if raw in P.COLOR_ORDERS else 0
                if str(self.loaded_settings.get(sdef.id, "")) == str(raw):
                    continue
                changes.append((sdef, P.encode_setting_value(sdef, raw)))
        except ValueError as e:
            messagebox.showerror("Settings", str(e))
            return

        def fn(link: Link) -> bool:
            reboot = False
            for sdef, data in changes:
                reboot |= link.request(Cmd.SET_SETTING, P.build_set_setting(sdef.id, data))[:1] == b"\x01"
            return reboot

        def done(reboot: bool) -> None:
            self.set_msg.set("saved - reboot the device to apply" if reboot else f"saved ({len(changes)} changed)")
            self._settings_load()
        self.worker.submit(fn, done, lambda e: self.set_msg.set(f"failed: {e}"))

    def _reboot(self) -> None:
        self.call(Cmd.REBOOT, done=lambda _: self.status_var.set("device rebooting..."))

    def _factory_reset(self) -> None:
        if messagebox.askyesno("Factory reset", "Restore default settings? This also erases the WiFi "
                                                "credentials: the device will not rejoin the network."):
            self.call(Cmd.FACTORY_RESET_SETTINGS, done=lambda _: self._settings_load())

    def _errors_load(self) -> None:
        self.call(Cmd.GET_ERRORS, done=lambda b: self._show_errors(P.decode_errors(b)))

    def _errors_clear(self) -> None:
        self.call(Cmd.CLEAR_ERRORS, done=lambda _: self._errors_load())

    def _show_errors(self, errors: list[P.ErrorEntry]) -> None:
        self.err_tree.delete(*self.err_tree.get_children())
        for e in errors:
            self.err_tree.insert("", "end", values=(e.code, e.name, e.severity_name, e.occurrences, e.last_uptime_ms))

    def _diag_load(self) -> None:
        self.call(Cmd.GET_DIAG, done=lambda b: self._show_diag(P.decode_diag(b.decode("utf-8", "replace"))))

    def _diag_reset(self) -> None:
        self.call(Cmd.RESET_DIAG, done=lambda _: self._diag_load())

    def _show_diag(self, rows: list[tuple[str, str, str, str]]) -> None:
        self.diag_tree.delete(*self.diag_tree.get_children())
        for r in rows:
            self.diag_tree.insert("", "end", values=r)

    def _cli_send(self, e: tk.Event) -> None:
        line = self.cli_in.get().strip()
        if not line:
            return
        self.cli_history.append(line)
        self.cli_pos = len(self.cli_history)
        self.cli_in.delete(0, "end")
        self._cli_print(f"> {line}\n")
        self.call(Cmd.CLI_EXEC, line.encode("ascii", "replace"),
                  lambda b: self._cli_print(b.decode("utf-8", "replace")))

    def _cli_print(self, text: str) -> None:
        self.cli_out.configure(state="normal")
        self.cli_out.insert("end", text)
        self.cli_out.see("end")
        self.cli_out.configure(state="disabled")

    def _cli_hist(self, step: int) -> None:
        if not self.cli_history:
            return
        self.cli_pos = max(0, min(len(self.cli_history), self.cli_pos + step))
        self.cli_in.delete(0, "end")
        if self.cli_pos < len(self.cli_history):
            self.cli_in.insert(0, self.cli_history[self.cli_pos])


def main() -> None:
    ap = argparse.ArgumentParser(description="LED strip host (desktop)")
    ap.add_argument("--device", help="device IP address (default: last used)")
    ap.add_argument("--debug", action="store_true", help="start in debug mode")
    args = ap.parse_args()
    root = tk.Tk()
    App(root, args)
    root.mainloop()


if __name__ == "__main__":
    main()
