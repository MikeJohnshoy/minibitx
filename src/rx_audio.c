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
// v3 architecture - four independent stages, each with one job:
//
//   1. A WIDE complex (Hilbert-style) bandpass filter that keeps one
//      side of dial center and rejects the other. This is the only
//      stage that needs to be complex/asymmetric, and it's deliberately
//      wide (SSB_FILTER_FPASS_HZ/FSTOP_HZ below) rather than narrow -
//      see "Why wide, not narrow" below.
//   2. Mix the filtered I/Q up to CW_PITCH_HZ (cw.h) and keep only the
//      real part - same product-detector math as always,
//      Re[(I+jQ)*(cos+jsin)] = I*cos - Q*sin.
//   3. A NARROW real bandpass centered on CW_PITCH_HZ, doing the actual
//      "single signal" selectivity - an 8-pole elliptic (Cauer) IIR, a
//      fixed design (not runtime-adjustable; see "Why elliptic, and why
//      fixed" below), chosen to approach a classic CW crystal filter's
//      shape factor rather than the gentler resonator-cascade shape v3
//      shipped with initially.
//   4. An AGC (envelope-following automatic gain control) that
//      normalizes toward a fixed target output level - see
//      AGC_TARGET_AMPLITUDE below for why a fixed multiplier alone
//      can't work.
//
// Why wide, not narrow (the v2 -> v3 change):
// v2 used ONE complex filter to do both jobs at once - its passband
// edge was both "how much of the audio range survives" and "how sharp
// the image rejection transition is". That's the wrong thing to
// conflate: on-air testing (2026-09) showed signals getting soft well
// before the edge of the nominal passband, because the filter's own
// equiripple roll-off was eating into what should have been a clean,
// flat "wanted" region. The fix, and the more conventional approach for
// a phasing-method receiver: let the complex filter do ONLY image
// rejection, across a passband wide enough to comfortably not matter
// (SSB_FILTER_FPASS_HZ, 1500 Hz - wide enough for a future SSB monitor
// too, not just today's CW use), and do the actual narrow "single
// signal" selectivity in a completely separate, much cheaper stage
// AFTER demodulation (stage 3), where the signal is already real and
// single-sided so an ordinary symmetric filter is perfectly fine - no
// more mirror-image ambiguity to worry about by that point. Widening
// stage 1's passband costs nothing extra: FIR tap count is set by the
// TRANSITION width, not by where the passband edge sits (Harris'
// estimate, `N ~= (Fs/transition_Hz)*(Astop_dB/22)`, has no Fpass term
// at all) - see docs/dsp_design_notes/rx_audio_demod_design.md SS7 for
// the numbers that confirmed this before any code changed.
//
// Why elliptic, and why fixed (the first v3 -> current stage-3 change):
// v3 originally built stage 3 from 4 identical, synchronously-tuned
// biquad resonator sections - cheap and easy to retune live, but its
// skirt stayed fundamentally gentle (a resonator cascade only ever rolls
// off, it never develops the equiripple "wall" a crystal ladder filter
// has), and its shape didn't get meaningfully sharper by adding more
// identical sections. An elliptic (Cauer) design gets there instead: for
// the SAME 8-pole cost, equiripple passband + zeros placed right at the
// band edges buys a ~1.9:1 shape factor (-60dB bandwidth : -6dB
// bandwidth) - in the same range as a real CW crystal filter, and far
// steeper than the resonator cascade ever reached. See
// docs/dsp_design_notes/rx_audio_demod_design.md SS8 for the measured
// comparison (the resonator cascade and elliptic's shapes side by side).
// The cost is that elliptic coefficients aren't a simple trig formula
// like the old RBJ biquad's - computing them needs solving elliptic
// integrals, not something to redo from an audio callback - so this
// stage is now a fixed design (SSB_FIR_TAPS-style precomputed
// coefficients, not runtime-tunable the way v3's first cut was).

#include "rx_audio.h"
#include "cw.h"
#include "vfo.h"
#include <math.h>

#define SAMPLE_RATE_HZ 96000

#define SSB_FIR_TAPS 327

