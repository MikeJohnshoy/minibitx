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
apart, the un-tuned one still audible quite strongly — traced to a
resonator's skirt staying gentle no matter how many identical sections
get cascaded (§8.1). After a resonator cascade (~83dB on that scenario)
still didn't reach real-crystal-filter territory, stage 3 was replaced
outright with a fixed 8-pole elliptic (Cauer) design (§8.2) — same pole
count, ~1.9:1 shape factor (in the range of a real CW crystal filter),
~99dB on the same scenario once fully settled (§8.5). This is now a
committed, fixed design, not runtime-adjustable — `rx_audio_set_filter_bw()`
existed for one revision and is gone again (§8.2). A second on-air report
(2026-09) then found that sharp filter's own edges weren't audible while
tuning — traced to the AGC's envelope sampling stage 3's own output, so
its makeup gain was canceling out exactly the attenuation the operator
was trying to hear. Fixed in two steps: first by moving the envelope to
sample stage 2 (§8.7), which solved the reported problem but surfaced a
further, more severe consequence of the real-audio folding effect (§8.4);
then by moving it again, to the raw input I/Q before stage 1 even runs
(§8.8) — frequency-independent by construction, so no downstream stage's
selectivity (or interaction between them) can leak into the AGC's gain
at all. §8.8's fix recovers stage 3's full ~99dB selectivity in the
*actual heard output*, not just a debug reading, and reproduces the
folding artifact at its original, honest ~-17dB rather than hiding or
exaggerating it — bench-verified, not yet re-confirmed on air, and not
yet tested with two simultaneous signals in one buffer (§8.8, §10).
Volume is remotely controllable via the rigctld server's `l`/`L AF`
commands (`docs/04_remote_control_and_iq_output.md`).

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
   settled, so an ordinary symmetric filter narrows it down to comfortable
   single-signal copy, independent of stage 1 entirely. A fixed 8-pole
   elliptic design, chosen to approach a real CW crystal filter's shape
   factor. See §8.
4. **An AGC**, covered in §5.

Verified against synthetic I/Q before ever touching real hardware
(`test_rx_audio.c`, not part of the build): a carrier at dial center
produces a clean 698.3 Hz tone (target ~700 Hz); a carrier at +5kHz
offset comes out heavily attenuated by the time it reaches the output
(now rejected by both stage 1 and stage 3 — see §8.6). §5 covers what
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

An asymmetric single-pole envelope follower tracks a magnitude estimate
(fast attack, slow release), and the applied gain is
`AGC_TARGET_AMPLITUDE / envelope`, clamped at `AGC_MAX_GAIN`. Verified
with an extended `test_rx_audio.c` before delivery: a case at the
bench-measured real amplitude (0.003) now produces output
indistinguishable from a full-scale (1.0) synthetic carrier — the exact
regression the bug above represents.

Which signal `envelope` tracks changed twice after this was first
shipped. v3 initially tracked stage 3's own output (the narrow-filtered
audio), on the reasoning that this made the envelope reflect the
combined selectivity of both filtering stages — which is what
`rx_audio_debug_agc_envelope()`'s callers wanted to measure at the time
(§7.4/§8.6). That reasoning was sound for the *debug reading*, but it had
a real cost for what actually reached the operator's ears: because gain
is computed from the same signal it's then applied to, the AGC couldn't
help but cancel out exactly the loudness variation stage 3's own
selectivity was supposed to produce. §8.7 covers the on-air report this
caused and the first fix attempted (the envelope moved to stage 2's
output), including a further consequence of §8.4's folding effect that
fix surfaced; §8.8 covers the fix that actually resolved it — the
envelope now tracks the **raw input I/Q**, before stage 1 even runs, so
it's frequency-independent and no downstream stage's own selectivity (or
any interaction between them) can leak into the AGC's gain at all.

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
f=  100 Hz: rejection ≈   4.5 dB   (near zero beat - fundamentally limited)
f=  300 Hz: rejection ≈  35.5 dB
f=  500 Hz: rejection ≈  46.2 dB
f= 1000 Hz: rejection ≈  34.1 dB
f= 1500 Hz: rejection ≈ −17.0 dB   (see the folding note in §8.4 - the
                                     "wrong" side measures LOUDER than
                                     the "wanted" side here, and that's
                                     still not stage 1 regressing)
