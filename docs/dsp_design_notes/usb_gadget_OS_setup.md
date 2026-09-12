# USB Gadget (UAC2 + CDC-ACM) OS Setup — Raspberry Pi 4, USB-C Port

Status: §3-§11 are all bench-confirmed end-to-end on real hardware
(2026-09, Pi 4, Raspberry Pi OS Trixie, Windows 11 host) - the gadget
binds cleanly (including on repeat restarts) with no xrun flood, either
with or without the USB-C cable attached; enumerates cleanly over a
USB-A host port (§10); and WSJT-X now opens "sBitx IQ" directly and
decodes FT8 correctly (§9, §11) - the last open question (whether audio
data actually reached the host at all, not just correct enumeration) is
resolved. §13 adds a second gadget function (CDC-ACM, for CAT control)
alongside the UAC2 one covered everywhere else in this document. §14
documents two related kernel-side shutdown hangs this combination
bench-confirmed on 2026-09 (both only when no USB host was ever
connected) and the workarounds applied in `usb_gadget.c` for each -
**both re-confirmed fixed** on a subsequent bench pass: repeated clean
shutdowns with no host ever connected, and a clean shutdown with WSJT-X
actually connected over USB (confirming the fixes didn't disturb the
already-working host-attached path either). FLRig/CAT itself is still
unverified against real FLRig traffic - §13's checklist still applies
for that specific piece.

## 1. Background

`usb_gadget.c` (see
[`04_remote_control_and_iq_output.md`](../04_remote_control_and_iq_output.md))
presents minibitx as a USB Audio Class 2.0 capture device over configfs —
but only if the underlying OS/kernel already has a USB device-mode
controller (UDC) bound and ready. On a fresh Raspberry Pi OS install, that
port comes up in *host* mode by default (or doesn't come up as a gadget
controller at all), which is exactly what an unmodified boot log shows:

```
uac: cannot create gadget root /sys/kernel/config/usb_gadget/sbitx_iq: No such file or directory
init: USB IQ gadget unavailable, continuing without it
```

`usb_gadget.c` already treats this as non-fatal (minibitx keeps running
over HPSDR/UDP regardless — see
[`05_process_and_threading_model.md`](../05_process_and_threading_model.md)),
but the steps below get the UAC2 gadget itself actually working.

This uses the Pi 4's **USB-C port** specifically — the board's four USB-A
ports are driven by a separate, host-only chip (VL805) and are completely
unaffected by anything in this document. Keyboard, mouse, flash drives,
etc. on those ports keep working exactly as before.

## 2. The Pi 4 quirk: peripheral mode has to be forced

The Pi 4's USB-C port genuinely does support USB device/peripheral (OTG)
mode for data - this isn't a power-only port, contrary to some old forum
claims. The catch: Pi 4 (and 5) don't have the `OTG_ID` sense line wired
on that port the way the Pi Zero's micro-USB port does, so the controller
can't auto-detect which role (host vs. peripheral) to take on its own.
`dr_mode` has to be set explicitly, or it won't reliably come up as a
gadget at all.

## 3. Config changes