// Stage 1: wide image-reject complex bandpass. Real symmetric lowpass
// prototype designed via scipy.signal.remez(SSB_FIR_TAPS,
// [0, 1500, 1900, 48000], [1, 0], weight=[1, 10], fs=96000) - passband
// edge 1500Hz, stopband edge 1900Hz, ~1.7dB passband ripple, -40dB
// worst-case stopband - then modulated by exp(j*2*pi*1500*m/Fs), m
// referenced to the filter's own center tap, to shift its response from
// symmetric [-1500,+1500] to one-sided [0, +3000] Hz. Coefficients
// pre-reversed at generation time (ssb_hr[i] == hr[N-1-i], same for
// ssb_hi) so the loop in ssb_filter_apply() below reproduces the
// textbook y[n] = sum_k h[k]*x[n-k] convolution - see
// docs/dsp_design_notes/rx_audio_demod_design.md SS7 for the full
// derivation, the reversal subtlety, and the numeric verification this
// table was checked against before being pasted in here.
//
// Passes baseband content from 0 up to +3000Hz above dial center (the
// "wanted" side, comfortably wide - see file header); rejects content
// below dial center, with rejection improving from a few dB right at
// zero beat (a fundamental limit shared by any filter - nothing can
// separate +0Hz from -0Hz) out to -40dB by roughly 1500-2000Hz. Stage 3
// below is what actually shapes single-signal selectivity now.
static const double ssb_hr[SSB_FIR_TAPS] = {
     0.00472215, -0.00002114, -0.00003969, -0.00006961, -0.00011114,
    -0.00016150, -0.00021969, -0.00028154, -0.00034457, -0.00040409,
    -0.00045675, -0.00049749, -0.00052287, -0.00052815, -0.00051054,
    -0.00046645, -0.00039427, -0.00029231, -0.00016067,  0.00000000,
     0.00018761,  0.00039892,  0.00062998,  0.00087507,  0.00112885,
     0.00138360,  0.00163348,  0.00186954,  0.00208671,  0.00227510,
     0.00243290,  0.00254717,  0.00263303,  0.00266151,  0.00264065,
     0.00257886,  0.00246894,  0.00231978,  0.00213206,  0.00191535,
     0.00167489,  0.00142112,  0.00116156,  0.00090716,  0.00066616,
     0.00044873,  0.00026227,  0.00011472,  0.00001135, -0.00004331,
    -0.00004725,  0.00000000,  0.00009665,  0.00023889,  0.00042126,
     0.00063610,  0.00087500,  0.00112753,  0.00138360,  0.00163154,
     0.00186146,  0.00206177,  0.00222514,  0.00234085,  0.00240789,
     0.00241989,  0.00237175,  0.00226963,  0.00211372,  0.00191145,
     0.00166944,  0.00139798,  0.00110810,  0.00081252,  0.00052378,
     0.00025532,  0.00001930, -0.00017243, -0.00031014, -0.00038575,
    -0.00039387, -0.00033165, -0.00019931,  0.00000000,  0.00026013,
     0.00057211,  0.00092454,  0.00130381,  0.00169486,  0.00208146,
     0.00244720,  0.00277575,  0.00305229,  0.00326292,  0.00339712,
     0.00344584,  0.00340487,  0.00327470,  0.00305457,  0.00275329,
     0.00238031,  0.00194957,  0.00147779,  0.00098353,  0.00048771,
     0.00001204, -0.00042216, -0.00079408, -0.00108504, -0.00127877,
    -0.00136258, -0.00132784, -0.00117059, -0.00089187, -0.00049789,
     0.00000000,  0.00058554,  0.00123806,  0.00193307,  0.00264318,
     0.00333898,  0.00399025,  0.00456706,  0.00504100,  0.00538654,
     0.00558190,  0.00561042,  0.00546133,  0.00512984,  0.00462087,
     0.00394329,  0.00311532,  0.00216248,  0.00111596,  0.00001411,
    -0.00110139, -0.00218483, -0.00318792, -0.00406261, -0.00476159,
    -0.00524066, -0.00545995, -0.00538558, -0.00499112, -0.00425865,
    -0.00317975, -0.00175614,  0.00000000,  0.00206603,  0.00440922,
     0.00698740,  0.00975003,  0.01263942,  0.01559237,  0.01854190,
     0.02141906,  0.02415516,  0.02668356,  0.02894165,  0.03087317,
     0.03242858,  0.03356919,  0.03426578,  0.03449970,  0.03426578,
     0.03356919,  0.03242858,  0.03087317,  0.02894165,  0.02668356,
     0.02415516,  0.02141906,  0.01854190,  0.01559237,  0.01263942,
     0.00975003,  0.00698740,  0.00440922,  0.00206603,  0.00000000,
    -0.00175614, -0.00317975, -0.00425865, -0.00499112, -0.00538558,
    -0.00545995, -0.00524066, -0.00476159, -0.00406261, -0.00318792,
    -0.00218483, -0.00110139,  0.00001411,  0.00111596,  0.00216248,
     0.00311532,  0.00394329,  0.00462087,  0.00512984,  0.00546133,
     0.00561042,  0.00558190,  0.00538654,  0.00504100,  0.00456706,
     0.00399025,  0.00333898,  0.00264318,  0.00193307,  0.00123806,
     0.00058554,  0.00000000, -0.00049789, -0.00089187, -0.00117059,
    -0.00132784, -0.00136258, -0.00127877, -0.00108504, -0.00079408,
    -0.00042216,  0.00001204,  0.00048771,  0.00098353,  0.00147779,
     0.00194957,  0.00238031,  0.00275329,  0.00305457,  0.00327470,
     0.00340487,  0.00344584,  0.00339712,  0.00326292,  0.00305229,
     0.00277575,  0.00244720,  0.00208146,  0.00169486,  0.00130381,
     0.00092454,  0.00057211,  0.00026013,  0.00000000, -0.00019931,
    -0.00033165, -0.00039387, -0.00038575, -0.00031014, -0.00017243,
     0.00001930,  0.00025532,  0.00052378,  0.00081252,  0.00110810,
     0.00139798,  0.00166944,  0.00191145,  0.00211372,  0.00226963,
     0.00237175,  0.00241989,  0.00240789,  0.00234085,  0.00222514,
     0.00206177,  0.00186146,  0.00163154,  0.00138360,  0.00112753,
     0.00087500,  0.00063610,  0.00042126,  0.00023889,  0.00009665,
     0.00000000, -0.00004725, -0.00004331,  0.00001135,  0.00011472,
     0.00026227,  0.00044873,  0.00066616,  0.00090716,  0.00116156,
     0.00142112,  0.00167489,  0.00191535,  0.00213206,  0.00231978,
     0.00246894,  0.00257886,  0.00264065,  0.00266151,  0.00263303,
     0.00254717,  0.00243290,  0.00227510,  0.00208671,  0.00186954,
     0.00163348,  0.00138360,  0.00112885,  0.00087507,  0.00062998,
     0.00039892,  0.00018761,  0.00000000, -0.00016067, -0.00029231,
    -0.00039427, -0.00046645, -0.00051054, -0.00052815, -0.00052287,
    -0.00049749, -0.00045675, -0.00040409, -0.00034457, -0.00028154,
    -0.00021969, -0.00016150, -0.00011114, -0.00006961, -0.00003969,
    -0.00002114,  0.00472215,
};

