# 08 — troubleshooting and bring-up

Status: has real content now (audio thread xruns, below) - no longer a
candidate to fold back into
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).

## Scope

Hardware bring-up gotchas that don't belong in the design docs proper:

- The si5351's I2C bus number is Linux-assigned (currently 22 via the
  `i2c-rtc-gpio` overlay), not a fixed hardware address — re-check with
  `i2cdetect -l` if the si5351 ever stops responding after an OS/kernel
  update, per the note in
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).
- Board-revision detection (`radio_hw_detect_version()`) and what to
  check if it misidentifies DE vs. v2 hardware.
- Anything else discovered during bring-up on real hardware that would
  otherwise get rediscovered the hard way a second time.

## Audio thread xruns (hw:0,0 capture/playback)

`sound.c`'s audio thread requests `SCHED_FIFO` at the max priority when
it starts (`sound_thread_start()`), matching a fix zbitx's own
`sbitx_sound.c` needed for the same underlying reason: as an ordinary
`SCHED_OTHER` thread, it competes with everything else on the system
and can be preempted long enough to miss an ALSA period. Requested via
`pthread_attr_t` so `pthread_create()` itself fails fast (typically
`EPERM`) if the privilege isn't available, rather than silently falling
back to normal scheduling well after `main()`'s "ready to serve!" line
has already printed. Not fatal if it fails (no root / no
`CAP_SYS_NICE` / no rtprio limit) — it warns once, synchronously, and
retries with default scheduling.

Two distinct xrun failure modes have shown up on real hardware, needing
different fixes:

- **A genuinely wedged device.** `xrun_recover()`'s bare
  `snd_pcm_prepare()` is the standard one-shot fix for a fresh
  `-EPIPE`, but on a device stuck for a more persistent reason,
  `prepare()` itself can fail — `snd_pcm_drop()` (discard whatever's in
  the ring buffer, rather than assume `prepare()` already put the
  device in a clean state) then `prepare()` again is a heavier fallback
  some ALSA drivers need to actually clear a stuck xrun. Both capture
  and playback call sites must check this function's return value and
  stop retrying if it's still negative — a caller that ignores a
  continued failure spins forever, printing "sound: xrun, recovering"
  at the full audio-loop rate with no backoff (a real bug: the playback
  path used to do exactly this, silently discarding the return value
  unlike the capture path beside it — fixed by disabling playback/CW
  sidetone output gracefully instead, with one clear message, rather
  than spinning).
- **No real-time scheduling privilege.** Reported symptom: an immediate
  xrun flood on startup with no HPSDR consumer even connected, right
  after a `"failed to set audio thread to SCHED_FIFO"` warning. Here
  every individual recovery genuinely succeeds — the device isn't
  stuck — but an ordinary-priority thread just isn't scheduled promptly
  enough to feed the next period before the one after that underruns
  too, forever, at the full ~93Hz loop rate. `xrun_note()`'s flood
  tracker rate-limits the logging, prints a one-time hint pointing at
  the real cause (grant `CAP_SYS_NICE`, e.g. `sudo setcap
  cap_sys_nice+ep ./minibitx`, or raise the rtprio limit), and adds a
  short breather so the retry loop doesn't itself worsen the CPU
  contention causing it.

**Playback must be fed continuously, not just during a CW burst.**
ALSA's underrun detection is tied to the hardware clock draining the
ring buffer against the software pointer, not to whether `writei()` is
being called — so only writing during an active CW burst let the
device sit with nothing arriving for however long the key was up,
draining its ~43ms buffer and underrunning between every single burst.
The first write of the *next* burst would then hit that stale underrun
and need recovery — exactly the "xrun, recovering" storm previously
seen on every key-down. Writing silence the rest of the time keeps the
device continuously running, the same design real sbitx's own
full-duplex audio path uses.
