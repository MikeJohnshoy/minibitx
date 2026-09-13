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
// The technique is a product detector, same idea a classic analog CW
// rig's BFO implements in hardware, split into three independent
// stages:
//
//   1. Run the incoming I/Q through a complex (Hilbert-style) bandpass
//      filter that keeps only ONE side of dial center and rejects the
//      other - see "Why complex, not just narrow" below for why this
//      replaced v1's plain single-pole lowpass.
//   2. Mix the filtered I/Q up to CW_PITCH_HZ (cw.h - the same pitch the
//      TX sidetone already uses, so RX and TX match) and keep only the
//      real part. A steady carrier sitting exactly at dial center comes
//      out as a steady CW_PITCH_HZ tone - not silence, for the same
//      reason cw.c's TX side never keys straight at 0 Hz either (see
//      cw.h's CW_PITCH_HZ comment).
//   3. Run the result through an AGC (envelope-following automatic gain
//      control) that normalizes toward a fixed target output level -
//      see AGC_TARGET_AMPLITUDE below for why a fixed multiplier alone
//      doesn't work: real signal amplitude at this point in the chain
//      (bench-measured, see that comment) varies far more than any one
//      constant could cover without either being silent on a weak
//      signal or clipping on a strong one.
//
// Why complex, not just narrow (the zero-beat symmetry fix):
// v1's stage 1 was a single-pole lowpass applied identically to I and Q
// - a real-valued filter, which is mathematically Hermitian-symmetric
// (|H(-f)| == |H(f)|) no matter how it's tuned. It could narrow the
// passband, but it could never tell a station 300Hz above dial center
// from one 300Hz below - both came through equally strong, unlike a
// real receiver's crystal/mechanical filter, whose skirt sits physically
// off-center relative to the BFO (see
// docs/dsp_design_notes/antialias_filter_design.md §3 for the same
// off-center-skirt idea used on the TX side). Fixing that needs a filter
// whose frequency response is NOT symmetric - a complex FIR, built here
// by taking a real symmetric lowpass prototype (scipy.signal.remez, same
// technique as antialias.c) and modulating it by a complex exponential
// referenced to its own center tap. That shifts its passband from
// [-300, +300] to one-sided [0, +600] Hz - passing
// content on one side of dial center, rejecting the other, with rejection
// improving the further a station sits from dial center (full derivation
// and bench numbers: docs/dsp_design_notes/rx_audio_demod_design.md §7).
//
// Referencing the modulating phase to the filter's own center tap (not
// absolute sample index 0) is what makes both halves of the resulting
// complex filter retain the same kind of tap-reduction symmetry
// antialias.c's real filter has: the real part comes out EVEN-symmetric
// (h[i'] == h[N-1-i'] here for coefficient tables), the imaginary
// (Hilbert) part comes out ODD-symmetric (h[i'] == -h[N-1-i']) with an
// exact zero at the center tap. This implementation does NOT hand-fold
// that symmetry into half-length loops, though - antialias.c didn't
// either (see its own header comment on the double-length history
// buffer): a manually-mirrored access pattern is harder for gcc to
// autovectorize than the straight sequential loop below, and at this
// tap count the straight version is still nowhere near a real
// constraint (well under 1% of one Pi 4 core - see the design doc for
// the actual multiply-add budget). The double-length history buffer
// below IS the same trick antialias.c uses, for the same reason: it
// keeps the inner loop's read a single contiguous slice with no
// wraparound branch, which is what actually let gcc's -O3 autovectorize
// it.

#include "rx_audio.h"
#include "cw.h"
#include "vfo.h"
#include <math.h>

#define SAMPLE_RATE_HZ 96000

#define SSB_FIR_TAPS 359

