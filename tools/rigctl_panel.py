#!/usr/bin/env python3
"""
rigctl_panel.py - a small standalone control panel for minibitx.

Talks the same plain-text rigctld protocol WSJT-X/Thetis/etc. already use
against minibitx's hamlib.c server (default TCP 4532 - see
docs/04_remote_control_and_iq_output.md) - nothing here is minibitx-specific
beyond the two commands it actually exercises:

    f  / F <hz>            get / set frequency
    l AF / L AF <0.0-1.0>  get / set the local CW monitor's volume

It also shows a live spectrum, fed by a second, independent UDP
connection to src/iq_stream.c's lightweight I/Q telemetry stream (UDP
port 4536, no relation to rigctld's TCP port above, and no relation to
the HPSDR Protocol 1 link WSJT-X/Thetis use for their own I/Q - see
iq_stream.h/.c's file headers for why this is its own third, minimal
path rather than reusing either). That stream carries the same native
96kHz baseband I/Q docs/02_rx_processing_pipeline.md describes, so what
the spectrum plot shows spans the full ±48kHz around dial center - the
same range docs/dsp_design_notes/antialias_filter_design.md's crystal-
filter analysis and rx_audio_demod_design.md's AGC-placement work were
both reasoning about, now actually visible.

Meant to run on a laptop, or on the Pi's own desktop if it has one - this
is a *client*, completely separate from the minibitx binary itself. Point
it at the Pi's hostname/IP and the rigctld port and it just needs a TCP
route to it - same as any other rigctld client (WSJT-X, Thetis, rigctl) -
plus, for the spectrum, a UDP route to the same host's port 4536 (most
LANs/home networks impose no extra firewalling here beyond what TCP
4532 already needed, but a locked-down network might).

Requires Python 3's standard library plus numpy (only numpy - the
spectrum is drawn on a plain Tkinter Canvas, no plotting library needed).
tkinter usually ships with Python on Windows/macOS; on Debian/Raspberry
Pi OS it's a separate package if missing: `sudo apt install python3-tk`.
numpy: `pip install numpy` (or `sudo apt install python3-numpy`).

Run it:

    python3 tools/rigctl_panel.py
"""

import json
import os
import socket
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import numpy as np
except ImportError:
    print("rigctl_panel.py needs numpy for the spectrum display "
          "(pip install numpy, or sudo apt install python3-numpy).",
          file=sys.stderr)
    sys.exit(1)

CONFIG_PATH = os.path.expanduser("~/.minibitx_panel.json")
DEFAULT_PORT = 4532
POLL_INTERVAL_S = 1.0
SOCKET_TIMEOUT_S = 2.0

# --- Spectrum (iq_stream.c) ---
IQ_STREAM_PORT = 4536          # src/iq_stream.h's IQ_STREAM_PORT
IQ_STREAM_MAGIC = b"IQS1"
SUBSCRIBE_INTERVAL_S = 1.0     # comfortably under iq_stream.c's 5s subscriber timeout
FFT_SIZE = 2048                # -> 96000/2048 = 46.875 Hz/bin across the full +-48kHz span
SPECTRUM_REDRAW_MS = 66        # ~15 fps - the FFT itself runs far faster than this and
                                # just keeps get_latest() fresh; no need to redraw faster
                                # than a human eye or a Tkinter Canvas benefits from
SPECTRUM_DB_FLOOR = -100.0     # y-axis floor, dBFS-style (0dB ~= one full-scale tone -
                                # see SpectrumClient's docstring on how that's calibrated)
SPECTRUM_DB_CEILING = 0.0
# The FFT itself still covers the full native +-48kHz (FFT_SIZE stays
# 2048 either way - resolution is unaffected), but only the middle
# +-15kHz gets drawn: docs/dsp_design_notes/antialias_filter_design.md's
# crystal-filter analysis puts the genuinely flat passband at only
# +-17.4/17.5kHz, with an asymmetric, increasingly attenuated skirt past
# that - so displaying the full +-48kHz mostly just shows the filter's
# own rolloff shape rather than real signal content, which is what
# prompted narrowing this.
SPECTRUM_DISPLAY_HALF_SPAN_HZ = 15000


