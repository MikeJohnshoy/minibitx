# USB UAC2 Decimation Filter: 96kHz → 48kHz

Status: implemented (`decim48k.c`, wired into `sound_process()` in
`sound.c`, ahead of `uac_push_iq()` only). Bench-verified numerically
(this document, §4) and now against a real UAC2 host too: WSJT-X on
Windows decoded FT8 correctly from the "sBitx IQ" capture device
(2026-09, see `usb_gadget_os_setup.md` §9/§11) - the resulting
24-bit/48kHz stream is clean enough for a real decoder, not just
numerically correct in isolation.

## 1. Background

`usb_gadget.c`'s UAC2 gadget has always advertised 48kHz (`c_srate`/
`p_srate` in `uac_gadget_create()`, and the ALSA PCM `uac_alsa_open()`
opens on the gadget's own `UAC2Gadget` card) - that's what
`usb_gadget.h`'s own architecture
comment always said minibitx streams. But `sound.c` runs the WM8731
codec at 96kHz (`SAMPLE_RATE`), and until now `uac_push_iq()` was called
with raw, un-decimated 96kHz-rate samples straight out of
`sound_process()`'s native-rate loop - the gadget/ALSA side was fed
samples at twice the rate it was configured for, with nothing to
reconcile the two.

Because the ring buffer between `uac_push_iq()` (producer) and
`uac_writer_thread()` (consumer, draining at the real, ALSA-paced 48kHz
rate - see `usb_gadget_os_setup.md` §7 for that fix) only drains at
48kHz while being fed at 96kHz, it would fill continuously and start
silently dropping the newest sample once full. That's not a clean
decimation - it's an uncontrolled, unfiltered drop of roughly half of
all samples, picked by whichever lost the race against the ring buffer
filling up. Any content above the *new* 24kHz Nyquist that should have
been filtered out before dropping samples would instead alias, on top of
already being a poor way to reduce the rate.

This surfaced while chasing Windows-not-detecting-the-gadget (a separate
problem, `usb_gadget_os_setup.md` §8) and evaluating whether minibitx
could someday interoperate with the QMX/Tab5 panadapter project
([tab5.lav.dk](https://tab5.lav.dk/), github.com/SteffenLav/qmx-panadapter)
- that project's Tab5 hardware is a fixed embedded USB host expecting
UAC2 I/Q stereo at exactly 48kHz/24-bit, not something that negotiates
an arbitrary rate the way desktop SDR software might. That made "just
relabel the gadget as 96kHz" the wrong call - actually decimating
properly is what keeps that door open, and fixes the sample-dropping
problem either way.

## 2. Why this filter can be cheap: it's the second stage of a cascade

`sound_process()` already runs every I/Q sample through
`antialias_apply()` (see
[`antialias_filter_design.md`](antialias_filter_design.md)) *before* this
new decimation step ever sees it - that filter has a 32kHz passband and
a 47.5kHz stopband, achieving about -81dB by 47.5kHz. So by the time
samples reach `decim48k_apply()`, content above ~47.5kHz is already
suppressed by roughly -81dB.

That means this filter only has to cover the "gap": from the new 24kHz
Nyquist (once decimated to 48kHz) up to wherever the antialias filter's
own rejection has already taken over. It does *not* need to independently
re-suppress everything all the way out past 47.5kHz - the existing
filter already did that job. This is why a comparatively modest 25-tap
filter is enough here despite a much narrower transition band (15kHz-
24kHz, 9kHz wide) than the antialias filter's own (32kHz-47.5kHz,
15.5kHz wide) - the two filters' jobs only overlap in that gap, and the
combined response is what actually matters.

## 3. Filter design

Using `scipy.signal.remez` (same method and tooling as
`antialias_filter_design.md`), at Fs=96kHz, exploring the tap-count
tradeoff:

| Fpass | Fstop | Transition | Taps | Passband ripple | Stopband |
|---|---|---|---|---|---|
| 16 kHz | 24 kHz | 8 kHz | 25 | 0.34 dB | -51 dB |
| 15 kHz | 24 kHz | 9 kHz | 21 | 0.55 dB | -50 dB |
| 15 kHz | 24 kHz | 9 kHz | **25** | **0.34 dB** | **-54 dB** |
| 15 kHz | 24 kHz | 9 kHz | 31 | 0.10 dB | -64 dB |
| 14 kHz | 24 kHz | 10 kHz | 25 | 0.24 dB | -57 dB |

Fstop is fixed at 24kHz - that's the hard Nyquist limit for a 48kHz
output rate; anything above it aliases on decimation, full stop. Fpass
at 15kHz keeps essentially all of the real signal the crystal filter
ever delivers (its own -3dB half-width is only ~17.4-17.5kHz per
`antialias_filter_design.md` §2, so 15kHz gives up very little).
25 taps was chosen as a reasonable middle ground - 31 taps buys another
10dB of stopband for 6 more multiplies per output sample, but 25 taps'
-54dB is already deep given the antialias filter's own -81dB is stacked
in front of it for anything past 47.5kHz; not worth the extra cost for
this application.

## 4. Chosen design

```
Sample rate (Fs):         96000 Hz
Decimation factor:        2 (-> 48000 Hz output)
Passband edge (Fpass):    15000 Hz
Stopband edge (Fstop):    24000 Hz  (fixed - the new Nyquist)
Filter length:            25 taps (Type I, linear phase, symmetric)
Achieved passband ripple: 0.343 dB (0-15kHz)
Achieved stopband:        -54.05 dB worst-case (24kHz and above)
```

Coefficients (`scipy.signal.remez(25, [0, 15000, 24000, 48000], [1, 0],
weight=[1, 10], fs=96000)`):

```
h[ 0] =  0.00359949      h[13] =  0.29725521
h[ 1] =  0.00298201      h[14] =  0.09648483
h[ 2] = -0.00687610      h[15] = -0.05092443
h[ 3] = -0.01768023      h[16] = -0.06718464
h[ 4] = -0.00898327      h[17] = -0.00730499
h[ 5] =  0.02024500      h[18] =  0.03267755
h[ 6] =  0.03267755      h[19] =  0.02024500
h[ 7] = -0.00730499      h[20] = -0.00898327
h[ 8] = -0.06718464      h[21] = -0.01768023
h[ 9] = -0.05092443      h[22] = -0.00687610
h[10] =  0.09648483      h[23] =  0.00298201
h[11] =  0.29725521      h[24] =  0.00359949
h[12] =  0.39168302
```

(Symmetric about h[12] - h[i] == h[24-i] - same as `antialias.c`'s
filter, though `decim48k.c` doesn't currently exploit that symmetry to
halve the multiply count the way `antialias_apply()` does, since this
filter only runs its multiply-accumulate on every *other* input sample
already - see §5.)

## 5. Implementation

`decim48k_apply()` (`decim48k.c`) is called once per 96kHz-rate input
sample, per rail (I and Q each get their own `struct decim48k_state` -
independent history and decimation phase, same pattern as
`antialias_state`). Every call updates the filter's history (needed so
the kept-output calls always have correct context), but the actual
25-tap multiply-accumulate only runs on the calls that produce a kept
output - the discarded half of calls return immediately after updating
history, which is a genuine compute saving on top of being simpler code
than filtering every sample and throwing half away afterward.

