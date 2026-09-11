// radio.c

#include "cw.h"
#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include "sound.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int freq_hdr = 7030000;
int in_tx = 0;

// xtal_filter_center is the crystal filter's own real, measured
// passband center (docs/dsp_design_notes/antialias_filter_design.md:
// ~40.0124 MHz on the board this default came from) - a physical
// property of the hardware, not a clock setting. RX and TX both aim to
// place their own mixing product as close to this frequency as
// possible, but they used to be forced to share a single value
// (bfo_freq, below) to do it, even though RX wants that value to sit
// AT the true center while TX deliberately wants it off to one side
// (see bfo_freq's comment). Splitting the "where is the filter's real
// center" question out into its own named constant lets each direction
// aim at it independently - see
// docs/dsp_design_notes/antialias_filter_design.md §3 for the design
// discussion this came out of. Varies board to board like bfo_freq
// does; override in hw_settings.ini if a different board's filter
// measures differently.
int xtal_filter_center = 40012400;

// bfo_freq is the real si5351 clk1 frequency used only while
// transmitting (radio_tx_apply(), below) - the literal "BFO" in the
// traditional sense, feeding this board's single balanced modulator.
// It is deliberately NOT xtal_filter_center - it sits TX_IF_OFFSET_HZ
// (cw.c) above it, on purpose: with a single real mixer and no I/Q
// hardware on this board (see the schematic), that offset is what
// separates the wanted CW TX product (which lands back near
// xtal_filter_center) from its unwanted mirror image, pushed out into
// the filter's stopband instead - full derivation in cw.c's
// TX_IF_OFFSET_HZ comment. In other words, bfo_freq == xtal_filter_center
// + TX_IF_OFFSET_HZ by calibration (not enforced anywhere in code) -
// this default (40035000) and xtal_filter_center's default (40012400)
// came from the same board, so they already agree out of the box. If
// RX sounds off-center, that's xtal_filter_center to adjust now, not
// this - bfo_freq has no RX-side consequence any more (that's the
// point of the split above). TX_IF_OFFSET_HZ is the one meant to be
// re-swept on the bench (cw.c says how); changing bfo_freq itself
// without re-deriving TX_IF_OFFSET_HZ to match will throw away the CW
// image suppression that constant depends on.
int bfo_freq = 40035000;
struct vfo lo;

// "Master" gates the WM8731's whole analog output path - the same
// DAC/output-mixer chain that carries CW (and any future TX) audio out.
// Real sbitx's own set_tx_power_levels() (sbitx.c) drives this exact same
// ALSA control to 95 during TX, with the comment "Muting Master also
// mutes the PA, killing TX power regardless of the DRIVE setting" -
// i.e. this isn't a volume knob, it's what gates how much drive actually
// reaches the exciter/PA. Previously set to 70 here, an unverified guess
// (see the cw.c sample-scaling comment for the still-open, separate
// question of whether the digital sample amplitude feeding the DAC is
// also under-calibrated) - matching sbitx's bench-confirmed 95 instead.
#define TX_MASTER_VOL 95

void radio_tune_to(uint32_t f) {
  freq_hdr = f;
  // Mixer 1 (clk2) places f exactly at the crystal filter's real
  // center - simple by construction now that this doesn't have to
  // also carry bfo_freq's deliberate TX-only offset (see
  // xtal_filter_center's comment above). Mixer 2 (clk1) is what
  // actually finishes the trip to RX_IF_FREQ_HZ from there; it's set
  // to xtal_filter_center + RX_IF_FREQ_HZ at startup (minibitx.c) and
  // restored to that same value here every time RX resumes
  // (radio_tx_apply(), below). This function must not touch clk1
  // itself - it's also called for plain RX retuning with no TX
  // transition involved.
  si5351bx_setfreq(2, f + xtal_filter_center);
  vfo_start(&lo, RX_IF_FREQ_HZ, lo.phase);
  set_lpf_40mhz(f); // enable the correct LPF for this band
}

// ---- TX worker thread ------------------------------------------------
//
// radio_set_tx() used to do its GPIO/relay-settling usleep()s and its
// sound_mixer() call inline, on whatever thread called it. That's fine
// for hamlib's or hpsdr_p1.c's network threads, but cw.c calls it from
// cw_poll_key(), which runs once per ~10.7ms audio block on the AUDIO
// thread (sound.c's audio_loop()). A single call there blocked for
// 20ms+ (PTT settle + relay settle + a fresh ALSA mixer handle open/
// attach/load/close), guaranteeing a missed capture period - exactly
// the "sound: xrun, recovering" logged on every key transition - and
// also making the physical key feel sluggish, since cw_poll_key()
// couldn't return to re-poll it until the blocking sequence finished.
//
// Fix: radio_set_tx() keeps its exact signature and still updates
// in_tx immediately/synchronously (cheap - every other guard in the
// codebase that reads in_tx keeps seeing a prompt, correct value).
// The actual slow hardware sequence is handed off to a dedicated
// worker thread via a mutex/condvar/pending-flag, so the calling
// thread (audio thread included) never blocks.
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tx_cond = PTHREAD_COND_INITIALIZER;
static int tx_pending = 0;    // 1 = worker has a state change to apply
static int tx_pending_on = 0; // the state to apply (1 = TX, 0 = RX)
static pthread_once_t tx_worker_once = PTHREAD_ONCE_INIT;

