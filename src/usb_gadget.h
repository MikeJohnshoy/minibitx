/*
 * usb_gadget.h — USB gadget composite device for minibitx: Audio Class 2.0
 * (UAC2) IQ streaming plus a CDC-ACM serial function for Kenwood
 * TS-480-subset CAT control. Both live in usb_gadget.c - the CAT command
 * handling is in its own clearly-marked section near the bottom of that
 * file, rather than a separate translation unit, since both functions are
 * just two halves of the one composite gadget this file already owns the
 * whole lifecycle of.
 *
 * Ported from the UAC2 section Mike (KB2ML) added to sbitx's hpsdr_p1.c -
 * only this self-contained piece, not the GTK/GLib-dependent TX/MOX code
 * that file also carried, since minibitx has no GTK main loop
 * (radio_set_tx()/radio_tune_to() instead). Touches only ALSA and
 * configfs, no HPSDR state, so it drops in as its own module.
 *
 * Provides a USB Audio Class 2.0 gadget device named "sBitx" that streams
 * 24-bit / 48 kHz stereo IQ (I = left, Q = right) to any SDR application
 * that can consume a USB audio input (e.g. SDR#, HDSDR, GQRX, SDR Console),
 * and a CDC-ACM serial port alongside it that control-surface software
 * (FLRig chief among them) can talk Kenwood-style CAT commands to, without
 * needing any TCP-to-serial bridge on the host side.
 *
 * Architecture overview:
 *   The Linux USB gadget framework is configured via the configfs API under
 *   /sys/kernel/config/usb_gadget/. Two functions are bound into one
 *   composite gadget, both created/torn down together by
 *   uac_gadget_create()/uac_gadget_destroy() in usb_gadget.c - see the path
 *   layout below for the exact attributes each carries. Device/product
 *   strings: "sBitx" / "sBitx IQ". Once bound to a UDC controller (detected
 *   automatically), the host sees a standard USB audio capture device
 *   called "sBitx" AND a standard USB serial port - Windows 10/11 auto-
 *   binds its inbox usbser.sys driver to the ACM interface with no custom
 *   INF needed, the same "just works" story UAC2 already gets (see
 *   docs/dsp_design_notes/usb_gadget_OS_setup.md for current bench-testing status).
 *
 *   Sample delivery: sound.c runs the codec at 96kHz, but this gadget is
 *   configured for 48kHz - sound_process() runs I/Q through
 *   decim48k_apply() (decim48k.c), a real 96kHz->48kHz decimating lowpass,
 *   before calling uac_push_iq(), so the 48kHz this gadget advertises is
 *   what it actually delivers (see
 *   docs/dsp_design_notes/usb_uac_decimation_design.md for the filter
 *   design and why this needed to be a real decimation, not just a
 *   relabeled sample rate). hpsdr_send_iq() (hpsdr_p1.c) gets its own
 *   native-96kHz copy from the same sound_process() call site,
 *   independently - USB audio and the HPSDR UDP stream are two separate
 *   consumers of the same IQ, neither depending on the other.
 *
 *   Producer/consumer split: uac_push_iq() only ever enqueues into a
 *   lock-free ring buffer and never blocks - the same architecture
 *   hpsdr_p1.c's IQ queue uses, for the same reason (a stalled/absent USB
 *   host must never be able to block sound.c's real-time audio thread and
 *   cause hardware xruns). A dedicated uac_writer_thread() is the sole
 *   consumer, draining the queue and performing the potentially-blocking
 *   ALSA write on its own thread - see dsp_design_notes/usb_gadget_OS_setup.md §7 for the
 *   xrun incident this fixed.
 *
 *   ALSA delivery — direct to the gadget's own card: binding the UAC2
 *   function to a UDC makes the kernel's u_audio/f_uac2 driver register
 *   its own independent ALSA card ("UAC2Gadget" in /proc/asound/cards).
 *   uac_writer_thread() opens that card's device-0 playback PCM directly
 *   and writes to it - that's what the kernel driver actually streams out
 *   the real USB isochronous endpoint. (An earlier version routed through
 *   a separate snd-aloop card instead, on the mistaken assumption that
 *   UAC2 reads its capture side from that loopback automatically - it
 *   doesn't, so nothing reached the USB link; see dsp_design_notes/usb_gadget_OS_setup.md
 *   §11 if that approach is ever tempting again.)
 *
 * Configfs gadget path layout (created by uac_gadget_create() in
 * usb_gadget.c):
 *   /sys/kernel/config/usb_gadget/sbitx_iq/
 *     idVendor, idProduct, bcdUSB, bcdDevice
 *     strings/0x409/manufacturer  = "sBitx"
 *     strings/0x409/product       = "sBitx IQ"
 *     strings/0x409/serialnumber  = "0000001"
 *     configs/c.1/
 *       strings/0x409/configuration = "Default"
 *       bmAttributes, MaxPower
 *       function symlinks -> functions/uac2.0/, functions/acm.usb0/
 *     functions/uac2.0/
 *       c_srate  = 48000
 *       c_ssize  = 3        (3 bytes = 24-bit)
 *       c_chmask = 3        (2 channels: L=I, R=Q)
 *       p_srate  = 48000    (playback side, unused but must be set)
 *       p_ssize  = 3
 *       p_chmask = 3
 *     functions/acm.usb0/
 *       (no attributes set — port_num is read-only/assigned by the kernel)
 *
 * Dependencies (must be present on the target system):
 *   Kernel modules : dwc2 (or other device-mode UDC), libcomposite
 *   Userspace libs : libasound2-dev (ALSA — for PCM write to the
 *                    UAC2Gadget card; minibitx already links -lasound
 *                    for sound.c)
 *   Kernel config  : CONFIG_USB_CONFIGFS_F_UAC2=y, CONFIG_USB_CONFIGFS_F_ACM=y
 *
 * Thread safety:
 *   uac_init()/uac_stop() are meant to be called once, from main(), around
 *   hpsdr_init()/hpsdr_poll(). uac_push_iq() runs on minibitx's audio
 *   thread (same thread that calls hpsdr_send_iq()) and only ever touches
 *   the lock-free queue - it never blocks and never touches the ALSA PCM
 *   handle directly. uac_writer_thread() (internal to usb_gadget.c) is
 *   the only thing that ever calls into ALSA on uac_pcm_handle - no lock
 *   is needed between the two: the queue's atomic head/tail split
 *   ownership exactly like hpsdr_p1.c's IQ queue (producer writes head
 *   only, consumer writes tail only).
 */