`sound_process()` (`sound.c`) calls `decim48k_apply()` for both rails on
every 96kHz sample, and only calls `uac_push_iq()` when both rails
report a kept output (they always agree, since both are fed in lockstep
every sample - see the comment at the call site). `hpsdr_send_iq()`
is unaffected - it still receives the full native 96kHz `i_samples[]`/
`q_samples[]` arrays, unchanged.

Verified against an independent Python reference implementation of the
same difference equation (max absolute deviation ~1.8e-12, i.e.
floating-point noise) and against a swept-tone test through the actual
compiled C code:

| Test tone | Measured gain |
|---|---|
| 1 kHz | -0.15 dB |
| 5 kHz | +0.16 dB |
| 10 kHz | -0.17 dB |
| 15 kHz | -0.17 dB |
| 18 kHz | -4.13 dB |
| 21 kHz | -15.70 dB |
| 30 kHz | -54.13 dB |
| 40 kHz | -55.44 dB |

This matches the predicted response from §4 closely - flat within the
0.34dB passband ripple spec through 15kHz, rolling off through the
transition band, and settling at the designed ~-54dB stopband depth by
30kHz and beyond.

## 6. What's still open

This document covers the DSP correctness of the rate conversion only.
Getting a real UAC2 host to actually see and use the resulting 48kHz
stream was tracked in `usb_gadget_os_setup.md` §§9-11 and is now fully
resolved: Windows not detecting the gadget device at all (§10) was fixed
by a USB-A host port (sidesteps the Pi 4's USB-C CC-line issue), §11
fixed a deeper bug where IQ samples were never actually reaching the
gadget's real USB endpoint at all (written to an unconnected
`snd-aloop` card instead of the gadget's own `UAC2Gadget` card), and
WSJT-X on Windows has since decoded FT8 correctly end-to-end (2026-09).
Interoperating with the QMX/Tab5 panadapter specifically would additionally need a
CDC-ACM Kenwood-style CAT serial gadget function alongside this UAC2
one, which minibitx does not have yet - see `usb_gadget_os_setup.md`
for that gap; this decimation fix is a necessary piece of that future
goal, not a complete answer to it on its own.