```

(These numbers are measured with stage 3, §8, also in the loop — since
that's what `test_rx_audio.c`'s case D now measures end-to-end, with the
current fixed elliptic design of §8.2 and a fully-settled 3-second
measurement window, §8.5. Stage 1 alone, evaluated directly in Python
against the complex filter only, shows the expected monotonic
improvement out to deep rejection by 1500-2000Hz with no such dip; §8.4
explains where the dip - now an outright sign flip - in the full chain
comes from.)

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
qualitative curve (§7.3's table) — and, since §8.7 moved the AGC's
envelope to sample stage 2, case D is back to being a clean stage-1-only
measurement again, no longer shaped by stage 3's own folding behavior the
way it briefly was (§8.4, §8.7).

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

### 8.1 Two false starts: a resonator, then a resonator cascade

By the time stage 2 (§3) has mixed the image-rejected signal up to
`CW_PITCH_HZ` and taken the real part, there's no more mirror-image
ambiguity left to design around — this stage is an ordinary, symmetric
audio bandpass, the same kind of thing a classic CW rig's analog audio
peak filter has always been. v3's first cut built it from a two-pole IIR
resonator (RBJ Audio EQ Cookbook's "constant 0dB peak gain" bandpass
formula), runtime-adjustable via `rx_audio_set_filter_bw()` (a few trig
calls per retune - cheap, unlike recomputing an FIR's whole coefficient
table from an audio callback).

On-air listening against v3 surfaced a real report: two CW signals 3kHz
apart, tuned to the higher one, with the other one still clearly audible
at roughly the pitch its 3kHz offset should fold to. The first thing
checked was whether this was actually a stage-1 problem — §7.3's
equiripple stopband oscillates around a roughly *constant* level across
the whole rejected side rather than improving with distance from the
transition band (confirmed by scanning stage 1 alone from -500Hz to
-5000Hz in Python: rejection sat in the -40 to -46dB range throughout,
not meaningfully better at -5000Hz than at -500Hz). So being "3kHz away"
buys little from stage 1 by itself. That's real, but it isn't the whole
story: why doesn't stage 3 clean up what stage 1 leaves behind?

Because a single two-pole resonator's skirt is genuinely gentle
close-in, in a way its -3dB width doesn't advertise. Measured directly
(Python, `f0=700Hz`, `bandwidth=300Hz` so `Q≈2.33`): only -5.9dB down at
1 bandwidth from center, -11.6dB at 2, -17.0dB at 5.3. A 2-pole resonator
only rolls off at 2 poles' worth of slope no matter how far out you go.

The standard fix for a resonator's gentle skirt is cascading several
identical sections - dB is additive per stage, so 4 sections should turn
that -17dB into roughly -68dB at the same offset. This became
`NARROW_FILTER_SECTIONS = 4`, each section built *wider* than the
overall target (`section_bw = overall_bw / sqrt(2^(1/N) - 1)`, a
correction verified to ~0.2% against the exact digital transfer
function, not just the idealized analog approximation) so the combined
cascade still landed on the requested -3dB width. It worked, and it took
the 3kHz-apart scenario from ~59dB to ~83dB (§8.6) - a real improvement,
delivered first.

But a resonator cascade has a ceiling a single formula doesn't reveal:
no matter how many *identical, synchronously-tuned* sections get added,
the shape stays a smooth, rounded lobe - dB accumulates, but the curve
never develops a real equiripple "wall" the way a crystal ladder filter
does. Measured against the full stopband sweep this codebase now uses to
judge shape factor (§8.2), the 4-section cascade never even reached
-60dB within a 3kHz search window - its own -6dB-to-"as deep as it gets"
ratio doesn't resolve to a clean shape factor at all, because it simply
doesn't have a floor to reach. Good enough to ship once, not good enough
to call "crystal-filter-grade."

### 8.2 The fix: a fixed 8-pole elliptic (Cauer) design

An elliptic filter spends its poles completely differently from a
resonator cascade: equiripple ripple across the passband, and
transmission zeros placed deliberately right at the band edges, buying
the steepest possible transition for a given order - which is much
closer to what a real crystal ladder filter's Cohn/Dishal synthesis
actually does. For the *same* 8-pole cost as the old cascade
(`scipy.signal.ellip(4, 0.5, 50, [(700-150)/48000, (700+150)/48000],
btype='bandpass', output='sos')` at `Fs=96000` - order 4 means 4
second-order sections, 8 poles total, 0.5dB passband ripple, 50dB
stopband target), the measured shape factor is:

```
-6dB bandwidth:  343 Hz
-60dB bandwidth: 649 Hz
shape factor:    1.89:1
```

That's in the same range real CW crystal/mechanical filters land in
(typically 1.5-2:1), and the cascade never got anywhere near it. See the
published filter-response artifact for the two shapes plotted together -
the cascade's smooth rounded lobe against the elliptic's near-rectangular
mesa with visible equiripple notches right at the transition.

The cost of an elliptic design is that its coefficients aren't a simple
trig formula the old RBJ biquad's were - they come from solving elliptic
integrals, which `scipy.signal.ellip` did once, offline. That's not
something to redo from an audio callback, so **this stage is now a fixed
design**, the same way stage 1's FIR coefficients are: 4 biquad sections
with individually different, precomputed `{b0,b1,b2,a1,a2}` coefficients
(`narrow_filter_coeffs[]` in `rx_audio.c`), cascaded through the same
`biquad_apply()` stage 3 already used. `rx_audio_set_filter_bw()`
existed for exactly one v3 revision and is gone again - a deliberate
choice (see rx_audio.c's "Why elliptic, and why fixed" and rx_audio.h),
not an oversight. scipy's `sos` output normalizes each section's `a0` to
`1.0` (confirmed to `~1e-16` before trusting it), matching how
`biquad_apply()` is written (no explicit division by `a0`).

### 8.3 Cost check: group delay and ring time

Nothing is free - an elliptic's steep transition trades against phase
linearity near the band edges, so this was bench-checked (Python,
`scipy.signal.group_delay`, summing each cascaded section's own group
delay rather than forming one high-order polynomial, which loses
precision fast for a narrowband design) before committing to it:

```
                          4-section cascade    8-pole elliptic
