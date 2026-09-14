# RX Audio Demodulation: Local CW Monitor Design

Status: implemented (`rx_audio.c`/`rx_audio.h`, wired into `sound.c` — see
§6 and [`../02_rx_processing_pipeline.md`](../02_rx_processing_pipeline.md)).
First on-air confirmation (2026-09): copyable CW audio while tuning
through FT8-band signals on 40m, after the AGC fix in §5. v1's
symmetric-around-zero-beat limitation was fixed in v2 by a complex
(Hilbert-style) bandpass filter (§7); on-air listening then found v2's
filter conflated image rejection with narrow selectivity in a way that
made signals sound soft well before the edge of the nominal passband.
v3 split those into two independent stages — a wide image-reject
filter (§7) and a separate narrow post-demodulation selectivity filter
(§8) — the more conventional phasing-receiver architecture. On-air
listening against v3 then surfaced a real report — two CW signals 3kHz
apart, the un-tuned one still audible quite strongly — traced to stage
3's single biquad section having a much gentler skirt than its -3dB
width suggests (§8.4); the fix (current) is cascading four identical
biquad sections (§8.5), which took that scenario's rejection from ~59dB
to ~83dB. Bench-verified numerically, including a settling-time
correction to the test harness itself (§8.6); not yet re-confirmed on
air.

## 1. Background

minibitx's baseband I/Q (see
[`../02_rx_processing_pipeline.md`](../02_rx_processing_pipeline.md)) has
always had two consumers, both external: `hpsdr_p1.c` and `usb_gadget.c`'s
UAC2 gadget, feeding a separate SDR application that does the actual
demodulation. That means listening to the receiver has always required a
second machine, or at least a second process, running something like
FLRig/FLdigi/SDR Console. There was no way to tune to a CW signal and
just hear it out of the box's own speaker.

`rx_audio.c` closes that gap for the one mode minibitx already transmits
— CW — without adding a new capture path or a second process fighting
minibitx for the WM8731. It's a plain function call
(`rx_audio_process()`) added to the same real-time audio callback that
already produces `i_samples[]`/`q_samples[]` for the network consumers
(`sound_process()`, `sound.c`), reusing that block directly.

## 2. Why this lives inside minibitx, not the separate panel app

A companion touchscreen app (`mb-radio`) is being built as a standalone
process specifically so minibitx's own scope doesn't grow to include a
GUI, encoders, or a waterfall display. RX audio demod is the one
exception kept inside minibitx itself, for a structural reason rather
than a scope one: minibitx already owns the WM8731 codec exclusively
(see [`01_hardware_init_and_control.md`](../01_hardware_init_and_control.md)),
and already has this exact block of I/Q sitting in memory, mid-callback,
before it's even packaged for the network. A second process doing the
same demodulation would need its own capture path into the same ALSA
device minibitx already has open, and would add a full network
round-trip of latency for something that's supposed to sound
immediate. Putting it here instead costs one function call.

## 3. The product detector

The core technique is a standard product detector — the same idea a
classic analog CW rig's BFO implements in hardware — now split into
**four** independent stages, each with exactly one job (v1/v2 combined
some of these; see §7.6 for why v3 pulled them apart):

1. **A wide complex bandpass filter, at the signal's original location.**
   `i_samples[]`/`q_samples[]` arrive centered on dial center (0 Hz
   offset) — whatever external control surface is tuning the radio
   (FLRig, for now; see
   [`../04_remote_control_and_iq_output.md`](../04_remote_control_and_iq_output.md))
   is expected to park the wanted signal there. This stage's only job is
   telling one side of dial center from the other (image rejection) —
   see §7 for the full design. It is deliberately wide, not narrow.
2. **Mix up to `CW_PITCH_HZ` and keep the real part.** `cw.h`'s
   `CW_PITCH_HZ` (700 Hz) is the same pitch the TX sidetone already
   uses, so RX and TX match. `Re[(I+jQ)(cos+jsin)] = I·cos − Q·sin`,
   using `vfo_read_iq()` (`vfo.c`) for the mixing oscillator — no new
   NCO code needed. A steady carrier sitting exactly at dial center
   comes out as a steady 700 Hz tone, not silence, for the same reason
   `cw.c`'s TX side never keys straight at 0 Hz either.
3. **A narrow real bandpass, after demodulation.** By this point the
   signal is real, single-sided audio — the image question is already
   settled, so an ordinary symmetric filter can narrow it down to
   whatever bandwidth actually makes for comfortable single-signal
   copy, independent of stage 1 entirely. See §8.