def load_config():
    try:
        with open(CONFIG_PATH, "r") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w") as f:
            json.dump(cfg, f)
    except OSError:
        pass  # best-effort - remembering the last host/port isn't worth
              # failing the app over


class RigctlClient:
    """One TCP connection to minibitx's rigctld server.

    rigctld is a plain line-request/line-reply protocol with no request
    IDs, so two commands must never be in flight on the same socket at
    once - there would be no way to tell which reply answers which
    request. self.lock enforces "one command at a time" between the
    panel's own actions (Tune, drag the volume slider) and the
    background poll loop's periodic refresh.
    """

    def __init__(self):
        self.sock = None
        self.lock = threading.Lock()

    def connect(self, host, port):
        s = socket.create_connection((host, port), timeout=SOCKET_TIMEOUT_S)
        s.settimeout(SOCKET_TIMEOUT_S)
        with self.lock:
            self.sock = s

    def disconnect(self):
        with self.lock:
            if self.sock is not None:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None

    def connected(self):
        with self.lock:
            return self.sock is not None

    def query(self, command):
        """Send one command line, return the first reply line (stripped),
        or None if not connected, or on any socket error - which also
        drops the connection. The caller finds out via connected()
        turning false; this never retries on its own."""
        with self.lock:
            if self.sock is None:
                return None
            try:
                self.sock.sendall((command + "\n").encode())
                buf = b""
                while not buf.endswith(b"\n"):
                    chunk = self.sock.recv(256)
                    if not chunk:
                        raise OSError("connection closed by remote")
                    buf += chunk
                return buf.decode(errors="replace").strip()
            except OSError:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None
                return None


