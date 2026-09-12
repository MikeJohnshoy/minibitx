// usb_gadget.h
//
// USB gadget composite device for minibitx: Audio Class 2.0
// (UAC2) IQ streaming plus a CDC-ACM serial function for Kenwood
// TS-480-subset CAT control. Both live in usb_gadget.c - the CAT command
// handling is in its own clearly-marked section near the bottom of that
// file, rather than a separate translation unit, since both functions are
// just two halves of the one composite gadget this file already owns the
// whole lifecycle of.
//
// Ported from the UAC2 section Mike (KB2ML) added to sbitx's hpsdr_p1.c.
// That file also carried a much larger HPSDR Protocol 1 rewrite (half-band
// decimation, MOX debounce, TX IQ upsampling) built against sbitx's GTK app
// — tx_on()/tx_off()/cmd_exec() and GLib's g_idle_add()/g_timeout_add() for
// deferring T/R switches onto the GTK main thread. minibitx has none of
// that (no GTK, no GLib main loop, radio_set_tx()/radio_tune_to() instead),
// so only this self-contained piece came across. It doesn't touch HPSDR
// state, GTK, or any sBitx-specific control API — just ALSA and configfs —
// so it drops in as its own module.
//
// Provides a USB Audio Class 2.0 gadget device named "sBitx" that streams
// 24-bit / 48 kHz stereo IQ (I = left, Q = right) to any SDR application
// that can consume a USB audio input (e.g. SDR#, HDSDR, GQRX, SDR Console),
// and — as of the second function added below — a CDC-ACM serial port
// alongside it that control-surface software (FLRig chief among them) can
// talk Kenwood-style CAT commands to, without needing any TCP-to-serial
// bridge on the host side.
//
// Architecture overview:
//   The Linux USB gadget framework is configured via the configfs API under
//   /sys/kernel/config/usb_gadget/. Two functions are bound into one
//   composite gadget, both created/torn down together by
//   uac_gadget_create()/uac_gadget_destroy() in usb_gadget.c. Beyond
//   configfs plumbing, the ACM side's only other logic is the CAT section
//   near the bottom of usb_gadget.c, which is what actually reads/writes
//   the resulting /dev/ttyGS0:
//     - uac2.0: bcdADC = 0x0200 (Audio Class 2.0), one AudioStreaming
//       interface, 2-ch, 24-bit PCM, 48000 Hz
//     - acm.usb0: a standard CDC-ACM serial function (f_acm) — essentially
//       no configurable attributes, just a directory to create
//   Device/product strings: "sBitx" / "sBitx IQ". Once the gadget is bound
//   to a UDC controller (detected automatically), the host sees a standard
//   USB audio capture device called "sBitx" AND a standard USB serial port.
//   Windows 10/11 auto-binds its inbox usbser.sys driver to the ACM
//   interface with no custom INF needed, the same "just works" story UAC2
//   already gets for audio class devices (both bench-confirmed
//   individually; running the two functions together in one composite
//   gadget is new as of this writing — see docs/usb_gadget_os_setup.md for
//   the current bench-testing status).
//
//   Sample delivery:
//     Contrary to what this comment used to say - minibitx's audio thread
//     does NOT produce baseband IQ at 48kHz; sound.c runs the codec at
//     96kHz (SAMPLE_RATE in sound.c), same as sbitx. uac_push_iq() used to
//     be called with raw 96kHz-rate samples straight from sound_process(),
//     silently double-feeding the (48kHz-configured) gadget/ALSA loopback -
//     since the ring buffer between them only drains at the real 48kHz
//     rate, roughly half of all samples were being dropped once it filled,
//     with no filtering (not a clean decimation, just whichever samples
//     lost the race). Fixed: sound_process() now runs I/Q through
//     decim48k_apply() (decim48k.c) - a real 96kHz->48kHz decimating
//     lowpass, cascaded after antialias_apply() - before calling
//     uac_push_iq(), so the 48kHz this gadget advertises is what it
//     actually delivers. See
//     docs/dsp_design_notes/usb_uac_decimation_design.md for the filter
//     design and why a real UAC2 host (the QMX/Tab5 panadapter project,
//     tab5.lav.dk, motivated getting this right) makes this worth doing
//     properly rather than just relabeling the gadget as 96kHz.
//     uac_push_iq() is still called once per (now 48kHz-rate) sample from
//     sound_process(), the same place that hands hpsdr_send_iq()
//     (hpsdr_p1.c) its own native-96kHz copy - hpsdr_p1.c's rate is
//     unaffected by any of this. This runs independently of whether an
//     HPSDR client is connected — USB audio and the HPSDR UDP stream are
//     two separate consumers of the same IQ, and neither module has a
//     dependency on the other.
//
//   Producer/consumer split (see usb_gadget.c's uac_writer_thread()
//   comment for the full incident this fixed): uac_push_iq() only ever
//   enqueues into a lock-free ring buffer and never blocks - the same
//   architecture hpsdr_p1.c's IQ queue uses, and for the same reason.
//   A dedicated uac_writer_thread() is the sole consumer: it drains the
//   queue and performs the (potentially blocking, if no USB host is
//   actually draining the gadget yet) ALSA write on its own thread.
//   Without this split, a stalled or absent USB host could block
//   sound.c's real-time SCHED_FIFO audio thread and cause real hardware
//   xruns on hw:0,0 - a completely unrelated failure with no obvious
//   cause from the symptom alone. See
//   docs/usb_gadget_os_setup.md for how this was diagnosed on real
//   hardware.
//
//   ALSA delivery — direct to the gadget's own card:
//     Binding the UAC2 function to a UDC (uac_gadget_create()) makes the
//     kernel's u_audio/f_uac2 driver register its own independent ALSA
//     card for it, always reported with id "UAC2Gadget" in
//     /proc/asound/cards. uac_writer_thread() opens that card's own
//     device-0 playback PCM directly and writes to it with a standard
//     snd_pcm_writei() call - that PCM is what the kernel driver actually
//     streams out over the real USB isochronous endpoint the host reads
//     as its capture/recording stream.
//
//     An earlier version of this routed through a separate snd-aloop
//     "Loopback" card instead, on the mistaken assumption that the UAC2
//     function reads its capture-side data from that loopback pair
//     automatically. It doesn't - snd-aloop's pair is fully self-
//     contained, so every sample written there stayed local to the Pi
//     and never reached the USB link. That bug was invisible at every
//     enumeration/configuration layer (both minibitx's own console and
//     the host's USB descriptors looked entirely correct); the only
//     symptom was silence in a recording app on the host side, which is
//     what exposed it (bench-confirmed 2026-09; see
//     docs/usb_gadget_os_setup.md §11). snd-aloop is not used by this
//     file any more.
//
// Configfs gadget path layout (created by uac_gadget_create() in
// usb_gadget.c):
//   /sys/kernel/config/usb_gadget/sbitx_iq/
//     idVendor, idProduct, bcdUSB, bcdDevice
//     strings/0x409/manufacturer  = "sBitx"
//     strings/0x409/product       = "sBitx IQ"
//     strings/0x409/serialnumber  = "0000001"
//     configs/c.1/
//       strings/0x409/configuration = "Default"
//       bmAttributes, MaxPower
//       function symlinks -> functions/uac2.0/, functions/acm.usb0/
//     functions/uac2.0/
//       c_srate  = 48000
//       c_ssize  = 3        (3 bytes = 24-bit)
//       c_chmask = 3        (2 channels: L=I, R=Q)
//       p_srate  = 48000    (playback side, unused but must be set)
//       p_ssize  = 3
//       p_chmask = 3
//     functions/acm.usb0/
//       (no attributes set — port_num is read-only/assigned by the kernel)
//
// Dependencies (must be present on the target system):
//   Kernel modules : dwc2 (or other device-mode UDC), libcomposite
//   Userspace libs : libasound2-dev (ALSA — for PCM write to the
//                    UAC2Gadget card; minibitx already links -lasound
//                    for sound.c)
//   Kernel config  : CONFIG_USB_CONFIGFS_F_UAC2=y, CONFIG_USB_CONFIGFS_F_ACM=y
//
// Thread safety:
//   uac_init()/uac_stop() are meant to be called once, from main(), around
//   hpsdr_init()/hpsdr_poll(). uac_push_iq() runs on minibitx's audio
//   thread (same thread that calls hpsdr_send_iq()) and only ever touches
//   the lock-free queue - it never blocks and never touches the ALSA PCM
//   handle directly. uac_writer_thread() (internal to usb_gadget.c) is
//   the only thing that ever calls into ALSA on uac_pcm_handle - no lock
//   is needed between the two: the queue's atomic head/tail split
//   ownership exactly like hpsdr_p1.c's IQ queue (producer writes head
//   only, consumer writes tail only).

