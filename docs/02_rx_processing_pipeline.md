# 02 — RX processing pipeline

This documents the receive signal path from antenna to baseband I/Q —
the part that's easy to get wrong, because most of it lives in analog
hardware and a fixed relationship between two si5351 clocks.

By this point in the process's life, GPIO, the si5351/I2C bus, and the
WM8731 codec's ALSA capture stream are already up and configured — see
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).
This document picks up from there: what happens to samples once they're
flowing, and what tuning changes.

minibitx has no onboard demodulation, no waterfall, no mode logic — the
connected SDR app does all of that. This document only covers what
happens before the signal leaves the Pi as baseband I/Q; where that I/Q
goes next is [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md).

## The chain, end to end

```
  Antenna
     |
     v
  Low Pass Filter (LPF) bank (select one)
     |
     v
  Mixer 1  <---  clk2, si5351 RX LO (varies with tuning)
     |            mixes received signal to xtal_filter_center - the
     |            crystal filter's own real, measured center
     v
  Crystal filter centered at ~40.0124 MHz, based on hardware spec 
     |
     v
  Mixer 2  <---  clk1, si5351 (fixed while receiving - xtal_filter_center
     |           + RX_IF_FREQ_HZ; switches to bfo_freq only for the
     |           duration of TX - see 03_tx_processing_pipeline.md),
     |           shifts output of crystal filter to 24kHz baseband
     v
  Low IF, centered at RX_IF_HZ (24000 Hz)
     |
     v
  ADC / wm8731 audio codec (sound.c, 96 kHz sample rate) gain is settable?
     |  
     v
  Software VFO (vfo.c, "lo" in radio.c) <--- FIXED at RX_IF_HZ (24000 Hz)
     |            sound.c: sound_process() calls vfo_read_iq() per sample
     v            converts real value A/D output to analytic I&Q at baseband
  Baseband I/Q (centered at 0 Hz)
     |
     v
  Anti-alias FIR (antialias.c, 21 taps, applied separately to I and Q)
     |
     v
  handed to interface software (hpsdr_p1, USB audio out, or simple network
            interface) to work with external applications
```

Two mixer stages, two si5351 clocks, two different jobs.

## Stage by stage
At each stage we can look at an example following a single CW signal
at 7030000 as it flows from the antenna through to I&Q outuput.

**LPF bank.** `radio_hw.c`'s `set_lpf_40mhz(frequency)` selects one of
four low-pass filter relays (`LPF_A`–`LPF_D`) based on the tuned
frequency — under 5.5 MHz, under 10.5 MHz, under 18.5 MHz, or under 30
MHz — and is a no-op if the frequency falls in the same band as the last
call. Pure analog front-end filtering; nothing here talks to either
si5351 clock. (Full relay init/idle-state details are in
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).)

**Mixer 1 — the RX LO (clk2), which sweeps with tuning.** This is the
only clock `radio_tune_to()` itself ever moves. `radio_tune_to(f)` in
`radio.c` sets it with `si5351bx_setfreq(2, f + xtal_filter_center)` —
mixing the desired RF frequency `f` up to `xtal_filter_center`, the
crystal filter's own real, measured passband center (a board-specific
calibration value, default 40,012,400 Hz - see
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md)
§3 for where that number comes from and why RX gets to aim at it
directly rather than sharing a value with TX). Whatever `f` you tune
to, the output of this stage always lands at that same fixed point;
that's the whole point of a superheterodyne front end, and it's also
*why* nothing downstream of this stage needs to know the current
operating frequency.

For our example cw signal at 7030000,
clk2 = 7,030,000 + 40,012,400 = 47,042,400 Hz

**Crystal filter.** A fixed bandpass filter centered at `bfo_freq`. This
is the receiver's actual selectivity — everything outside its passband
is rejected before the signal ever reaches Mixer 2. minibitx does no
mode-dependent filtering of its own (no CW/SSB bandwidth switching); the
connected SDR app is expected to do any further filtering digitally on
the IQ it receives. (Measured filter response and a proposed digital
anti-alias filter to complement it live in
[`dsp_design_notes/`](dsp_design_notes/).)