// Complex bandpass filter: real symmetric lowpass prototype designed via
// scipy.signal.remez(SSB_FIR_TAPS, [0, 300, 700, 48000],
// [1, 0], weight=[1, 10], fs=96000) - passband edge 300Hz, stopband
// edge 700Hz, achieving ~1.7dB passband ripple and -40dB worst-case
// stopband - then modulated by exp(j*2*pi*300*m/Fs), m referenced to
// the filter's own center tap, to shift its response from symmetric
// [-300,+300] to one-sided [0, +600] Hz. Coefficients
// pre-reversed at generation time (ssb_hr[i] == hr[N-1-i], same for
// ssb_hi) so the loop in ssb_filter_apply() below - structurally
// identical to antialias_apply() - reproduces the textbook
// y[n] = sum_k h[k]*x[n-k] convolution; see
// docs/dsp_design_notes/rx_audio_demod_design.md §7 for the full
// derivation and the numeric image-rejection verification this table
// was checked against before being pasted in here.
//
// Passes baseband content from 0 up to +600Hz above dial center
// (the "wanted" side); rejects content below dial center, with rejection
// improving from a few dB right at zero beat (a fundamental limit - no
// filter can separate +0Hz from -0Hz) up past 40dB by 500Hz.
static const double ssb_hr[SSB_FIR_TAPS] = {
     0.00472338,  0.00040056,  0.00041983,  0.00043927,  0.00045878,
     0.00047837,  0.00049792,  0.00051741,  0.00053677,  0.00055593,
     0.00057479,  0.00059329,  0.00061138,  0.00062897,  0.00064587,
     0.00066228,  0.00067796,  0.00069296,  0.00070744,  0.00072117,
     0.00073433,  0.00074692,  0.00075888,  0.00077007,  0.00078032,
     0.00078931,  0.00079665,  0.00080199,  0.00080535,  0.00080746,
     0.00081084,  0.00082093,  0.00081266,  0.00081371,  0.00081118,
     0.00080723,  0.00080187,  0.00079505,  0.00078675,  0.00077699,
     0.00076577,  0.00075310,  0.00073903,  0.00072362,  0.00070686,
     0.00068868,  0.00066933,  0.00064855,  0.00062649,  0.00060323,
     0.00057858,  0.00055276,  0.00052587,  0.00049800,  0.00046930,
     0.00043991,  0.00040986,  0.00037906,  0.00034734,  0.00031474,
     0.00028161,  0.00024890,  0.00021700,  0.00018204,  0.00014932,
     0.00011595,  0.00008289,  0.00005022,  0.00001802, -0.00001357,
    -0.00004442, -0.00007441, -0.00010342, -0.00013129, -0.00015788,
    -0.00018306, -0.00020670, -0.00022854, -0.00024859, -0.00026656,
    -0.00028235, -0.00029592, -0.00030705, -0.00031562, -0.00032152,
    -0.00032457, -0.00032461, -0.00032149, -0.00031514, -0.00030546,
    -0.00029236, -0.00027566, -0.00025515, -0.00023079, -0.00020272,
    -0.00017047, -0.00013420, -0.00009374, -0.00004903,  0.00000000,
     0.00005341,  0.00011126,  0.00017359,  0.00024044,  0.00031184,
     0.00038780,  0.00046835,  0.00055350,  0.00064321,  0.00073755,
     0.00083639,  0.00093974,  0.00104759,  0.00115984,  0.00127645,
     0.00139740,  0.00152257,  0.00165186,  0.00178512,  0.00192229,
     0.00206331,  0.00220811,  0.00235647,  0.00250805,  0.00266284,
     0.00282115,  0.00298187,  0.00314565,  0.00331190,  0.00348051,
     0.00365132,  0.00382411,  0.00399865,  0.00417476,  0.00435220,
     0.00453071,  0.00471005,  0.00489001,  0.00507040,  0.00525081,
     0.00543135,  0.00561131,  0.00579069,  0.00596930,  0.00614667,
     0.00632264,  0.00649708,  0.00666969,  0.00684011,  0.00700801,
     0.00717321,  0.00733559,  0.00749495,  0.00765091,  0.00780296,
     0.00795121,  0.00809578,  0.00823541,  0.00837096,  0.00850157,
     0.00862723,  0.00874772,  0.00886283,  0.00897239,  0.00907626,
     0.00917427,  0.00926623,  0.00935195,  0.00943135,  0.00950433,
     0.00957055,  0.00963049,  0.00968320,  0.00972924,  0.00976845,
     0.00980040,  0.00982526,  0.00984315,  0.00985399,  0.00985763,
     0.00985399,  0.00984315,  0.00982526,  0.00980040,  0.00976845,
     0.00972924,  0.00968320,  0.00963049,  0.00957055,  0.00950433,
     0.00943135,  0.00935195,  0.00926623,  0.00917427,  0.00907626,
     0.00897239,  0.00886283,  0.00874772,  0.00862723,  0.00850157,
     0.00837096,  0.00823541,  0.00809578,  0.00795121,  0.00780296,
     0.00765091,  0.00749495,  0.00733559,  0.00717321,  0.00700801,
     0.00684011,  0.00666969,  0.00649708,  0.00632264,  0.00614667,
     0.00596930,  0.00579069,  0.00561131,  0.00543135,  0.00525081,
     0.00507040,  0.00489001,  0.00471005,  0.00453071,  0.00435220,
     0.00417476,  0.00399865,  0.00382411,  0.00365132,  0.00348051,
     0.00331190,  0.00314565,  0.00298187,  0.00282115,  0.00266284,
     0.00250805,  0.00235647,  0.00220811,  0.00206331,  0.00192229,
     0.00178512,  0.00165186,  0.00152257,  0.00139740,  0.00127645,
     0.00115984,  0.00104759,  0.00093974,  0.00083639,  0.00073755,
     0.00064321,  0.00055350,  0.00046835,  0.00038780,  0.00031184,
     0.00024044,  0.00017359,  0.00011126,  0.00005341,  0.00000000,
    -0.00004903, -0.00009374, -0.00013420, -0.00017047, -0.00020272,
    -0.00023079, -0.00025515, -0.00027566, -0.00029236, -0.00030546,
    -0.00031514, -0.00032149, -0.00032461, -0.00032457, -0.00032152,
    -0.00031562, -0.00030705, -0.00029592, -0.00028235, -0.00026656,
    -0.00024859, -0.00022854, -0.00020670, -0.00018306, -0.00015788,
    -0.00013129, -0.00010342, -0.00007441, -0.00004442, -0.00001357,
     0.00001802,  0.00005022,  0.00008289,  0.00011595,  0.00014932,
     0.00018204,  0.00021700,  0.00024890,  0.00028161,  0.00031474,
     0.00034734,  0.00037906,  0.00040986,  0.00043991,  0.00046930,
     0.00049800,  0.00052587,  0.00055276,  0.00057858,  0.00060323,
     0.00062649,  0.00064855,  0.00066933,  0.00068868,  0.00070686,
     0.00072362,  0.00073903,  0.00075310,  0.00076577,  0.00077699,
     0.00078675,  0.00079505,  0.00080187,  0.00080723,  0.00081118,
     0.00081371,  0.00081266,  0.00082093,  0.00081084,  0.00080746,
     0.00080535,  0.00080199,  0.00079665,  0.00078931,  0.00078032,
     0.00077007,  0.00075888,  0.00074692,  0.00073433,  0.00072117,
     0.00070744,  0.00069296,  0.00067796,  0.00066228,  0.00064587,
     0.00062897,  0.00061138,  0.00059329,  0.00057479,  0.00055593,
     0.00053677,  0.00051741,  0.00049792,  0.00047837,  0.00045878,
     0.00043927,  0.00041983,  0.00040056,  0.00472338,
};

