# RX Anti-Alias FIR Filter: Analysis and Design

Status: implemented (`antialias.c`, wired into `sound_process()` - see
§9 and [`../02_rx_processing_pipeline.md`](../02_rx_processing_pipeline.md)).
This document describes the crystal filter data and the FIR filter
design it justifies, after mixing down to baseband I&Q.

## 1. Background

Sampling at 96kHz does not guarantee zero residual aliasing - it depends on
exactly how wide the crystal filter's own passband is relative to the 
48kHz Nyquist edge. This document works that out from real measured
data, and designs a FIR filter to clean up whatever residual aliasing
remains, while preserving as much usable spectrum as possible.

## 2. Measured crystal filter data

This data is from the spec sheet from a 
network analyzer sweep for the actual installed crystal filter.  This design doc
was provided by Evan AC9TU as we were looking at the zbitx radio performance.  It may
or may not be the same as is in the sbitx boards, but it is at least representative.

| Parameter | Value |
|---|---|
| Center frequency | 40.0124 MHz |
| Bandwidth (-3dB) | 34.848 kHz |
| Quality factor (Q) | 1148.20 |
| Bandwidth (-6dB label) | 36.828 kHz |

| Lower side | Frequency | Offset from center | Attenuation |
|---|---|---|---|
| Cutoff | 39.9949 MHz | -17.5 kHz | -4.0 dB |
| "-6dB" point | 39.9940 MHz | -18.4 kHz | -7.2 dB (label is approximate) |
| "-60dB" point | 25.2374 MHz | n/a | **discarded - bad data** |

| Upper side | Frequency | Offset from center | Attenuation |
|---|---|---|---|
| Cutoff | 40.0298 MHz | +17.4 kHz | -4.0 dB |
| "-6dB" point | 40.0308 MHz | +18.4 kHz | -8.0 dB (label is approximate) |
| -60dB point | 40.0409 MHz | +28.5 kHz | -61.1 dB |

The lower-side "-60dB point" (25.2374 MHz, listed with attenuation
"nan") may be a bad data point.  Only the upper skirt has a trustworthy deep-stopband
measurement; the lower skirt's measured roll-off rate (72463.867
dB/octave, versus the upper skirt's 204746.485 dB/octave - roughly 2.8x
steeper) shows the two skirts are meaningfully asymmetric, with the
lower skirt rolling off more slowly.

## 3. bfo_freq selection

The bfo_freq is selected to to put the crystal filter in the center
of the IF.  On transmit if we generated the cw waveform at true baseband (say, at
700Hz cw pitch) then mixing this up to the crystal filters 40MHz range would provide two closely spaced sum and difference signals in the center of the filters range if we set the bfo_freq to match the crystal filter center freq.  So instead we generate the cw waveform at the higher end of the baseband IF range (we raise it by an amount TX_IF_OFFSET_HZ).  Again, if we had the bfo_freq equal to the crystal filter center freq, our two sum and difference mixer outputs would now be out at each end of the crystal filter.  

Placing `bfo_freq` above the filter's true measured center is what shifts the *wanted* CW TX product near
the filter's low-loss center while pushing the *unwanted* mirror product out into the stopband. 

Practical consequence: `bfo_freq` and `TX_IF_OFFSET_HZ` are coupled, not
independent. But neither value should change for a given set of hardware.

### Splitting the filter-center reference out of bfo_freq

The above explains why `bfo_freq` sits deliberately off the filter's
true center for TX. What it glossed over: RX used to reuse that exact
same `bfo_freq` value too (`radio_tune_to()`'s old
`f + bfo_freq - RX_IF_FREQ_HZ` formula), even though RX has no reason to
want it off-center at all - RX just wants the tuned signal placed as
close to the filter's real passband peak as possible. That worked
reasonably well only because `RX_IF_FREQ_HZ` (24,000 Hz - a pure
digital/audio-IF choice, unrelated to the crystal filter) happened to
sit close to `TX_IF_OFFSET_HZ + CW_PITCH_HZ` (23,300 Hz - a pure
TX/image-rejection choice): RX landed within 1,400 Hz of the filter's
true center as an incidental side effect, not by design. The real cost
was the coupling itself: change `RX_IF_FREQ_HZ` for some unrelated
digital-IF reason, and you'd unknowingly shift RX off-center *and* be
tempted to "fix" it by changing `bfo_freq`, which would then throw away
`TX_IF_OFFSET_HZ`'s careful image-rejection calibration - two unrelated
design decisions, entangled through one shared number.

