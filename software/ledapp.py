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

Dark-only UI. Views: Live (LED strip, tempo, level, tone, spectrum + run controls), Calibration,
Firmware (device details, update). The Debug toggle (top bar, only for debug firmware) adds
Settings (WiFi credentials), Errors, Diagnostics, CLI and a request log.

    python software/ledapp.py [--device IP] [--debug]
"""

from __future__ import annotations

import argparse
import collections
import json
import math
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
from ledhost.dsp import biquad_db, pchip  # noqa: E402
from ledhost.protocol import BuildFlag, Cmd, DeviceMode, ParamType  # noqa: E402

SETTINGS_FILE = Path(__file__).resolve().parent / "ledapp_settings.json"
DEFAULT_HOST_SETTINGS = {"device": "", "live_hz": 60, "debug_mode": False, "filter_overlay": False,
                         "processed_overlay": False}
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
        self.live_taps = 0                                      # spectrum taps to subscribe
        self.on_layout: Callable[[P.SpectrumLayout], None] | None = None

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
        body = link.request(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(self.live_port, self.live_hz, self.live_taps))
        if self.on_layout:
            self.post(self.on_layout, P.SpectrumLayout.decode(body))

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
# UI theme (dark only)
# ---------------------------------------------------------------------------

C = {
    "bg": "#0d0f14", "panel": "#151820", "panel2": "#1c2029", "line": "#262b36", "socket": "#20242d",
    "text": "#e8eaf0", "muted": "#8b92a1", "faint": "#555c6a",
    "accent": "#6d8dff", "accent_hi": "#8aa3ff", "ok": "#3ddc97", "warn": "#f5b14c", "bad": "#ff6470",
}
FONT = ("Segoe UI", 10)
FONT_SM = ("Segoe UI", 9)
FONT_H = ("Segoe UI Semibold", 11)
FONT_NUM = ("Segoe UI Semibold", 28)
FONT_GENRE = ("Segoe UI Semibold", 18)
FONT_MONO = ("Consolas", 10)


def mix(c1: str, c2: str, t: float) -> str:
    """Blend two #rrggbb colours (t = 0 -> c1, 1 -> c2)."""
    a, b = int(c1[1:], 16), int(c2[1:], 16)
    ch = [round(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t) for s in (16, 8, 0)]
    return "#%02x%02x%02x" % tuple(ch)


# ---------------------------------------------------------------------------
# Spectrum view helpers
# ---------------------------------------------------------------------------

SPEC_DB_RANGE = (-100.0, 0.0)       # left axis, dBFS of the device's FFT bins
FILTER_DB_RANGE = (-36.0, 12.0)     # right axis, filter overlay gain
FREQ_TICKS = (50, 100, 200, 500, 1000, 2000, 5000)
FILTER_COLORS = ("#f5b14c", "#ff6db0", "#4cc9f0", "#c38bff")   # one per filter overlay


def apply_theme(root: tk.Tk) -> None:
    root.configure(bg=C["bg"])
    st = ttk.Style(root)
    st.theme_use("clam")
    st.configure(".", background=C["bg"], foreground=C["text"], fieldbackground=C["panel2"],
                 bordercolor=C["line"], lightcolor=C["panel2"], darkcolor=C["panel2"], troughcolor=C["panel2"],
                 focuscolor=C["panel2"], selectbackground=C["accent"], selectforeground="#ffffff",
                 insertcolor=C["text"], arrowcolor=C["muted"], font=FONT, borderwidth=0)
    st.map(".", foreground=[("disabled", C["faint"])])
    for name, bg in (("", C["bg"]), ("Card.", C["panel"])):
        st.configure(f"{name}TFrame", background=bg)
        st.configure(f"{name}TLabel", background=bg)
        st.configure(f"{name}TCheckbutton", background=bg, indicatorbackground=C["panel2"],
                     indicatorforeground="#ffffff", indicatormargin=(0, 0, 8, 0), padding=2)
        st.map(f"{name}TCheckbutton", background=[("active", bg)],
               indicatorbackground=[("selected", C["accent"]), ("active", C["line"])])
        st.configure(f"{name}TRadiobutton", background=bg, indicatorbackground=C["panel2"],
                     indicatorforeground="#ffffff", indicatormargin=(0, 0, 8, 0), padding=2)
        st.map(f"{name}TRadiobutton", background=[("active", bg)],
               indicatorbackground=[("selected", C["accent"]), ("active", C["line"])])
        st.configure(f"{name}Horizontal.TScale", background=C["accent"], troughcolor=C["panel2"],
                     lightcolor=C["accent"], darkcolor=C["accent"], bordercolor=bg, sliderlength=12, gripcount=0)
        st.map(f"{name}Horizontal.TScale", background=[("active", C["accent_hi"]), ("disabled", C["line"])])
    st.configure("Muted.TLabel", foreground=C["muted"])
    st.configure("Card.Muted.TLabel", background=C["panel"], foreground=C["muted"], font=FONT_SM)
    st.configure("Title.TLabel", background=C["panel"], font=FONT_H)
    st.configure("Status.TLabel", background=C["bg"], foreground=C["muted"])

    st.configure("TButton", background=C["panel2"], foreground=C["text"], padding=(14, 6), relief="flat")
    st.map("TButton", background=[("pressed", C["line"]), ("active", C["line"]), ("disabled", C["panel"])])
    st.configure("Accent.TButton", background=C["accent"], foreground="#ffffff")
    st.map("Accent.TButton", background=[("pressed", C["accent"]), ("active", C["accent_hi"]),
                                          ("disabled", C["panel2"])])
    st.configure("Seg.TButton", background=C["panel2"], foreground=C["muted"], padding=(10, 6))
    st.map("Seg.TButton", background=[("active", C["line"])])
    st.configure("SegOn.TButton", background=C["accent"], foreground="#ffffff", padding=(10, 6))
    st.map("SegOn.TButton", background=[("active", C["accent_hi"])])

    for w in ("TEntry", "TCombobox", "TSpinbox"):
        st.configure(w, fieldbackground=C["panel2"], foreground=C["text"], bordercolor=C["line"],
                     lightcolor=C["panel2"], darkcolor=C["panel2"], padding=5, borderwidth=1,
                     background=C["panel2"], arrowsize=12)
        st.map(w, bordercolor=[("focus", C["accent"])], lightcolor=[("focus", C["accent"])],
               fieldbackground=[("readonly", C["panel2"]), ("disabled", C["panel"])],
               foreground=[("disabled", C["faint"])], background=[("active", C["line"])],
               selectbackground=[("readonly", C["panel2"])], selectforeground=[("readonly", C["text"])])
    root.option_add("*TCombobox*Listbox.background", C["panel2"])
    root.option_add("*TCombobox*Listbox.foreground", C["text"])
    root.option_add("*TCombobox*Listbox.selectBackground", C["accent"])
    root.option_add("*TCombobox*Listbox.selectForeground", "#ffffff")
    root.option_add("*TCombobox*Listbox.font", FONT)
    root.option_add("*TCombobox*Listbox.borderWidth", 0)

    st.configure("TNotebook", background=C["bg"], borderwidth=0, tabmargins=(0, 0, 0, 0),
                 bordercolor=C["bg"], lightcolor=C["bg"], darkcolor=C["bg"])
    st.configure("TNotebook.Tab", background=C["bg"], foreground=C["muted"], padding=(18, 8), borderwidth=0,
                 bordercolor=C["bg"], lightcolor=C["bg"], darkcolor=C["bg"], font=FONT)
    st.map("TNotebook.Tab", background=[("selected", C["panel"]), ("active", C["panel2"])],
           foreground=[("selected", C["text"]), ("active", C["text"])],
           lightcolor=[("selected", C["panel"])], bordercolor=[("selected", C["panel"])],
           expand=[("selected", (0, 0, 0, 0))], padding=[("selected", (18, 8))])
    # flat notebook: tab strip + client area without the clam frame
    st.layout("TNotebook", [("Notebook.client", {"sticky": "nswe"})])
    st.layout("TNotebook.Tab", [("Notebook.tab", {"sticky": "nswe", "children": [
        ("Notebook.padding", {"side": "top", "sticky": "nswe", "children": [
            ("Notebook.label", {"side": "top", "sticky": ""})]})]})])
    st.configure("Treeview", background=C["panel"], fieldbackground=C["panel"], foreground=C["text"],
                 rowheight=26, borderwidth=0)
    st.map("Treeview", background=[("selected", C["accent"])])
    st.configure("Treeview.Heading", background=C["panel2"], foreground=C["muted"], relief="flat", padding=6)
    st.map("Treeview.Heading", background=[("active", C["line"])])
    st.configure("Vertical.TScrollbar", background=C["panel2"], troughcolor=C["bg"], arrowcolor=C["muted"],
                 bordercolor=C["bg"], lightcolor=C["panel2"], darkcolor=C["panel2"], gripcount=0)
    st.map("Vertical.TScrollbar", background=[("active", C["line"])])
    st.configure("Horizontal.TProgressbar", background=C["accent"], troughcolor=C["panel2"], thickness=8,
                 bordercolor=C["panel"], lightcolor=C["accent"], darkcolor=C["accent"])