group delay @ CW_PITCH_HZ:     1.9 ms               2.6 ms
peak group delay (near edges): 2.0 ms              10.4 ms
ring-down after a 50ms dit,
  to -40dB:                    4.8 ms              10.9 ms
```

The number that actually matters for "does copy sound delayed" - group
delay right at the tone itself - barely moves (well under 1ms of real
difference, nowhere near the ~10ms threshold where a human notices audio
lag). The cost shows up specifically near the passband edges, where the
elliptic's equiripple phase behavior concentrates: peak group delay
roughly quintuples, and a simulated CW dit rings about 2× longer before
decaying to -40dB. Both numbers are still comfortably under any real
keying speed's element duration (a dot at 40 WPM is ~30ms), so this
wasn't expected to be audible - worth confirming by ear once this is on
real hardware, not just asserted from the bench numbers (see §10).

### 8.4 A subtlety: real-audio folding, now more extreme

§7.3's rejection table has a striking result at `f=1500Hz`: rejection
goes *negative* (-17.0dB) - the "wrong" (mirror-image) side measures
**louder** than the "wanted" side. That is not stage 1 regressing, and
not a new bug - it's the same real-audio folding effect first documented
against the resonator cascade, taken further by a much sharper stage 3.

Stage 2 mixes up by `+CW_PITCH_HZ` and takes the real part, and a real
signal's spectrum is inherently mirror-symmetric around 0Hz - so a
baseband offset of `f=-1500Hz` produces a mixed frequency of
`700-1500=-800Hz`, which folds to an audible `+800Hz` tone, sitting right
at the edge of stage 3's now much narrower passband (-6dB edges at
roughly 528/872Hz). Meanwhile the "wanted" `f=+1500Hz` tone's own pitch,
2200Hz, lands deep in the elliptic's scalloped stopband floor (§8.2's
plot). The folded artifact, being much closer to `CW_PITCH_HZ` after
folding, survives stage 3 *better* than the tone that was actually
supposed to be on the wanted side - enough to flip the sign entirely,
where the old resonator cascade only ever softened the number toward
"unremarkable." This is an inherent consequence of a real (not complex)
BFO mix + real narrow filter for stage 2/3, not something a sharper
stage 3 can fix (a sharper stage 3 is what made it *more* visible) - and
it only matters for offsets large enough to fold back near `CW_PITCH_HZ`
(roughly beyond `2×CW_PITCH_HZ`), well outside where a real CW signal
would be tuned in practice. `test_rx_audio.c`'s case D docstring flags
it directly rather than letting it read as a regression.

### 8.5 Settling time, re-measured for the sharper filter

This codebase already learned once (against the resonator cascade) that
a sharper stage 3 needs a longer measurement window than v1/v2 ever did
- the filter's own ring-down grows, and the AGC's 300ms release
smoothing (§5) then has to catch up to a still-decaying envelope on top
of that. Because the elliptic design rings a little longer per-filter
than the cascade did (§8.3: 10.9ms vs 4.8ms to -40dB), this was
re-checked rather than assumed to still be fine at the previous 2-second
window - the same "verify, don't just extrapolate" discipline this
constant kept earning through the whole v3 effort.

Measured directly with a 5-second, 1-second-chunked scratch test on the
real 3kHz-apart scenario (`f=-3000Hz`, folding to ~2300Hz):

```
after 1s: rejection = 73.4 dB
after 2s: rejection = 96.7 dB
after 3s: rejection = 99.2 dB   (stable)
after 4s: rejection = 99.2 dB
after 5s: rejection = 99.2 dB
```

Convergence now takes closer to 3 seconds, not 2.
`test_rx_audio.c`'s measurement window is `N=288000` (3 seconds) across
every case as of this revision, so every reported number in this doc and
in the harness's own output reflects true steady state. This is a
property of the specific filter in use - a future change to the elliptic
design's order/ripple/stopband targets, or the AGC's release constant,
would be worth re-checking against this same settling test rather than
assuming the window is still long enough.

### 8.6 Verification summary

`test_rx_audio.c`'s case F (the real scenario that motivated all of
§8): wanted signal at dial center vs. an interferer 3kHz away, folding
to ~2300Hz. Rejection climbed steadily as stage 3 got sharper:

```
v3 initial (1 resonator section):   ~59 dB
resonator cascade (4 sections):     ~83 dB
current (8-pole elliptic, fixed):   ~99 dB
```

Case E confirms the elliptic's own shape in isolation - offsets chosen
to stay inside stage 1's flat passband, so what's measured is stage 3
alone: essentially flat (+1.8dB) at 150Hz off `CW_PITCH_HZ`, falling
steeply to -32.8dB by 300Hz off, and down in a scalloped -50 to -70dB
floor by 900-1200Hz off - the near-rectangular shape §8.2 predicted,
confirmed end-to-end in C rather than just in Python.

(This section's numbers predate §8.7's AGC fix - they were measured
against `rx_audio_debug_agc_envelope()`, which at the time reflected
stage 3's own output. Kept here as the historical record of stage 3's
*raw* shape; §8.7 has the current, actually-audible numbers.)

### 8.7 On-air report: the sharp filter's edges weren't audible - the AGC was sampling the wrong stage

(This section documents the first fix attempted, and the new problem it
surfaced. §8.8 has the better fix that superseded it - kept here as the
record of why the simpler-looking "just sample one stage earlier" idea
wasn't quite right yet.)

A second on-air report (2026-09), after §8.2's elliptic redesign shipped:
tuning across a CW signal, the narrow filter's ~300Hz-wide skirt - real,
bench-verified, ~99dB deep by §8.6 - wasn't noticeable. Not "less sharp
than expected"; barely there at all.

**Root cause.** Stage 4's AGC (§5) tracked `|narrowed|` - stage 3's own
output - and applied `gain = AGC_TARGET_AMPLITUDE / agc_env` to that same
signal. That's a closed loop: whatever stage 3 did to a tone's amplitude,
the AGC measured and undid in the same breath, before it ever reached the
codec. This was deliberate, and reasonable, for one thing:
`rx_audio_debug_agc_envelope()`'s job of reflecting the narrow filter's
*true, gain-independent* selectivity for bench testing (§7.4, §8.6) - but
it meant that reading and the *actual PCM output* were two very different
things, and only the reading showed selectivity. minibitx has no ADC/RF-
level AGC at all (`sound.c`'s `RX_CAPTURE_GAIN_PERCENT` is a fixed analog
gain stage, set once at startup, never touched again) - this envelope
follower is the *only* AGC anywhere in the project, and it exists purely
for the local CW monitor's own output level, decoupled from everything
else minibitx does (the raw I/Q reaching HPSDR/UAC2 is untouched by it).

**The fix.** `agc_env` now tracks `|audio|` - stage 2's output, i.e.
*before* stage 3's narrow filter - while the resulting gain is still
applied to `narrowed` (stage 3's output) for the actual PCM sample. This
breaks the closed loop: the AGC now normalizes for general passband
conditions (still its original job - real signal/band-noise levels
bench-measured 2025-09 swung across nearly three orders of magnitude, §5)
without being a function of the one filter's attenuation the operator is
trying to hear.

**Verified (`test_rx_audio.c`, now measuring actual PCM output for
anything meant to show stage 3's effect, not the debug envelope - see
its own updated file header):**

- **Case E** (a single tone, swept across offsets that stay inside stage
  1's flat passband - the realistic "tuning across a lone CW signal"
  case the report was actually about): output now follows stage 3's real
  shape almost exactly - flat through ±150Hz (0.0dB), a steep knee by
  200-450Hz off (-11.8dB, -33.8dB, -51.9dB), then the expected
  equiripple-scalloped floor out to -67.9dB by 1200Hz off. This is the
  direct confirmation the fix solves the reported problem.
- **Case B** (+5kHz, well outside stage 1's own passband): output RMS
  now drops -50.3dB vs. an in-band carrier - real attenuation reaching
  the codec, not normalized away.
- **Case D** (stage-1 image rejection, `+f` vs `-f`): reads cleanly again
  now that the envelope no longer includes stage 3's folding-driven
  asymmetry - a sensible 4.7 -> 18.8 -> 39.6 -> 43.5 -> 41.2dB
  progression, no more of the `+1500Hz` row's negative (image-louder-
  than-wanted) reading §8.4 described.
- **Case F** (the real two-signals-3kHz-apart scenario): now measures
  ~57dB via actual output, honestly *less* than stage 3's own raw ~99dB
  (still true, still bench-verified against the debug envelope - see the
  note added to §8.6). The gap is the AGC-desense tradeoff of moving the
  envelope upstream: the interferer at -3000Hz is also deep in stage 1's
  own rejection zone, so the AGC's stage-1+2-derived envelope is smaller
  for that scenario too, and its makeup gain rises to compensate - giving
  back roughly the portion of attenuation that happened *before* the
  AGC's new reference point. Case E, not Case F, is the number that
  answers the original report - a single signal with nothing else in
  stage 1's passband is the common case, and it now works.

**A new problem this surfaced, not yet resolved.** Re-running Case D's
`+f`/`-f` comparison against actual PCM output (rather than the
envelope) at `f=1500Hz` - right where §8.4's real-audio folding effect
puts `-1500Hz`'s folded tone (~800Hz) close to stage 3's passband edge,
while `+1500Hz`'s own folded tone (~2200Hz) sits deep in stage 3's
stopband floor - shows the wanted (`+1500Hz`) side playing back
**~58dB quieter** than the unwanted mirror-image side, not just
mismatched by a few dB:

```
f=1500Hz:  +f (wanted):  output RMS = 430,572
           -f (image):   output RMS = 348,122,863   (~58dB louder)