static const double ssb_hi[SSB_FIR_TAPS] = {
     0.00143245, -0.00000420, -0.00000391,  0.00000000,  0.00001095,
     0.00003213,  0.00006664,  0.00011662,  0.00018418,  0.00027000,
     0.00037485,  0.00049749,  0.00063712,  0.00079043,  0.00095516,
     0.00112611,  0.00129974,  0.00146956,  0.00163127,  0.00177775,
     0.00190482,  0.00200551,  0.00207676,  0.00211260,  0.00211193,
     0.00207071,  0.00199041,  0.00186954,  0.00171252,  0.00152017,
     0.00130041,  0.00105507,  0.00079872,  0.00052941,  0.00026008,
    -0.00000000, -0.00024317, -0.00046143, -0.00064675, -0.00079336,
    -0.00089525, -0.00094956, -0.00095327, -0.00090716, -0.00081172,
    -0.00067157, -0.00049067, -0.00027697, -0.00003741,  0.00021776,
     0.00047975,  0.00073727,  0.00098129,  0.00120098,  0.00138870,
     0.00153568,  0.00163702,  0.00168747,  0.00168592,  0.00163154,
     0.00152766,  0.00137763,  0.00118936,  0.00096961,  0.00073043,
     0.00048135,  0.00023360, -0.00000000, -0.00020818, -0.00038021,
    -0.00050642, -0.00057906, -0.00059229, -0.00054291, -0.00042986,
    -0.00025532, -0.00002352,  0.00025806,  0.00058023,  0.00093129,
     0.00129843,  0.00166733,  0.00202367,  0.00235273,  0.00264113,
     0.00287619,  0.00304780,  0.00314768,  0.00317087,  0.00311513,
     0.00298192,  0.00277575,  0.00250495,  0.00218021,  0.00181580,
     0.00142731,  0.00103286,  0.00065138,  0.00030085, -0.00000000,
    -0.00023444, -0.00038779, -0.00044828, -0.00040739, -0.00026069,
    -0.00000805,  0.00034646,  0.00079408,  0.00132212,  0.00191382,
     0.00254920,  0.00320569,  0.00385893,  0.00448372,  0.00505515,
     0.00554939,  0.00594513,  0.00622413,  0.00637248,  0.00638119,
     0.00624679,  0.00597184,  0.00556498,  0.00504100,  0.00442062,
     0.00372971,  0.00299883,  0.00226216,  0.00155612,  0.00091915,
     0.00038838, -0.00000000, -0.00021299, -0.00022198, -0.00000428,
     0.00045621,  0.00116782,  0.00213010,  0.00333410,  0.00476159,
     0.00638576,  0.00817139,  0.01007572,  0.01204963,  0.01403890,
     0.01598567,  0.01783041,  0.01951336,  0.02097680,  0.02216667,
     0.02303438,  0.02353865,  0.02364668,  0.02333563,  0.02259337,
     0.02141906,  0.01982362,  0.01782939,  0.01546963,  0.01278809,
     0.00983710,  0.00667733,  0.00337488,  0.00000000, -0.00337488,
    -0.00667733, -0.00983710, -0.01278809, -0.01546963, -0.01782939,
    -0.01982362, -0.02141906, -0.02259337, -0.02333563, -0.02364668,
    -0.02353865, -0.02303438, -0.02216667, -0.02097680, -0.01951336,
    -0.01783041, -0.01598567, -0.01403890, -0.01204963, -0.01007572,
    -0.00817139, -0.00638576, -0.00476159, -0.00333410, -0.00213010,
    -0.00116782, -0.00045621,  0.00000428,  0.00022198,  0.00021299,
     0.00000000, -0.00038838, -0.00091915, -0.00155612, -0.00226216,
    -0.00299883, -0.00372971, -0.00442062, -0.00504100, -0.00556498,
    -0.00597184, -0.00624679, -0.00638119, -0.00637248, -0.00622413,
    -0.00594513, -0.00554939, -0.00505515, -0.00448372, -0.00385893,
    -0.00320569, -0.00254920, -0.00191382, -0.00132212, -0.00079408,
    -0.00034646,  0.00000805,  0.00026069,  0.00040739,  0.00044828,
     0.00038779,  0.00023444,  0.00000000, -0.00030085, -0.00065138,
    -0.00103286, -0.00142731, -0.00181580, -0.00218021, -0.00250495,
    -0.00277575, -0.00298192, -0.00311513, -0.00317087, -0.00314768,
    -0.00304780, -0.00287619, -0.00264113, -0.00235273, -0.00202367,
    -0.00166733, -0.00129843, -0.00093129, -0.00058023, -0.00025806,
     0.00002352,  0.00025532,  0.00042986,  0.00054291,  0.00059229,
     0.00057906,  0.00050642,  0.00038021,  0.00020818,  0.00000000,
    -0.00023360, -0.00048135, -0.00073043, -0.00096961, -0.00118936,
    -0.00137763, -0.00152766, -0.00163154, -0.00168592, -0.00168747,
    -0.00163702, -0.00153568, -0.00138870, -0.00120098, -0.00098129,
    -0.00073727, -0.00047975, -0.00021776,  0.00003741,  0.00027697,
     0.00049067,  0.00067157,  0.00081172,  0.00090716,  0.00095327,
     0.00094956,  0.00089525,  0.00079336,  0.00064675,  0.00046143,
     0.00024317,  0.00000000, -0.00026008, -0.00052941, -0.00079872,
    -0.00105507, -0.00130041, -0.00152017, -0.00171252, -0.00186954,
    -0.00199041, -0.00207071, -0.00211193, -0.00211260, -0.00207676,
    -0.00200551, -0.00190482, -0.00177775, -0.00163127, -0.00146956,
    -0.00129974, -0.00112611, -0.00095516, -0.00079043, -0.00063712,
    -0.00049749, -0.00037485, -0.00027000, -0.00018418, -0.00011662,
    -0.00006664, -0.00003213, -0.00001095, -0.00000000,  0.00000391,
     0.00000420, -0.00143245,
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
//
// Double-length history buffer, same trick antialias.c uses: every
// sample is written at two mirrored positions so a TAPS-long read never
// needs to wrap, keeping the loop branch-free for gcc's -O3 to
// autovectorize. Not hand-folded into half-length loops despite hr/hi's
// even/odd symmetry (see the file header derivation) - same tradeoff
// antialias.c already made: a mirrored-index access pattern is harder
// to autovectorize than this straight sequential loop, and at this tap
// count (~1300 multiply-adds/sample, ~125M/sec at 96kHz) the unfolded
// version is nowhere near a real constraint on a Pi 4.
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

// Stage 3: narrow real bandpass, post-demodulation - an 8-pole elliptic
// (Cauer) IIR, cascaded as 4 direct-form-II biquad sections. By this
// point in the chain the signal is already real, single-sided audio
// (stage 1 already resolved the image-reject question), so there is no
// symmetry concern left to design around.
//
// This replaced v3's original narrow filter - 4 IDENTICAL,
// synchronously-tuned RBJ resonator sections, runtime-adjustable via a
// simple trig formula (Q = f0/bandwidth_hz). That was cheap and easy to
// retune live, but a resonator cascade's skirt stays fundamentally
// gentle no matter how many sections get added - it rolls off, but never
// develops a real equiripple "wall". An elliptic design spends the same
// 8 poles very differently: equiripple ripple in the passband, and
// transmission zeros placed right at the band edges, buying a ~1.9:1
// shape factor (-60dB bandwidth : -6dB bandwidth) - in the range of a
// real CW crystal filter, and well past what the resonator cascade ever
// reached. See docs/dsp_design_notes/rx_audio_demod_design.md §8 for the
// measured comparison (the two shapes plotted side by side) and the
// group-delay/ring-time cost that comes with it (small: ~2.6ms group
// delay at CW_PITCH_HZ vs ~1.9ms for the old cascade, well under a CW
// element's duration at any real keying speed).
//
// The cost that DOES matter: elliptic coefficients aren't a simple trig
// formula the way the old RBJ biquad's were - computing them means
// solving elliptic integrals (scipy.signal.ellip did this once, offline,
// not something to redo from an audio callback). So this stage is now a
// FIXED design, like stage 1's FIR coefficients below - not
// runtime-tunable the way v3's first cut was (rx_audio_set_filter_bw()
// existed for exactly one v3 revision and is gone again).
//
// Design point: order=4 (8 poles total), 0.5dB passband ripple, 50dB
// stopband, centered on CW_PITCH_HZ with a ~300Hz -3dB width -
// scipy.signal.ellip(4, 0.5, 50, [(700-150)/48000, (700+150)/48000],
// btype='bandpass', output='sos') at Fs=96000. Each row below is one
// second-order section as {b0, b1, b2, a1, a2} - scipy's sos convention
// already normalizes a0 to 1.0 per section (confirmed to ~1e-16 before
// trusting it here), matching how biquad_apply() below is written.
struct biquad_state {
    double b0, b1, b2, a1, a2;   // coefficients
    double x1, x2, y1, y2;       // history
};

#define NARROW_FILTER_SECTIONS 4

static const double narrow_filter_coeffs[NARROW_FILTER_SECTIONS][5] = {
    // { b0, b1, b2, a1, a2 }
    { 0.0031347125317966271, -0.0062262711904984037, 0.003134712531796628, -1.9880750263609597, 0.99051500563718153 },
    { 1, -1.9997094875296553, 1, -1.9906166255419793, 0.99224567397195218 },
    { 1, -1.9948828058283654, 0.99999999999999989, -1.9932472877766807, 0.99636250549261574 },
    { 1, -1.9992168539672679, 0.99999999999999978, -1.9963811875913184, 0.99766425853740737 },
};

struct narrow_filter_state {
    struct biquad_state stage[NARROW_FILTER_SECTIONS];
};

static struct narrow_filter_state narrow_filter;

static double biquad_apply(struct biquad_state *f, double x) {
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
             - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

static double narrow_filter_apply(struct narrow_filter_state *f, double x) {
    double y = x;
    for (int i = 0; i < NARROW_FILTER_SECTIONS; i++)
        y = biquad_apply(&f->stage[i], y);
    return y;
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
// estimate (of the STAGE 3 output, so it reflects the narrow filter's
// real selectivity - see rx_audio_debug_agc_envelope() in rx_audio.h);
// the gain applied each sample is AGC_TARGET_AMPLITUDE / agc_env, so as
// agc_env rises/falls the output rides back toward the target instead
// of tracking the raw input amplitude directly.
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

    for (int i = 0; i < NARROW_FILTER_SECTIONS; i++) {
        narrow_filter.stage[i].b0 = narrow_filter_coeffs[i][0];
        narrow_filter.stage[i].b1 = narrow_filter_coeffs[i][1];
        narrow_filter.stage[i].b2 = narrow_filter_coeffs[i][2];
        narrow_filter.stage[i].a1 = narrow_filter_coeffs[i][3];
        narrow_filter.stage[i].a2 = narrow_filter_coeffs[i][4];
        narrow_filter.stage[i].x1 = narrow_filter.stage[i].x2 = 0.0;
        narrow_filter.stage[i].y1 = narrow_filter.stage[i].y2 = 0.0;
    }

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
        // Stage 1: wide complex bandpass - image rejection only, see the
        // file header for why this replaced v2's single combined filter.
        double fi, fq;
        ssb_filter_apply(&ssb_state, i_samples[k], q_samples[k], &fi, &fq);

        // Stage 2: mix up to CW_PITCH_HZ and keep only the real part.
        // Re[(fi + j*fq) * (cos + j*sin)] = fi*cos - fq*sin.
        int bfo_cos, bfo_sin;
        vfo_read_iq(&bfo, &bfo_cos, &bfo_sin);
        double c = (double)bfo_cos / 1073741824.0;
        double s = (double)bfo_sin / 1073741824.0;
        double audio = fi * c - fq * s;

        // Stage 3: narrow real bandpass - the actual single-signal
        // selectivity, decoupled from stage 1's image rejection.
        double narrowed = narrow_filter_apply(&narrow_filter, audio);

        // Stage 4: AGC - track a smoothed envelope of |narrowed| (fast
        // attack so a strong signal keying up doesn't clip before the
        // envelope catches up, slow release so gain doesn't pump on
        // every CW dit/dah gap), then scale so the envelope itself sits
        // at AGC_TARGET_AMPLITUDE regardless of how large or small the
        // raw input actually is - see the constants above for why a
        // fixed multiplier alone can't work here.
        double mag = fabs(narrowed);
        double alpha = (mag > agc_env) ? agc_attack_alpha : agc_release_alpha;
        agc_env += alpha * (mag - agc_env);

        double gain = AGC_TARGET_AMPLITUDE / (agc_env > 1e-9 ? agc_env : 1e-9);
        if (gain > AGC_MAX_GAIN) gain = AGC_MAX_GAIN;

        double sample = narrowed * gain * rx_volume;
        if (sample >  2000000000.0) sample =  2000000000.0;
        if (sample < -2000000000.0) sample = -2000000000.0;
        out[k] = (int32_t)sample;
    }
}
