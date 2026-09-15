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
// "Why elliptic, and why fixed" for why.

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

#endif /* RX_AUDIO_H */