4. **An AGC**, covered in §5.

Verified against synthetic I/Q before ever touching real hardware
(`test_rx_audio.c`, not part of the build): a carrier at dial center
produces a clean 698.3 Hz tone (target ~700 Hz); a carrier at +5kHz
offset comes out heavily attenuated by the time it reaches the output
(now rejected by both stage 1 and stage 3 — see §8.2). §5 covers what
changed in that test once the AGC was added; §7/§8 cover the later tests
added as the filtering itself evolved.

## 4. Why an FFT wasn't used instead

The initial idea was to take a 1024-point FFT of the I/Q and read off
the bin(s) near the center. Two problems with that, worked out before
writing any code:

- **The bin math itself**: at minibitx's real 96,000 Hz sample rate, a
  1024-point FFT gives 96000/1024 ≈ 93.75 Hz/bin — not the ~25kHz/bin
  that was the original assumption.
- **The bigger issue**: an FFT is a snapshot-per-block tool, good for a
  waterfall display, but the wrong tool for producing *continuous*
  demodulated audio — that would require an inverse FFT plus
  overlap-add, and would be locked to the bin-width's granularity rather
  than being continuously tunable. Worse, demodulating literally at the
  center bin (0 Hz offset) produces silence for a steady carrier, not a
  tone — mixing up to `CW_PITCH_HZ` first (§3) is what avoids that.

A time-domain NCO-plus-filter product detector is the standard technique
for exactly this job, is arbitrarily tunable, and needed no FFT at all.

## 5. The AGC: why a fixed gain could not work

v1 shipped with a fixed multiplier (`RX_AUDIO_PEAK_AMPLITUDE`, then
200,000,000) turning the demodulated audio into a PCM sample, calibrated
only against `test_rx_audio.c`'s synthetic, unit-amplitude carrier. That
produced total silence on real hardware — not quiet, silence, no
background hiss at all — and took several rounds of elimination to
diagnose, because the actual bug turned out to be three bugs deep:

1. **Stale local checkout.** The delivered `sound.c`/`minibitx.c` were
   briefly rebased onto a copy of the repo that was ~50 commits behind
   the real `origin/main`, including several of the user's own
   in-progress edits. Caught via `git fetch`/`git merge-base
   --is-ancestor` before anything was delivered; not a hardware issue at
   all, just a process one, but it cost a round of back-and-forth.
2. **The WM8731 `Master` control was hardcoded muted.** `setup_audio_codec()`
   (`sound.c`) had `sound_mixer("hw:0", "Master", 0)` — literally
   "Mute local speaker" in the original comment, from back when nothing
   meaningful was ever written to that output. No amount of correct DSP
   downstream could be audible with the analog output stage pinned to
   zero. Fixed initially by raising the default; then found that
   `radio.c`'s `radio_tx_apply()` *also* drove the same shared `Master`
   control (to `TX_MASTER_VOL`, feeding the TX exciter) and re-muted it
   on every RX return without ever restoring it — meaning RX audio (and
   the CW sidetone) were only ever briefly audible during a TX burst
   itself. The real, final fix (not a v1 detail, see §9) was
   discovering `Master` has independent left/right volume registers and
   splitting local-monitor level (left) from TX exciter drive (right)
   entirely, so neither can step on the other again.
3. **The actual bug**: even with `Master` correctly unmuted, a
   bench-added diagnostic (temporary, since removed — printed peak I/Q
   and peak PCM output once a second) showed real signal amplitude at
   this point in the chain sitting around **0.0015–0.0085** of the
   synthetic test's 1.0 reference — while tuned across live FT8-band
   signals on 40m, 2026-09. That's 100-600x smaller than what
   `RX_AUDIO_PEAK_AMPLITUDE` was calibrated against, producing peak PCM
   output of only ~10,000–64,000 out of a possible 2 billion (roughly
   -90 dBFS) — below the noise floor of the DAC, not just quiet.

A fixed multiplier tuned for that measured range would also be wrong the
moment the band got louder — real signal strength varies far more than
any single constant can track, which is exactly the problem every real
receiver's AGC (automatic gain control) exists to solve. `rx_audio.c`
now runs the demodulated audio through an envelope-following AGC instead
of a fixed gain:

```c
#define AGC_TARGET_AMPLITUDE 500000000.0  // ~23% of int32 full scale
#define AGC_ATTACK_MS    5.0   // fast - catch a loud transient before it clips
#define AGC_RELEASE_MS 300.0   // slow - don't pump between a CW dit and its gap
#define AGC_MAX_GAIN 8.0e11    // ceiling so near-silence doesn't amplify toward infinity
```

An asymmetric single-pole envelope follower tracks `|audio|` (fast
attack, slow release), and the applied gain is
`AGC_TARGET_AMPLITUDE / envelope`, clamped at `AGC_MAX_GAIN`. As of v3,
the AGC tracks the envelope of stage 3's output (the narrow-filtered
audio), not the raw post-demod signal — so the envelope reflects the
*combined* selectivity of both filtering stages, which is what
`rx_audio_debug_agc_envelope()`'s callers actually want to measure (see
§7.4/§8.2). Verified with an extended `test_rx_audio.c` before delivery:
a case at the bench-measured real amplitude (0.003) now produces output
indistinguishable from a full-scale (1.0) synthetic carrier — the exact
regression the bug above represents.

## 6. Wiring into `sound.c`

Three small, targeted additions, applied against `origin/main` directly
rather than a stale local branch (see §5, item 1):

```c
// sound_process(), in place of the old memset(output_speaker, 0, ...):
rx_audio_process(i_samples, q_samples, n_samples, output_speaker);
```

```c
// audio_loop()'s playback fill, a new branch alongside the existing
// cw_tx_active() one:
} else if (!in_tx) {
    for (int i = 0; i < n; i++) {
        play_buf[i * 2] = spk_buf[i];      // rx_audio.c's demod tone
        play_buf[i * 2 + 1] = 0;
    }
} else {
    memset(play_buf, 0, (size_t)n * 2 * sizeof(int32_t)); // TX, not keyed
}
```

`rx_audio_init()` is called once at startup (`minibitx.c`), right after
`cw_init()` — it needs the same VFO phase table already built for that.
This wiring hasn't changed since v1; everything described in §7/§8 below
is internal to `rx_audio_process()`.

## 7. Stage 1: the wide image-reject filter

### 7.1 The problem it fixes