static const double ssb_hi[SSB_FIR_TAPS] = {
     0.00184870,  0.00014777,  0.00014558,  0.00014273,  0.00013917,
     0.00013491,  0.00012993,  0.00012422,  0.00011777,  0.00011058,
     0.00010265,  0.00009397,  0.00008456,  0.00007444,  0.00006361,
     0.00005212,  0.00003998,  0.00002723,  0.00001389, -0.00000000,
    -0.00001442, -0.00002935, -0.00004475, -0.00006061, -0.00007685,
    -0.00009342, -0.00011019, -0.00012702, -0.00014382, -0.00016061,
    -0.00017790, -0.00019709, -0.00021206, -0.00022949, -0.00024607,
    -0.00026228, -0.00027807, -0.00029331, -0.00030793, -0.00032184,
    -0.00033496, -0.00034719, -0.00035846, -0.00036870, -0.00037782,
    -0.00038568, -0.00039230, -0.00039743, -0.00040104, -0.00040307,
    -0.00040325, -0.00040160, -0.00039807, -0.00039259, -0.00038514,
    -0.00037572, -0.00036421, -0.00035040, -0.00033397, -0.00031474,
    -0.00029290, -0.00026926, -0.00024419, -0.00021314, -0.00018195,
    -0.00014708, -0.00010950, -0.00006912, -0.00002586,  0.00002031,
     0.00006938,  0.00012142,  0.00017645,  0.00023444,  0.00029538,
     0.00035927,  0.00042616,  0.00049575,  0.00056832,  0.00064354,
     0.00072140,  0.00080211,  0.00088545,  0.00097139,  0.00105992,
     0.00115085,  0.00124397,  0.00133912,  0.00143631,  0.00153566,
     0.00163716,  0.00174044,  0.00184466,  0.00194989,  0.00205825,
     0.00216603,  0.00227564,  0.00238591,  0.00249684,  0.00260827,
     0.00272001,  0.00283184,  0.00294361,  0.00305511,  0.00316615,
     0.00327650,  0.00338605,  0.00349467,  0.00360188,  0.00370791,
     0.00381206,  0.00391429,  0.00401458,  0.00411248,  0.00420790,
     0.00430074,  0.00439074,  0.00447756,  0.00456095,  0.00464081,
     0.00471712,  0.00478975,  0.00485835,  0.00492232,  0.00498182,
     0.00503753,  0.00508756,  0.00513324,  0.00517369,  0.00520896,
     0.00523892,  0.00526343,  0.00528237,  0.00529565,  0.00530317,
     0.00530478,  0.00530039,  0.00528998,  0.00527353,  0.00525081,
     0.00522214,  0.00518704,  0.00514574,  0.00509827,  0.00504444,
     0.00498437,  0.00491816,  0.00484581,  0.00476729,  0.00468261,
     0.00459187,  0.00449525,  0.00439286,  0.00428471,  0.00417077,
     0.00405134,  0.00392673,  0.00379658,  0.00366153,  0.00352147,
     0.00337663,  0.00322721,  0.00307336,  0.00291531,  0.00275325,
     0.00258741,  0.00241799,  0.00224520,  0.00206930,  0.00189053,
     0.00170908,  0.00152532,  0.00133935,  0.00115153,  0.00096211,
     0.00077131,  0.00057943,  0.00038674,  0.00019351,  0.00000000,
    -0.00019351, -0.00038674, -0.00057943, -0.00077131, -0.00096211,
    -0.00115153, -0.00133935, -0.00152532, -0.00170908, -0.00189053,
    -0.00206930, -0.00224520, -0.00241799, -0.00258741, -0.00275325,
    -0.00291531, -0.00307336, -0.00322721, -0.00337663, -0.00352147,
    -0.00366153, -0.00379658, -0.00392673, -0.00405134, -0.00417077,
    -0.00428471, -0.00439286, -0.00449525, -0.00459187, -0.00468261,
    -0.00476729, -0.00484581, -0.00491816, -0.00498437, -0.00504444,
    -0.00509827, -0.00514574, -0.00518704, -0.00522214, -0.00525081,
    -0.00527353, -0.00528998, -0.00530039, -0.00530478, -0.00530317,
    -0.00529565, -0.00528237, -0.00526343, -0.00523892, -0.00520896,
    -0.00517369, -0.00513324, -0.00508756, -0.00503753, -0.00498182,
    -0.00492232, -0.00485835, -0.00478975, -0.00471712, -0.00464081,
    -0.00456095, -0.00447756, -0.00439074, -0.00430074, -0.00420790,
    -0.00411248, -0.00401458, -0.00391429, -0.00381206, -0.00370791,
    -0.00360188, -0.00349467, -0.00338605, -0.00327650, -0.00316615,
    -0.00305511, -0.00294361, -0.00283184, -0.00272001, -0.00260827,
    -0.00249684, -0.00238591, -0.00227564, -0.00216603, -0.00205825,
    -0.00194989, -0.00184466, -0.00174044, -0.00163716, -0.00153566,
    -0.00143631, -0.00133912, -0.00124397, -0.00115085, -0.00105992,
    -0.00097139, -0.00088545, -0.00080211, -0.00072140, -0.00064354,
    -0.00056832, -0.00049575, -0.00042616, -0.00035927, -0.00029538,
    -0.00023444, -0.00017645, -0.00012142, -0.00006938, -0.00002031,
     0.00002586,  0.00006912,  0.00010950,  0.00014708,  0.00018195,
     0.00021314,  0.00024419,  0.00026926,  0.00029290,  0.00031474,
     0.00033397,  0.00035040,  0.00036421,  0.00037572,  0.00038514,
     0.00039259,  0.00039807,  0.00040160,  0.00040325,  0.00040307,
     0.00040104,  0.00039743,  0.00039230,  0.00038568,  0.00037782,
     0.00036870,  0.00035846,  0.00034719,  0.00033496,  0.00032184,
     0.00030793,  0.00029331,  0.00027807,  0.00026228,  0.00024607,
     0.00022949,  0.00021206,  0.00019709,  0.00017790,  0.00016061,
     0.00014382,  0.00012702,  0.00011019,  0.00009342,  0.00007685,
     0.00006061,  0.00004475,  0.00002935,  0.00001442,  0.00000000,
    -0.00001389, -0.00002723, -0.00003998, -0.00005212, -0.00006361,
    -0.00007444, -0.00008456, -0.00009397, -0.00010265, -0.00011058,
    -0.00011777, -0.00012422, -0.00012993, -0.00013491, -0.00013917,
    -0.00014273, -0.00014558, -0.00014777, -0.00184870,
};