#ifndef USB_GADGET_H
#define USB_GADGET_H

/* Configure the composite UAC2+ACM gadget via configfs and open the UAC2
 * side's ALSA PCM. Call once, after hpsdr_init()/hpsdr_poll(). Returns 0 if
 * both the gadget and the PCM are ready, -1 if either step fails (e.g. no
 * UDC found) — not a hard failure for the rest of minibitx, which keeps
 * running over HPSDR/UDP either way. The ACM function comes up as part of
 * the same gadget bind; cat_init() (below) is what actually opens and
 * uses the resulting /dev/ttyGS0, independently of this succeeding or
 * failing. */
int uac_init(void);

/* Deliver one 48 kHz IQ sample pair, normalized to [-1.0, +1.0]. Enqueues
 * into a lock-free ring buffer for uac_writer_thread() to drain and flush
 * to the ALSA loopback one period at a time - never blocks; silently
 * drops the sample if the queue is full (writer thread stalled behind a
 * slow/absent USB host). No-op if uac_init() hasn't succeeded (or after
 * uac_stop()). */
void uac_push_iq(double i_val, double q_val);

/* Tear down the gadget and release the ALSA PCM handle. Safe to call even
 * if uac_init() was never called or failed. */
void uac_stop(void);

/* Returns 1 while the UAC2 stream is initialized and ready to accept
 * samples, 0 otherwise. */
int uac_is_active(void);

/* Kenwood TS-480-subset CAT control, reached over this gadget's CDC-ACM
 * (ttyGS) serial function - see the "CAT control" section at the bottom
 * of usb_gadget.c for the command set and implementation, and
 * docs/04_remote_control_and_iq_output.md for why TS-480 specifically.
 *
 * This exists for control surfaces that only know how to talk CAT to a
 * real (or real-enough) Kenwood radio - FLRig chief among them, since it
 * has no generic "connect to a Hamlib rigctld-style server" option the
 * way WSJT-X does. WSJT-X keeps using the existing Hamlib NET rigctl
 * server (hamlib.c) unchanged; this is a second, independent control
 * surface, not a replacement.
 *
 * Like uac_init(), this is best-effort: cat_init() failing (most commonly
 * because the ACM gadget function isn't bound - no USB gadget support on
 * this hardware/kernel, or the gadget failed to bind at all, see
 * dsp_design_notes/usb_gadget_OS_setup.md) is not fatal. minibitx keeps running on
 * whatever subset of control surfaces actually came up. Call after
 * uac_init() (the two functions bind together as one gadget, but
 * cat_init()/cat_stop() have no other dependency on uac_init() having
 * succeeded). */
int cat_init(void);
void cat_stop(void);

#endif /* USB_GADGET_H */