```

Mechanism: stage 1 already attenuates the `-1500Hz` image heavily (by
design - that's its job), so the AGC's stage-1+2-derived envelope reads
very small for that case, and makeup gain rises to compensate - same as
any weak in-passband signal would get boosted. But because the resulting
*folded* tone (~800Hz) sits near stage 3's passband center, stage 3
barely attenuates it on the way out - so that large makeup gain isn't
being "spent" suppressing a genuinely strong signal, it's amplifying an
already-mostly-rejected folding artifact back up to near full loudness.
The pre-fix design didn't show this as a *loudness* problem because
`gain = target / envelope-of-the-same-signal` always self-normalizes
everything to the same target regardless of value - both sides of this
same `f=1500Hz` case would have played at similarly loud, unhelpfully
undifferentiated volume before (consistent with the original "can't hear
any edges" report). Post-fix, the *sign* of the mismatch is worse: the
correct signal goes quiet and a phantom image 1500Hz away goes loud,
which reads to an operator as a spurious strong signal appearing out of
nowhere rather than as uniform loudness. Only checked at `f=1000Hz` and
`f=1500Hz` so far (both show it, `-9.0dB` and `-58.2dB`) - the actual
boundary of where this starts, how narrow the affected zone is, and
whether it's audible in normal tuning (an operator would have to tune
roughly `CW_PITCH_HZ` or more past where they'd expect a "quiet edge",
in the wrong direction) all need a proper sweep before this is called
understood, let alone fixed. See §10.

### 8.8 A better fix: sample the raw input instead

§8.7's fix traded one problem for another - both stage 3-sampling and
stage 2-sampling tie the AGC's gain to a signal that some upstream
stage has *already* shaped asymmetrically by frequency (stage 3's own
selectivity in the first case, stage 1's image rejection in the second),
so either the AGC cancels out the very thing you're trying to measure
in the first place, or its makeup gain compensates for one stage's
attenuation in a way that fights a *different* stage's attenuation of
the resulting folded tone. The idea that actually breaks this pattern
(suggested during the write-up of §8.7's open problem): sample `agc_env`
from `sqrt(i^2+q^2)` of the **raw input I/Q**, before stage 1 runs at
all.

The reasoning: for a single complex baseband tone, that raw magnitude is
frequency-independent - a unit-amplitude tone reads the same magnitude
whether it's sitting at dial center, at `+1500Hz`, or at its `-1500Hz`
mirror image, since `sqrt(cos^2+sin^2) = 1` regardless of which
frequency the cosine/sine pair is rotating at. Unlike stage 1's or stage
2's output, the raw input carries none of any downstream stage's own
frequency-selective shaping - so the AGC's gain stops being a function
of *which* frequency produced the energy it's measuring, and starts
being a true "how much is in the whole captured band right now" reading
- the most literal version of "why an AGC exists at all" (§5). Every
later stage's frequency-dependent behavior - stage 1's image rejection,
stage 3's narrow selectivity, and the real-audio folding interaction
between them (§8.4) - now survives into the final output completely
undiluted, because gain no longer varies with any of it.

**Verified (`test_rx_audio.c`, same methodology as §8.7 - PCM output for
anything frequency-dependent):**

- **Case D** (`+f`/`-f` image rejection): now exactly reproduces the
  *original*, pre-any-AGC-placement-change numbers - `4.5, 35.5, 46.2,
  34.1, -17.0 dB` at `100/300/500/1000/1500Hz` - because the raw-input
  envelope is identical (`1.000000`) for every `+f`/`-f` pair tested, so
  the AGC contributes zero asymmetry of its own. The `f=1500Hz` folding
  artifact (§8.4) is back to its honest, original `-17.0dB` - not hidden
  (§7's original design), not exaggerated to `-58dB` (§8.7's stage-2
  fix) - just measured faithfully, because the AGC is no longer part of
  the story either way.
- **Case E** (single tone, stage 1's flat passband, isolating stage 3):
  effectively unchanged from §8.7's numbers (0.0, -10.2, -32.8, -51.9,
  -49.6, -58.7, -67.8 dB) - expected, since the raw-input envelope was
  already constant across this offset range for the same reason §8.7's
  stage-2 envelope was. This case still directly confirms the original
  on-air report is fixed.
- **Case F** (two signals 3kHz apart): now measures **99.2dB**, matching
  stage 3's own raw selectivity (§8.6) exactly, in the *actual heard
  output* - recovering the full number §8.7's intermediate fix could
  only get to ~57dB on, since the raw-input AGC no longer lets stage 1's
  attenuation of the interferer leak into the makeup gain and partially
  compensate it back out.
- **Case B** (+5kHz): -91.0dB - a bit deeper than §8.7's -50.3dB, for the
  same reason as case F: nothing upstream of the raw input is inflating
  the AGC's gain to compensate for stage 1's own attenuation of this
  offset.
- **Settling time**: re-checked the same way as §8.5/§8.7 (chunked
  measurement at 1s/2s/3s on case F) - stable at 99.2dB already by 1s.
  For these synthetic single-tone cases the raw-input envelope doesn't
  need to settle at all (it's a constant), so the only settling left is
  stage 3's own physical ring-down, unchanged from §8.3/§8.5 and well
  inside the window's 3s margin.

One side effect, same in kind as §8.7's (expected, not a bug): a second,
completely unrelated signal anywhere in the whole captured passband -
not just stage 1's ~1500-1900Hz window, the *entire* band the antialias
filter and crystal filter pass through to the ADC - now pulls `agc_env`
up and the wanted signal's makeup gain down. This is "AGC desense" in
its most literal form, the same behavior a real receiver's front-end AGC
has, and arguably the more correct trade-off of the two tried so far:
it's what an AGC derived from actual RF/IF energy would do, rather than
one derived from a specific demodulator stage's necessarily narrower
view. Not yet checked with two *simultaneous* signals in one buffer
(every case here still tests one tone per `rx_audio_process()` run,
`rx_audio_init()` between them) - worth doing before calling this fully
verified.

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
  stopband) and **stage 3's elliptic design point** (order 4, 0.5dB
  ripple, 50dB stopband target, 300Hz -3dB width, §8.2) are reasonable
  starting points, not bench-tuned final answers — real on-air listening
  time may push either narrower, wider, toward different ripple/stopband
  targets, or toward a sharper/decimated stage-1 design (§7.3) if this
  combination of rejection and audio "feel" isn't right yet. Since stage
  3 is now a fixed design (§8.2), changing it means regenerating
  `narrow_filter_coeffs[]` from Python and rebuilding, not a runtime
  knob.
- **Group delay / ring time / settling time** from the elliptic stage 3
  (§8.3/§8.5) are all bench-verified numerically (group delay barely
  moves at the tone itself; ring time and full AGC settling both grow,
  but stay well under real CW element durations) but not yet checked by
  ear for anything that matters to a live operator — whether a signal
  fading in and out (QSB, or a station starting to send) now has a
  perceptible "catching up" lag that v1/v2/the resonator-based v3
  didn't have.
- **The folding artifact near `f=1500Hz`** (§8.4) is back to its
  original, honest reading after §8.8's raw-input AGC fix - `-17.0dB` at
  `f=1500Hz`, an inherent property of the filter chain itself (stage 1's
  own asymmetric response combined with where each side's folded pitch
  happens to land relative to stage 3's passband), not something either
  AGC-placement attempt was creating (§8.7's stage-2 fix exaggerated it
  to `-58dB`; the original stage-3-sampling design hid it as a loudness
  problem entirely). Still only checked at five discrete offsets (Case
  D) - a denser sweep would still be worth doing to map the artifact's
  exact boundary and whether it's perceptible in realistic tuning
  (roughly `CW_PITCH_HZ` or more past a signal in the *wrong*
  direction), but this is now a question about the filter design itself,
  not about the AGC.
- **§8.8's raw-input AGC hasn't been tested with two simultaneous
  signals in one buffer** - every case in `test_rx_audio.c`, including
  case F, still tests one tone per `rx_audio_process()` call with a
  fresh `rx_audio_init()` between them. A real band has many signals
  present at once; worth adding a two-tone-in-one-buffer case (or
  testing against real antenna I/Q) to confirm the "AGC desense from an
  unrelated in-band signal" behavior §8.8 describes qualitatively
  actually behaves as expected quantitatively, and to get a first read on
  how much a busy band pulls gain down for a single CW signal in
  practice.
- **Runtime control** — `rx_audio_set_volume()` is now reachable
  remotely via the rigctld server's `l`/`L AF` commands
  (`docs/04_remote_control_and_iq_output.md`), and there's a standalone
  `tools/rigctl_panel.py` control panel that uses it. Stage 3's width is
  not a runtime knob at all (§8.2), so there is nothing left to wire up
  for it.