struct ssb_filter_state {
    double hist_i[2 * SSB_FIR_TAPS];
    double hist_q[2 * SSB_FIR_TAPS];
    int pos;   // write cursor, always in [0, SSB_FIR_TAPS)
};

static struct ssb_filter_state ssb_state;

// Complex convolution: (i_in + j*q_in) rail history against the complex
// filter (ssb_hr + j*ssb_hi). Re/Im expand to the standard 4-multiply
// complex-times-complex, done here as two real convolutions each reused
// across both output rails:
//   i_out = sum(hr*hist_i) - sum(hi*hist_q)
//   q_out = sum(hr*hist_q) + sum(hi*hist_i)
static void ssb_filter_apply(struct ssb_filter_state *f, double i_in, double q_in,
                              double *i_out, double *q_out) {
    f->hist_i[f->pos] = i_in;
    f->hist_i[f->pos + SSB_FIR_TAPS] = i_in;
    f->hist_q[f->pos] = q_in;
    f->hist_q[f->pos + SSB_FIR_TAPS] = q_in;

    int base = f->pos + 1;
    double acc_re = 0.0, acc_im = 0.0;
    for (int i = 0; i < SSB_FIR_TAPS; i++) {
        double hi_val = f->hist_i[base + i];
        double hq_val = f->hist_q[base + i];
        acc_re += ssb_hr[i] * hi_val - ssb_hi[i] * hq_val;
        acc_im += ssb_hr[i] * hq_val + ssb_hi[i] * hi_val;
    }

    f->pos++;
    if (f->pos == SSB_FIR_TAPS) f->pos = 0;
    *i_out = acc_re;
    *q_out = acc_im;
}

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
// Untested starting points - expect to retune by ear once there's a
// real signal to judge by.
#define AGC_ATTACK_MS    5.0
#define AGC_RELEASE_MS 300.0

