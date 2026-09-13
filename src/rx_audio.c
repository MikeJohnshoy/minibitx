// rx_audio.c
//
// Turns the receiver's own baseband I/Q into an audible CW tone, played
// out minibitx's local audio output - the same WM8731 codec cw.c already
// uses for the TX sidetone (see sound.c's audio_loop()).
//
// This intentionally lives inside minibitx, not the separate mb-radio
// panel app: minibitx already owns the WM8731 codec exclusively, and
// already has this block's I/Q sitting in sound.c's audio thread before
// it's even packaged for the network - this is a second consumer of
// data that's already flowing, not a new capture path a second process
// would have to duplicate (and then fight minibitx for the same
// physical audio device).
//
// The technique is a standard product detector - the same idea a
// classic analog CW rig's BFO implements in hardware - split into two
// independent stages so "how narrow" and "what pitch" stay separate
// knobs instead of one tangled one:
//
//   1. Lowpass-filter the incoming I/Q *before* moving it anywhere. The
//      wanted station is expected to sit near 0 Hz offset from dial
//      center (an external control surface - FLRig, for now - tunes it
//      there), so a lowpass applied right here, at its original
//      location, is what actually rejects everything else in the
//      passband. Its cutoff IS the narrow-filter width: a free,
//      independently tunable parameter, unlike an FFT bin's fixed
//      width.
//   2. Mix the now-narrowed I/Q up to CW_PITCH_HZ (cw.h - the same
//      pitch the TX sidetone already uses, so RX and TX match) and keep
//      only the real part. A steady carrier sitting exactly at dial
//      center comes out as a steady CW_PITCH_HZ tone - not silence,
//      for the same reason cw.c's TX side never keys straight at 0 Hz
//      either (see cw.h's CW_PITCH_HZ comment).
//
// v1's filter is deliberately a single-pole (6dB/octave) lowpass, not a
// sharp multi-pole/FIR design - the simplest thing that actually works,
// to get the whole chain proven on the bench first. None of the
// constants below are bench-verified yet (this file has never been run
// against a real signal) - expect to retune RX_AUDIO_FILTER_CUTOFF_HZ,
// RX_AUDIO_PEAK_AMPLITUDE, and possibly the BFO's sign once it is.

#include "rx_audio.h"
#include "cw.h"
#include "vfo.h"
#include <math.h>

#define SAMPLE_RATE_HZ 96000

// v1: single-pole lowpass, both I and Q rails. Passband is roughly
// +-RX_AUDIO_FILTER_CUTOFF_HZ around dial center - narrower rejects more
// adjacent-channel noise but demands more precise tuning to actually
// catch a signal; wider is more forgiving but noisier. Untested starting
// point - adjust via rx_audio_set_filter_bw() and see what actually
// sounds right on the bench.
#define RX_AUDIO_FILTER_CUTOFF_HZ 150

// Peak PCM amplitude at 100% volume - same role as sound.c's
// SIDETONE_PEAK_AMPLITUDE. Bench-unverified starting point; the right
// value depends on how much a real received signal's amplitude survives
// down to this stage, which nothing has measured yet.
#define RX_AUDIO_PEAK_AMPLITUDE 200000000.0

#define RX_AUDIO_FILTER_MIN_HZ   20    // don't let the passband collapse to nothing
#define RX_AUDIO_FILTER_MAX_HZ 2000    // don't let it swallow the whole thing either

struct onepole_lp {
    double y;      // last output - all the filter's state
    double alpha;  // set by onepole_set_cutoff(), below
};

static struct vfo bfo;                 // CW_PITCH_HZ mixing oscillator
static struct onepole_lp lp_i, lp_q;   // narrow-filter state, one per rail
static double rx_volume = 0.5;         // 0.0-1.0 - see rx_audio_set_volume()

static double onepole_apply(struct onepole_lp *f, double x) {
    f->y += f->alpha * (x - f->y);
    return f->y;
}

// Standard RC-equivalent single-pole coefficient (the same math as an
// exponential moving average) - see any DSP text's derivation of a
// lowpass built from one multiply-add.
static void onepole_set_cutoff(struct onepole_lp *f, int cutoff_hz) {
    double rc = 1.0 / (2.0 * M_PI * (double)cutoff_hz);
    double dt = 1.0 / (double)SAMPLE_RATE_HZ;
    f->alpha = dt / (rc + dt);
}

void rx_audio_init(void) {
    // BFO sign is a starting guess, not bench-verified - if a station
    // parked exactly at dial center sounds wrong (e.g. tuning direction
    // feels backwards), flip this to -CW_PITCH_HZ and recheck.
    vfo_start(&bfo, CW_PITCH_HZ, 0);
    onepole_set_cutoff(&lp_i, RX_AUDIO_FILTER_CUTOFF_HZ);
    onepole_set_cutoff(&lp_q, RX_AUDIO_FILTER_CUTOFF_HZ);
    lp_i.y = 0.0;
    lp_q.y = 0.0;
}

void rx_audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    rx_volume = (double)percent / 100.0;
}

void rx_audio_set_filter_bw(int cutoff_hz) {
    if (cutoff_hz < RX_AUDIO_FILTER_MIN_HZ) cutoff_hz = RX_AUDIO_FILTER_MIN_HZ;
    if (cutoff_hz > RX_AUDIO_FILTER_MAX_HZ) cutoff_hz = RX_AUDIO_FILTER_MAX_HZ;
    onepole_set_cutoff(&lp_i, cutoff_hz);
    onepole_set_cutoff(&lp_q, cutoff_hz);
}

void rx_audio_process(const double *i_samples, const double *q_samples,
                       int n, int32_t *out) {
    for (int k = 0; k < n; k++) {
        // Stage 1: narrow the passband down around dial center, before
        // moving anything - see the file header for why this has to
        // happen first, at the signal's original location.
        double fi = onepole_apply(&lp_i, i_samples[k]);
        double fq = onepole_apply(&lp_q, q_samples[k]);

        // Stage 2: mix up to CW_PITCH_HZ and keep only the real part.
        // Re[(fi + j*fq) * (cos + j*sin)] = fi*cos - fq*sin.
        int bfo_cos, bfo_sin;
        vfo_read_iq(&bfo, &bfo_cos, &bfo_sin);
        double c = (double)bfo_cos / 1073741824.0;
        double s = (double)bfo_sin / 1073741824.0;
        double audio = fi * c - fq * s;

        double sample = audio * rx_volume * RX_AUDIO_PEAK_AMPLITUDE;
        if (sample >  2000000000.0) sample =  2000000000.0;
        if (sample < -2000000000.0) sample = -2000000000.0;
        out[k] = (int32_t)sample;
    }
}