class SpectrumClient:
    """Subscribes to iq_stream.c's UDP telemetry stream and keeps a
    rolling FFT of the most recent FFT_SIZE baseband I/Q samples ready
    for the GUI to draw.

    Fully independent of RigctlClient's TCP connection - its own UDP
    socket, its own subscribe/keepalive traffic (iq_stream.c drops a
    subscriber that goes quiet for 5s, so this just re-sends a bare
    datagram every SUBSCRIBE_INTERVAL_S to stay subscribed), its own
    receive thread. If that UDP port isn't reachable (firewalled, or an
    older minibitx build without iq_stream.c), the spectrum panel just
    never gets data - the frequency/volume controls over rigctld keep
    working regardless, since the two connections don't know about each
    other any more than iq_stream.c and hamlib.c do on the minibitx side.

    dB calibration: iq_stream.c scales samples so a full-scale baseband
    tone reads close to int16 full-scale (32767 - see iq_stream.c's file
    header). For a Hann-windowed FFT of a single tone sitting exactly on
    a bin center, dividing that bin's magnitude by (FFT_SIZE * mean(window))
    recovers the input tone's amplitude - so 0dB here means "as loud as a
    single full-scale input tone would read," a normal dBFS-style
    reference, not an absolutely-calibrated S-meter (no different from
    what any SDR app's own spectrum display does without a signal
    generator to calibrate against).
    """

    def __init__(self):
        self.sock = None
        self.host = None
        self.stop_event = threading.Event()
        self.recv_thread = None
        self.keepalive_thread = None
        self.lock = threading.Lock()
        self.sample_buf = np.zeros(0, dtype=np.complex128)
        self.latest_db = None  # np.ndarray (FFT_SIZE,), fftshifted low->high freq, or None
        self.window = np.hanning(FFT_SIZE)
        self.window_mean = float(np.mean(self.window)) or 1.0

    def start(self, host):
        self.stop()  # tolerate start() called twice without an intervening stop()
        self.host = host
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.5)
        self.stop_event.clear()
        self.sample_buf = np.zeros(0, dtype=np.complex128)
        with self.lock:
            self.latest_db = None
        self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.keepalive_thread = threading.Thread(target=self._keepalive_loop, daemon=True)
        self.recv_thread.start()
        self.keepalive_thread.start()

    def stop(self):
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def _keepalive_loop(self):
        while not self.stop_event.is_set():
            sock = self.sock
            if sock is not None:
                try:
                    sock.sendto(b"S", (self.host, IQ_STREAM_PORT))
                except OSError:
                    pass
            time.sleep(SUBSCRIBE_INTERVAL_S)

    def _recv_loop(self):
        while not self.stop_event.is_set():
            sock = self.sock
            if sock is None:
                break
            try:
                data, _addr = sock.recvfrom(2048)
            except OSError:
                continue
            if len(data) < 12 or data[0:4] != IQ_STREAM_MAGIC:
                continue
            n_samples = struct.unpack(">I", data[8:12])[0]
            if len(data) != 12 + n_samples * 4:
                continue  # torn/short packet - drop it, next one will be fine

            raw = np.frombuffer(data, dtype=">i2", count=n_samples * 2, offset=12)
            iq = raw.astype(np.float64).reshape(-1, 2)
            # Conjugated (-Q, not +Q) - display-orientation correction only,
            # not a bug workaround. minibitx's raw baseband I/Q (shared
            # identically by hpsdr_p1.c and usb_gadget.c's UAC2 gadget, not
            # just this stream) has a real, single spectral inversion built
            # in: vfo.c's vfo_read_iq() returns I=cos(phase)/Q=+sin(phase)
            # with phase advancing at the *positive* RX_IF_FREQ_HZ rate, and
            # sound.c multiplies the real IF sample by that directly with no
            # compensating sign anywhere downstream - working through the
            # mixing math (see docs/dsp_design_notes/iq_stream_design.md)
            # shows a station at +Δ Hz above dial center lands at baseband
            # frequency -Δ, i.e. tuning up moves every station right instead
            # of the conventional-SDR-display left. That's a property of the
            # shared RX chain every I/Q consumer (WSJT-X/Thetis on HPSDR,
            # a UAC2 host) already lives with - not something to silently
            # "fix" here by touching vfo.c/sound.c, which could disturb an
            # already-working setup elsewhere. Conjugating here only flips
            # this one display's left/right sense to match the orientation
            # operators expect from a conventional SDR waterfall.
            samples = (iq[:, 0] - 1j * iq[:, 1]) / 32767.0

            self.sample_buf = np.concatenate((self.sample_buf, samples))
            if len(self.sample_buf) > FFT_SIZE * 2:
                self.sample_buf = self.sample_buf[-FFT_SIZE * 2:]

            if len(self.sample_buf) >= FFT_SIZE:
                block = self.sample_buf[-FFT_SIZE:]
                spectrum = np.fft.fftshift(np.fft.fft(block * self.window))
                mag = np.abs(spectrum) / (FFT_SIZE * self.window_mean)
                db = 20.0 * np.log10(mag + 1e-12)
                with self.lock:
                    self.latest_db = db

    def get_latest(self):
        with self.lock:
            return None if self.latest_db is None else self.latest_db.copy()


