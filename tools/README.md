# tools/

Client-side utilities that talk to a running minibitx over the network -
none of these run on the Pi as part of minibitx itself, and none require
changes to build/run minibitx (beyond whatever server-side support they
depend on, noted per tool below).

## `rigctl_panel.py`

A small desktop GUI: a frequency readout/entry (with quick +/- step
buttons) and a volume slider. Talks the same plain-text rigctld protocol
WSJT-X/Thetis/etc. already use against minibitx's built-in server
(`hamlib.c`, default TCP 4532) - see
[`../docs/04_remote_control_and_iq_output.md`](../docs/04_remote_control_and_iq_output.md)
for the full command set. Volume relies on the `l`/`L AF` level commands
added alongside this tool - a minibitx build from before that change
will still connect and show frequency, but volume will read/set nothing.

Run it on a laptop, or on the Pi's own desktop if it has one - it's a
separate process from `minibitx`, connecting over TCP like any other
rigctld client, so it works either locally (`127.0.0.1`) or from anywhere
that can reach the Pi's rigctld port.

**Requirements:** Python 3's standard library only, plus `tkinter` - which
usually ships with Python on Windows/macOS, but is a separate package on
Debian/Raspberry Pi OS:

```
sudo apt install python3-tk
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
so a poll tick can't overwrite what you're mid-way through typing.
