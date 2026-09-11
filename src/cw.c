// cw.c
//
// Straight-key CW support: polls a physical key wired into GPIO
// (radio_hw.h's CW_KEY line), holds PTT/the T/R relay for the duration
// of a keying burst, and generates a single tone gated by a
// table-driven envelope. minibitx stays a support layer for external
// SDR apps so there is no iambic keyer, no macros - this gets clean dots and dashes
// out, nothing more.
//
// Table-driven Blackman-Harris attack/decay envelope: 480 samples (5ms
// rise and fall time at 96kHz, similar to the implementation in sBitx's own 
// CW keyer uses in modem_cw.c), rising from ~0 to 1.0. Table-driven so
// a different keying shape later is a table swap, not a logic change.
// The same table is used forward for attack (key down) and backward
// for decay (key up), same trick sbitx's own keyer uses.

#include "cw.h"
#include "radio.h"
#include "radio_hw.h"
#include "vfo.h"
#include <stdio.h>

#define CW_ENVELOPE_LEN 480

// CW_PITCH_HZ (the sidetone/keying pitch) now lives in cw.h - radio.c
// needs it too, to correct clk2 during TX (see cw.h's comment on it and
// radio_tx_apply() in radio.c).

// Where the actual TX-modulating tone sits, relative to CW_PITCH_HZ.
//
// The crystal filter's passband is fixed by the crystals themselves and
// doesn't move - measured (docs/dsp_design_notes/antialias_filter_design.md)
// at ~40.0124 MHz center, ~35kHz wide, with a steep skirt above +17.4kHz
// (down to -61dB by +28.5kHz). bfo_freq (radio.c, default 40035000) is
// *not* that center - it's offset ~22.6kHz above it, on purpose: this is
// the same "BFO at the filter's edge" placement real sbitx's own design
// article describes (VU2ESE, "The sBitx", section "The local oscillator(s)":
// clock 1 sits ~25kHz above the filter's passband center for exactly this
// reason). The RX chain already relies on this same offset without saying
// so explicitly - antialias_filter_design.md's own analysis assumes it.
//
// A single real mixer (this hardware has one, confirmed against the
// schematic - no I/Q/quadrature stage) always produces both bfo_freq+f
// and bfo_freq-f from an audio tone at f. With the BFO sitting at the
// filter's edge instead of its center, only ONE of those two lands inside
// the passband:
//   - difference (bfo_freq - f): with f = TX_IF_OFFSET_HZ + CW_PITCH_HZ
//     (~23.3 kHz), bfo_freq - f lands right at the filter's measured
//     center - solidly in the passband.
//   - sum (bfo_freq + f): lands ~28.5kHz above the passband's upper edge,
//     right where the measured data shows -61dB of rejection.
//
// This is why the earlier attempt at "generate the tone 24kHz higher"
// (see the old comment this replaced, and 03_tx_processing_pipeline.md's
// "Known limitations") made things worse instead of better: it reused
// RX_IF_FREQ_HZ (24000, a different constant - the RX-side second-IF
// target used in radio_tune_to()'s clk2 formula) rather than this filter's
// actual measured offset from bfo_freq, so it landed on the wrong side of
// the skirt. 22600 comes from bfo_freq (40035000) minus
// xtal_filter_center (radio.c, 40012400 - this radio's measured filter
// center) - it's a starting point for a bench check, not a value
// guaranteed correct on every board without verifying against that
// board's own filter (the measured data behind 22600 came from a
// different, "representative" unit, not this exact one). Note this
// difference product actually lands CW_PITCH_HZ short of
// xtal_filter_center, not exactly on it (40035000 - 23300 = 40011700,
// vs xtal_filter_center's 40012400) - a small, real residual from how
// this constant was derived, not an oversight; see radio_tx_apply()'s
// clk2 comment (radio.c) for where that residual gets accounted for.
//
// TO RE-TUNE ON THE BENCH: this constant only affects TX - no RX
// implications, unlike bfo_freq, so it's safe to sweep on its own. Try
// steps of +-2000 to +-4000 Hz around 22600, rebuilding and keying down
// at the same test frequency each time. Read the *image* suppression
// off a connected SDR's spectrum/waterfall (compare dB levels between
// the wanted peak and the residual second tone, same as before) and
// bisect toward whichever direction deepens the null - no wattmeter
// needed for this, just the spectrum display. Log trials here as you
// go, e.g.:
//   22600 -> ~40dB down (baseline)
#define TX_IF_OFFSET_HZ 22600

// How many cw_poll_key() calls (audio blocks) to hold TX after the key
// goes up before actually releasing PTT/the relay - standard semi
// break-in, so the relay doesn't chatter between individual dits and
// dahs. ~300ms at the assumed ~10.7ms/block cadence above.
#define CW_HANG_POLLS 28