def dark_title_bar(root: tk.Tk) -> None:
    """Ask Windows 10/11 for a dark window frame (no effect elsewhere)."""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        root.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(root.winfo_id())
        on = ctypes.c_int(1)
        for attr in (20, 19):           # DWMWA_USE_IMMERSIVE_DARK_MODE (new / pre-20H1 id)
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(on), ctypes.sizeof(on)) == 0:
                break
    except (AttributeError, OSError):
        pass


def card(parent: tk.Widget, title: str = "", subtitle: str = "", padding: int = 16) -> ttk.Frame:
    """Raised panel with an optional title; returns the content frame."""
    outer = ttk.Frame(parent, style="Card.TFrame", padding=padding)
    if title:
        head = ttk.Frame(outer, style="Card.TFrame")
        head.pack(fill="x", pady=(0, 10))
        ttk.Label(head, text=title, style="Title.TLabel").pack(side="left")
        if subtitle:
            ttk.Label(head, text=subtitle, style="Card.Muted.TLabel").pack(side="left", padx=(10, 0))
        outer.head = head          # type: ignore[attr-defined]
    return outer


class ScrollFrame(ttk.Frame):
    """Vertically scrollable container (``.body`` holds the content)."""

    def __init__(self, parent: tk.Widget) -> None:
        super().__init__(parent)
        self.canvas = tk.Canvas(self, highlightthickness=0, background=C["bg"])
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
        self.worker.on_layout = self._on_layout
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

        self.active_algo = 0
        self.shown_genre = -1
        self.spec_ok = False
        self.filt_items: list[int] = []
        self.filt_on = tk.BooleanVar(value=bool(self.cfg["filter_overlay"]))
        self.proc_on = tk.BooleanVar(value=bool(self.cfg["processed_overlay"]))
        self.filt_curves: dict[int, int] = {}
        self.tap_state: dict[int, tuple[list[float], list[float]]] = {}
        self.spec_layout: P.SpectrumLayout | None = None
        self.spec_logf: list[float] = []
        self.q0 = 0

        root.title("LED Strip")
        root.geometry("1180x760")
        root.minsize(640, 520)
        apply_theme(root)
        dark_title_bar(root)
        self.debug_var = tk.BooleanVar(value=bool(self.cfg["debug_mode"]))
        self.rate_var = tk.StringVar(value=f"{int(self.cfg['live_hz'])} Hz")
        self._build_top_bar()
        self.nb = ttk.Notebook(root)
        self.nb.pack(fill="both", expand=True, padx=16, pady=(0, 16))
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

    def _build_top_bar(self) -> None:
        f = ttk.Frame(self.root, padding=(16, 12))
        f.pack(fill="x")
        self.status_dot = tk.Canvas(f, width=10, height=10, background=C["bg"], highlightthickness=0)
        self.status_dot.pack(side="left")
        self.status_dot_item = self.status_dot.create_oval(1, 1, 9, 9, fill=C["faint"], width=0)
        self.status_var = tk.StringVar(value="Not connected")
        ttk.Label(f, textvariable=self.status_var, style="Status.TLabel").pack(side="left", padx=(8, 0))

        ttk.Checkbutton(f, text="Debug", variable=self.debug_var, command=self._toggle_debug).pack(side="right")
        self.conn_btn = ttk.Button(f, text="Connect", style="Accent.TButton", command=self._connect_toggle)
        self.conn_btn.pack(side="right", padx=(8, 16))
        ttk.Button(f, text="Scan", command=self._scan).pack(side="right", padx=(8, 0))
        self.dev_var = tk.StringVar(value=self.cfg["device"])
        self.dev_combo = ttk.Combobox(f, textvariable=self.dev_var, width=18)
        self.dev_combo.pack(side="right")
        self.dev_combo.bind("<Return>", lambda e: self._connect())
        self.dev_combo.bind("<<ComboboxSelected>>", lambda e: self._connect())

    def _set_status(self, text: str, state: str = "idle") -> None:
        self.status_var.set(text)
        color = {"ok": C["ok"], "busy": C["warn"], "error": C["bad"]}.get(state, C["faint"])
        self.status_dot.itemconfigure(self.status_dot_item, fill=color)

    def _build_main_tab(self) -> None:
        tab = ttk.Frame(self.nb, padding=(0, 16, 0, 0))
        self.nb.add(tab, text="Live")
        self.main_tab = tab

        # live view: one canvas, laid out on resize, updated in place per frame
        self.live_card = ttk.Frame(tab, style="Card.TFrame")
        self.cv = tk.Canvas(self.live_card, background=C["panel"], highlightthickness=0)
        self.cv.pack(fill="both", expand=True, padx=4, pady=4)
        self.cv.bind("<Configure>", lambda e: self._layout_live())
        self.led_items: list[int] = []
        self.led_colors: list[str] = []
        self.band_items: list[tuple[int, int]] = []
        self.stats_item = self.cv.create_text(0, 0, text="")    # replaced by _layout_live

        # controls
        side = card(tab, "Controls")
        self.side = side
        g = ttk.Frame(side, style="Card.TFrame")
        g.pack(fill="x")
        self.ctl_grid = g

        def section(label: str) -> ttk.Frame:
            f = ttk.Frame(g, style="Card.TFrame")
            f.columnconfigure(0, weight=1)
            if label:
                ttk.Label(f, text=label, style="Card.Muted.TLabel").grid(row=0, column=0, sticky="w")
            return f

        src = section("Source")
        seg = ttk.Frame(src, style="Card.TFrame")
        seg.grid(row=1, column=0, sticky="ew", pady=(4, 0))
        seg.columnconfigure((0, 1), weight=1, uniform="seg")
        self.run_mode_var = tk.IntVar(value=0)
        self.seg_btns = [ttk.Button(seg, text=t, command=lambda m=m: self._set_run_mode(m))
                         for m, t in ((0, "Audio"), (1, "Pattern"))]
        for i, b in enumerate(self.seg_btns):
            b.grid(row=0, column=i, sticky="ew", padx=(0, 4) if i == 0 else (4, 0))
        self._update_seg()

        alg = section("Algorithm")
        self.algo_combo = ttk.Combobox(alg, state="readonly")
        self.algo_combo.grid(row=1, column=0, sticky="ew", pady=(4, 0))
        self.algo_combo.bind("<<ComboboxSelected>>",
                             lambda e: self._select_algorithm(self.algo_combo.current()))

        pat = section("Pattern")
        self.pattern_var = tk.IntVar(value=0)
        self.pattern_spin = ttk.Spinbox(pat, from_=0, to=0, textvariable=self.pattern_var, width=6,
                                        command=lambda: self._run_config(pattern=self.pattern_var.get()))
        self.pattern_spin.grid(row=1, column=0, sticky="w", pady=(4, 0))

        bri_sec = section("")
        bh = ttk.Frame(bri_sec, style="Card.TFrame")
        bh.grid(row=0, column=0, sticky="ew")
        ttk.Label(bh, text="Brightness", style="Card.Muted.TLabel").pack(side="left")
        self.bri_var = tk.IntVar(value=0)
        self.bri_label = ttk.Label(bh, text="0%", style="Card.Muted.TLabel")
        self.bri_label.pack(side="right")
        bri = ttk.Scale(bri_sec, from_=0, to=254, variable=self.bri_var, style="Card.Horizontal.TScale",
                        command=lambda v: self.bri_label.configure(text=f"{round(float(v) / 2.54)}%"))
        bri.grid(row=1, column=0, sticky="ew", pady=(10, 4))          # 255 = "unchanged", so 0..254
        bri.bind("<ButtonRelease-1>", lambda e: self._run_config(brightness=int(self.bri_var.get())))

        view = section("")
        lv = ttk.Frame(view, style="Card.TFrame")
        lv.grid(row=0, column=0, sticky="ew")
        self.live_on = tk.BooleanVar(value=True)
        ttk.Checkbutton(lv, text="Live view", variable=self.live_on, style="Card.TCheckbutton",
                        command=self._toggle_live).pack(side="left")
        rate = ttk.Combobox(lv, textvariable=self.rate_var, values=("30 Hz", "60 Hz", "100 Hz"), state="readonly",
                            width=7)
        rate.pack(side="right")
        rate.bind("<<ComboboxSelected>>", lambda e: self._set_rate())
        ttk.Checkbutton(view, text="After processing", variable=self.proc_on, style="Card.TCheckbutton",
                        command=self._overlays_changed).grid(row=1, column=0, sticky="w", pady=(10, 0))
        ttk.Checkbutton(view, text="Filter overlay", variable=self.filt_on, style="Card.TCheckbutton",
                        command=self._overlays_changed).grid(row=2, column=0, sticky="w", pady=(6, 0))
        self.ctl_sections = [src, alg, pat, bri_sec, view]

        self.main_wide: bool | None = None
        tab.bind("<Configure>", self._relayout_main)

    def _relayout_main(self, e: tk.Event) -> None:
        """Side panel right of the live view on wide windows, below it on narrow ones."""
        wide = e.width >= 700
        if wide == self.main_wide:
            return
        self.main_wide = wide
        self.live_card.grid_forget()
        self.side.grid_forget()
        tab = self.main_tab
        # controls: one column in the side panel, two columns when stacked under the live view
        g = self.ctl_grid
        for sec in self.ctl_sections:
            sec.grid_forget()
        cols = 1 if wide else 2
        for c in range(2):
            g.columnconfigure(c, weight=1 if c < cols else 0, uniform="ctl" if c < cols else "")
        for i, sec in enumerate(self.ctl_sections):
            r, c = divmod(i, cols)
            sec.grid(row=r, column=c, sticky="ew", pady=(0, 14), padx=(0, 16) if (cols == 2 and c == 0) else 0)
        if wide:
            self.live_card.grid(row=0, column=0, sticky="nsew")
            self.side.grid(row=0, column=1, sticky="ns", padx=(16, 0))
            tab.columnconfigure(0, weight=1)
            tab.columnconfigure(1, weight=0, minsize=240)
            tab.rowconfigure(0, weight=1)
            tab.rowconfigure(1, weight=0)
        else:
            self.live_card.grid(row=0, column=0, sticky="nsew")
            self.side.grid(row=1, column=0, sticky="ew", pady=(16, 0))
            tab.columnconfigure(0, weight=1)
            tab.columnconfigure(1, weight=0, minsize=0)
            tab.rowconfigure(0, weight=1)

    def _set_run_mode(self, mode: int) -> None:
        self.run_mode_var.set(mode)
        self._update_seg()
        self._run_config(run_mode=mode)

    def _update_seg(self) -> None:
        for m, b in enumerate(self.seg_btns):
            b.configure(style="SegOn.TButton" if m == self.run_mode_var.get() else "Seg.TButton")

    def _build_calib_tab(self) -> None:
        tab = ttk.Frame(self.nb, padding=(0, 16, 0, 0))
        self.nb.add(tab, text="Calibration")
        bar = card(tab, padding=12)
        bar.pack(fill="x")
        self.calib_btn = ttk.Button(bar, text="Enter calibration", style="Accent.TButton", command=self._toggle_calib)
        self.calib_btn.pack(side="left")
        self.calib_filter = ttk.Combobox(bar, state="readonly", width=28)
        self.calib_filter.pack(side="left", padx=(12, 0))
        self.calib_filter.bind("<<ComboboxSelected>>", lambda e: self._build_param_editors())
        self.dirty_var = tk.StringVar()
        ttk.Label(bar, textvariable=self.dirty_var, style="Card.TLabel", foreground=C["warn"]).pack(side="left",
                                                                                                padx=12)
        for text, cmd in (("Import", self._calib_import), ("Export", self._calib_export),
                          ("Reset all", self._calib_reset), ("Reload", self._calib_reload)):
            ttk.Button(bar, text=text, command=cmd).pack(side="right", padx=(6, 0))
        self.save_btn = ttk.Button(bar, text="Save", style="Accent.TButton", command=self._calib_save)
        self.save_btn.pack(side="right", padx=(6, 0))
        self.param_area = ScrollFrame(tab)
        self.param_area.pack(fill="both", expand=True, pady=(16, 0))
        self.param_vars: dict[tuple[int, int], tk.Variable] = {}
        self.param_widgets: list[tk.Widget] = []

    def _build_fw_tab(self) -> None:
        tab = ttk.Frame(self.nb, padding=(0, 16, 0, 0))
        self.nb.add(tab, text="Firmware")

        dev = card(tab, "Device")
        dev.pack(fill="x")
        self.dev_grid = ttk.Frame(dev, style="Card.TFrame")
        self.dev_grid.pack(fill="x")
        self._show_device_rows([("Status", "Not connected")])

        up = card(tab, "Update")
        up.pack(fill="x", pady=(16, 0))
        self.fw_target = tk.IntVar(value=P.FwTarget.APP)
        ttk.Radiobutton(up, text="Application image (.bin)", value=P.FwTarget.APP, style="Card.TRadiobutton",
                        variable=self.fw_target).pack(anchor="w")
        ttk.Radiobutton(up, text="Calibration region image", value=P.FwTarget.CALIB, style="Card.TRadiobutton",
                        variable=self.fw_target).pack(anchor="w", pady=(2, 10))
        row = ttk.Frame(up, style="Card.TFrame")
        row.pack(fill="x")
        self.fw_file = tk.StringVar()
        ttk.Entry(row, textvariable=self.fw_file).pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="Browse", command=self._fw_browse).pack(side="left", padx=(8, 0))
        self.fw_prog = ttk.Progressbar(up, maximum=1.0)
        self.fw_prog.pack(fill="x", pady=(14, 6))
        self.fw_msg = tk.StringVar()
        ttk.Label(up, textvariable=self.fw_msg, style="Card.Muted.TLabel").pack(anchor="w")
        btns = ttk.Frame(up, style="Card.TFrame")
        btns.pack(fill="x", pady=(12, 0))
        self.fw_btn = ttk.Button(btns, text="Upload", style="Accent.TButton", command=self._fw_upload)
        self.fw_btn.pack(side="left")
        ttk.Button(btns, text="Abort", command=self.fw_abort.set).pack(side="left", padx=(8, 0))
        ttk.Button(btns, text="Download calibration image", command=self._calib_download).pack(side="right")

    def _show_device_rows(self, rows: list[tuple[str, str]]) -> None:
        for w in self.dev_grid.winfo_children():
            w.destroy()
        for r, (k, v) in enumerate(rows):
            ttk.Label(self.dev_grid, text=k, style="Card.Muted.TLabel").grid(row=r, column=0, sticky="w",
                                                                             padx=(0, 32), pady=2)
            ttk.Label(self.dev_grid, text=v, style="Card.TLabel").grid(row=r, column=1, sticky="w", pady=2)

    def _build_debug_tab(self) -> None:
        self.dbg_tab = ttk.Frame(self.nb, padding=(0, 16, 0, 0))
        sub = ttk.Notebook(self.dbg_tab)
        sub.pack(fill="both", expand=True)

        # settings (WiFi credentials etc.)
        st = ttk.Frame(sub, style="Card.TFrame", padding=20)
        sub.add(st, text="Settings")
        labels = {"device_name": "Device name", "sta_ssid": "WiFi network (SSID)", "sta_password": "WiFi password",
                  "max_current_ma": "Current limit [mA]", "num_leds": "LED count", "color_order": "Colour order"}
        self.set_vars: dict[str, tk.StringVar] = {}
        for row, sdef in enumerate(s for s in P.SETTINGS if s.id in (1, 3, 4, 8, 9, 14)):
            ttk.Label(st, text=labels[sdef.name], style="Card.Muted.TLabel").grid(row=row, column=0, sticky="w",
                                                                                 pady=6, padx=(0, 16))
            var = tk.StringVar()
            self.set_vars[sdef.name] = var
            if sdef.name == "color_order":
                w: tk.Widget = ttk.Combobox(st, textvariable=var, values=P.COLOR_ORDERS, state="readonly", width=8)
            else:
                w = ttk.Entry(st, textvariable=var, width=36, show="•" if sdef.name == "sta_password" else "")
                if sdef.name == "sta_password":
                    self.pw_entry = w
            w.grid(row=row, column=1, sticky="w", pady=6)
        self.pw_show = tk.BooleanVar()
        ttk.Checkbutton(st, text="Show", variable=self.pw_show, style="Card.TCheckbutton",
                        command=lambda: self.pw_entry.configure(show="" if self.pw_show.get() else "•")).grid(
            row=2, column=2, sticky="w", padx=(8, 0))
        b = ttk.Frame(st, style="Card.TFrame")
        b.grid(row=10, column=0, columnspan=3, sticky="w", pady=(16, 8))
        ttk.Button(b, text="Apply", style="Accent.TButton", command=self._settings_apply).pack(side="left")
        ttk.Button(b, text="Reload", command=self._settings_load).pack(side="left", padx=(8, 0))
        ttk.Button(b, text="Reboot device", command=self._reboot).pack(side="left", padx=(8, 0))
        ttk.Button(b, text="Factory reset", command=self._factory_reset).pack(side="left", padx=(8, 0))
        self.set_msg = tk.StringVar(value="Network changes take effect after a reboot.")
        ttk.Label(st, textvariable=self.set_msg, style="Card.Muted.TLabel").grid(row=11, column=0, columnspan=3,
                                                                                 sticky="w")

        # errors
        er = ttk.Frame(sub, style="Card.TFrame", padding=16)
        sub.add(er, text="Errors")
        b = ttk.Frame(er, style="Card.TFrame")
        b.pack(fill="x", pady=(0, 12))
        ttk.Button(b, text="Refresh", command=self._errors_load).pack(side="left")
        ttk.Button(b, text="Clear", command=self._errors_clear).pack(side="left", padx=(8, 0))
        self.err_tree = self._tree(er, ("code", "name", "severity", "count", "last [ms]"))

        # diagnostics
        dg = ttk.Frame(sub, style="Card.TFrame", padding=16)
        sub.add(dg, text="Diagnostics")
        b = ttk.Frame(dg, style="Card.TFrame")
        b.pack(fill="x", pady=(0, 12))
        ttk.Button(b, text="Refresh", command=self._diag_load).pack(side="left")
        ttk.Button(b, text="Reset max/min", command=self._diag_reset).pack(side="left", padx=(8, 0))
        self.diag_tree = self._tree(dg, ("group", "name", "value", "unit"))

        # CLI
        cl = ttk.Frame(sub, style="Card.TFrame", padding=16)
        sub.add(cl, text="CLI")
        self.cli_out = tk.Text(cl, height=20, font=FONT_MONO, state="disabled", background=C["bg"],
                               foreground=C["text"], insertbackground=C["text"], relief="flat", borderwidth=0,
                               highlightthickness=0, padx=10, pady=8, selectbackground=C["accent"])
        self.cli_out.pack(fill="both", expand=True)
        self.cli_in = ttk.Entry(cl, font=FONT_MONO)
        self.cli_in.pack(fill="x", pady=(10, 0))
        self.cli_in.bind("<Return>", self._cli_send)
        self.cli_in.bind("<Up>", lambda e: self._cli_hist(-1))
        self.cli_in.bind("<Down>", lambda e: self._cli_hist(1))
        self.cli_history: list[str] = []
        self.cli_pos = 0

        # request log
        lg = ttk.Frame(sub, style="Card.TFrame", padding=16)
        sub.add(lg, text="Log")
        self.log_list = tk.Listbox(lg, font=FONT_MONO, background=C["bg"], foreground=C["muted"], relief="flat",
                                   borderwidth=0, highlightthickness=0, selectbackground=C["accent"],
                                   activestyle="none")
        self.log_list.pack(fill="both", expand=True)

    def _tree(self, parent: tk.Widget, cols: tuple[str, ...]) -> ttk.Treeview:
        t = ttk.Treeview(parent, columns=cols, show="headings", height=14)
        for c in cols:
            t.heading(c, text=c, anchor="w")
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
            self._set_status("Enter the device IP address or use Scan", "error")
            return
        self.cfg["device"] = host
        self._set_status(f"Connecting to {host}…", "busy")
        self.conn_btn.configure(text="Disconnect", style="TButton")
        self.worker.connect(host)

    def _scan(self) -> None:
        self._set_status("Scanning…", "busy")

        def run() -> None:
            found = discover()
            self.results.put((self._scan_done, (found,)))
        threading.Thread(target=run, daemon=True).start()

    def _scan_done(self, found: list[dict[str, Any]]) -> None:
        self.dev_combo.configure(values=[d["ip"] for d in found])
        if not found:
            self._set_status("No device found - is it on the same network?", "error")
            return
        self._set_status(f"Found {len(found)} device{'s' if len(found) > 1 else ''}")
        if not self.worker.target:
            self.dev_var.set(found[0]["ip"])
            self._connect()

    def _on_link_status(self, why: str) -> None:
        if self.connected:
            self.connected = False
            self.live.latest = None
            self._layout_live()
            self._show_device_rows([("Status", "Not connected")])
        if not self.worker.target:
            self._set_status("Not connected")
            self.conn_btn.configure(text="Connect", style="Accent.TButton")
        elif why == "connecting":
            self._set_status(f"Connecting to {self.worker.target}…", "busy")
        else:
            self._set_status(why[:1].upper() + why[1:] + " - retrying", "error")

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
        algo = algs[info.active_algorithm] if info.active_algorithm < len(algs) else None
        self.worker.live_taps = self._wanted_taps(algo, schemas, values)
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
        self._set_status(f"Connected · {self.live.device_ip}", "ok")
        self._show_device_rows([
            ("Name", info.device_name), ("Firmware", f"{info.fw_version}  ({build})"), ("Built", info.build_date),
            ("Address", self.live.device_ip), ("WiFi", f"{info.rssi_dbm} dBm"),
            ("Audio", f"{info.sample_rate_hz} Hz, {info.hop_size}-sample blocks"),
            ("Last reset", f"reason {info.reset_reason}")])
        self.run_mode_var.set(info.run_mode)
        self._update_seg()
        self.algo_combo.configure(values=[a.name for a in algs])
        if info.active_algorithm < len(algs):
            self.algo_combo.current(info.active_algorithm)
        self.active_algo = info.active_algorithm
        self.pattern_spin.configure(to=max(0, info.num_patterns - 1))
        self.pattern_var.set(info.active_pattern)
        self.bri_var.set(min(info.brightness, 254))
        self.bri_label.configure(text=f"{round(min(info.brightness, 254) / 2.54)}%")
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
        self._set_status(f"{what}: {e}", "error" if not self.connected else "ok")

    def _log(self, line: str) -> None:
        self.log_list.insert("end", time.strftime("%H:%M:%S ") + line)
        if self.log_list.size() > 500:
            self.log_list.delete(0)
        self.log_list.see("end")

    # -- main tab ---------------------------------------------------------------------

    def _run_config(self, **kw: int) -> None:
        self.call(Cmd.SET_RUN_CONFIG, P.build_set_run_config(**kw))

    def _select_algorithm(self, idx: int) -> None:
        self.active_algo = idx
        self._run_config(algorithm=idx)
        self._draw_filters()            # overlays follow the running algorithm's filters
        self._subscribe()

    def _set_rate(self) -> None:
        self.cfg["live_hz"] = self.worker.live_hz = int(self.rate_var.get().split()[0])
        self._subscribe()

    def _toggle_live(self) -> None:
        self._subscribe()

    # Live view geometry is computed on resize; per frame only colours and coordinates change.

    def _layout_live(self) -> None:
        cv = self.cv
        cv.delete("all")
        w, h = max(cv.winfo_width(), 300), max(cv.winfo_height(), 260)
        p = 28
        avail = w - 2 * p
        n = self.info.num_leds if (self.info and self.connected) else 60

        # header: LED count + mode on the left, link stats + WiFi strength on the right
        head = f"{n} LEDs" if self.connected and self.info else "Not connected"
        t = cv.create_text(p, p, text=head, fill=C["text"] if self.connected else C["muted"], font=FONT_H, anchor="w")
        self.mode_item = cv.create_text(cv.bbox(t)[2] + 12, p, text="", fill=C["warn"], font=FONT_SM, anchor="w")
        x = w - p
        if self.connected and self.info:
            rssi = self.info.rssi_dbm
            t = cv.create_text(x, p, text=f"{rssi} dBm", fill=C["muted"], font=FONT_SM, anchor="e")
            x = cv.bbox(t)[0] - 8
            lit = 4 if rssi >= -55 else 3 if rssi >= -67 else 2 if rssi >= -75 else 1 if rssi >= -85 else 0
            col = C["ok"] if lit >= 3 else C["warn"] if lit == 2 else C["bad"]
            for i in range(4):
                bx = x - (3 - i) * 6
                cv.create_line(bx, p + 6, bx, p + 6 - (4 + 3 * i), width=4, capstyle="round",
                               fill=col if i < lit else C["line"])
            x -= 3 * 6 + 18
        self.stats_item = cv.create_text(x, p, text="", fill=C["faint"], font=FONT_SM, anchor="e")

        # strip: square LEDs with a glow on a rail; wraps into rows on narrow windows
        per_row = max(1, int(avail // 16))
        rows = -(-n // per_row)
        per_row = -(-n // rows)
        s = min(avail / per_row, 34.0)
        row_h = s * 1.25
        x0 = p + (avail - per_row * s) / 2 + s / 2
        y0 = p + 34 + s / 2
        core = s * 0.36
        self.led_items = []
        for r in range(rows):
            cnt = min(per_row, n - r * per_row)
            y = y0 + r * row_h
            cv.create_rectangle(x0 - s / 2, y - s / 2, x0 + (cnt - 0.5) * s, y + s / 2, fill=C["bg"], width=0)
            for i in range(cnt):
                cx = x0 + i * s
                self.led_items.append(cv.create_rectangle(cx - core, y - core, cx + core, y + core,
                                                          fill=C["socket"], width=0))
        self.led_colors = [""] * n
        y = y0 + (rows - 1) * row_h + s / 2 + 36

        # readouts: tempo with a beat pulse | genre | level and tone meters
        cv.create_text(p, y, text="TEMPO  ·  BPM", fill=C["faint"], font=FONT_SM, anchor="w")
        self.bpm_item = cv.create_text(p, y + 34, text="--", fill=C["text"], font=FONT_NUM, anchor="w")
        self.beat_item = cv.create_oval(p + 112, y + 28, p + 124, y + 40, fill=C["socket"], width=0)
        cv.create_text(p + 160, y, text="GENRE", fill=C["faint"], font=FONT_SM, anchor="w")
        self.genre_item = cv.create_text(p + 160, y + 34, text="--", fill=C["muted"], font=FONT_GENRE, anchor="w")
        self.shown_genre = -1
        wide = avail >= 640
        mx0 = p + 340 if wide else p
        my = y if wide else y + 80
        mx1 = w - p
        cv.create_text(mx0, my, text="LEVEL", fill=C["faint"], font=FONT_SM, anchor="w")
        cv.create_line(mx0 + 4, my + 22, mx1 - 4, my + 22, width=8, capstyle="round", fill=C["panel2"])
        self.level_item = cv.create_line(mx0 + 4, my + 22, mx0 + 4, my + 22, width=8, capstyle="round",
                                         fill=C["accent"], state="hidden")
        cv.create_text(mx0, my + 46, text="TONE", fill=C["faint"], font=FONT_SM, anchor="w")
        cv.create_text(mx1, my + 46, text="bright ›", fill=C["faint"], font=FONT_SM, anchor="e")
        cv.create_line(mx0 + 4, my + 66, mx1 - 4, my + 66, width=2, fill=C["line"])
        self.tone_item = cv.create_oval(mx0, my + 61, mx0 + 10, my + 71, fill=C["warn"], width=0, state="hidden")
        self.meter_x = (mx0 + 4, mx1 - 4)
        self.meter_y = (my + 22, my + 66)

        self._layout_spectrum(cv, p, w - p, max(y + 84, my + 92), h - p)
        self.drawn_frame = None

    def _layout_spectrum(self, cv: tk.Canvas, x_left: float, x_right: float, top: float, bottom: float) -> None:
        """Spectrum plot: log frequency axis, dBFS axis, optional filter-response axis (right)."""
        self.spec_ok = bottom - top > 110
        self.filt_items = []
        self.tap_state = {}
        if not self.spec_ok:
            return
        filt = self.cfg["filter_overlay"]
        cv.create_text(x_left, top, text="SPECTRUM  ·  dBFS", fill=C["faint"], font=FONT_SM, anchor="w")
        fs = self.info.sample_rate_hz if self.info else 16000
        self.f_lo, self.f_hi = 30.0, fs / 2
        l, r = x_left + 34, x_right - (40 if filt else 6)
        t, b = top + 22, bottom - 20
        self.plot = (l, r, t, b)
        cv.create_rectangle(l, t, r, b, fill=C["bg"], width=0)
        lo_db, hi_db = SPEC_DB_RANGE
        for db in range(int(lo_db), int(hi_db) + 1, 20 if b - t > 160 else 40):
            yy = self._y_db(db)
            cv.create_line(l, yy, r, yy, fill=C["line"])
            cv.create_text(l - 6, yy, text=str(db), fill=C["faint"], font=FONT_SM, anchor="e")
        for fq in FREQ_TICKS:
            if self.f_lo < fq < self.f_hi:
                xx = self._x_f(fq)
                cv.create_line(xx, t, xx, b, fill=C["line"])
                cv.create_text(xx, b + 10, text=f"{fq // 1000}k" if fq >= 1000 else str(fq), fill=C["faint"],
                               font=FONT_SM)
        cv.create_text(r, b + 10, text="Hz", fill=C["faint"], font=FONT_SM, anchor="e")
        if filt:
            flo, fhi = FILTER_DB_RANGE
            for db in range(int(flo), int(fhi) + 1, 12 if b - t > 160 else 24):
                cv.create_text(r + 6, self._y_filt(db), text=f"{db:+d}" if db else "0", fill=C["muted"],
                               font=FONT_SM, anchor="w")
            cv.create_text(x_right, top, text="FILTER RESPONSE  ·  dB", fill=C["muted"], font=FONT_SM, anchor="e")

        # query points: one every 3 px, interpolated in log frequency
        npts = max(2, int((r - l) / 3))
        self.q_x = [l + (r - l) * i / (npts - 1) for i in range(npts)]
        self.q_f = [self.f_lo * (self.f_hi / self.f_lo) ** (i / (npts - 1)) for i in range(npts)]
        self.q_logf = [math.log(f) for f in self.q_f]
        flat = [v for x in self.q_x for v in (x, b)]
        self.spec_fill = cv.create_polygon(*flat, r, b, l, b, fill=mix(C["accent"], C["bg"], 0.78), outline="",
                                           state="hidden")
        self.spec_peak = cv.create_line(*flat, fill=mix(C["accent"], C["bg"], 0.5), width=1, state="hidden")
        self.spec_line = cv.create_line(*flat, fill=C["accent"], width=2, state="hidden")
        self.proc_line = cv.create_line(*flat, fill=C["ok"], width=2, state="hidden")
        self.spec_msg = cv.create_text((l + r) / 2, (t + b) / 2, text="", fill=C["faint"], font=FONT)
        self._draw_filters()

    def _x_f(self, f: float) -> float:
        l, r, _, _ = self.plot
        return l + (r - l) * math.log(f / self.f_lo) / math.log(self.f_hi / self.f_lo)

    def _y_db(self, db: float) -> float:
        _, _, t, b = self.plot
        lo, hi = SPEC_DB_RANGE
        return b - (b - t) * (min(max(db, lo), hi) - lo) / (hi - lo)

    def _y_filt(self, db: float) -> float:
        _, _, t, b = self.plot
        lo, hi = FILTER_DB_RANGE
        return b - (b - t) * (min(max(db, lo), hi) - lo) / (hi - lo)

    @staticmethod
    def _enabled_filters(algo: P.Algorithm | None, schemas: dict[int, P.StageSchema],
                         values: dict[int, list[float]]) -> list[tuple[P.StageSchema, list[float]]]:
        """Filter stages of @algo that are switched on (type != off), in pipeline order.

        A filter stage is recognised by its parameters (type, frequency, q, gain), so a new
        filter instance needs no host change.
        """
        out = []
        for sid in (algo.stage_ids if algo else []):
            st = schemas.get(sid)
            vals = values.get(sid, [])
            if st and [p.name for p in st.params[:4]] == ["type", "frequency", "q", "gain"] \
                    and len(vals) >= 4 and int(round(vals[0])) > 0:
                out.append((st, vals))
        return out

    def _active_algorithm(self) -> P.Algorithm | None:
        if 0 <= self.active_algo < len(self.algorithms):
            return self.algorithms[self.active_algo]
        return None

    def _wanted_taps(self, algo: P.Algorithm | None, schemas: dict[int, P.StageSchema],
                     values: dict[int, list[float]]) -> int:
        """Spectrum taps for the current view: input (after the first stage, the front-end),
        the output of each enabled filter (filter overlay) and the end of the pipeline
        (processed overlay). Unticked overlays are not requested, so the device skips them."""
        if not algo or not algo.stage_ids:
            return 0
        mask = 1 << algo.stage_ids[0]
        if self.cfg["filter_overlay"]:
            for st, _ in self._enabled_filters(algo, schemas, values):
                mask |= 1 << st.id
        if self.cfg["processed_overlay"]:
            mask |= 1 << P.TAP_END
        return mask

    def _subscribe(self) -> None:
        """(Re)send LIVE_SUBSCRIBE with the live rate and the taps the view needs."""
        if not self.connected:
            return
        taps = self._wanted_taps(self._active_algorithm(), self.schemas, self.values)
        hz = self.worker.live_hz if self.live_on.get() else 0
        self.worker.live_taps = taps
        self.call(Cmd.LIVE_SUBSCRIBE, P.build_live_subscribe(self.live.port, hz, taps),
                  lambda body: self._on_layout(P.SpectrumLayout.decode(body)))
        self.live.reset()

    def _on_layout(self, lay: P.SpectrumLayout) -> None:
        fs = self.info.sample_rate_hz if self.info else 16000
        self.spec_layout = lay
        self.spec_logf = [math.log(f) for f in lay.freqs(fs)] if len(lay.edges) > 1 else []
        self.tap_state = {}
        if getattr(self, "spec_ok", False) and self.spec_logf:
            # the curve starts at the first point instead of extrapolating below it
            self.q0 = next((i for i, lf in enumerate(self.q_logf) if lf >= self.spec_logf[0]), 0)

    def _filters_changed(self) -> None:
        """Filter parameters changed: redraw responses; a filter switched on/off changes the taps."""
        self._draw_filters()
        if self._wanted_taps(self._active_algorithm(), self.schemas, self.values) != self.worker.live_taps:
            self._subscribe()

    def _overlays_changed(self) -> None:
        self.cfg["filter_overlay"] = self.filt_on.get()
        self.cfg["processed_overlay"] = self.proc_on.get()
        self._subscribe()
        self._layout_live()             # the filter-response axis appears / disappears

    def _draw_filters(self) -> None:
        """Filter overlay: per enabled filter its response (dashed, right axis), a marker with
        type and frequency, and a curve item for its measured output spectrum. Rebuilt only
        when filters, algorithm or layout change."""
        cv = self.cv
        for it in self.filt_items:
            cv.delete(it)
        self.filt_items = []
        self.filt_curves: dict[int, int] = {}
        if not getattr(self, "spec_ok", False) or not self.info:
            return
        l, r, t, b = self.plot
        roomy = b - t >= 150                # legend and filter labels only where they fit
        legend = [("Input", C["accent"])]
        if self.cfg["processed_overlay"]:
            legend.append(("After processing", C["ok"]))
        if self.cfg["filter_overlay"]:
            fs = float(self.info.sample_rate_hz)
            filters = self._enabled_filters(self._active_algorithm(), self.schemas, self.values)
            if not filters:
                self.filt_items.append(cv.create_text(r - 8, t + 12, text="no filter enabled", fill=C["muted"],
                                                      font=FONT_SM, anchor="e"))
            y0 = self._y_filt(0)
            if filters:
                self.filt_items.append(cv.create_line(l, y0, r, y0, fill=C["line"], dash=(2, 4)))
            for n, (st, vals) in enumerate(filters):
                col = FILTER_COLORS[n % len(FILTER_COLORS)]
                ftype, f0, q, gain = int(round(vals[0])), vals[1], vals[2], vals[3]
                resp = biquad_db(ftype, f0, q, gain, fs, self.q_f)
                pts = [v for x, d in zip(self.q_x, resp) for v in (x, self._y_filt(d))]
                self.filt_items.append(cv.create_line(*pts, fill=mix(col, C["bg"], 0.25), width=1, dash=(6, 4)))
                self.filt_curves[st.id] = cv.create_line(l, b, r, b, fill=col, width=2, state="hidden")
                self.filt_items.append(self.filt_curves[st.id])
                if self.f_lo < f0 < self.f_hi:
                    xx, yy = self._x_f(f0), self._y_filt(biquad_db(ftype, f0, q, gain, fs, [f0])[0])
                    opts = st.params[0].options
                    kind = opts[ftype] if ftype < len(opts) else ""
                    self.filt_items.append(cv.create_oval(xx - 4, yy - 4, xx + 4, yy + 4, fill=col, width=0))
                    if roomy:
                        self.filt_items.append(cv.create_text(xx + 8, yy - 10, fill=col, font=FONT_SM, anchor="w",
                                                              text=f"{st.name} · {kind} {f0:.0f} Hz"))
                legend.append((f"{st.name} output", col))
        cv.tag_raise(self.proc_line)        # the processed overlay stays visible over filter outputs
        x = l + 10
        for text, col in (legend if roomy else []):
            self.filt_items.append(cv.create_rectangle(x, t + 8, x + 10, t + 12, fill=col, width=0))
            it = cv.create_text(x + 14, t + 10, text=text, fill=C["muted"], font=FONT_SM, anchor="w")
            self.filt_items.append(it)
            x = cv.bbox(it)[2] + 16

    def _curve(self, tap: int, db: list[float], hold: bool = False) -> tuple[list[float], list[float]]:
        """Smoothed (fast attack, soft release) and interpolated points of one tap."""
        st = self.tap_state.get(tap)
        if st is None or len(st[0]) != len(db):
            st = self.tap_state[tap] = (list(db), list(db))
        disp, pk = st
        for i, v in enumerate(db):
            d = v if v > disp[i] else disp[i] * 0.7 + v * 0.3
            disp[i] = d
            pk[i] = d if d > pk[i] else pk[i] - 0.35          # peak hold falls ~20 dB/s
        qf = self.q_logf[self.q0:]
        ys = pchip(self.spec_logf, disp, qf)
        pts = [v for x, y in zip(self.q_x[self.q0:], ys) for v in (x, self._y_db(y))]
        if not hold:
            return pts, []
        return pts, [v for x, y in zip(self.q_x[self.q0:], pchip(self.spec_logf, pk, qf)) for v in (x, self._y_db(y))]

    def _genre_name(self, g: int) -> str:
        """Name from the music stage's "genre" enum (device schema), so new genres need no host change."""
        if g <= 0:
            return "--"
        for st in self.schemas.values():
            for prm in st.params:
                if prm.name == "genre" and prm.type == ParamType.ENUM and g < len(prm.options):
                    name = prm.options[g]
                    return name.upper() if len(name) <= 3 else name.capitalize()
        return f"#{g}"

    def _draw(self, f: P.LiveData) -> None:
        cv = self.cv
        leds = f.leds
        for i in range(min(len(self.led_items), len(leds) // 3)):
            r, g, b = leds[3 * i], leds[3 * i + 1], leds[3 * i + 2]
            key = "%02x%02x%02x" % (r, g, b)
            if key == self.led_colors[i]:
                continue
            self.led_colors[i] = key
            # colours as sent to the strip (brightness and power limit applied), drawn linearly
            cv.itemconfigure(self.led_items[i], fill="#" + key if r | g | b else C["socket"])

        (mx0, mx1), (ly, ty) = self.meter_x, self.meter_y
        lv = f.level / 32767
        cv.coords(self.level_item, mx0, ly, mx0 + (mx1 - mx0) * lv, ly)
        cv.itemconfigure(self.level_item, state="normal" if lv > 0.01 else "hidden",
                         fill=mix(C["accent"], C["ok"], lv))
        tx = mx0 + (mx1 - mx0) * f.centroid / 32767
        cv.coords(self.tone_item, tx - 5, ty - 5, tx + 5, ty + 5)
        cv.itemconfigure(self.tone_item, state="normal")

        cv.itemconfigure(self.bpm_item, text=f"{f.bpm_x10 / 10:.0f}" if f.bpm_x10 else "--")
        now = time.perf_counter()
        if f.beat_count != self.last_beat:
            if self.last_beat >= 0:
                self.beat_until = now
            self.last_beat = f.beat_count
        k = max(0.0, 1.0 - (now - self.beat_until) / 0.3)
        cv.itemconfigure(self.beat_item, fill=mix(C["socket"], C["bad"], k))
        if f.genre != self.shown_genre:
            self.shown_genre = f.genre
            cv.itemconfigure(self.genre_item, text=self._genre_name(f.genre),
                             fill=C["text"] if f.genre else C["muted"])
        cv.itemconfigure(self.mode_item, text={1: "CALIBRATION", 2: "FIRMWARE UPDATE"}.get(f.device_mode, ""))
        self._draw_spectrum(f)
        if f.device_mode != self.device_mode and f.device_mode in (0, 1, 2):
            self.device_mode = DeviceMode(f.device_mode)
            self._update_mode_ui()

    def _draw_spectrum(self, f: P.LiveData) -> None:
        if not self.spec_ok:
            return
        cv = self.cv
        algo = self._active_algorithm()
        in_tap = algo.stage_ids[0] if algo and algo.stage_ids else None
        base_items = (self.spec_fill, self.spec_peak, self.spec_line, self.proc_line)
        if not self.spec_logf or in_tap is None or in_tap not in f.spectra:
            for it in base_items + tuple(self.filt_curves.values()):
                cv.itemconfigure(it, state="hidden")
            cv.itemconfigure(self.spec_msg, text="Waiting for spectrum data" if self.live_on.get() else "")
            return
        cv.itemconfigure(self.spec_msg, text="")
        pts, pk = self._curve(in_tap, f.spectrum_db(in_tap), hold=True)
        xs, b = self.q_x[self.q0:], self.plot[3]
        cv.coords(self.spec_line, *pts)
        cv.coords(self.spec_fill, *pts, xs[-1], b, xs[0], b)
        cv.coords(self.spec_peak, *pk)
        for it in base_items[:3]:
            cv.itemconfigure(it, state="normal")
        overlays = [(P.TAP_END, self.proc_line)] if self.cfg["processed_overlay"] else []
        overlays += list(self.filt_curves.items())
        for tap, item in overlays:
            if tap in f.spectra:
                cv.coords(item, *self._curve(tap, f.spectrum_db(tap))[0])
                cv.itemconfigure(item, state="normal")
            else:
                cv.itemconfigure(item, state="hidden")
        if not (self.cfg["processed_overlay"] and P.TAP_END in f.spectra):
            cv.itemconfigure(self.proc_line, state="hidden")

    def _tick(self) -> None:
        """UI loop: worker results, then the newest live frame (if any). Always re-armed."""
        try:
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
        finally:
            self.root.after(FRAME_MS, self._tick)

    def _stats_tick(self) -> None:
        lv = self.live
        text = ""
        if self.connected and lv.frames:
            text = f"{lv.frames * 2} fps  ·  {lv.age_sum / lv.frames:.1f} ms jitter  ·  {lv.dropped} lost"
        elif self.connected and self.live_on.get():
            text = "waiting for live data"
        self.cv.itemconfigure(self.stats_item, text=text)
        lv.frames, lv.age_sum = 0, 0.0
        self.root.after(500, self._stats_tick)

    # -- calibration ---------------------------------------------------------------

    def _update_mode_ui(self) -> None:
        cal = self.device_mode == DeviceMode.CALIBRATION
        self.calib_btn.configure(text="Exit calibration" if cal else "Enter calibration",
                                 style="TButton" if cal else "Accent.TButton")
        self.save_btn.state(["!disabled"] if cal else ["disabled"])
        for w in self.param_widgets:
            w.state(["!disabled"] if cal else ["disabled"])
        self.dirty_var.set("● Unsaved changes" if self.calib_dirty else "")

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
            outer = card(body, st.name, st.description)
            outer.pack(fill="x", pady=(0, 12), padx=(0, 8))
            ttk.Button(outer.head, text="Reset", command=lambda s=sid: self._calib_reset(s)).pack(side="right")
            box = ttk.Frame(outer, style="Card.TFrame")
            box.pack(fill="x")
            box.columnconfigure(1, weight=1)
            for p in st.params:
                self._param_row(box, sid, p)
        self._update_mode_ui()

    def _param_row(self, box: ttk.Frame, sid: int, p: P.ParamSchema) -> None:
        row = p.id
        unit = "" if p.type == ParamType.ENUM else p.unit
        ttk.Label(box, text=p.name.replace("_", " ") + (f"  ({unit})" if unit else ""), style="Card.Muted.TLabel",
                  width=26).grid(row=row, column=0, sticky="w", padx=(0, 12), pady=4)
        vals = self.values.get(sid, [])
        cur = vals[p.id] if p.id < len(vals) else p.default
        key = (sid, p.id)
        if p.type == ParamType.BOOL:
            var: tk.Variable = tk.BooleanVar(value=cur >= 0.5)
            w = ttk.Checkbutton(box, variable=var, style="Card.TCheckbutton",
                                command=lambda: self._param_changed(key, float(var.get())))
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
            sc = ttk.Scale(box, from_=p.min, to=p.max, variable=var, command=from_scale,
                           style="Card.Horizontal.TScale")
            sc.grid(row=row, column=1, sticky="ew", pady=4)
            def from_spin(*_: Any) -> None:
                try:
                    self._param_changed(key, float(var.get()))
                except (tk.TclError, ValueError):
                    pass                # half-typed number
            sp = ttk.Spinbox(box, from_=p.min, to=p.max, increment=p.step or 1, textvariable=var, width=10,
                             command=from_spin)
            sp.bind("<Return>", from_spin)
            sp.grid(row=row, column=2, padx=(12, 0), pady=4)
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
        self._filters_changed()

    def _reload_values(self) -> None:
        def fn(link: Link) -> dict[int, list[float]]:
            return {sid: P.decode_params(link.request(Cmd.GET_PARAMS, bytes([sid])))[1] for sid in self.schemas}

        def done(values: dict[int, list[float]]) -> None:
            self.values = values
            self._build_param_editors()
            self._filters_changed()
        self.worker.submit(fn, done, lambda e: self._error("read parameters", e))

    def _calib_save(self) -> None:
        def done(_: bytes) -> None:
            self.calib_dirty = False
            self._update_mode_ui()
            self._set_status("Parameters saved to flash", "ok")
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
        self.call(Cmd.REBOOT, done=lambda _: self._set_status("Device rebooting…", "busy"))

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
