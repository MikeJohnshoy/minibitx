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
  thread driving `sound_process()`, and the main thread's idle loop.
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
- Console reporting: no periodic status line any more.
  `status_print()` (`status.c`) still exists but isn't called from
  anywhere - it used to be a single call right after the init-complete
  line, removed since ongoing operational visibility already comes from
  the `rigctl:`/`hpsdr:` command echoes described in
  [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md),
  which report a freq/PTT change at the moment it happens rather than a
  point-in-time snapshot.
- Failure handling at startup: which subsystems are fatal if they fail
  to come up (GPIO, HPSDR socket bind, audio capture) versus
  which are best-effort and allowed to be absent (Hamlib/rigctld, the
  USB gadget, the INA260 power monitor).
