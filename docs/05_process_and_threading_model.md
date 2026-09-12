# 05 — process and threading model

Status: stub.

## Scope

- `main()`'s startup sequence in `minibitx.c` (GPIO/hardware init →
  oscillator/VFO/board-rev/INA260 → Hamlib → HPSDR → USB gadget →
  audio codec → audio thread) — see
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)
  for the first part of that sequence in detail. Each step logs its own
  `init: ...` result; the sequence ends with
  `minibitx: radio hardware initialization complete`. Hamlib starts
  before HPSDR specifically so startup is the exact reverse of the
  shutdown sequence below (sound → USB gadget → HPSDR → Hamlib) - the
  two have no dependency on each other either way, so this is for that
  symmetry, not because the other order was broken.
- The thread structure once running: the HPSDR poll/listener thread, the
  Hamlib accept thread plus one thread per connected client, the audio
  thread driving `sound_process()`, the USB gadget's UAC writer thread
  (see [`usb_gadget_os_setup.md`](usb_gadget_os_setup.md) §7), a
  dedicated TX worker thread (below), and the main thread's idle loop.
- **TX transitions run on their own worker thread** (`radio.c`'s
  `radio_tx_worker()`), not on whichever thread calls `radio_set_tx()`.
  This replaced an earlier version where `radio_set_tx()` did its
  PTT/relay-settling `usleep()`s and ALSA mixer call inline, on the
  caller's own thread — harmless from Hamlib's or `hpsdr_p1.c`'s network
  threads, but `cw.c` calls `radio_set_tx()` from `cw_poll_key()`, which
  runs once per ~10.7ms audio block on the real-time audio thread
  (`sound.c`'s `audio_loop()`). A single call there blocking 20ms+ (PTT
  settle + relay settle + opening/closing a fresh ALSA mixer handle)
  guaranteed a missed capture period — the `sound: xrun, recovering`
  logged on every key transition — and made the physical key feel
  sluggish, since `cw_poll_key()` couldn't return to re-poll it until
  the blocking sequence finished. Fix: `radio_set_tx()` keeps its exact
  signature and still updates `in_tx` immediately/synchronously (cheap —
  every other guard in the codebase that reads `in_tx`, e.g.
  `cw_tx_active()` and the network MOX logic, needs to see the new state
  right away, even though the physical relay/mixer change is still
  pending); the actual slow hardware sequence (`radio_tx_apply()`) is
  handed to the worker thread via a mutex/condvar/pending-flag pair, so
  the calling thread — audio thread included — never blocks.
- Graceful shutdown: `main()` installs a `SIGINT`/`SIGTERM` handler
  (Ctrl+C, or a normal `kill`/`systemctl stop` - not `SIGKILL`, which
  can't be caught) that sets a flag; the idle loop notices it, parks
  PTT/the T/R relay low (`radio_set_tx(0)`, in case the key was down at
  the moment of the signal), then tears down in the order above:
  `sound_thread_stop()` first (stops the real-time audio thread that
  feeds both `hpsdr_send_iq()` and `uac_push_iq()`, so neither consumer
  races a producer still calling into it), then `uac_stop()`,
  `hpsdr_stop()`, `hamlib_stop()`. See
  [`usb_gadget_os_setup.md`](usb_gadget_os_setup.md) §8 for the failure
  mode this fixed (a restart-without-rebooting used to leave the USB
  gadget's configfs tree bound to a dead process) and for the
  independent self-healing fix that still covers `SIGKILL`/a crash,
  neither of which this handler can catch.
- Console reporting: no periodic status line. `status.c`/`status.h` (a
  single-line, redraw-in-place frequency/TX-RX display, once called
  right after the init-complete line) were removed entirely - ongoing
  operational visibility comes from the `rigctl:`/`hpsdr:` command
  echoes described in
  [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md),
  which report a freq/PTT change at the moment it happens rather than a
  point-in-time snapshot.
- Failure handling at startup: which subsystems are fatal if they fail
  to come up (GPIO, HPSDR socket bind, audio capture) versus
  which are best-effort and allowed to be absent (Hamlib/rigctld, the
  USB gadget, the INA260 power monitor).
