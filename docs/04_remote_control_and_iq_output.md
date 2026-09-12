# 04 — remote control and I/Q output

This covers everything minibitx exposes to the outside world: how an
external control surface tunes and keys the radio, and how the baseband
I/Q produced in
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) gets to
the SDR application actually using it. The two are documented together
because one file — `hpsdr_p1.c` — does both jobs.

## The single-entry-point pattern

`radio_tune_to()` and `radio_set_tx()` (`radio.c`) are the *only*
functions that touch hardware to change frequency or key the
transmitter. Every control surface calls into these two functions rather
than poking GPIO or the si5351 directly — so adding a new control
surface never means a second place that can put the hardware in an
inconsistent state. Today there are two callers:

- **Hamlib/rigctld** (`hamlib.c`) — a live frequency control surface
  (`F`), primarily for WSJT-X and similar apps.
- **Kenwood-CAT emulation over USB** (the CAT section of `usb_gadget.c`)
  — a second, independent frequency/PTT control surface (`FA`), for
  control apps like FLRig that can't speak Hamlib's rigctld protocol —
  see below.
- **HPSDR's inbound command parser** (`hpsdr_p1.c`) — calls
  `radio_set_tx()` only, for MOX/PTT (below).

## Hamlib / rigctld server

`hamlib.c` runs a minimal rigctld-compatible TCP server (`hamlib_init()`,
default port 4532, one thread accepting connections and one per client).
It implements a small plain-text rigctl command set:

| Command | Behavior |
|---|---|
| `f` / `F <hz>` | get / set frequency — `F` calls `radio_tune_to()` |
| `t` / `T <0\|1>` | get / set PTT — `T` calls `radio_set_tx()`; any nonzero value means TX (no separate mic/data state) |
| `m` / `M <mode> <passband>` | get / set mode — **cosmetic only**, stored but never acted on, since minibitx has no onboard demod |
| `chk_vfo` | always reports "not in VFO mode" (single-VFO radio) |
| `dump_state` | minimal capability dump for client negotiation — deliberately advertises no RIT/XIT/IF-shift/preamp/attenuator/onboard-filter support, and an empty TX range (no TX audio path yet) |
| `q` / `Q` / `quit` | disconnect |

It's a small command set on purpose: minibitx isn't the thing making
demod/filtering decisions, the SDR app is. Point an SDR app's CAT/rig
control at `127.0.0.1:4532` (rig model "Hamlib NET rigctl") alongside its
HPSDR connection for live retuning.

Every command that changes or reports state also echoes to the console,
one line per command, e.g. `rigctl: F 7074000 -> tuned to 7074000 Hz` or
`rigctl: T 1 -> TX on`. `radio_tune_to()`/`radio_set_tx()` (`radio.c`)
themselves print nothing — each control surface logs its own result,
since it's the one that knows which command triggered the change. This
is minibitx's primary window into operational state once startup
finishes; see
[`05_process_and_threading_model.md`](05_process_and_threading_model.md).

## Kenwood-CAT emulation over USB (for FLRig)