**`/boot/firmware/config.txt`** (this moved from `/boot/config.txt` a
couple of Raspberry Pi OS releases back - Trixie keeps it at the
`firmware` path; check both if `dtoverlay` changes don't seem to take).
Add:

```
dtoverlay=dwc2,dr_mode=peripheral
```

**`/boot/firmware/cmdline.txt`** - this file must stay exactly **one
line**. Append (space-separated, no newline):

```
modules-load=dwc2
```

This gets `dwc2` loaded early enough in boot to register as a UDC before
anything (like minibitx) needs it.

**`/etc/modules-load.d/minibitx-usb-gadget.conf`** - create this new
file containing just this line:

```
libcomposite
```

`dwc2` alone isn't enough: `usb_gadget.c` also needs `libcomposite`
(registers the `/sys/kernel/config/usb_gadget/` configfs subsystem - its
absence is exactly the "No such file or directory" in the log above).
`snd-aloop` is **not** needed - an earlier version of this document and
of `usb_gadget.c` assumed the UAC2 function read its data from an
`snd-aloop` loopback pair; it doesn't. See §11 for what actually feeds
the gadget's audio (its own kernel-created `UAC2Gadget` ALSA card) and
why the loopback module was never actually load-bearing here.

## 4. Power: the part most people trip over

Once the USB-C port is forced into peripheral mode, it can't simultaneously
power the Pi - it becomes a pure data link to whatever host it's plugged
into. **Power the Pi through the GPIO header's 5V/GND pins (or a HAT)
instead of through USB-C from here on.** This is Raspberry Pi's own
recommended setup for Pi 4 gadget mode, not a workaround - drawing power
over the same cable while also using it for data depends on a "low-power
USB mode" bootloader feature that's reported as unreliable across
different host systems, so don't rely on it.

## 5. Reboot and verify

Before even launching minibitx:

```
ls /sys/kernel/config/usb_gadget/     # should exist (even empty) - libcomposite loaded
ls /sys/class/udc/                    # should show one entry - dwc2 came up as a peripheral UDC
```

The first command is expected to print **nothing** on success - an empty
directory listing means `/sys/kernel/config/usb_gadget/` exists (which is
the whole check), it's just empty until minibitx creates its `sbitx_iq`
subtree there. An actual failure looks like `ls: cannot access
'/sys/kernel/config/usb_gadget/': No such file or directory` instead.
Don't try to confirm a module loaded by typing its name as a command
(e.g. typing `libcomposite` at the prompt) - module names aren't
programs, that'll just get you `command not found`; use `lsmod | grep
libcomposite` if you want to check directly instead of relying on the
two commands above.

There's no `snd-aloop`/`Loopback` card to check for at this stage (see
§11) - the card that actually matters here (`UAC2Gadget`) only appears
once minibitx has bound the gadget to the UDC, so `cat /proc/asound/cards`
is a §9/§11-time check, not a pre-flight one.

If `/sys/class/udc/` is empty, the overlay didn't take (recheck
`config.txt` and that you edited the `firmware` path) or something else
claimed the controller.

## 6. Permission denied creating the gadget root

Even with everything in §5 checking out (UDC present, `usb_gadget/`
directory existing, `Loopback` card present), minibitx's own attempt to
create its gadget still fails the first time, just with a different
error than before:

```
uac: cannot create gadget root /sys/kernel/config/usb_gadget/sbitx_iq: Permission denied
```

This is expected and is not a setup mistake - it's a different problem
than §1-5 fixed. `/sys/kernel/config/usb_gadget/` is root-owned
(0755 root:root), and minibitx runs as an ordinary user (`pi`), not
root - so `mkdir()` under it is refused by normal Unix file permissions,
same category of problem `sound_thread_start()`'s SCHED_FIFO capability
already solves for real-time scheduling (see the Makefile).

Fix: grant the binary `cap_dac_override` alongside the `cap_sys_nice` it
already has - this capability lets a process bypass ordinary
read/write/execute permission checks, which is exactly what's needed to
mkdir/write under a root-owned configfs tree without running minibitx as
root outright:

```
sudo setcap cap_sys_nice,cap_dac_override+ep ./minibitx
```

`Makefile` already reapplies this after every rebuild (see its
`minibitx:` recipe) - if you're building on the Pi and pulled this repo
after that Makefile change, a plain `make` picks this up automatically
and no manual `setcap` call is needed. If you're not using the Makefile
to build, run the `setcap` line above by hand after each rebuild.

This is broader than the narrowest possible fix (a udev rule chowning
just `/sys/kernel/config/usb_gadget/` to a group `pi` belongs to would
be more surgical, but is more fragile to get the boot-ordering right
against `systemd-modules-load.service`) - on a single-purpose radio
appliance like this, granting the one trusted binary this capability is
a reasonable trade, consistent with how the SCHED_FIFO capability is
already handled.

## 7. Real-hardware xrun flood with the gadget enabled but idle

Bench-confirmed 2026-09: once §6's permission fix lets the gadget bind
successfully, starting minibitx with **no USB host actually draining
"sBitx IQ" yet** (not plugged into a host, or plugged in but no app has
opened it for capture) reproduces sound.c's real hardware xrun flood on
hw:0,0 - the same symptom the earlier SCHED_FIFO/priority work fixed,
but from a completely different, new cause. Confirm this isn't the old
problem recurring by checking the startup log doesn't show the "failed
to set audio thread to SCHED_FIFO" warning - if it's absent (real-time
scheduling is working), this section's fix is the relevant one instead.

Root cause: the previous `usb_gadget.c` wrote to the ALSA loopback PCM
synchronously, inline, from `uac_push_iq()` - which runs on sound.c's
real-time audio thread. If nothing reads the other side of that
loopback pair, its ring buffer fills within a few blocks and every
subsequent write there fails, triggering a recovery attempt - real
kernel work, paid for on the *radio's own* real-time audio thread, every
~10.7ms, forever. That's enough added latency for the audio thread to
miss its own hw:0,0 deadlines - i.e. an idle, unrelated USB link could
starve the actual radio hardware. This is fixed in `usb_gadget.c`: the
ALSA write moved to its own dedicated `uac_writer_thread()`, decoupled
from the audio thread by a lock-free ring buffer (the same architecture
`hpsdr_p1.c`'s IQ pacer thread already uses, and for the same reason -
see that file's own history for the fuller derivation of why a real-time
producer thread can never be allowed to block on a downstream
consumer's pace). No OS-level configuration change was needed for this
one - it was a code fix.

Confirmed fixed: minibitx now starts clean, no xrun flood, both with the
USB-C cable disconnected and with it connected to a host.

## 8. UDC still busy on the second start

Bench-confirmed 2026-09: the gadget binds cleanly the *first* time
minibitx runs after a reboot, but a subsequent start (Ctrl+C and
re-run, a crash, or a service manager restarting it) fails instead:

```
uac: cannot bind to UDC 'fe980000.usb': Device or resource busy
init: USB IQ gadget unavailable, continuing without it
```

Root cause: `minibitx.c`'s `main()` has no signal handler at all - it's
just `while (1) sleep(1);` - so the *only* way the process ever ends is
being killed out from under it. `uac_stop()` (which unbinds the gadget
from the UDC and tears down the configfs tree) never runs in that case.
The gadget directory and its UDC binding are configfs/kernel state, not
process state - they simply persist, still bound to the now-dead
process's gadget instance. The kernel refuses to write a UDC name into
an already-bound gadget's `UDC` file (`EBUSY`), even to rebind the exact
same UDC to what is, from configfs's point of view, a brand new attempt
- so every restart after the first fails this way, forever, until
something explicitly unbinds it.

Fixed in `usb_gadget.c`: `uac_gadget_create()` now checks the leftover
gadget's `UDC` file *before* trying to (re)bind - if it already names a
controller, that means a previous run left it bound, so it's unbound
first before reconfiguring and rebinding. This is self-healing
regardless of *how* the previous process ended - a clean exit, Ctrl+C, a
crash, or `SIGKILL` all leave the same on-disk state, and this check
repairs all of them the same way, rather than depending on catching
every possible termination signal - which matters because `SIGKILL` and
a crash genuinely can't be caught, so this backstop is the only thing
that covers those two cases regardless of anything else.

**A second bug hid inside the first fix's own unbind call**
(bench-confirmed 2026-09): the self-heal check above - and
`uac_gadget_destroy()`'s normal clean-shutdown unbind, which has the
same code - originally wrote a true empty string to `UDC`
(`uac_write_attr(path, "")`). `uac_write_attr()` writes exactly
`strlen(value)` bytes, and `strlen("") == 0`, so this was a genuine
0-byte `write(2)` call. That call "succeeds" (0 bytes requested, 0
bytes written), but a 0-length write does not reliably reach the kernel
gadget driver's `UDC` store callback at all - so the gadget stayed
bound, and the very next bind attempt still failed with `EBUSY`, on
every single restart, even though the log showed the self-heal message
firing and reporting no error. The universal shell idiom for this,
`echo "" > UDC`, actually writes **one byte** - the newline `echo`
appends - not zero; that one byte is what actually reaches the store
callback and triggers the real unbind. Fixed by writing `"\n"` instead
of `""` at both call sites (`uac_gadget_create()`'s self-heal check and
`uac_gadget_destroy()`). A third, smaller gap fixed alongside this:
`uac_gadget_up` (which gates whether `uac_stop()` calls
`uac_gadget_destroy()` at all) used to only get set after a *fully*
successful `uac_init()` - so a run whose own bind attempt failed left
nothing to clean it up on that same run's shutdown either, silently
pushing the problem onto the next start's self-heal check instead of
fixing it immediately. `uac_gadget_create()` now sets it itself, as soon
as it creates or finds a gadget directory, regardless of whether the
bind at the end succeeds.

If you're seeing the exact symptom this section originally described
(gadget "still bound" on every restart, never actually recovering) on a
binary built before this second fix, rebuild and just start minibitx
again - no manual `rmmod`/reboot should be needed, since the corrected
unbind write now actually reaches the kernel. A reboot is still the
fallback if it's ever wedged deeper than that (this fix addresses the
mechanism actually observed, not every conceivable failure mode).

Separately, `minibitx.c` now also installs a `SIGINT`/`SIGTERM` handler
(Ctrl+C and a normal `kill`/`systemctl stop`, respectively - not
`SIGKILL`) that runs an actual graceful shutdown - `sound_thread_stop()`,
`uac_stop()`, `hpsdr_stop()`, `hamlib_stop()`, and parking PTT/the T/R
relay low via `radio_set_tx(0)` - instead of the previous behavior of
just dying on the spot. That's a genuine improvement on top of the
self-healing fix above (a normal Ctrl+C exit now unbinds the gadget and
closes its ALSA handle itself, rather than relying on the *next* start
to clean up after it), but the self-healing check above is still what's
actually load-bearing for `SIGKILL`/crash recovery, since nothing in
userspace runs in either of those cases.

Confirmed fixed: minibitx now binds the gadget successfully on repeated
restarts, not just the first one after a reboot.

## 9. Confirming it works end-to-end

Once the above all check out, start minibitx and look for:

```
init: USB IQ gadget bound to UDC '<name>'
uac: UAC2Gadget ALSA PCM opened: hw:N,0 @ 48000 Hz, 24-bit, 2 ch
uac: USB IQ audio stream ready — device name: 'sBitx IQ'
```

instead of the "gadget unavailable" line from §1, and no xrun flood on
hw:0,0 either with or without a host attached (§7). Plug the USB-C cable
into a host computer and a UAC2 audio-capture device named "sBitx IQ"
should enumerate there; any app that can record from a standard USB audio
input (SDR#, HDSDR, GQRX, SDR Console, or a plain audio recorder as a
first smoke test) should be able to pull the 24-bit/48kHz stereo I/Q
stream (L = I, R = Q).

**Bench-confirmed end-to-end, 2026-09** (Pi 4, Windows 11 host, USB-A
port per §10): WSJT-X selected the "sBitx IQ" capture device directly
and decoded FT8 correctly - real signal, real decode, not just a
non-zero meter. This confirms the whole chain at once: §11's fix
(writing to the real `UAC2Gadget` card instead of the dead-end
`snd-aloop` loopback) actually reaches the host, the host correctly
activates the streaming interface once an app opens it, and the
24-bit/48kHz I/Q stream `decim48k.c` produces is clean enough for a real
decoder to lock onto. The clock-drift concern the diyAudio
RPi4-OTG-audio community thread raised for USB gadget audio didn't turn
out to be a problem here, at least not at FT8's tolerances.

## 10. Windows: "Unknown USB Device (Device Descriptor Request Failed)"

Bench-observed 2026-09: Windows 11 Device Manager shows this under
"Universal Serial Bus controllers" the moment the Pi's USB-C cable is
plugged in, with minibitx's own console showing the gadget bound
successfully (§5-§9 all otherwise checking out).

This is **not a UAC2/audio problem, and almost certainly not a
`usb_gadget.c` configuration problem either** - "Device Descriptor
Request Failed" happens during the very first step of USB enumeration
(the host's initial partial `GET_DESCRIPTOR(DEVICE)` control request),
before Windows has read the configuration descriptor, before it's
picked a class driver, and long before anything UAC2-specific (like the
Clock Source Entity requirement `usb_uac_decimation_design.md` and
earlier discussion here worried about) would even matter. Everything
`uac_gadget_create()` writes under configfs - `idVendor`, the UAC2
function's rate/size/channel settings, the strings - lives at a higher
protocol layer than this failure. So this points at the physical/
electrical link, not application-level configuration: the cable, the
host's USB port, or power.

Prioritized things to try, cheapest first:

1. **Swap the cable.** This is the single most commonly reported fix
   for this exact symptom on Pi 4 gadget-mode setups. The Pi 4 has a
   well-documented USB-C hardware defect (a single shared pull-down
   resistor across both CC lines, instead of the one-per-line the USB-C
   spec calls for) that causes some cables - particularly "smart"/
   electronically-marked ones, Apple's own USB-C cables among them - to
   misbehave against the Pi's port. Community reports for gadget mode
   specifically point to plain USB-C-to-A cables (AmazonBasics USB-C
   2.0 and Anker Powerline are the two named in the thread that matches
   this symptom most closely) as reliably working, and a marked/
   high-wattage-charging cable as reliably not. Since this project's
   setup already powers the Pi via GPIO rather than the USB-C cable
   (§4), a plain USB-C-to-A data cable is the right kind to use here
   regardless - there's no power-delivery role for this cable to play
   at all.
2. **Try a different USB port on the Windows PC** - ideally a USB 2.0
   port wired directly to the motherboard, not a USB 3.x port, not a
   hub, not a front-panel/extension header. dwc2 here is a plain USB
   2.0 High-Speed controller; eliminating hubs and extension cables
   removes several variables that have independently been reported to
   cause exactly this failure with unrelated USB 2.0 gadgets.
3. **Confirm the Pi's own power is solid** - GPIO 5V/GND (§4), not the
   USB-C cable, and not a marginal supply. A brownout at the exact
   moment dwc2 tries to respond to the host's enumeration request could
   plausibly produce this same failure.
4. **Watch `dmesg -w` on the Pi itself** while plugging into the
   Windows PC, to see whether dwc2 logs anything at all at that moment
   (a reset detected, a speed negotiated, an error). Nothing logged on
   the Pi side despite Windows showing the failed-descriptor device
   points even more strongly at the cable/port than at anything
   softwareconfigurable here.
5. **If another host is available** (a Linux or Mac machine, even
   briefly), try enumerating there instead. If it enumerates cleanly
   elsewhere, this is Windows/port/cable-specific, not a fundamental
   problem with the gadget itself; if it fails everywhere, that points
   harder at the cable or the Pi's own port/power.

Bench-confirmed 2026-09: item 2 resolved this - switching to a USB-A
port on the Windows PC (the cable itself was already a plain USB-C-to-A
type) got past this specific failure. A "USB Composite Device" then
enumerates cleanly in Device Manager with no warning icon and
`SET_CONFIGURATION` accepted - see §11 for what was found once past this
point.

## 11. Enumerates fine, but silence: the IQ data never reached the gadget

Bench-observed 2026-09, once §10's Windows enumeration problem was
resolved: a full USB Device Tree Viewer descriptor dump showed a
completely correct UAC2 descriptor set (IAD grouping all three
interfaces, both Clock Source Units, Terminals, Feature Units, Format
Type I descriptors, matching Other-Speed-Configuration/Device-Qualifier
descriptors, all string descriptors present) and `Current Config Value:
0x01` - Windows had accepted the configuration. Opening the
corresponding input in Audacity showed **0 amplitude even tuned to a
strong FT8 signal**, with minibitx's own console reporting everything
normal (gadget bound, PCM opened, HPSDR streaming fine to a separate
client).

Root cause: `uac_alsa_open()` was writing IQ samples into an `snd-aloop`
"Loopback" card, not into the UAC2 gadget's own ALSA card. Binding a
UAC2 function via configfs makes the kernel's `u_audio`/`f_uac2` driver
register its **own independent ALSA sound card** for that function -
confirmed via `cat /proc/asound/cards` on the Pi with the gadget bound:

```
 0 [audioinjectorpi]: audioinjector-p - audioinjector-pi-soundcard
 1 [Loopback       ]: Loopback - Loopback
 2 [Loopback_1     ]: Loopback - Loopback
 3 [UAC2Gadget     ]: UAC2_Gadget - UAC2_Gadget
```

Card 3, `UAC2Gadget`, is the one actually wired to the real USB
isochronous endpoint the host reads. Cards 1 and 2 are `snd-aloop`'s
usual pair, entirely self-contained - writing into one side only ever
reaches the *other side of that same virtual pair*, never anything
outside it. `usb_gadget.c`'s original design (and this document's
original §3/§5, and `usb_gadget.h`'s architecture comment) assumed the
UAC2 function itself read its outbound audio from that loopback pair
automatically. It doesn't, and never did - there is no such linkage
built into `f_uac2`/`libcomposite`, and nothing in this project ever ran
a bridging daemon (`alsaloop` or similar) to actually connect the two
cards. Every IQ sample written since UAC2 support was first added had
been going into a dead end: consumed only by the other end of the
loopback pair, which nothing ever read.

This bug was invisible at every layer checked before this point:
minibitx's own console reported success (the ALSA write to the loopback
card genuinely does succeed), and the host's USB enumeration/
configuration is governed entirely by the configfs descriptors
`uac_gadget_create()` writes - completely independent of what, if
anything, is feeding the PCM behind them. Silence in an actual recording
app was the only symptom, which is why it only surfaced once real
end-to-end testing (§9) was finally attempted.

Fixed in `usb_gadget.c`: `uac_alsa_open()` now locates the `UAC2Gadget`
card directly (by its `/proc/asound/cardN/id`, which is driver-assigned
and stable regardless of this project's own `sbitx_iq` configfs instance
name) and opens its device-0 playback PCM (`hw:N,0`) instead of any
`snd-aloop` device. `snd-aloop` is no longer a dependency of this
project at all - removed from §3's `modules-load.d` file and from
`usb_gadget.h`'s dependency list.

**Bench-observed immediately after this fix**: with the real
`UAC2Gadget` card wired up, `uac_writer_thread()`'s `snd_pcm_writei()`
calls started failing with `Input/output error` (`-EIO`) - continuously,
one attempt per ~10.7ms period, forever - as soon as minibitx started,
well before any host was attached. This part is expected, not a
regression: unlike the old `snd-aloop` path (which silently buffered
writes with nothing ever reading them - the bug this section fixes), the
real gadget PCM correctly refuses writes outright whenever no USB host
has actually activated the capture streaming interface (cable
unplugged, or plugged in but no app has opened the stream for reading
yet) - the kernel can't queue endpoint requests in that state.

Retrying at full ~10.7ms pace forever in that state was its own bug,
though, and directly at odds with this project's standing design: the
USB gadget has always been meant to be fully optional - minibitx runs
fine with no cable attached at all (`usb_gadget.h`, §1 above) - so
paying for a real ioctl round-trip a hundred times a second, forever,
just because nothing is plugged in isn't "optional," it's a permanent
low-grade cost hiding behind that word. Same lesson as §7's xrun-flood
fix and `hpsdr_p1.c`'s IQ pacer thread, just surfacing a third time in a
new spot. Fixed in `uac_writer_thread()`: the retry pace itself now
backs off - ramping from one period up to a ~1s cap as the failure
streak grows - and the log line only fires on the *transition* into and
out of "no host draining," not on every retry, so a permanently
unplugged gadget costs one line at startup and then nothing further, not
a line every second forever either.

**Re-verified end-to-end, 2026-09**: WSJT-X on Windows opened "sBitx IQ"
and decoded FT8 correctly - see §9 for the full confirmation. The "no
USB host draining" transition logged once at startup and cleared on its
own (logged once more) as soon as WSJT-X opened the capture stream, with
no further console noise after that.

Two more subtleties went into making that transition log trustworthy,
both in `uac_writer_thread()`: it starts out assuming "no host draining"
rather than the reverse, because this thread's very first writes happen
before any host has had a chance to attach at all - an optimistic start
would have logged a spurious "draining" transition on essentially every
cold boot with no cable connected, the normal, fully-supported way to
run this daemon. And a single successful write (or a short run of them)
right after opening a fresh PCM isn't trusted as proof a host is
actually draining it either - the gadget's ring buffer is `UAC_PERIODS`
periods deep, so that many writes can succeed purely because there's
room in an empty buffer, even with nothing reading the other end
(bench-observed: a fresh bind with no USB cable attached still reported
one successful write before the real, sustained failure). Only a run of
successes longer than the buffer could have absorbed for free is treated
as real evidence of a host.

## 12. A single harmless Windows toast right after a Pi reboot - before minibitx even runs

Bench-observed 2026-09: a "USB device not recognized" toast appears on
the Windows host right around when the Pi finishes booting, with the
USB-C cable already connected - but minibitx is started by hand over
SSH *after* boot, so this happens before any gadget has been created at
all, and before minibitx exists as a running process. It's never seen
again afterward, including across repeated minibitx restarts on the
same boot - once minibitx actually binds its gadget, everything
enumerates and works cleanly (§9).

This is a different symptom from §10's "Device Descriptor Request
Failed," both in wording and in timing, and shouldn't be conflated with
it: §10 was about minibitx's own gadget failing to enumerate once
bound; this happens with nothing bound yet at all. Since `cmdline.txt`
only loads the bare `dwc2` module early in boot (§3) - no `g_serial`,
`g_ether`, or any other auto-attaching gadget function is configured to
grab the UDC on its own - there's no phantom gadget this project's setup
is presenting during that window. The likely explanation sits entirely
inside the kernel's own `dwc2` driver probe: forcing the controller into
peripheral mode (`dr_mode=peripheral`) means it does its own PHY
bring-up/reset as it registers itself as a UDC, and `dwc2` has a known
history of occasionally producing a brief, spurious connect-like
transition during that init, with nothing bound yet to answer with real
descriptors - Windows sees something appear, gets nothing coherent
back, and reports it.

Not something to chase in this codebase: it happens before minibitx is
even running, so there's nothing in `usb_gadget.c` (or anywhere else in
this project) that could be causing or fixing it. One harmless toast per
reboot, no functional impact - if it's ever worth confirming precisely,
the next step would be `dmesg -w` spanning the boot itself (not just
minibitx's later startup) to check whether `dwc2`'s probe logs a
reset/connect event at the moment the toast appears - not pursued
further as of this writing.

## 13. Adding CDC-ACM for CAT control (FLRig) - untested combination, check this first

As of this writing, `usb_gadget.c` creates a second function in the same
gadget alongside `uac2.0`: `acm.usb0`, a standard CDC-ACM serial port,
bound into the same `configs/c.1`. The CAT control section near the
bottom of that same file (see
[`04_remote_control_and_iq_output.md`](../04_remote_control_and_iq_output.md))
opens the resulting `/dev/ttyGS0` on the Pi side and answers Kenwood
TS-480-subset CAT commands on it - the intended use is FLRig on Windows
opening whatever COM port Windows assigns the gadget, with no bridge
software of any kind.

**Why this needs its own bench pass, not just an assumption that it
inherits §9-§11's success:** every confirmation earlier in this document
was of a single-function (UAC2-only) gadget. Two things are genuinely
new with a second function present:

- **Composite descriptor correctness.** Two functions in one
  configuration means the kernel's gadget framework has to emit a
  correct Interface Association Descriptor grouping for the ACM pair
  (control + data interface) alongside UAC2's own interface(s), and
  Windows has to walk that composite descriptor correctly to bind
  *both* the audio class driver AND `usbser.sys` to their respective
  interfaces. This is exactly the kind of thing that's fine on paper
  (per the kernel gadget-testing docs and Microsoft's own `usbser.sys`
  documentation, both consulted while building this) but is worth
  seeing actually enumerate before trusting it - the UAC2-only case
  took real bench iteration (§9, §11) to get right despite looking
  correct at every earlier check too.
- **No custom driver should be needed**, but confirm it: Windows 10/11
  auto-binds the inbox `usbser.sys` driver to any interface presenting
  standard `bInterfaceClass=0x02`/`bInterfaceSubClass=0x02` (CDC ACM),
  the same "just works" story UAC2 already gets for audio class
  devices - if Device Manager instead shows an unknown/unrecognized
  device for the second interface, that's the first thing to check
  (Device Manager's own descriptor view, or a USB analyzer, would show
  whether the IAD grouping came out wrong).

**What to check on the bench, in order:**

1. Console log at startup should now read
   `init: USB gadget bound to UDC '<name>' (UAC2 audio + ACM CAT)`
   followed by `init: CAT (Kenwood TS-480 subset) listening on /dev/ttyGS0`
   - both functions bound, both minibitx-side threads up.
2. Windows Device Manager should show *both* a "sBitx IQ" audio device
   (as before) and a new COM port under "Ports (COM & LPT)" - if only
   one appears, that's the composite-descriptor question above.
3. Confirm the UAC2 audio path still works exactly as it did before
   this change (§9) - adding a function to the same gadget shouldn't
   affect it, but this is the one place a subtle interaction (e.g. USB
   bandwidth/endpoint numbering conflicts) would show up first.
4. Open the new COM port in a plain terminal (PuTTY, etc.) at any baud
   rate (CDC-ACM doesn't actually use the configured baud/framing for
   anything - it's carried over USB, not a real UART) and send `ID;` -
   expect `ID020;` back. Then `FA;` - expect the current frequency as
   `FA<11 digits>;`.
5. Only once 1-4 look right, try FLRig itself: set its rig to a
   Kenwood TS-480, point it at the new COM port, any baud rate. Confirm
   frequency reads correctly and `FA` from FLrig retunes minibitx.
6. The `IF;` response format (used for FLRig's combined status
   display) was reconstructed from the general Kenwood IF convention,
   not confirmed character-for-character against the QMX's own manual
   text - see the field-by-field comment on the `IF` handler in
   `usb_gadget.c`'s CAT section. If
   FLRig's status display looks wrong (frequency in the wrong place,
   mode misread) while `FA`/`MD`/`TQ` individually work fine via the
   terminal test in step 4, this is the first place to check - ideally
   against a packet capture of a real QMX's `IF` response.

None of this is a hard dependency for the UAC2/WSJT-X path already
confirmed working (§9, §11) - `cat_init()` failing, or FLRig never being
tested, doesn't affect audio streaming or Hamlib/WSJT-X control at all.

## 14. Shutdown hangs forever (unkillable) with no USB host ever connected - kernel bug, worked around

**Bench-confirmed 2026-09**, Pi 4, kernel `6.18.39+rpt-rpi-v8` (Debian
1:6.18.39-1+rpt1): starting minibitx with no USB cable connected at all,
then stopping it (Ctrl+C), reliably hung the process forever -
`ps -o pid,stat,wchan:32,comm -p <pid>` showed `STAT` as `Dl+`
(uninterruptible sleep), and it did not respond to `kill -9`. The only
way to recover was rebooting the Pi. `sudo dmesg` at the time of the
hang showed the kernel's own hung-task detector firing, with this stack:

```
INFO: task minibitx:<pid> blocked for more than 120 seconds.
task:minibitx state:D ...
Call trace:
 schedule+0x3c/0xf0
 gserial_free_port+0xcc/0x140 [u_serial]
 gserial_free_line+0x60/0xa0 [u_serial]
 acm_free_instance+0x24/0x48 [usb_f_acm]
 usb_put_function_instance+0x2c/0x50 [libcomposite]
 acm_attr_release+0x18/0x38 [usb_f_acm]
 config_item_cleanup+0x5c/0x90
 config_item_put+0x70/0xb0
 configfs_rmdir+0x210/0x340
 vfs_rmdir+0x94/0x210
 do_rmdir+0x158/0x1a0
 __arm64_sys_unlinkat+0xa4/0xe0
```

That identifies the exact call: `rmdir()` on the ACM function's own
configfs directory (`functions/acm.usb0`) - the step in
`uac_gadget_destroy()` that asks the kernel to actually free the
underlying `gserial`/`/dev/ttyGS0` port. `gserial_free_port()` itself is
what's stuck in `schedule()` - a kernel-side block inside
`u_serial.c`/`usb_f_acm.c` on this kernel build, most likely waiting on
USB transfer/endpoint state tied to the ACM data endpoints that never
resolves when no host ever enumerated to claim them. This is *not* our
own `/dev/ttyGS0` file descriptor still being open - the UDC unbind
write, both config-symlink `unlink()`s, and the `uac2.0` function's own
`rmdir()` all completed successfully before this call was even reached
(confirmed by the call stack itself: this is a later step in the same
function, and there's no earlier hang reported).

**Workaround applied in `usb_gadget.c`** (see the comment in
`uac_gadget_destroy()`): shutdown no longer calls `rmdir()` on
`functions/acm.usb0` at all. Everything else in that function's teardown
still runs (UDC unbind, both symlink removals, the `uac2.0` function's
own directory, the strings/config directories) - only that one call is
skipped. This is safe because configfs is entirely in-memory and doesn't
survive a reboot: never freeing this one function across a graceful
shutdown just means the kernel object (and its directory, and anything
still containing it - `functions/`, and the gadget root above that) stays
allocated, harmlessly, for the rest of that boot. `uac_gadget_create()`'s
own self-heal logic already tolerates a leftover directory tree (its
`mkdir()`/`symlink()` calls already treat `EEXIST` as success, for
exactly the "previous run didn't shut down cleanly" case this was built
for) - so restarting minibitx again on the same boot reuses the same
never-freed ACM function rather than recreating it, with no code changes
needed on the create side.

**What this means in practice:**

- A clean Ctrl+C shutdown with no host ever connected now completes and
  returns to the shell promptly - the actual bug this section exists to
  fix.
- `uac: gadget removed` no longer prints on shutdown (that line assumed a
  full teardown); it's replaced with `uac: gadget partially removed
  (UDC unbound, config detached; the ACM/CAT function's own directory is
  intentionally left in place until reboot...)` - expected, not an error.
- Restarting minibitx again on the same boot should work fine (the
  ACM function is reused, not recreated) - if you hit anything unexpected
  there (the new COM port not reappearing on the host side, for
  instance), that's worth its own bench report, since it would be new
  territory this workaround doesn't cover.
- If this was actually hit because a host WAS connected earlier in the
  session and then disconnected before shutdown, that's a meaningfully
  different scenario than the one bench-confirmed here (no host, ever)
  and worth its own report too - this workaround was written and tested
  against the "no host ever connected" case specifically.
- If a future Raspberry Pi OS kernel update fixes this upstream
  (`u_serial.c`'s `gserial_free_port()` blocking forever regardless of
  host presence looks like a genuine kernel bug, not intended behavior),
  this workaround could potentially be removed - re-enabling the
  `functions/acm.usb0` `rmdir()` and re-testing a no-host shutdown would
  be the way to check.

**Follow-on finding, same bench session:** the CAT reader thread
(`cat_thread_fn()`, in the CAT section of `usb_gadget.c`) briefly had its
lifecycle changed to `pthread_join()` on shutdown instead of
`pthread_detach()`, to guarantee it had genuinely exited (and released
its `/dev/ttyGS0` fd) before `uac_gadget_destroy()` ran. Bench-testing
that change - fresh reboot, no USB host ever connected, start minibitx,
Ctrl+C - reproduced the *same class* of unkillable shutdown hang, just
relocated: this time the process never even reached `uac: ALSA PCM
closed` (the first line `uac_stop()` prints, called *after* `cat_stop()`
in the shutdown sequence), meaning the hang moved into `cat_stop()`
itself - specifically, everything points at the newly-added
`pthread_join()` call, since the exact same `cat_stop()` (without a
join) demonstrably did *not* hang in the original bug report - that
trace ran the same close()-and-return sequence and continued on to
finish `uac_stop()`'s own teardown before hanging later, in
`uac_gadget_destroy()`.

The likely explanation is the same theme as the `rmdir()` bug above:
the reader thread's own close()/exit path apparently can also block
inside the same ACM/`u_serial.c` gadget machinery when no host was ever
connected. The fix was to revert `cat_stop()` back to *not* joining that
thread (see the comment on `cat_init()`/`cat_stop()`) - if that thread's
own close() ever hangs the same way, it now hangs alone, without taking
the rest of shutdown down with it. This does give up the ordering
guarantee the join was meant to provide (that the ACM tty is genuinely
released before `uac_gadget_destroy()` touches that function's configfs
tree) - but nothing downstream actually depends on that guarantee, since
this section's `rmdir()` workaround already avoids touching
`functions/acm.usb0` at all.

**Net effect of both fixes together, bench-re-confirmed 2026-09:**
repeated fresh-reboot/no-cable/Ctrl+C cycles now shut down cleanly and
promptly every time, printing `uac: gadget partially removed (...)` and
`minibitx: shutdown complete` rather than hanging - both the `rmdir()`
this section originally documented and the CAT thread's own teardown
are confirmed fixed, not just believed fixed. A shutdown with WSJT-X
actually connected over USB was also re-tested afterward and stays
clean, confirming neither fix disturbed the already-working
host-attached path (which never went through either hazardous code
path to begin with, so this was more a sanity check than an expected
risk).

If a *third* hang ever shows up somewhere else in this same shutdown
sequence, look for the same signature first (a print that should have
followed immediately never appears) before assuming something new and
unrelated - this exact subsystem has now produced two hangs from the
same underlying cause on this kernel build.