#ifndef USB_GADGET_H
#define USB_GADGET_H

// Configure the composite UAC2+ACM gadget via configfs and open the UAC2
// side's ALSA PCM. Call once, after hpsdr_init()/hpsdr_poll(). Returns 0 if
// both the gadget and the PCM are ready, -1 if either step fails (e.g. no
// UDC found) — not a hard failure for the rest of minibitx, which keeps
// running over HPSDR/UDP either way. The ACM function comes up as part of
// the same gadget bind; cat_init() (below) is what actually opens and
// uses the resulting /dev/ttyGS0, independently of this succeeding or
// failing.
int uac_init(void);

// Deliver one 48 kHz IQ sample pair, normalized to [-1.0, +1.0]. Enqueues
// into a lock-free ring buffer for uac_writer_thread() to drain and flush
// to the ALSA loopback one period at a time - never blocks; silently
// drops the sample if the queue is full (writer thread stalled behind a
// slow/absent USB host). No-op if uac_init() hasn't succeeded (or after
// uac_stop()).
void uac_push_iq(double i_val, double q_val);

// Tear down the gadget and release the ALSA PCM handle. Safe to call even
// if uac_init() was never called or failed.
void uac_stop(void);

// Returns 1 while the UAC2 stream is initialized and ready to accept
// samples, 0 otherwise.
int uac_is_active(void);

// Kenwood TS-480-subset CAT control, reached over this gadget's CDC-ACM
// (ttyGS) serial function - see the "CAT control" section at the bottom
// of usb_gadget.c for the command set and implementation, and
// docs/04_remote_control_and_iq_output.md for why TS-480 specifically.
//
// This exists for control surfaces that only know how to talk CAT to a
// real (or real-enough) Kenwood radio - FLRig chief among them, since it
// has no generic "connect to a Hamlib rigctld-style server" option the
// way WSJT-X does. WSJT-X keeps using the existing Hamlib NET rigctl
// server (hamlib.c) unchanged; this is a second, independent control
// surface, not a replacement.
//
// Like uac_init(), this is best-effort: cat_init() failing (most commonly
// because the ACM gadget function isn't bound - no USB gadget support on
// this hardware/kernel, or the gadget failed to bind at all, see
// usb_gadget_os_setup.md) is not fatal. minibitx keeps running on
// whatever subset of control surfaces actually came up. Call after
// uac_init() (the two functions bind together as one gadget, but
// cat_init()/cat_stop() have no other dependency on uac_init() having
// succeeded).
int cat_init(void);
void cat_stop(void);

#endif // USB_GADGET_H