class Panel(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("minibitx control panel")
        self.resizable(False, False)

        self.client = RigctlClient()
        self.poll_thread = None
        self.poll_stop = threading.Event()
        self.freq_entry_focused = False

        self.spectrum = SpectrumClient()
        self.spectrum_running = False
        self.current_freq_hz = None

        cfg = load_config()

        # --- connection row ---
        conn = ttk.Frame(self, padding=8)
        conn.grid(row=0, column=0, sticky="ew")
        ttk.Label(conn, text="Host:").grid(row=0, column=0)
        self.host_var = tk.StringVar(value=cfg.get("host", ""))
        ttk.Entry(conn, textvariable=self.host_var, width=18).grid(row=0, column=1, padx=4)
        ttk.Label(conn, text="Port:").grid(row=0, column=2)
        self.port_var = tk.StringVar(value=str(cfg.get("port", DEFAULT_PORT)))
        ttk.Entry(conn, textvariable=self.port_var, width=6).grid(row=0, column=3, padx=4)
        self.connect_btn = ttk.Button(conn, text="Connect", command=self.on_connect_clicked)
        self.connect_btn.grid(row=0, column=4, padx=8)
        self.status_var = tk.StringVar(value="disconnected")
        self.status_label = ttk.Label(conn, textvariable=self.status_var, foreground="#a00")
        self.status_label.grid(row=0, column=5, padx=4)

        # --- frequency ---
        freq = ttk.LabelFrame(self, text="Frequency (Hz)", padding=8)
        freq.grid(row=1, column=0, sticky="ew", padx=8, pady=4)
        self.freq_display_var = tk.StringVar(value="—")
        ttk.Label(freq, textvariable=self.freq_display_var, font=("monospace", 20)).grid(
            row=0, column=0, columnspan=6, pady=(0, 6))

        self.freq_entry_var = tk.StringVar()
        entry = ttk.Entry(freq, textvariable=self.freq_entry_var, width=14, font=("monospace", 12))
        entry.grid(row=1, column=0, columnspan=3)
        entry.bind("<FocusIn>", lambda e: setattr(self, "freq_entry_focused", True))
        entry.bind("<FocusOut>", lambda e: setattr(self, "freq_entry_focused", False))
        entry.bind("<Return>", lambda e: self.on_tune_clicked())
        ttk.Button(freq, text="Tune", command=self.on_tune_clicked).grid(row=1, column=3, padx=4)

        steps = ttk.Frame(freq)
        steps.grid(row=2, column=0, columnspan=6, pady=(6, 0))
        for label, delta in [("-1k", -1000), ("-100", -100), ("-10", -10),
                              ("+10", 10), ("+100", 100), ("+1k", 1000)]:
            ttk.Button(steps, text=label, width=5,
                       command=lambda d=delta: self.on_step_clicked(d)).pack(side="left", padx=2)

        # --- volume ---
        vol = ttk.LabelFrame(self, text="Volume", padding=8)
        vol.grid(row=2, column=0, sticky="ew", padx=8, pady=(4, 8))
        self.vol_var = tk.IntVar(value=50)
        self.vol_scale = ttk.Scale(vol, from_=0, to=100, orient="horizontal",
                                    variable=self.vol_var, length=280,
                                    command=self.on_volume_dragged)
        self.vol_scale.grid(row=0, column=0, padx=(0, 8))
        # The actual L AF command only goes out on release, not on every
        # tick of the drag - see on_volume_released.
        self.vol_scale.bind("<ButtonRelease-1>", self.on_volume_released)
        self.vol_label = ttk.Label(vol, text="50%", width=5)
        self.vol_label.grid(row=0, column=1)

        # --- spectrum ---
        spec = ttk.LabelFrame(self, text="Spectrum (±15kHz around dial)", padding=8)
        spec.grid(row=3, column=0, sticky="ew", padx=8, pady=(0, 8))
        self.spectrum_canvas_w = 560
        self.spectrum_canvas_h = 180
        self.spectrum_canvas = tk.Canvas(spec, width=self.spectrum_canvas_w,
                                          height=self.spectrum_canvas_h,
                                          background="#111", highlightthickness=0)
        self.spectrum_canvas.pack()
        self.spectrum_status_var = tk.StringVar(value="no spectrum data yet")
        ttk.Label(spec, textvariable=self.spectrum_status_var).pack(anchor="w", pady=(4, 0))

        self.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---- connection handling ----

    def on_connect_clicked(self):
        if self.client.connected():
            self.disconnect()
            return
        host = self.host_var.get().strip()
        try:
            port = int(self.port_var.get().strip())
        except ValueError:
            messagebox.showerror("minibitx panel", "Port must be a number.")
            return
        if not host:
            messagebox.showerror("minibitx panel", "Enter the Pi's hostname or IP address.")
            return
        try:
            self.client.connect(host, port)
        except OSError as e:
            messagebox.showerror("minibitx panel", f"Couldn't connect to {host}:{port}\n{e}")
            return

        save_config({"host": host, "port": port})
        self.status_var.set("connected")
        self.status_label.configure(foreground="#0a0")
        self.connect_btn.configure(text="Disconnect")

        # One immediate refresh so the panel isn't blank while it waits
        # for the first poll tick.
        self.refresh_once()

        self.poll_stop.clear()
        self.poll_thread = threading.Thread(target=self.poll_loop, daemon=True)
        self.poll_thread.start()

        # Independent of the rigctld connection above - see SpectrumClient's
        # docstring. Started/stopped alongside it purely because "connect"
        # is the one button this panel has; a failure here never affects
        # rigctld's own connection.
        self.spectrum.start(host)
        if not self.spectrum_running:
            self.spectrum_running = True
            self.after(SPECTRUM_REDRAW_MS, self.redraw_spectrum)

    def disconnect(self):
        self.poll_stop.set()
        self.client.disconnect()
        self.spectrum.stop()
        self.spectrum_running = False
        self.current_freq_hz = None
        self.spectrum_status_var.set("no spectrum data yet")
        self.spectrum_canvas.delete("all")
        self.status_var.set("disconnected")
        self.status_label.configure(foreground="#a00")
        self.connect_btn.configure(text="Connect")

    def on_close(self):
        self.disconnect()
        self.destroy()

    # ---- background polling ----
    #
    # Runs on its own thread so a slow/stalled connection can never
    # freeze the GUI's own event loop. All widget updates it triggers go
    # through self.after(...) to hand them back to the main thread rather
    # than touching Tk widgets directly from here.

    def poll_loop(self):
        while not self.poll_stop.is_set():
            if not self.client.connected():
                self.after(0, self.disconnect)
                return
            self.refresh_once()
            time.sleep(POLL_INTERVAL_S)

    def refresh_once(self):
        freq_reply = self.client.query("f")
        vol_reply = self.client.query("l AF")
        if freq_reply is None or vol_reply is None:
            self.after(0, self.disconnect)
            return
        self.after(0, lambda: self.apply_freq(freq_reply))
        self.after(0, lambda: self.apply_volume(vol_reply))

    def apply_freq(self, reply):
        try:
            hz = int(reply)
        except ValueError:
            return
        self.current_freq_hz = hz
        self.freq_display_var.set(f"{hz:,}".replace(",", "."))
        # Don't clobber text the operator is mid-way through typing.
        if not self.freq_entry_focused:
            self.freq_entry_var.set(str(hz))

    def apply_volume(self, reply):
        try:
            pct = round(float(reply) * 100)
        except ValueError:
            return
        self.vol_var.set(pct)
        self.vol_label.configure(text=f"{pct}%")

    # ---- user actions ----

    def on_tune_clicked(self):
        if not self.client.connected():
            return
        try:
            hz = int(self.freq_entry_var.get().strip())
        except ValueError:
            messagebox.showerror("minibitx panel", "Frequency must be a whole number of Hz.")
            return
        threading.Thread(target=lambda: self.client.query(f"F {hz}"), daemon=True).start()

    def on_step_clicked(self, delta):
        if not self.client.connected():
            return
        try:
            hz = int(self.freq_entry_var.get().strip())
        except ValueError:
            return
        hz = max(0, hz + delta)
        self.freq_entry_var.set(str(hz))
        threading.Thread(target=lambda: self.client.query(f"F {hz}"), daemon=True).start()

    def on_volume_dragged(self, _value):
        # Live label update while dragging; see on_volume_released for
        # when the L AF command actually goes out.
        pct = int(round(self.vol_var.get()))
        self.vol_label.configure(text=f"{pct}%")

    def on_volume_released(self, _event):
        if not self.client.connected():
            return
        pct = int(round(self.vol_var.get()))
        val = pct / 100.0
        threading.Thread(target=lambda: self.client.query(f"L AF {val:.3f}"), daemon=True).start()

    # ---- spectrum ----
    #
    # Runs entirely on the main/Tk thread via self.after() - SpectrumClient
    # does its own socket work and FFT math on its own threads and just
    # hands back a finished numpy array through get_latest(); this method
    # only ever touches the Canvas, never blocks.

    def redraw_spectrum(self):
        if not self.spectrum_running:
            return  # disconnect() already cleared the canvas
        if not self.client.connected():
            self.spectrum_running = False
            return

        db = self.spectrum.get_latest()
        canvas = self.spectrum_canvas
        w, h = self.spectrum_canvas_w, self.spectrum_canvas_h
        canvas.delete("all")

        if db is None:
            self.spectrum_status_var.set(
                f"waiting for I/Q telemetry on UDP {IQ_STREAM_PORT} "
                "(older minibitx builds without iq_stream.c won't send any)")
        else:
            # db spans the full native +-48kHz (fftshifted, bin 0 = -48kHz,
            # bin FFT_SIZE/2 = dial center) - crop to the middle +-15kHz for
            # display (see SPECTRUM_DISPLAY_HALF_SPAN_HZ's comment on why).
            bin_hz = 96000.0 / FFT_SIZE
            center = len(db) // 2
            half_bins = min(int(round(SPECTRUM_DISPLAY_HALF_SPAN_HZ / bin_hz)), center)
            db = db[center - half_bins:center + half_bins]

            n = len(db)
            xs = np.arange(n) * (w / n)
            clipped = np.clip(db, SPECTRUM_DB_FLOOR, SPECTRUM_DB_CEILING)
            frac = (clipped - SPECTRUM_DB_FLOOR) / (SPECTRUM_DB_CEILING - SPECTRUM_DB_FLOOR)
            ys = h - frac * h
            coords = np.empty(n * 2)
            coords[0::2] = xs
            coords[1::2] = ys
            canvas.create_line(*coords.tolist(), fill="#3fa", width=1)

            # Dial-center line plus frequency ticks across the displayed
            # +-SPECTRUM_DISPLAY_HALF_SPAN_HZ span.
            span = half_bins * bin_hz  # actual displayed half-span, close to
                                        # SPECTRUM_DISPLAY_HALF_SPAN_HZ but
                                        # snapped to a whole number of bins
            canvas.create_line(w / 2, 0, w / 2, h, fill="#555", dash=(2, 2))
            for offset_hz, label in ((-span, f"-{span/1000:.0f}k"), (-span / 2, f"-{span/2000:.0f}k"),
                                      (0, "dial"), (span / 2, f"+{span/2000:.0f}k"),
                                      (span, f"+{span/1000:.0f}k")):
                x = (offset_hz + span) / (2 * span) * w
                x = min(max(x, 4), w - 4)  # keep the end labels from clipping off-canvas
                canvas.create_text(x, h - 8, text=label, fill="#999", font=("monospace", 8))

            freq_note = f" (dial {self.current_freq_hz:,} Hz)".replace(",", ".") \
                if self.current_freq_hz is not None else ""
            self.spectrum_status_var.set(
                f"live - {FFT_SIZE}-pt FFT, {bin_hz:.1f} Hz/bin{freq_note}")

        self.after(SPECTRUM_REDRAW_MS, self.redraw_spectrum)


if __name__ == "__main__":
    Panel().mainloop()