**Mixer 2 — clk1, fixed while receiving.** This mixer brings the
crystal-filter output (centered at `xtal_filter_center`) down to a low
IF of `RX_IF_HZ` (24000 Hz) that the audio codec can actually sample.
Its LO is si5351 `clk1`, set to `xtal_filter_center + RX_IF_FREQ_HZ`
(40,036,400 Hz by default) in `minibitx.c` at startup, and restored to
that same value every time RX resumes after a TX burst
(`radio_tx_apply()`, `radio.c`) — it does not sweep with tuning for the
same reason Mixer 1's output doesn't need to: whatever `f` you're tuned
to, Mixer 1 already brought it to the same fixed `xtal_filter_center`
point, so Mixer 2 only ever has to undo that one fixed offset. It is,
however, *not* fixed for the entire life of the process any more: while
transmitting, `radio_tx_apply()` retunes it to `bfo_freq` instead - a
deliberately different, off-center value used only for CW image
suppression - and restores this RX value the moment TX ends. See
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) for why
TX needs its own value here rather than reusing this one.

**ADC / audio codec.** `sound.c` reads from the already-open ALSA capture
device (opened and configured per
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)) at
96 kHz and hands each block of raw samples to `sound_process()`. What
arrives here is the low IF signal — real-valued, centered around
`RX_IF_HZ`, not yet I/Q.

**Software VFO — fixed at RX_IF_HZ, never swept.** `vfo.c` implements a
digital NCO (`struct vfo`, the global `lo` in `radio.c`) that generates
quadrature (cos/sin) mixing signals. `sound_process()` calls
`vfo_read_iq()` once per sample and multiplies the incoming real IF
sample by both the cosine and sine outputs, producing the I and Q
channels — a standard digital quadrature downconversion, taking the
fixed 24 kHz IF down to baseband (0 Hz). Like the BFO, this oscillator's
frequency is fixed at `RX_IF_HZ` and does not change when you retune;
only its *phase* is preserved across calls; see `RX_IF_HZ` in `radio.h`
for the single place this constant is defined.

**Anti-alias FIR filter.** Right after mixing to I/Q, `sound_process()`
runs each rail through `antialias_apply()` (`antialias.c`) — a 21-tap,
symmetric (linear-phase) FIR lowpass, independently on I and independently
on Q, sharing one coefficient table but each with its own history state
(`struct antialias_state`). This exists because sampling a real IF signal
at 96 kHz and synthesizing I/Q from it doesn't automatically guarantee
zero aliasing right at the ±48 kHz Nyquist edge — see
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md)
for the measured crystal-filter data this was designed against. The
chosen design (32 kHz passband edge, 47.5 kHz stopband edge) preserves
essentially all of the usable spectrum the crystal filter itself already
delivers, adding roughly -81 dB of stopband rejection on top of the
crystal filter's own real (but more gradual) rolloff — comfortably over
-100 dB combined right where aliasing would actually occur. It's cheap:
the symmetric coefficients mean only 11 distinct multiplies per output
sample rather than 21, and a double-length history buffer avoids any
wraparound branch in the inner loop.

**Baseband I/Q → the two streaming consumers.** `sound_process()`
concludes by handing its (now anti-aliased) I/Q arrays off to be streamed
out — see
[`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md)
for `hpsdr_send_iq()` and `uac_push_iq()`.

## Retuning

`radio_tune_to(f)` in `radio.c` is the only function that changes what RF
frequency the receiver is listening to, and it only ever touches two
things: `clk2` (Mixer 1's LO) and the LPF bank. It does **not** touch
`clk1` and does **not** change the software VFO's frequency — the
software VFO stays fixed at `RX_IF_HZ` for the life of the process, and
`radio_tune_to()` itself never moves `clk1` either way (it has no notion
of TX at all - see [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md)
for the one place `clk1` *does* move, `radio_tx_apply()`, and why that
lives there instead of here). As long as no TX burst is in progress,
`clk1` sits at its RX value (`xtal_filter_center + RX_IF_FREQ_HZ`) and
`radio_tune_to()` alone is enough to retune the receiver. Who's allowed
to call `radio_tune_to()`, and how, is covered in
[`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md).