`hamlib.c`'s rigctld server works fine for WSJT-X (it has a native
"Hamlib NET rigctl" rig type), but FLRig has no such option — it only
ever speaks CAT over a serial port to what it believes is a real radio.
Rather than build FLRig a TCP-to-serial bridge (`com0com`/`com2tcp` on
Windows, or similar), `usb_gadget.c` implements a second, independent
control surface — in its own clearly-marked "Kenwood TS-480-subset CAT
control" section near the bottom of that file, alongside the UAC2 code
rather than a separate translation unit, since both are just two
functions of the one composite gadget `usb_gadget.c` already owns —
that answers as a Kenwood TS-480 over the USB gadget's own CDC-ACM
serial function (see [`usb_gadget_os_setup.md`](usb_gadget_os_setup.md)
for the gadget composite-device details) — so FLRig just opens the COM
port Windows assigns the gadget, no extra software involved. The TS-480
subset was picked because it's exactly the CAT dialect the QRP Labs
QMX/QMX+ already emulates for the same reason (old enough to be widely
supported, and QMX has no genuine SSB TX either, so its command set
already excludes modes minibitx can't produce).

Implemented commands (Kenwood convention: "set" commands get no reply;
only bare "get" queries do):

| Command | Behavior |
|---|---|
| `ID` | get only — always replies `020` (the TS-480's ID code) |
| `FA` / `FB` | get / set frequency, 11-digit Hz — `FA` calls `radio_tune_to()`; `FB` mirrors `FA` on get and is accepted-but-ignored on set (single VFO) |
| `TX` / `RX` | bare, immediate PTT, no reply — calls `radio_set_tx()`, same "local CW key wins" guard as Hamlib's `T` |
| `TQ` | get / set PTT (0/1) — another way to ask for the same thing as `TX`/`RX` |
| `MD` | get / set mode — **cosmetic only**, same reasoning as Hamlib's `M`; defaults to `3` (CW), the one mode minibitx can actually transmit |
| `IF` | get only — combined status string (frequency, TX/RX, mode); RIT/XIT/memory/scan/split/tone all reported as off/zero since minibitx has none of them |

Anything else is silently ignored, matching real Kenwood radios rather
than inventing an error reply convention that doesn't exist in the CAT
protocol. Like every other control surface here, `cat_init()` failing
(most commonly: no USB gadget support on this hardware/kernel, or the
gadget failed to bind) is not fatal — minibitx keeps running on whatever
subset of control surfaces actually came up.

WSJT-X needs no changes and keeps using the Hamlib server above; this is
purely additive.

## HPSDR Protocol 1 — inbound (control) and outbound (I/Q)

`hpsdr_p1.c` implements a minimal openHPSDR Protocol 1 link over UDP,
and handles both directions of that link:

**Inbound (EP2):** frequency and mode control live entirely in the
rigctld server above — the *only* thing `hpsdr_p1.c` reads from the
inbound stream is the MOX (PTT) bit, since some SDR apps key PTT through
the I/Q link's C0 byte even while using CAT for everything else. When it
sees that bit change, it calls `radio_set_tx()` — the same entry point
Hamlib uses, never a separate path — and echoes it to the console as
`hpsdr: MOX -> TX on` / `hpsdr: MOX -> TX off`, tagged `hpsdr:` rather
than `rigctl:` so it's clear which control surface actually drove the
change.

Getting this parser right took a few real bugs shaking out on the
bench, all against `process_ep2_frame()`/`handle_command()`
(`hpsdr_p1.c`): an early, looser version with no exact-length checks on
inbound packets let torn/malformed EP2 frames decode into nonsensical,
wildly-varying C&C addresses with the MOX bit effectively random — the
strict per-packet-type length checks now in `handle_command()` (modeled
on piHPSDR's `hpsdrsim.c`, a mature reference for this side of the
protocol) fixed that. Two more surfaced even with strict lengths in
place: reading MOX from every C&C address (matching `hpsdrsim.c`
literally) still picked up unrelated register data as spurious PTT
activity, fixed by only acting on it from address 0; and comparing the
network's want-TX bit against the shared `in_tx` (which the local key
in `cw.c` also writes) let an unchanged, already-stale network value
look like a fresh request the instant the local key's hang timer
released TX, latching TX on permanently after a single key press. Fixed
with `net_mox` — a variable tracking only the network's own last-seen
MOX bit, independent of `in_tx` — so only a genuine *change* in what
the network is sending can trigger an action, never `in_tx` moving for
an unrelated (local-key) reason.

**Outbound (I/Q):** `hpsdr_send_iq()` (called once per audio-thread
block from `sound_process()`) only ever appends to a lock-free
single-producer/single-consumer ring buffer and returns immediately - a
dedicated pacer thread (`hpsdr_pacer_thread()`) is the sole consumer,
draining it and sending one packet every 1.3125ms (126 samples @
96kHz), paced against an absolute wake time so the long-run rate never
drifts. This replaced an earlier version that packetized and sent
inline, directly from the audio thread, in one tight loop per block -
which fired a whole block's ~9 packets back-to-back within ~1ms, then
sent nothing for the remaining ~10ms until the next block, a bursty
pattern real HPSDR hardware never produces. SDR Console tolerated it;
SparkSDR's jitter buffer apparently did not (the leading suspect for a
~170ms audio warble reported against it specifically - same I/Q
content either way, only its arrival timing differed from what real
hardware would produce). Two earlier fix attempts were tried and
reverted before landing on the ring buffer: `usleep()` between packets
directly on the audio thread caused immediate, continuous xruns (a
capture period is real wall-clock time already fully spent; sleeping
on top of it is a permanent rate deficit, not absorbable jitter, and
the capture ring buffer overran within about 6 iterations); a
mutex-protected queue reproduced the identical xrun symptom for a
different reason - a plain mutex shared between the audio thread's
`SCHED_FIFO` max priority (see
[`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md))
and this file's ordinary-priority pacer is a textbook priority-inversion
trap, since the default Linux pthread mutex has no priority-inheritance
protocol. The lock-free design avoids blocking either side by
construction: the producer always advances (dropping a sample rather
than ever waiting on the consumer), and the consumer just drains
whatever's available each tick. I and Q values are scaled up before
sending to make SDR apps happier. This file has no dependency on
`usb_gadget.c` — each holds its own independent copy of the I/Q.

## USB Audio Class (UAC2) output

`usb_gadget.c` presents the radio as a standard USB Audio Class 2.0
capture device, if the hardware/kernel support it (needs a USB
device-mode controller and `libcomposite`) - see
[`usb_gadget_os_setup.md`](usb_gadget_os_setup.md) for the Raspberry Pi 4
config.txt/cmdline.txt changes and GPIO-power caveat this requires; it is
not on by default on a stock Raspberry Pi OS install. It's fed its own I/Q copy
directly from `sound.c`, with no ALSA/gadget dependency on
`hpsdr_p1.c`. Ported near-verbatim from the UAC2 section of sbitx's
`hpsdr_p1.c`, since that code had no sBitx/GTK dependency of its own —
only ALSA and Linux configfs/sysfs — making the port mechanical.

The gadget advertises 48kHz, but `sound.c` runs natively at 96kHz -
`sound_process()` runs I/Q through a real decimating lowpass
(`decim48k.c`) before handing it to `uac_push_iq()`, so what the gadget
delivers actually matches what it advertises. See
[`dsp_design_notes/usb_uac_decimation_design.md`](dsp_design_notes/usb_uac_decimation_design.md)
for the filter design and the real UAC2 host (a panadapter project) that
motivated getting this right. `hpsdr_p1.c` is unaffected - it still gets
native 96kHz I/Q.

Either stream works without the other: a client connected over USB audio
alone, with no HPSDR app connected, still gets I/Q, and vice versa. None
of `uac_init()`, `cat_init()`, or `hamlib_init()` failing is treated as
fatal at startup — minibitx keeps running on whatever subset of
control/streaming surfaces came up successfully; see
[`05_process_and_threading_model.md`](05_process_and_threading_model.md).
