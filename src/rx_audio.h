// rx_audio.h
//
// Demodulates a listenable CW tone out of the receiver's own I/Q, so the
// operator can hear a signal through the box's own local audio output -
// see rx_audio.c for the design.

#ifndef RX_AUDIO_H
#define RX_AUDIO_H

#include <stdint.h>

// Call once at startup, after vfo_init_phase_table().
void rx_audio_init(void);

// 0-100. Scales the demodulated audio before it reaches the codec.
void rx_audio_set_volume(int percent);

// Current volume, 0-100, in the same units rx_audio_set_volume() takes -
// added for hamlib.c's "l AF" (get_level) rigctld command, so a remote
// control panel can read back the volume it didn't itself just set (e.g.
// on connect, before ever calling set_volume).
int rx_audio_get_volume(void);

// The narrow, post-demodulation "single signal" selectivity filter
// centered on CW_PITCH_HZ - a separate stage from the wide image-reject
// filter upstream of it (see rx_audio.c's file header and
// docs/dsp_design_notes/rx_audio_demod_design.md §7/§8 for why those two
// are deliberately independent) - is a fixed 8-pole elliptic design, not
// runtime-adjustable. An earlier revision had a rx_audio_set_filter_bw()
// here; it's gone deliberately, not an oversight - see rx_audio.c's
// "Why elliptic, and why fixed" for why: the SHAPE (coefficients) is
// fixed. Whether the operator hears it at all is a different, much
// cheaper question - rx_audio_set_narrow_filter() below just switches
// between the filter's output and its bypass, no coefficient math
// involved, so it doesn't reopen that earlier decision.

// Enable (1, the default) or bypass (0) stage 3, the narrow filter
// above. The filter itself keeps running either way (its history stays
// warm) - only which signal reaches stage 4's AGC/output changes - so
// there's no settling-time thump when toggling back on. Wired to
// rigctld's "u"/"U NARROW" (hamlib.c) so the control panel (tools/
// rigctl_panel.py) can toggle it remotely - see rx_audio.c for why this
// is a plain on/off rather than a runtime-adjustable width.
void rx_audio_set_narrow_filter(int enable);

// Current narrow-filter enable state, 0 or 1 - same "let a client read
// back what it didn't itself just set" reasoning as
// rx_audio_get_volume().
int rx_audio_get_narrow_filter(void);

// Demodulates one block's worth of already-mixed baseband I/Q (the same
// i_samples[]/q_samples[] sound.c's sound_process() already computes for
// hpsdr_send_iq()) into n real PCM samples ready for the codec's local
// monitor channel. Call once per audio block, same n as the I/Q block it
// was given.
void rx_audio_process(const double *i_samples, const double *q_samples,
                       int n, int32_t *out);

// Debug/test only - the AGC's current smoothed envelope estimate (see
// rx_audio.c). As of "Why the AGC samples the raw input, not stage 2 or
// stage 3" in rx_audio.c's file header, this tracks the RAW input I/Q's
// magnitude, sampled before stage 1 even runs - deliberately moved there
// (via an intermediate stop at stage 2's output, which had its own
// problem - see the file header) so the AGC's makeup gain no longer
// depends on any frequency-selective stage's own behavior, and so no
// longer cancels out any of it in the final PCM output. That also means
// this envelope is frequency-independent by construction (a steady tone
// reads the same magnitude regardless of its frequency or which side of
// dial center it's on) - it is NOT a window into any single stage's
// selectivity any more, not stage 1's image rejection and not stage 3's
// narrow filter. Measure the actual PCM output (rms/peak of
// rx_audio_process()'s out[] array) for either of those instead, now
// that the AGC no longer erases them. What this envelope IS still useful
// for: confirming the AGC's own target-amplitude/gain math against a
// known input amplitude (test_rx_audio.c's cases A/B/C). Not needed by
// normal callers.
double rx_audio_debug_agc_envelope(void);

// Debug/test only - the METER envelope's current smoothed estimate (see
// rx_audio.c's "Two envelopes, two jobs" above rx_audio_get_strength_db()).
// Unlike rx_audio_debug_agc_envelope() above, this one DOES track stage
// 1's image rejection and stage 3's selectivity/bypass state, by design -
// it's tapped from narrowed (post-stage-3-or-bypass, pre-AGC-gain), not
// the raw input. Exists mainly so test_rx_audio.c can check that
// distinction directly rather than only indirectly through
// rx_audio_get_strength_db()'s rounded dB output. Not needed by normal
// callers.
double rx_audio_debug_meter_envelope(void);

// Current signal-strength estimate for rigctld's "l STRENGTH" (hamlib.c) -
// a real Hamlib RIG_LEVEL, unlike NARROW above, so it's implemented in
// the standard convention real Hamlib clients expect: an integer number
// of dB relative to a nominal S9 reference (0 = S9, negative = below S9
// in 6dB/S-unit steps down toward S0, positive = "S9+N dB"). Built on the
// METER envelope (rx_audio_debug_meter_envelope() above), NOT the AGC's
// own agc_env - deliberately: this reads what's actually reaching the
// speaker (post image-rejection, post narrow-filter-or-bypass), not "how
// much energy is anywhere in the whole captured band" the way an
// agc_env-based reading would. Read-only and NOT wattmeter/signal-
// generator calibrated either way - see rx_audio.c and
// docs/dsp_design_notes/rx_gain_and_level_calibration.md §9 for exactly
// what the reference point does and doesn't mean, and for the earlier,
// wideband version this replaced. No set_level equivalent, same as a
// real rig's S-meter.
int rx_audio_get_strength_db(void);

#endif /* RX_AUDIO_H */