The fix: a new constant, `xtal_filter_center` (`radio.c`, default
40,012,400 Hz - the measured center from §2 above), names the crystal
filter's real passband center directly, as a physical/measured property
of the hardware rather than a clock setting. RX now aims at it exactly:
`radio_tune_to()` computes `clk2 = f + xtal_filter_center`, and `clk1`
(previously fixed at `bfo_freq` for the whole process) now sits at
`xtal_filter_center + RX_IF_FREQ_HZ` while receiving, switching to
`bfo_freq` only for the duration of each TX burst
(`radio_tx_apply()`, `radio.c`) - the same clock, two different values,
retuned on every RX/TX transition the same way `clk2` already was.
`bfo_freq` keeps its old default and its old TX-only job unchanged; the
relationship between the two is now explicit rather than implicit:
`bfo_freq == xtal_filter_center + TX_IF_OFFSET_HZ`, by calibration (not
enforced in code - `TX_IF_OFFSET_HZ` is still the one meant to be
re-swept on the bench, per `cw.c`'s comment).

Net effect: TX's actual clk1/clk2 values, and therefore its transmitted
frequency and image suppression, are unchanged (verified algebraically
identical to the pre-split formulas for today's constants - see
[`../03_tx_processing_pipeline.md`](../03_tx_processing_pipeline.md)'s
"Known limitations"). RX gets a small, intentional correction (its
mixed signal now lands exactly on `xtal_filter_center` instead of
1,400 Hz low) that's unlikely to be measurable given the filter's ~35
kHz width, but the real payoff is structural: `RX_IF_FREQ_HZ` and
`TX_IF_OFFSET_HZ` can each change independently going forward without
silently perturbing the other. As with `bfo_freq`, re-verify on air
after any change here - this touches real TX hardware, and the
derivation being algebraically sound is not a substitute for a bench
check.

## 4. How much does the analog filter help at Nyquist?

Mapping the crystal filter's shape directly onto the 24kHz digital IF
(mixing shifts center frequency, not relative bandwidth), the 96kHz
Nyquist edge (48kHz absolute) falls at +24kHz offset from the IF center.

That point falls between two real, measured upper-skirt points (+18.4kHz
at -8.0dB, and +28.5kHz at -61.1dB), so it can be found by interpolation
rather than extrapolation:

```
slope = (-61.1 - (-8.0)) / (28.5 - 18.4) = -5.257 dB/kHz
attenuation at +24kHz = -8.0 + (24 - 18.4) x (-5.257) = -37.4 dB
```

So the analog crystal filter alone already provides roughly **-37dB of
attenuation right at the new 96kHz Nyquist edge**, deepening further
beyond it (reaching the measured -61.1dB by 52.5kHz absolute). The
digital filter does not need to do all the anti-aliasing work itself -
it only needs to add enough on top of this existing analog rejection to
reach a comfortable combined margin, right at the edge.

## 5. Filter placement and goal

As established previously: the FIR belongs **after** mixing to baseband,
applied identically and independently to the I and Q rails (a plain
real-coefficient lowpass FIR run twice, once per rail) - not before
mixing, and not as anything more exotic than a standard lowpass.

Goal, given the "maximize usable spectrum, no decimation" objectives as
preserve as much of the crystal filter's real, delivered bandwidth as
possible (out past the -3dB corner at 17.4-17.5kHz, into the shoulder
where the analog filter is already rolling off but still passing real
signal), and only clean up the narrow region right against the 48kHz
Nyquist wall where aliasing risk actually exists.

## 6. Design exploration

Using `scipy.signal.remez` (the same Parks-McClellan equiripple method
now used for the 64bit sbitx LPF), at Fs=96kHz we can choose, trading of filter flatness,
transitions width dna stopband depth.  This stopband performance comes on top
of the crystal filter, so it feels like there is plenty of rejection for any digital 
artifacts.

| Fpass | Fstop | Transition width | Taps needed for solid performance |
|---|---|---|---|
| 26 kHz | 46.0 kHz | 20.0 kHz | 15 taps -> -78.8dB stopband, 0.018dB ripple |
| 30 kHz | 47.0 kHz | 17.0 kHz | 21 taps -> -92.3dB stopband, 0.004dB ripple |
| 32 kHz | 47.5 kHz | 15.5 kHz | 21 taps -> -81.2dB stopband, 0.015dB ripple |

All three are remarkably cheap with respect to performance impact- a
handful of taps - because of the huge
natural guard band this architecture has between the wanted signal and
the Nyquist edge. 

Given the "maximize usable spectrum" priority, the widest-passband
option (32kHz/47.5kHz) is chosen: it preserves essentially all
delivered signal out to within half a kHz of Nyquist, backed by real
analog attenuation that's already substantial by that point, at a
trivial computational cost (21 taps).

## 7. Chosen design

```
Sample rate (Fs):        96000 Hz
Passband edge (Fpass):   32000 Hz
Stopband edge (Fstop):   47500 Hz
Filter length:           21 taps (Type I, linear phase, symmetric)
Achieved passband ripple: 0.0153 dB (0-32kHz)
Achieved stopband:        -81.16 dB worst-case (47.5-48kHz)
Combined with analog filter's own ~-37dB+ at Nyquist and deeper beyond:
comfortably over -100dB combined rejection in the aliasing-relevant zone.
```

Coefficients (`scipy.signal.remez(21, [0, 32000, 47500, 48000], [1, 0], weight=[1, 10], fs=96000)`):

```
h[ 0] =  0.00232502      h[11] =  0.16869859
h[ 1] = -0.00630660      h[12] = -0.13468276
h[ 2] =  0.01119704      h[13] =  0.08949296
h[ 3] = -0.01332687      h[14] = -0.04531016
h[ 4] =  0.00734644      h[15] =  0.01168262
h[ 5] =  0.01168262      h[16] =  0.00734644
h[ 6] = -0.04531016      h[17] = -0.01332687
h[ 7] =  0.08949296      h[18] =  0.01119704
h[ 8] = -0.13468276      h[19] = -0.00630660
h[ 9] =  0.16869859      h[20] =  0.00232502
h[10] =  0.81864267
```

(Symmetric about h[10], as expected for a linear-phase Type I FIR - h[i]
== h[20-i]. This symmetry is worth keeping in the eventual C
implementation, since it halves the multiplies needed per output sample.)

## 8. See attached plot

`response_plot.png` shows three traces across the 0-48kHz digital IF
range: the measured/modeled analog crystal filter response (dashed
gray), the digital FIR alone (blue), and the combined analog+digital
response (red). The combined trace shows a clean, wide passband out to
about 32kHz before rolling off sharply to a deep null well before
48kHz - exactly the intended shape.

## 9. Implementation

This FIR has been added into `sound.c`amd is applied separately to the `i_samples[]` and
`q_samples[]` arrays in `sound_process()`, after the mixing step and
before `hpsdr_send_iq()`/`uac_push_iq()`. Because the filter is
symmetric, only 11 distinct multiplies are needed per output sample
(exploit `h[i] == h[20-i]` by summing paired input samples before
multiplying), not 21.  The code is ready for gcc optimization and should be 
quite easy on on the raspberry pi processors.

Before inserting this filter into the RX pipeline I could see the recognizable unwanted image
of - for example - FT8 signal activity on a SDR receiver spectrum display.  It wasn't really
audible, but adding this filter removed the unwanted image with minimal processing cost.