static void radio_tx_apply(int tx_on) {
  if (tx_on) {
    // Entering TX: clk1 switches from its RX value
    // (xtal_filter_center + RX_IF_FREQ_HZ) to the real BFO
    // (bfo_freq) - see bfo_freq's comment above for why TX needs a
    // different, deliberately off-center clk1 than RX does. clk2 is
    // retuned to match: mixer 1 must deliver freq_hdr at the antenna
    // from whatever mixer 2 actually hands it, which - per
    // TX_IF_OFFSET_HZ's own bench derivation in cw.c - lands
    // CW_PITCH_HZ short of xtal_filter_center, not exactly on it
    // (a real, deliberate residual from that calibration, not an
    // oversight here). This produces the exact same clk2 value the
    // old `freq_hdr + bfo_freq - RX_IF_FREQ_HZ + CW_PITCH_HZ`
    // formula did - they're algebraically identical for today's
    // constants (the old formula's dependence on RX_IF_FREQ_HZ was
    // never really about RX_IF_FREQ_HZ; it only existed to cancel
    // out bfo_freq's own now-removed dependence on it). Applied
    // here - the one place all TX (straight key via cw.c, and remote
    // MOX via hpsdr_p1.c, possibly after its own radio_tune_to() to a
    // split TX frequency) funnels through - rather than in
    // radio_tune_to() itself, since that call is also used for plain
    // RX retuning and must not carry either TX-only clock value.
    // Both clocks are set before PTT/the relay so they're correct
    // before any RF actually reaches the antenna.
    si5351bx_setfreq(1, bfo_freq);
    si5351bx_setfreq(2, freq_hdr + xtal_filter_center - CW_PITCH_HZ);
    radio_hw_set_ptt(1);
    usleep(20000); // let PTT assert before keying the relay
    radio_hw_set_tx_relay(1);
    sound_mixer("hw:0", "Master", TX_MASTER_VOL); // feed the exciter
  } else {
    sound_mixer("hw:0", "Master", 0); // mute before dropping the relay
    radio_hw_set_ptt(0);
    usleep(5000); // let the relay settle before dropping PTT
    radio_hw_set_tx_relay(0);
    // Restore clk1 to its RX value and clk2 to the plain RX formula
    // now that TX has fully disengaged - matters most for the
    // straight-key path, which has no separate radio_tune_to() call
    // to undo this on its own (unlike hpsdr_p1.c's MOX-with-split-TX-
    // frequency path, which already retunes back to last_rx_freq
    // after MOX off - this is a harmless no-op redundant restore in
    // that case).
    si5351bx_setfreq(1, xtal_filter_center + RX_IF_FREQ_HZ);
    si5351bx_setfreq(2, freq_hdr + xtal_filter_center);
  }
}

static void *radio_tx_worker(void *arg) {
  (void)arg;

  for (;;) {
    pthread_mutex_lock(&tx_mutex);
    while (!tx_pending)
      pthread_cond_wait(&tx_cond, &tx_mutex);
    int on = tx_pending_on;
    tx_pending = 0;
    pthread_mutex_unlock(&tx_mutex);

    radio_tx_apply(on);
  }

  return NULL;
}

static void radio_tx_worker_start(void) {
  pthread_t worker;
  pthread_create(&worker, NULL, radio_tx_worker, NULL);
}

// switch between RX and TX
void radio_set_tx(int tx_on) {
  pthread_once(&tx_worker_once, radio_tx_worker_start);

  in_tx = tx_on ? 1 : 0; // mirrors sbitx: hardware state follows intent,
                         // updated immediately so other threads' guards
                         // (cw_tx_active(), the network MOX logic, ...)
                         // see the new state right away, even though the
                         // physical relay/mixer change is still pending
                         // on the worker thread below

  pthread_mutex_lock(&tx_mutex);
  tx_pending_on = tx_on;
  tx_pending = 1;
  pthread_cond_signal(&tx_cond);
  pthread_mutex_unlock(&tx_mutex);
}