Unlike sbitx's original receiver — which gets genuine single-sideband
selectivity from its crystal filter's fixed, physically asymmetric skirt
relative to the BFO (the same "filter method" used for TX sideband
generation, see
[`antialias_filter_design.md`](antialias_filter_design.md) §3) — v1's
narrow filter (§3) was a plain real-coefficient lowpass, applied
identically to the I and Q rails. Any filter built that way has a
frequency response that's mirror-symmetric around 0 Hz (`|H(-f)| ==
|H(f)|` for any real-coefficient filter): it could not distinguish a
signal 300 Hz above dial center from one 300 Hz below, because the
physical asymmetry that gives sbitx's receiver its single-sided
selectivity lives entirely in the analog IF chain, upstream of the
wideband, symmetric I/Q this pipeline hands to `rx_audio.c` (the same
I/Q the network SDR path uses). Practical effect, flagged during on-air
testing (2026-09): tuning across a CW station produced the expected
pitch change through zero beat, but the signal sounded equally strong on
both sides rather than being rejected on one.

A CPU-headroom check ruled out just leaving this to an external desktop
SDR app instead: minibitx was using ~7% of one Pi 4 core at the time,
comfortable room to spare, but more importantly a laptop-side app's DSP
only ever touches *its own* audio output — it can't reach back and
improve minibitx's own onboard speaker, which is the entire point of
`rx_audio.c` (a standalone receiver with no laptop attached). Fixing the
zero-beat symmetry problem for the onboard monitor has to happen here.

### 7.2 The fix: a complex bandpass filter

The standard fix is a phasing-method (Hilbert-transform-related) filter:
replace the real, symmetric lowpass with a **complex** filter
`h[m] = hr[m] + j·hi[m]`, applied to the complex baseband signal via
complex convolution:

```
yr[n] = (hr*xr)[n] - (hi*xi)[n]
yi[n] = (hr*xi)[n] + (hi*xr)[n]
```

Construction: start from a real, symmetric lowpass prototype `h_lp[m]`
(`scipy.signal.remez`, the same technique §"9. Implementation" of
[`antialias_filter_design.md`](antialias_filter_design.md) used), then
modulate it by a complex exponential referenced to the filter's own
center tap:

```
h[m] = h_lp[m] * exp(j*2*pi*Fpass*m/Fs),   m = tap_index - center_tap
```

A real symmetric lowpass has a passband straddling 0 Hz,
`[-Fpass, +Fpass]`. Shifting it by exactly `+Fpass` slides that passband
to be one-sided, `[0, +2*Fpass]` — content on the wanted side of dial
center passes, content on the other side falls in what was the lowpass's
own stopband and gets rejected. Referencing the modulating phase to the
filter's *own center tap* (not absolute sample index 0) is what keeps
both halves of the result exploitably symmetric: `hr[m]` comes out
EVEN-symmetric (`hr[m] == hr[-m]`, same as `h_lp` itself, since cosine is
even), and `hi[m]` comes out ODD-symmetric (`hi[m] == -hi[-m]`, with an
exact zero at the center tap, since sine is odd). Both were confirmed
numerically to machine precision (residual 0.0) before any C was
written.

### 7.3 Choosing the tap count and transition width

Harris' FIR-length rule of thumb (`N ≈ (Fs/transition_Hz)·(Astop_dB/22)`,
computed at `Fs=96000`) shows tap count exploding for a transition close
to DC — the kind that would give the best rejection right at zero beat:

| transition | stopband | approx. taps |
|---|---|---|
| 300 Hz | 40 dB | ~582 |
| 200 Hz | 40 dB | ~873 |
| 100 Hz | 40 dB | ~1745 |
| 50 Hz  | 60 dB | ~5236 |

A modest 300-500 Hz transition at 30-40 dB stays in the low hundreds of
taps instead. Decimating the sample rate before this filter (precedented
in this codebase by `decim48k.c`'s existing 96kHz→48kHz path) would make
a sharper design cheaper, but wasn't needed: the modest design below
already costs well under 1% of one Pi 4 core, so there was no pressure to
add that complexity for a design point that already works.

Crucially, this formula has **no `Fpass` term at all** — tap count is
set purely by the transition width, not by where the passband edge
sits. That's the fact that drove the v2→v3 change in §7.6: widening the
passband from 600Hz to 3000Hz cost nothing.

Design point actually built (v3): `Fpass=1500 Hz`, `Fstop=1900 Hz`
(`scipy.signal.remez(327, [0, 1500, 1900, 48000], [1, 0], weight=[1, 10],
fs=96000)`), landing at **327 taps**, 1.72 dB passband ripple, -40.0 dB
worst-case stopband on the real prototype — actually a few taps *fewer*
than v2's narrower `Fpass=300/Fstop=700` design (359 taps), confirming
the no-`Fpass`-dependence claim above. After the center-tap-relative
modulation described in §7.2, this passes baseband content from 0 up to
+3000 Hz above dial center (the "wanted" side — deliberately wide, see
§7.6) and rejects the mirror side, with rejection depending on how far
from dial center a station sits — there's no way around that: right at
zero beat, `+0 Hz` and `-0 Hz` are the same frequency, so no filter of
any length can separate them. The numeric image-rejection check
(`scipy`, dual synthetic tones at `+f`/`-f`, then bench-reproduced in C
— see §7.4) came out as:

```
f=  100 Hz: rejection ≈  5.1 dB   (near zero beat - fundamentally limited)
f=  300 Hz: rejection ≈ 26.8 dB
f=  500 Hz: rejection ≈ 66.6 dB
f= 1000 Hz: rejection ≈ 41.7 dB
f= 1500 Hz: rejection ≈  3.8 dB   (see the folding note in §8.3 - this
                                    isn't stage 1 getting worse, it's
                                    both the "+f" and "-f" tones already
                                    being 30-40dB down from a dial-center
                                    reference by this offset once stage
                                    3's cascade, §8.5, is this sharp - the
                                    ratio between two already-quiet
                                    signals stops being a meaningful
                                    number)
