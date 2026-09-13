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
//   3. Run the result through an AGC (envelope-following automatic gain
//      control) that normalizes toward a fixed target output level -
//      see AGC_TARGET_AMPLITUDE below for why a fixed multiplier alone
//      doesn't work: real signal amplitude at this point in the chain
//      (bench-measured, see that comment) varies far more than any one
//      constant could cover without either being silent on a weak
//      signal or clipping on a strong one.
//
// v1's filter is deliberately a single-pole (6dB/octave) lowpass, not a
// sharp multi-pole/FIR design - the simplest thing that actually works,
// to get the whole chain proven on the bench first. RX_AUDIO_FILTER_
// CUTOFF_HZ and the BFO's sign are still bench-unverified starting
// points - expect to retune/flip them once there's a real signal to
// judge by ear.

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

// v1 shipped with a fixed peak-PCM-amplitude multiplier here (the same
// role as sound.c's SIDETONE_PEAK_AMPLITUDE), calibrated only against a
// synthetic, unit-amplitude test carrier (test_rx_audio.c). Bench data
// off a real antenna (FT8 band noise/signals on 40m, 2025-09) showed the
// actual post-mix signal sitting around 0.0015-0.0085 of that synthetic
// 1.0 reference - a fixed multiplier tuned for one of those scales is
// either silent on the other or clips on anything stronger, and real
// band conditions swing far wider than either bench sample. An AGC
// (automatic gain control) is the standard fix, and what every real
// receiver does for this same reason - it normalizes toward a target
// output level regardless of how strong the incoming signal actually is,
// rather than assuming one fixed relationship between input and output
// amplitude.
//
// Target output amplitude the AGC rides toward, once its envelope
// estimate has settled - comfortably below the +-2e9 clamp so real
// peaks (louder than the envelope's own smoothed average) still fit
// without clipping.
#define AGC_TARGET_AMPLITUDE 500000000.0

// Fast attack (catch a loud transient - a strong signal keying up -
// before it clips) and slow release (ride the overall band-noise/signal
// level rather than pumping between a CW dit and the gap after it).
// Untested starting points, same caveat as the filter cutoff below.
#define AGC_ATTACK_MS    5.0
#define AGC_RELEASE_MS 300.0

// Ceiling on the gain the AGC can apply - without this, near-total
// silence (envelope estimate near 0) would drive gain toward infinity
// and turn the noise floor into full-scale hiss the moment the band
// goes quiet.
#define AGC_MAX_GAIN 8.0e11

#define RX_AUDIO_FILTER_MIN_HZ   20    // don't let the passband collapse to nothing
#define RX_AUDIO_FILTER_MAX_HZ 2000    // don't let it swallow the whole thing either

struct onepole_lp {
    double y;      // last output - all the filter's state
    double alpha;  // set by onepole_set_cutoff(), below
};

static struct vfo bfo;                 // CW_PITCH_HZ mixing oscillator
static struct onepole_lp lp_i, lp_q;   // narrow-filter state, one per rail
static double rx_volume = 0.5;         // 0.0-1.0 - see rx_audio_set_volume()

// AGC envelope follower state - agc_env tracks a smoothed |audio|
// estimate; the gain applied each sample is AGC_TARGET_AMPLITUDE /
// agc_env, so as agc_env rises/falls the output rides back toward the
// target instead of tracking the raw input amplitude directly.
static double agc_env = 0.0;
static double agc_attack_alpha, agc_release_alpha;

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

// Time-constant (not cutoff-frequency) version of the same one-pole
// coefficient, for the AGC's attack/release smoothing below - alpha such
// that a step input reaches ~63% of the way there after time_ms.
static double onepole_alpha_from_ms(double time_ms) {
    double dt = 1.0 / (double)SAMPLE_RATE_HZ;
    double tau = time_ms / 1000.0;
    return 1.0 - exp(-dt / tau);
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

    agc_attack_alpha  = onepole_alpha_from_ms(AGC_ATTACK_MS);
    agc_release_alpha = onepole_alpha_from_ms(AGC_RELEASE_MS);
    agc_env = 0.0;
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

double rx_audio_debug_agc_envelope(void) {
    return agc_env;
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

        // Stage 3: AGC - track a smoothed envelope of |audio| (fast
        // attack so a strong signal keying up doesn't clip before the
        // envelope catches up, slow release so gain doesn't pump on
        // every CW dit/dah gap), then scale so the envelope itself sits
        // at AGC_TARGET_AMPLITUDE regardless of how large or small the
        // raw input actually is - see the constants above for why a
        // fixed multiplier alone can't work here.
        double mag = fabs(audio);
        double alpha = (mag > agc_env) ? agc_attack_alpha : agc_release_alpha;
        agc_env += alpha * (mag - agc_env);

        double gain = AGC_TARGET_AMPLITUDE / (agc_env > 1e-9 ? agc_env : 1e-9);
        if (gain > AGC_MAX_GAIN) gain = AGC_MAX_GAIN;

        double sample = audio * gain * rx_volume;
        if (sample >  2000000000.0) sample =  2000000000.0;
        if (sample < -2000000000.0) sample = -2000000000.0;
        out[k] = (int32_t)sample;
    }
}