// Ceiling on the gain the AGC can apply - without this, near-total
// silence (envelope estimate near 0) would drive gain toward infinity
// and turn the noise floor into full-scale hiss the moment the band
// goes quiet.
#define AGC_MAX_GAIN 8.0e11

static struct vfo bfo;                 // CW_PITCH_HZ mixing oscillator
static double rx_volume = 0.5;         // 0.0-1.0 - see rx_audio_set_volume()

// AGC envelope follower state - agc_env tracks a smoothed |audio|
// estimate; the gain applied each sample is AGC_TARGET_AMPLITUDE /
// agc_env, so as agc_env rises/falls the output rides back toward the
// target instead of tracking the raw input amplitude directly.
static double agc_env = 0.0;
static double agc_attack_alpha, agc_release_alpha;

// Time-constant (not cutoff-frequency) one-pole coefficient, for the
// AGC's attack/release smoothing - alpha such that a step input reaches
// ~63% of the way there after time_ms.
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

    for (int i = 0; i < 2 * SSB_FIR_TAPS; i++) {
        ssb_state.hist_i[i] = 0.0;
        ssb_state.hist_q[i] = 0.0;
    }
    ssb_state.pos = 0;

    agc_attack_alpha  = onepole_alpha_from_ms(AGC_ATTACK_MS);
    agc_release_alpha = onepole_alpha_from_ms(AGC_RELEASE_MS);
    agc_env = 0.0;
}

void rx_audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    rx_volume = (double)percent / 100.0;
}

double rx_audio_debug_agc_envelope(void) {
    return agc_env;
}

void rx_audio_process(const double *i_samples, const double *q_samples,
                       int n, int32_t *out) {
    for (int k = 0; k < n; k++) {
        // Stage 1: complex bandpass - narrows the passband AND breaks
        // the symmetric-around-dial-center problem v1 had. See the file
        // header for the full picture.
        double fi, fq;
        ssb_filter_apply(&ssb_state, i_samples[k], q_samples[k], &fi, &fq);

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