static const double cw_envelope[CW_ENVELOPE_LEN] = {
    0.000060, 0.000061, 0.000062, 0.000065, 0.000070, 0.000075, 0.000082, 0.000090, 0.000099,
    0.000110, 0.000121, 0.000135, 0.000149, 0.000165, 0.000182, 0.000200, 0.000220, 0.000241,
    0.000264, 0.000288, 0.000314, 0.000341, 0.000369, 0.000400, 0.000432, 0.000465, 0.000500,
    0.000537, 0.000576, 0.000617, 0.000659, 0.000704, 0.000750, 0.000798, 0.000848, 0.000901,
    0.000956, 0.001012, 0.001071, 0.001133, 0.001197, 0.001263, 0.001331, 0.001403, 0.001477,
    0.001553, 0.001633, 0.001715, 0.001800, 0.001888, 0.001979, 0.002073, 0.002171, 0.002271,
    0.002375, 0.002483, 0.002594, 0.002709, 0.002827, 0.002949, 0.003075, 0.003205, 0.003339,
    0.003477, 0.003620, 0.003766, 0.003917, 0.004073, 0.004233, 0.004398, 0.004568, 0.004743,
    0.004923, 0.005108, 0.005298, 0.005494, 0.005695, 0.005901, 0.006114, 0.006332, 0.006556,
    0.006786, 0.007023, 0.007265, 0.007515, 0.007770, 0.008032, 0.008302, 0.008578, 0.008861,
    0.009151, 0.009448, 0.009753, 0.010066, 0.010386, 0.010714, 0.011050, 0.011394, 0.011747,
    0.012108, 0.012477, 0.012855, 0.013242, 0.013637, 0.014042, 0.014456, 0.014880, 0.015313,
    0.015756, 0.016208, 0.016671, 0.017144, 0.017627, 0.018120, 0.018624, 0.019139, 0.019665,
    0.020201, 0.020749, 0.021309, 0.021880, 0.022462, 0.023057, 0.023663, 0.024282, 0.024913,
    0.025556, 0.026212, 0.026881, 0.027563, 0.028258, 0.028967, 0.029689, 0.030424, 0.031173,
    0.031937, 0.032714, 0.033506, 0.034312, 0.035133, 0.035968, 0.036819, 0.037684, 0.038565,
    0.039461, 0.040373, 0.041301, 0.042244, 0.043204, 0.044180, 0.045173, 0.046182, 0.047207,
    0.048250, 0.049310, 0.050387, 0.051481, 0.052593, 0.053723, 0.054870, 0.056036, 0.057219,
    0.058421, 0.059642, 0.060881, 0.062139, 0.063416, 0.064712, 0.066027, 0.067362, 0.068717,
    0.070091, 0.071484, 0.072898, 0.074332, 0.075787, 0.077261, 0.078756, 0.080272, 0.081809,
    0.083367, 0.084946, 0.086546, 0.088167, 0.089810, 0.091475, 0.093161, 0.094869, 0.096599,
    0.098352, 0.100126, 0.101923, 0.103742, 0.105584, 0.107448, 0.109335, 0.111245, 0.113178,
    0.115134, 0.117113, 0.119115, 0.121141, 0.123190, 0.125263, 0.127359, 0.129479, 0.131622,
    0.133790, 0.135981, 0.138196, 0.140435, 0.142699, 0.144986, 0.147298, 0.149633, 0.151994,
    0.154378, 0.156787, 0.159220, 0.161678, 0.164160, 0.166666, 0.169198, 0.171753, 0.174334,
    0.176938, 0.179568, 0.182222, 0.184901, 0.187604, 0.190332, 0.193084, 0.195861, 0.198663,
    0.201489, 0.204340, 0.207215, 0.210114, 0.213038, 0.215987, 0.218959, 0.221956, 0.224978,
    0.228023, 0.231093, 0.234186, 0.237304, 0.240445, 0.243610, 0.246799, 0.250012, 0.253248,
    0.256508, 0.259791, 0.263097, 0.266427, 0.269779, 0.273154, 0.276553, 0.279973, 0.283417,
    0.286883, 0.290371, 0.293881, 0.297413, 0.300967, 0.304542, 0.308139, 0.311758, 0.315397,
    0.319057, 0.322739, 0.326440, 0.330162, 0.333905, 0.337667, 0.341449, 0.345251, 0.349072,
    0.352912, 0.356771, 0.360649, 0.364545, 0.368459, 0.372392, 0.376342, 0.380309, 0.384294,
    0.388296, 0.392314, 0.396349, 0.400400, 0.404466, 0.408549, 0.412646, 0.416759, 0.420886,
    0.425027, 0.429183, 0.433353, 0.437535, 0.441731, 0.445940, 0.450161, 0.454395, 0.458640,
    0.462897, 0.467165, 0.471443, 0.475732, 0.480031, 0.484340, 0.488658, 0.492986, 0.497321,
    0.501665, 0.506017, 0.510376, 0.514743, 0.519116, 0.523495, 0.527881, 0.532271, 0.536667,
    0.541068, 0.545473, 0.549882, 0.554294, 0.558709, 0.563127, 0.567547, 0.571969, 0.576393,
    0.580817, 0.585241, 0.589666, 0.594090, 0.598514, 0.602936, 0.607356, 0.611775, 0.616190,
    0.620603, 0.625012, 0.629417, 0.633817, 0.638213, 0.642603, 0.646987, 0.651365, 0.655736,
    0.660100, 0.664456, 0.668804, 0.673144, 0.677474, 0.681795, 0.686105, 0.690405, 0.694694,
    0.698971, 0.703236, 0.707489, 0.711729, 0.715955, 0.720168, 0.724366, 0.728549, 0.732716,
    0.736868, 0.741004, 0.745123, 0.749224, 0.753308, 0.757373, 0.761420, 0.765447, 0.769455,
    0.773442, 0.777409, 0.781355, 0.785279, 0.789181, 0.793060, 0.796917, 0.800750, 0.804559,
    0.808344, 0.812103, 0.815838, 0.819546, 0.823229, 0.826885, 0.830513, 0.834114, 0.837687,
    0.841231, 0.844746, 0.848232, 0.851688, 0.855114, 0.858509, 0.861873, 0.865206, 0.868506,
    0.871774, 0.875010, 0.878212, 0.881380, 0.884514, 0.887614, 0.890679, 0.893709, 0.896703,
    0.899661, 0.902582, 0.905467, 0.908315, 0.911125, 0.913897, 0.916631, 0.919326, 0.921982,
    0.924599, 0.927177, 0.929714, 0.932211, 0.934667, 0.937082, 0.939456, 0.941788, 0.944078,
    0.946326, 0.948531, 0.950694, 0.952813, 0.954889, 0.956921, 0.958909, 0.960853, 0.962752,
    0.964607, 0.966417, 0.968181, 0.969900, 0.971573, 0.973200, 0.974781, 0.976316, 0.977803,
    0.979245, 0.980639, 0.981986, 0.983285, 0.984537, 0.985742, 0.986898, 0.988006, 0.989067,
    0.990078, 0.991042, 0.991957, 0.992823, 0.993640, 0.994408, 0.995127, 0.995797, 0.996418,
    0.996989, 0.997511, 0.997984, 0.998406, 0.998780, 0.999103, 0.999377, 0.999601, 0.999776,
    0.999900, 0.999975, 1.000000,
};
static struct vfo cw_tone;        // CW_PITCH_HZ - local sidetone monitor only
static struct vfo cw_tx_carrier;  // CW_PITCH_HZ + TX_IF_OFFSET_HZ - actual TX drive
static int envelope_pos = 0;   // 0 = silent, CW_ENVELOPE_LEN-1 = full output
static int key_down = 0;       // last polled key state
static int tx_active = 0;      // PTT/relay currently asserted for a keying burst
static int hang_counter = 0;   // polls remaining before TX releases

