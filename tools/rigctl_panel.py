#!/usr/bin/env python3
"""
rigctl_panel.py - a small standalone control panel for minibitx.

Talks the same plain-text rigctld protocol WSJT-X/Thetis/etc. already use
against minibitx's hamlib.c server (default TCP 4532 - see
docs/04_remote_control_and_iq_output.md) - nothing here is minibitx-specific
beyond the two commands it actually exercises:

    f  / F <hz>            get / set frequency
    l AF / L AF <0.0-1.0>  get / set the local CW monitor's volume

Meant to run on a laptop, or on the Pi's own desktop if it has one - this
is a *client*, completely separate from the minibitx binary itself. Point
it at the Pi's hostname/IP and the rigctld port and it just needs a TCP
route to it - same as any other rigctld client (WSJT-X, Thetis, rigctl).

Requires only Python 3's standard library. tkinter usually ships with
Python on Windows/macOS; on Debian/Raspberry Pi OS it's a separate
package if missing: `sudo apt install python3-tk`.

Run it:

    python3 tools/rigctl_panel.py
"""

import json
import os
import socket
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

CONFIG_PATH = os.path.expanduser("~/.minibitx_panel.json")
DEFAULT_PORT = 4532
POLL_INTERVAL_S = 1.0
SOCKET_TIMEOUT_S = 2.0


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


class Panel(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("minibitx control panel")
        self.resizable(False, False)

        self.client = RigctlClient()
        self.poll_thread = None
        self.poll_stop = threading.Event()
        self.freq_entry_focused = False

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

    def disconnect(self):
        self.poll_stop.set()
        self.client.disconnect()
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


if __name__ == "__main__":
    Panel().mainloop()