```

(These numbers are measured with stage 3, §8, also in the loop — since
that's what `test_rx_audio.c`'s case D now measures end-to-end, with the
4-section cascade of §8.5 and a fully-settled 2-second measurement
window, §8.6. Stage 1 alone, evaluated directly in Python against the
complex filter only, shows the expected monotonic improvement out to
deep rejection by 1500-2000Hz with no such dip; §8.3 explains where the
dip in the full chain comes from.)

### 7.4 Implementation and verification

`rx_audio.c` embeds two pre-computed coefficient tables, `ssb_hr[327]`
and `ssb_hi[327]` (the even/odd-symmetric real and imaginary halves from
§7.2), and a small `struct ssb_filter_state` holding double-length
history buffers for the I and Q rails — the same double-write trick
`antialias.c` uses (`hist[pos]` and `hist[pos+TAPS]` always equal, so a
`TAPS`-long read never needs to wrap), for the same reason: it keeps the
inner convolution loop branch-free so gcc's `-O3` can autovectorize it.
`ssb_filter_apply()` expands the complex convolution into two real
sub-sums reused across both output rails:

```c
i_out = sum(hr*hist_i) - sum(hi*hist_q)
q_out = sum(hr*hist_q) + sum(hi*hist_i)
```

That's 4 real convolutions of length 327 (~1308 multiply-adds/sample,
~126M/sec at 96kHz) — this implementation does **not** hand-fold the
even/odd symmetry into half-length loops the way the theory in §7.2
would allow (summing/subtracting mirrored samples before multiplying, as
`antialias_filter_design.md` §"7. Coefficients" describes for the real
antialias filter). `antialias.c` itself didn't take that manual-folding
route either — a mirrored-index access pattern is harder for gcc to
autovectorize than the plain sequential loop, and at this tap count the
unfolded version is nowhere near a real constraint. Same answer either
way: ride the compiler's autovectorization on a wraparound-free
double-buffered loop, not manual coefficient-halving — consistent with
how `antialias.c` already chose to spend that same tradeoff.

One subtlety that mattered getting this right: the coefficient tables
had to be stored **time-reversed** relative to how `scipy.signal.remez`
produces them, to match the direction `antialias_apply()`'s double-buffer
loop actually reads history in (oldest-to-newest at increasing array
index). Reversing an asymmetric (odd-symmetric `hi[]`) filter's tap order
without accounting for that would have silently produced the *mirror
image* of the intended filter — passing the wrong sideband instead of
the wanted one. This was caught before writing any hardware-facing code
by simulating the exact C loop structure in Python against the
straightforward `y[n] = sum_k h[k]*x[n-k]` reference definition (max
error on the order of 1e-15, floating-point noise), and re-confirmed
after each C rewrite (v2 and v3) by extending `test_rx_audio.c` with a
case D (image rejection at several `f`, comparing the AGC envelope for a
`+f` tone against its `-f` mirror). v2's numbers matched the Python
design prediction to one decimal place; v3's still track the same
qualitative curve, now shaped by stage 3 too (§7.3's table, §8.3).

### 7.5 Which side is "wanted"

`+Fpass` in §7.2's modulation was an arbitrary choice — it keeps the
side *above* dial center and rejects *below*. On-air (2026-09, against
the v2 design): stations tuned below roughly a 500Hz sidetone pitch
faded out; ones above 700Hz stayed strong out to 1300Hz. Whether that's
the orientation an operator actually wants to zero-beat toward is still
open — flipping it is a one-line sign change (`-Fpass` instead of
`+Fpass`) followed by regenerating the coefficient tables, not a
redesign. Still undecided as of v3 (see §10).

### 7.6 Widening it: decoupling image rejection from selectivity

v2 used this same complex filter with `Fpass=300Hz` — i.e. the filter's
own passband edge was *also* serving as the final CW selectivity width.
On-air listening (2026-09) found this made signals sound soft well
before the edge of the nominal 600Hz-wide passband: the filter's own
equiripple roll-off was eating directly into what should have been a
clean, flat "wanted" region, because one filter was being asked to do
two jobs that don't actually need to be coupled — reject the image
(needs asymmetry, but can tolerate a wide passband) and shape the final
bandwidth (needs no asymmetry at all, since by definition it happens
*after* the image question is settled).

The more conventional phasing-receiver architecture keeps these
separate: a wideband image-reject network (spec'd across however much
audio range you ever want, flat within that range) followed by
whatever audio-domain selectivity filtering an operator actually wants,
applied to the resulting single real channel. §7.3's Harris formula is
what makes this free rather than a tradeoff: since tap count depends
only on transition width, not passband width, widening `Fpass` from 300
to 1500 Hz cost nothing (327 taps vs. 359 — actually slightly *fewer*).
1500 Hz was chosen with headroom for a possible future onboard SSB
monitor in mind, not just today's CW use, though building one is a
separate, much bigger project (SSB needs its own audio-domain detection
scheme, and digital-mode decode like FT8 is WSJT-X's job entirely, not
something to replicate here) — widening this filter doesn't imply that
work, it just means the one genuinely hard piece (image rejection)
wouldn't need a redesign if that happens later.

The actual "how narrow" knob moved to a new stage 3, §8.

## 8. Stage 3: the narrow (post-demodulation) selectivity filter

### 8.1 Why a biquad, not another FIR

By the time stage 2 (§3) has mixed the image-rejected signal up to
`CW_PITCH_HZ` and taken the real part, there's no more mirror-image
ambiguity left to design around — this stage is an ordinary, symmetric
audio bandpass, the same kind of thing a classic CW rig's analog audio
peak filter has always been. Two reasons this is built from two-pole IIR
resonator sections (RBJ Audio EQ Cookbook's "constant 0dB peak gain"
bandpass, now cascaded four deep — §8.4/§8.5) rather than another
`scipy.signal.remez` FIR like every other filter in this codebase:

- **Cheap adjustability.** `rx_audio_set_filter_bw()` needs to change the
  filter's width at runtime (a future tuning encoder or CAT/rigctl
  extension - see §10). Recomputing a biquad's 5 coefficients from a new
  `Q` is a few trig calls; recomputing an FIR's whole coefficient table
  in real time is not something to do from an audio callback.
- **It's simply cheap.** A single biquad is ~5 multiply-adds per sample
  total, vs. hundreds for an equivalently narrow FIR (Harris' formula
  again: a genuinely narrow transition costs real taps, same as §7.3 -
  a resonator sidesteps that by not being a linear-phase design at all,
  which doesn't matter here since nothing downstream cares about phase
  linearity in a single audio tone).

Formula (`f0 = CW_PITCH_HZ`, `Q = f0 / bandwidth_hz`):

```
w0 = 2*pi*f0/Fs,  alpha = sin(w0)/(2*Q)
b0 =  alpha/a0,  b1 = 0,  b2 = -alpha/a0
a1 = -2*cos(w0)/a0,  a2 = (1-alpha)/a0,  a0 = 1+alpha
```

Default bandwidth `RX_AUDIO_FILTER_DEFAULT_BW_HZ = 300` (untested
starting point, same caveat as every other constant in this design —
expect to retune by ear) is the width of the whole cascade, not any one
section — §8.5 covers how a single section's own width is derived from
it. For a single section built directly for 300Hz (i.e. before the
cascade correction existed), Python confirmed measured -3dB points at
562-867Hz (305Hz wide, target 300Hz), poles at magnitude 0.990
(comfortably inside the unit circle — stable); §8.4 covers why that
single section, on its own, turned out not to be enough.

### 8.2 Verification: are the two stages actually decoupled?

The whole point of §7.6's change was to make "how wide is the image-reject
filter" and "how narrow is the final selectivity" into two independent
knobs. `test_rx_audio.c`'s case E checks this directly: a tone at dial
center (always at stage 3's peak, wherever stage 1's edges are) should
barely change if stage 3's width changes, while a tone well inside stage
1's wide passband but well outside stage 3's default width should change
a lot. Measured:

```
dial-center tone:          narrow(300Hz)=0.862  wide(2000Hz)=0.862  (unchanged)
+1000Hz-offset tone:       narrow(300Hz)=0.035   wide(2000Hz)=0.784  (+27.0dB)
```

Confirms it: stage 1's own attenuation of the +1000Hz tone doesn't
change when stage 3's width changes (as it shouldn't - they're separate
filters now), and stage 3 alone accounts for the full swing (11.9dB with
the original single biquad section; 27.0dB now that stage 3 is the
4-section cascade of §8.5 — cascading sharpens the skirt in both
directions, so widening it back out buys back more too).

### 8.3 A subtlety: real-audio folding at large rejected-side offsets

§7.3's rejection table has an odd-looking dip at `f=1500Hz` (3.8dB,
worse than `f=1000Hz`'s 41.7dB) that isn't a stage-1 regression. Stage 2
mixes up by `+CW_PITCH_HZ` and takes the real part, and a real signal's
spectrum is inherently mirror-symmetric around 0Hz - so a baseband
offset of `f=-1500Hz` produces a mixed frequency of `700-1500=-800Hz`,
which folds to an audible `+800Hz` tone. That folded 800Hz sits much
closer to stage 3's 700Hz peak than the unfolded arithmetic suggests, so
stage 3 partially *recovers* it even though stage 1 rejected the
original tone deeply. This is an inherent consequence of using a real
(not complex) BFO mix + real narrow filter for stage 2/3, not a bug to
fix - and it only matters for offsets large enough to fold back near
`CW_PITCH_HZ` (roughly beyond `2*CW_PITCH_HZ`), well outside where a
real CW signal would be tuned in practice.

Now that stage 3 is the sharper 4-section cascade (§8.5), this dip looks
different but for a reason that isn't actually about folding getting
worse: at `f=1500Hz` both the wanted `+1500Hz` tone and the folded
`-1500Hz` artifact are now themselves 30-40dB down from a dial-center
reference, because the cascade's much narrower passband also rolls off
legitimate wanted-side content that far from `CW_PITCH_HZ` - it isn't
just rejecting the image any more, it's attenuating both sides toward
the noise floor. The *ratio* between two already-quiet signals stops
being a meaningful selectivity number at that specific offset, which is
why `test_rx_audio.c`'s case D docstring flags it as an expected
artifact rather than a regression to chase.

### 8.4 Why a single section wasn't enough

On-air listening against v3 surfaced a real report: two CW signals 3kHz
apart, tuned to the higher one, with the other one still clearly audible
at roughly the pitch its 3kHz offset should fold to. The first thing
checked was whether this was actually a stage-1 problem — §7.3's
equiripple stopband oscillates around a roughly *constant* level across
the whole rejected side rather than improving with distance from the
transition band (confirmed by scanning stage 1 alone from -500Hz to
-5000Hz in Python: rejection sat in the -40 to -46dB range throughout,
not meaningfully better at -5000Hz than at -500Hz). So being "3kHz away"
buys little from stage 1 by itself — a station 3kHz off is rejected
about as well as one 500Hz off. That's a real, inherent property of this
filter design, but it isn't the whole story, and it prompted the direct
question: why doesn't stage 3 clean up what stage 1 leaves behind?

The answer is that a single two-pole resonator's skirt is genuinely
gentle close-in, in a way its -3dB width doesn't advertise. Measured
directly (Python, the single-section biquad's own isolated frequency
response, `f0=700Hz`, `bandwidth=300Hz` so `Q≈2.33`):

```
1.0 bandwidths from center:  -5.9 dB
2.0 bandwidths from center: -11.6 dB
5.3 bandwidths from center: -17.0 dB
```

A 2-pole resonator only ever rolls off at 2 poles' worth of slope no
matter how far out you go — there's no equivalent of an FIR's stopband
floor that keeps improving with a wider transition. For the user's
scenario (an interferer at a pitch a few bandwidths away from
`CW_PITCH_HZ`), a single section was only ever going to claw back
another 15-20dB on top of whatever stage 1 already provided — nowhere
near enough once stage 1's own rejection at that spacing is already
limited by §7.3's roughly-constant equiripple floor.

### 8.5 Cascading: the bandwidth-correction math

The standard fix for a resonator's gentle skirt is the same one classic
analog CW audio filters use: put several identical sections in series.
dB is additive per stage, so N sections turn the single-section numbers
above into roughly `N×` the dB at the same offset — 4 sections turn
`5.3` bandwidths' `-17.0dB` into roughly `-68dB`. `NARROW_FILTER_SECTIONS`
is `4`, chosen as a reasonable middle point (2 would barely help; 8
starts trading a lot of extra group delay, §8.6, for diminishing
returns) — not yet bench-tuned against real listening.

The one thing cascading changes that isn't free: N identical sections,
each independently built for some bandwidth `B`, narrow the *combined*
-3dB width of the whole cascade well below `B` itself — that's the whole
point, it's what makes the skirt steeper, but it means naively reusing
the single-section formula per stage would make
`rx_audio_set_filter_bw(overall_bw_hz)` silently mean something
narrower than its name says. The classic result for N cascaded,
synchronously-tuned single-resonance stages gives the correction:

```
section_bw_hz = overall_bw_hz / sqrt(2^(1/N) - 1)
```

For `N=4` that factor is ≈0.4350 — each section has to be built about
2.3× *wider* than the overall width the caller actually asked for, so
that the combined cascade lands back on the requested width. This was
verified numerically against the *exact* digital biquad transfer
function (not just the idealized analog approximation the formula comes
from) via a bisection search for the true -3dB frequencies of the actual
cascaded difference equation, across target bandwidths of 100/200/300/
500/800/1200Hz — the ratio held at 0.434-0.4353 throughout, accurate to
about 0.2%, before being trusted in `narrow_filter_set_bandwidth()`.

### 8.6 Settling time: why the test harness needed a longer window

Once the cascade was in place and `test_rx_audio.c` extended with a case
F matching the user's actual reported scenario (dial-center wanted
signal vs. an interferer at `f=-3000Hz`, folding to ~2300Hz per §8.3),
the measured rejection (72.6dB) came in noticeably below the Python
full-chain prediction (82.6dB) — a gap worth chasing down before
trusting either number.

The cause turned out to be the test harness itself, not the filter: the
existing 1-second measurement window (unchanged since v1) was long
enough for every case up through the single-section stage 3, but not for
the 4-section cascade measuring a heavily-attenuated signal. Two things
now both contribute latency between "signal starts" and "AGC envelope
reflects steady state": each cascaded resonant section adds its own
ring-down, and the AGC's own 300ms release-time smoothing (§5) then has
to catch up to an envelope that's still decaying on top of that. Neither
alone was a problem before; stacked, they pushed convergence for a
heavily-rejected signal out past the 1-second window.

Measured directly with a 5-second, 1-second-chunked scratch test
(processing the interferer signal in successive 1-second calls and
reading the AGC envelope after each):

```
after 1s: rejection = 72.6 dB
after 2s: rejection = 82.7 dB
after 3s: rejection = 82.7 dB   (stable)
after 4s: rejection = 82.7 dB
after 5s: rejection = 82.7 dB
```

Convergence takes about 2 seconds for this specific heavily-attenuated
scenario, matching the Python prediction (82.6dB) almost exactly once
given that long. `test_rx_audio.c`'s measurement window is now 2 seconds
(`N=192000`) across every case, not just case F, so every reported
number in this doc and in the harness's own output reflects true
steady state rather than an under-settled snapshot. This is a property
of the 4-section cascade specifically — a future change to
`NARROW_FILTER_SECTIONS` or the AGC's release constant would be worth
re-checking against this same settling test.

## 9. Master output split (L/R independence)

Related fix, landed alongside the AGC work: the WM8731's `Master`
control turned out to have independent left/right volume registers, and
L/R physically feed two different destinations on this board — L drives
the local speaker/headphone audio amp (`rx_audio.c`'s output, and
`cw.c`'s TX sidetone), R drives the mainboard's diode mixer that
upconverts the DSP's TX carrier for the RF chain. `sound_set_local_monitor()`
now sets only the left channel, once, at startup, and is never touched
again; `sound_set_tx_drive()` sets only the right channel, from
`radio.c`'s `radio_tx_apply()`, for the duration of a TX burst. Before
this, both shared one value, and every TX/RX transition would silently
re-mute whichever purpose wasn't currently active — see §5, item 2, for
the debugging trail this caused.

## 10. What's not decided yet

- **BFO sign** (the sign passed to `vfo_start(&bfo, CW_PITCH_HZ, 0)`) is
  still an untested starting point, not a bench calibration — worth
  checking by ear now that audio is actually reaching a speaker.
- **Which side is "wanted"** (§7.5) — still arbitrary, still a one-line
  sign flip away from reversing.
- **`AGC_TARGET_AMPLITUDE`/`AGC_ATTACK_MS`/`AGC_RELEASE_MS`** are
  reasonable-looking starting points, not yet tuned against extended
  listening — particularly whether 300ms release feels right between
  CW characters, or pumps/lags noticeably.
- **Stage 1's design point** (`Fpass=1500`/`Fstop=1900`, 327 taps, -40dB
  stopband), **stage 3's default width** (300Hz), and
  **`NARROW_FILTER_SECTIONS`** (4, §8.5) are all reasonable starting
  points, not bench-tuned final answers — real on-air listening time may
  push any of them narrower, wider, toward more/fewer cascaded sections,
  or toward a sharper/decimated stage-1 design (§7.3) if this combination
  of rejection and audio "feel" isn't right yet.
- **Group delay / settling time** from the 4-section cascade (§8.6) is
  bench-verified in the AGC-envelope sense (rejection converges within
  ~2 seconds for a heavily-attenuated signal) but not yet checked by ear
  for anything that matters to a live operator — e.g. whether switching
  `rx_audio_set_filter_bw()` at runtime, or a signal fading in and out
  (QSB, or a station starting to send), now has a perceptible "catching
  up" lag that v1/v2/the single-section v3 didn't have.
- **Runtime control** — `rx_audio_set_volume()` and
  `rx_audio_set_filter_bw()` both exist but nothing calls either one yet;
  wiring them to a future physical encoder (the `mb-radio` panel app) or
  a CAT/rigctl extension is unstarted.