void cw_init(void) {
    vfo_start(&cw_tone, CW_PITCH_HZ, 0);
    vfo_start(&cw_tx_carrier, CW_PITCH_HZ + TX_IF_OFFSET_HZ, 0);
    envelope_pos = 0;
    key_down = 0;
    tx_active = 0;
    hang_counter = 0;
}

void cw_poll_key(void) {
    key_down = radio_hw_key_down();

    if (key_down) {
        if (!tx_active) {
            tx_active = 1;
            radio_set_tx(1);
            //printf("key down!\n");
        }
        hang_counter = CW_HANG_POLLS;
    } else if (tx_active) {
        if (hang_counter > 0) {
            hang_counter--;
        } else {
            radio_set_tx(0);
            tx_active = 0;
        }
    }
}

int cw_tx_active(void) {
    return tx_active;
}

double cw_get_sample(void) {
    if (key_down) {
        if (envelope_pos < CW_ENVELOPE_LEN - 1) envelope_pos++;
    } else {
        if (envelope_pos > 0) envelope_pos--;
    }

    int tone = vfo_read(&cw_tone);           // Q30 fixed-point sine (vfo.c)
    double tone_f = (double)tone / 1073741824.0;
    return tone_f * cw_envelope[envelope_pos];
}

// The actual TX-modulating waveform - same envelope as cw_get_sample()
// (advanced there, once per sample; this reads the position, it doesn't
// advance it again), but at the IF-shifted carrier frequency instead of
// the bare sidetone pitch. See TX_IF_OFFSET_HZ above for why. Callers
// must call cw_get_sample() once per sample first (it owns the envelope
// advance) and this second, for the same sample, to get both the sidetone
// and the TX drive in sync.
double cw_get_tx_sample(void) {
    int tone = vfo_read(&cw_tx_carrier);
    double tone_f = (double)tone / 1073741824.0;
    return tone_f * cw_envelope[envelope_pos];
}
