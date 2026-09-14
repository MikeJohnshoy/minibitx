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

// Debug/test only - the AGC's current smoothed envelope estimate of the
// post-filter, pre-gain audio magnitude (see rx_audio.c). Because the
// AGC deliberately normalizes rx_audio_process()'s actual PCM output
// toward a fixed target regardless of input strength, this envelope -
// not the PCM output - is what still reflects the narrow filter's real
// selectivity (an out-of-passband signal settles to a much smaller
// envelope than an in-passband one of the same input amplitude, even
// though both eventually reach similar output loudness). Used by
// test_rx_audio.c; not needed by normal callers.
double rx_audio_debug_agc_envelope(void);

#endif /* RX_AUDIO_H */
