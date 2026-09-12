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

// xtal_filter_center: the crystal filter's real, measured passband
// center (~40.0124 MHz on this board) - a physical property of the
// hardware, not a clock setting. RX aims its mixing product AT this
// value; bfo_freq (below) deliberately does not. See
// docs/dsp_design_notes/antialias_filter_design.md §3 for why the two
// used to be coupled (and aren't any more). Board-specific; override in
// hw_settings.ini if a different board's filter measures differently.
int xtal_filter_center = 40012400;

// bfo_freq: the real si5351 clk1 frequency used only while transmitting
// (radio_tx_apply(), below). Deliberately NOT xtal_filter_center - it
// sits TX_IF_OFFSET_HZ (cw.c) above it, which is what separates the
// wanted CW TX product from its image (full derivation in cw.c's
// TX_IF_OFFSET_HZ comment). bfo_freq == xtal_filter_center +
// TX_IF_OFFSET_HZ by calibration, not enforced in code - changing
// bfo_freq without re-deriving TX_IF_OFFSET_HZ to match throws away the
// CW image suppression that constant depends on. RX is unaffected
// either way (adjust xtal_filter_center for that).
int bfo_freq = 40035000;
struct vfo lo;

// "Master" gates the WM8731's whole analog output path, which is what
// actually feeds the exciter/PA during TX - not a volume knob (see
// docs/03_tx_processing_pipeline.md's TX_MASTER_VOL bullet for the
// sbitx-matching derivation of 95).
#define TX_MASTER_VOL 95

void radio_tune_to(uint32_t f) {
  freq_hdr = f;
  // clk2 places f at the crystal filter's real center; clk1 is left
  // untouched here (it's set at startup and only ever retuned by
  // radio_tx_apply() below) - this call is also used for plain RX
  // retuning with no TX transition involved, so it must not carry
  // either TX-only clock value.
  si5351bx_setfreq(2, f + xtal_filter_center);
  vfo_start(&lo, RX_IF_FREQ_HZ, lo.phase);
  set_lpf_40mhz(f); // enable the correct LPF for this band
}

// TX transitions run on this dedicated worker thread rather than
// whatever thread calls radio_set_tx() - see
// docs/05_process_and_threading_model.md for why (short version: cw.c
// calls radio_set_tx() from the real-time audio thread, which must
// never block on the ~20ms+ PTT/relay/ALSA sequence below).
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tx_cond = PTHREAD_COND_INITIALIZER;
static int tx_pending = 0;    // 1 = worker has a state change to apply
static int tx_pending_on = 0; // the state to apply (1 = TX, 0 = RX)
static pthread_once_t tx_worker_once = PTHREAD_ONCE_INIT;

static void radio_tx_apply(int tx_on) {
  if (tx_on) {
    // clk1 -> bfo_freq, clk2 -> freq_hdr + xtal_filter_center -
    // CW_PITCH_HZ (a real, deliberate residual from TX_IF_OFFSET_HZ's
    // bench calibration - see cw.c - not an oversight). Algebraically
    // identical to the pre-split formula this replaced - see
    // docs/03_tx_processing_pipeline.md "Known limitations" if that
    // needs re-deriving. This is the one place all TX (straight key
    // via cw.c, remote MOX via hpsdr_p1.c) funnels through, rather
    // than radio_tune_to() itself, which is also used for plain RX
    // retuning. Capture is muted first - before PTT/the relay/either
    // clock, i.e. before any TX RF exists at all - see
    // sound_set_rx_capture()'s comment (sound.c) for why, and the
    // tx_off branch below for the matching restore.
    sound_set_rx_capture(0);
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
    // Restore clk1/clk2 to their RX values - matters most for the
    // straight-key path, which (unlike hpsdr_p1.c's MOX path) has no
    // separate radio_tune_to() call of its own to undo this.
    si5351bx_setfreq(1, xtal_filter_center + RX_IF_FREQ_HZ);
    si5351bx_setfreq(2, freq_hdr + xtal_filter_center);
    // Restore Capture only now that the relay has actually settled -
    // any earlier would feed the DSP chain raw relay-transient noise.
    sound_set_rx_capture(1);
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

  in_tx = tx_on ? 1 : 0; // updated synchronously so other threads'
                         // guards (cw_tx_active(), network MOX logic)
                         // see it right away - see
                         // docs/05_process_and_threading_model.md

  pthread_mutex_lock(&tx_mutex);
  tx_pending_on = tx_on;
  tx_pending = 1;
  pthread_cond_signal(&tx_cond);
  pthread_mutex_unlock(&tx_mutex);
}
