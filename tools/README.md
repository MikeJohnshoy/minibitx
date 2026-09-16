# tools/

Client-side utilities that talk to a running minibitx over the network -
none of these run on the Pi as part of minibitx itself, and none require
changes to build/run minibitx (beyond whatever server-side support they
depend on, noted per tool below).

## `rigctl_panel.py`

A small desktop GUI: a frequency readout/entry (with quick +/- step
buttons), a volume slider, and a live spectrum display. Talks the same
plain-text rigctld protocol WSJT-X/Thetis/etc. already use against
minibitx's built-in server (`hamlib.c`, default TCP 4532) - see
[`../docs/04_remote_control_and_iq_output.md`](../docs/04_remote_control_and_iq_output.md)
for the full command set. Volume relies on the `l`/`L AF` level commands
added alongside this tool - a minibitx build from before that change
will still connect and show frequency, but volume will read/set nothing.

The spectrum is fed by a second, independent connection - UDP to
`src/interfaces/iq_stream.c`'s lightweight I/Q telemetry stream (port 4536) - kept
deliberately separate from both the HPSDR Protocol 1 link WSJT-X/Thetis
use for their own I/Q (`hpsdr_p1.c`, single-client - a second client
there would silently steal the stream from whichever SDR app connected
first) and the CAT/audio USB gadget (`usb_gadget.c`). See
[`../docs/dsp_design_notes/iq_stream_design.md`](../docs/dsp_design_notes/iq_stream_design.md)
for the full design and bench verification. A minibitx build from before
`iq_stream.c` existed will still connect for frequency/volume; the
spectrum panel will just say it's waiting for data that never arrives.

Run it on a laptop, or on the Pi's own desktop if it has one - it's a
separate process from `minibitx`, connecting over TCP (rigctld) and UDP
(spectrum) like any other client, so it works either locally
(`127.0.0.1`) or from anywhere on the network that can reach the Pi.

**Requirements:** Python 3's standard library, `tkinter`, and `numpy`
(for the spectrum's FFT - the plot itself is drawn on a plain Tkinter
Canvas, no plotting library needed). `tkinter` usually ships with Python
on Windows/macOS, but is a separate package on Debian/Raspberry Pi OS:

```
sudo apt install python3-tk
pip install numpy   # or: sudo apt install python3-numpy
```

**Run:**

```
python3 tools/rigctl_panel.py
```

Enter the Pi's hostname or IP and the rigctld port (4532 by default),
click Connect. The panel remembers the last host/port used
(`~/.minibitx_panel.json`) so subsequent launches don't need retyping
them. Frequency and volume both poll once a second while connected, so
the panel stays current even if something else (another rigctld client,
FLRig's CAT, an HPSDR app's MOX) changes state in the meantime - except
the frequency entry box itself, which is left alone while it has focus
so a poll tick can't overwrite what you're mid-way through typing. The
spectrum starts/stops with the same Connect/Disconnect button, spans the
full ±48kHz native-96kHz baseband range around dial center (see
`iq_stream_design.md` for what actually limits how much of that is real,
undistorted signal vs. crystal-filter skirt), and updates at roughly
15fps regardless of how much faster the underlying FFT itself runs.
