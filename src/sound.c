// sound.c
// Minimal ALSA full-duplex driver for minibitx.

#include "antialias.h"
#include "cw.h"
#include "decim48k.h"
#include "hpsdr_p1.h"
#include "hw_settings.h"
#include "radio.h"
#include "sound.h"
#include "usb_gadget.h"
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE 96000
#define CHANNELS 2         /* stereo: L = RX / R = Mic (capture) */
#define PERIOD_FRAMES 1024 /* frames per period (matches old cfg) */
#define MAX_FRAMES 4096

// WM8731 "Line" input path is a plain on/off switch, not a gain control
// (bench-confirmed: `amixer -c 0 sget 'Line'` shows `Capabilities:
// cswitch` only) - kept as a named constant so it reads as a deliberate
// boolean rather than a stray literal. RX_CAPTURE_GAIN_PERCENT below is
// the real analog gain stage.
#define RX_LINE_INPUT_ON 1

// WM8731 'Capture' - the real analog gain stage ahead of the ADC (no RF
// preamp anywhere in this RX chain). sound_mixer() maps this ALSA
// percent (0-100) onto the control's real 0-31 raw range
// (`percent * 31 / 100`). 70 (raw step 21, ~-3.0dB) is a bench-derived
// choice, not yet a final calibration - see
// docs/dsp_design_notes/rx_gain_and_level_calibration.md for the data
// behind it, and rx_clip_check() below for the ongoing safety net if
// some future signal ever proves it too hot.
#define RX_CAPTURE_GAIN_PERCENT 70

/* ------------------------------------------------------------------ */
/*  TX sample scaling - see docs/03_tx_processing_pipeline.md          */
/*  "Adjusting power levels" for what each constant means and the      */
/*  bench data behind its value; hw_settings.h for the per-band        */
/*  'scale' calibration table TX_SAMPLE_HEADROOM is anchored against   */
/* ------------------------------------------------------------------ */
#define TX_DRIVE 50
#define TX_SAMPLE_HEADROOM (1000000000.0 / (TX_DRIVE * HW_DEFAULT_TX_SCALE))
#define TX_SAMPLE_CLAMP 2000000000.0 // stay well inside int32 range
#define TX_GAIN_CORRECTION 0.045
// Fixed sidetone PCM peak (left channel only - never reaches the PA),
// deliberately independent of TX_GAIN_CORRECTION - see
// docs/03_tx_processing_pipeline.md for why that coupling used to bite.
#define SIDETONE_PEAK_AMPLITUDE 10000000.0

/* ------------------------------------------------------------------ */
/*  Module state                                                      */
/* ------------------------------------------------------------------ */
static snd_pcm_t *pcm_capture = NULL;
static snd_pcm_t *pcm_playback = NULL;
static pthread_t audio_thread;
static volatile int g_running = 0;

/* ------------------------------------------------------------------ */
/*  ALSA mixer helper                                                 */
/* ------------------------------------------------------------------ */
void sound_mixer(char *card_name, char *element, int make_on) {
  long min, max;
  snd_mixer_t *handle;
  snd_mixer_selem_id_t *sid;

  snd_mixer_open(&handle, 0);
  snd_mixer_attach(handle, card_name);
  snd_mixer_selem_register(handle, NULL, NULL);
  snd_mixer_load(handle);

  snd_mixer_selem_id_alloca(&sid);
  snd_mixer_selem_id_set_index(sid, 0);
  snd_mixer_selem_id_set_name(sid, element);
  snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

  if (!elem) {
    snd_mixer_close(handle);
    return;
  }

  if (snd_mixer_selem_has_capture_switch(elem))
    snd_mixer_selem_set_capture_switch_all(elem, make_on);
  else if (snd_mixer_selem_has_playback_switch(elem))
    snd_mixer_selem_set_playback_switch_all(elem, make_on);
  else if (snd_mixer_selem_has_playback_volume(elem)) {
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_set_playback_volume_all(elem, make_on * max / 100);
  } else if (snd_mixer_selem_has_capture_volume(elem)) {
    snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
    snd_mixer_selem_set_capture_volume_all(elem, make_on * max / 100);
  } else if (snd_mixer_selem_is_enumerated(elem))
    snd_mixer_selem_set_enum_item(elem, 0, make_on);

  snd_mixer_close(handle);
}

/* ------------------------------------------------------------------ */
/*  Codec hardware setup - barebones WM8731 init                      */
/* ------------------------------------------------------------------ */
void setup_audio_codec(void) {
  sound_mixer("hw:0", "Input Mux",
              0); // 'Line In' (bench-confirmed - see RX_CAPTURE_GAIN_PERCENT's comment above)
  sound_mixer("hw:0", "Line", RX_LINE_INPUT_ON); // just un-mutes the line path - see comment above
  sound_mixer("hw:0", "Capture", RX_CAPTURE_GAIN_PERCENT); // the real analog gain stage
  sound_mixer("hw:0", "Mic", 0);
  sound_mixer("hw:0", "Master", 0); // Mute local speaker
  sound_mixer("hw:0", "Output Mixer HiFi", 1);
  sound_mixer("hw:0", "Output Mixer Line Bypass", 0);
  sound_mixer("hw:0", "Output Mixer Mic Sidetone", 0);
}

// Mute/restore the WM8731 'Capture' gain around a TX burst - called
// from radio.c's radio_tx_apply() on every TX/RX transition. See
// docs/dsp_design_notes/rx_gain_and_level_calibration.md §8 for why
// (protects the ADC from whatever bleeds into RX during TX) and the
// exact ordering this depends on.
void sound_set_rx_capture(int enable) {
  sound_mixer("hw:0", "Capture", enable ? RX_CAPTURE_GAIN_PERCENT : 0);
}

/* ------------------------------------------------------------------ */
/*  ALSA PCM helpers                                                  */
/* ------------------------------------------------------------------ */
static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir) {
  snd_pcm_t *pcm = NULL;
  int err;

  if ((err = snd_pcm_open(&pcm, dev, dir, 0)) < 0) {
    fprintf(stderr, "sound: cannot open %s (%s): %s\n", dev,
            dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", snd_strerror(err));
    return NULL;
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm, hw);

  snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
  snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);

  unsigned int rate = SAMPLE_RATE;
  snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);

  snd_pcm_uframes_t period = PERIOD_FRAMES;
  snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);

  /* 4 periods ~ 85 ms buffer - enough headroom for a Pi */
  snd_pcm_uframes_t buffer = period * 4;
  snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

  if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
    fprintf(stderr, "sound: hw_params failed (%s): %s\n",
            dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", snd_strerror(err));
    snd_pcm_close(pcm);
    return NULL;
  }

  printf("sound: opened %s %s @ %u Hz, period %lu, buffer %lu\n", dev,
         dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", rate, (unsigned long)period,
         (unsigned long)buffer);

  return pcm;
}

/* Recover from an ALSA xrun/suspend. Returns 0 on success. Both call
 * sites must check this return value and stop retrying if it's still
 * negative - see docs/08_troubleshooting_and_bringup.md for the retry-
 * storm bug that taught us that, and why a plain prepare() isn't
 * always enough (the drop()+prepare() fallback below is for that). */
static int xrun_recover(snd_pcm_t *pcm, int err) {
  if (err == -EPIPE) { /* underrun / overrun */
    err = snd_pcm_prepare(pcm);
    if (err < 0) {
      snd_pcm_drop(pcm);
      err = snd_pcm_prepare(pcm);
    }
  } else if (err == -ESTRPIPE) { /* suspended */
    while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
      usleep(10000);
    if (err < 0)
      err = snd_pcm_prepare(pcm);
  }
  return err;
}

/* ------------------------------------------------------------------ */
/*  xrun flood tracking                                                */
/* ------------------------------------------------------------------ */
//
// Rate-limits "xrun, recovering" logging and adds a one-time hint plus a
// short breather once a flood is detected - a flood here means the
// device itself isn't stuck (recovery keeps "succeeding"), just that an
// ordinary-priority audio thread can't keep up with real time. See
// docs/08_troubleshooting_and_bringup.md "Audio thread xruns" for the
// full failure mode and the SCHED_FIFO fix.
#define XRUN_FLOOD_WINDOW_NS 1000000000L /* 1 second */
#define XRUN_FLOOD_THRESHOLD 10          /* xruns within the window = "flooding" */

struct xrun_tracker {
  int count;
  struct timespec window_start;
  int hint_shown;
};

static void xrun_note(struct xrun_tracker *t, const char *label) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  long elapsed_ns = (now.tv_sec - t->window_start.tv_sec) * 1000000000L +
                    (now.tv_nsec - t->window_start.tv_nsec);
  if (t->count == 0 || elapsed_ns > XRUN_FLOOD_WINDOW_NS || elapsed_ns < 0) {
    t->window_start = now;
    t->count = 0;
  }
  t->count++;

  if (t->count <= XRUN_FLOOD_THRESHOLD) {
    fprintf(stderr, "sound: xrun, recovering (%s)\n", label);
    return;
  }

  if (!t->hint_shown) {
    fprintf(stderr,
            "sound: xrun flood on %s (>%d/sec) - each individual recovery "
            "is succeeding, so the device isn't stuck; the audio thread "
            "simply isn't keeping up with real time. If the startup log "
            "showed \"failed to set audio thread to SCHED_FIFO\", that is "
            "almost certainly why - grant real-time scheduling, e.g. "
            "'sudo setcap cap_sys_nice+ep ./minibitx', run as root, or "
            "raise the rtprio limit for this user via "
            "/etc/security/limits.d. Backing off and continuing to retry "
            "rather than spinning at full rate.\n",
            label, XRUN_FLOOD_THRESHOLD);
    t->hint_shown = 1;
  }
  usleep(20000); /* breather so this loop isn't itself pegging a core */
}

/* ------------------------------------------------------------------ */
/*  IQ mixing                                                         */
/* ------------------------------------------------------------------ */
// Permanent, always-compiled clip guard on the raw ADC sample ("rf"
// below, the one signal that reflects RX_CAPTURE_GAIN_PERCENT directly)
// - successor to a temporary bench diagnostic, see
// docs/dsp_design_notes/rx_gain_and_level_calibration.md §6-7 for that
// history and why this checks only for the rising edge of a clip
// (nothing periodic). One fabs() and one compare per sample - negligible
// next to the mixing/FIR-filter arithmetic already done below.
static int rx_clipping = 0;

static void rx_clip_check(double rf) {
  int clipped_now = fabs(rf) >= 0.999;
  if (clipped_now && !rx_clipping) {
    fprintf(stderr,
            "sound: *** CLIPPING *** freq=%d capture=%d%% - RF front "
            "end is overdriving the ADC, consider lowering "
            "RX_CAPTURE_GAIN_PERCENT\n",
            freq_hdr, RX_CAPTURE_GAIN_PERCENT);
  }
  rx_clipping = clipped_now;
}

static void sound_process(int32_t *input_rx, int32_t *input_mic, int32_t *output_speaker,
                          int32_t *output_tx, int n_samples) {
  static double i_samples[4096];
  static double q_samples[4096];
  static int vfo_ready = 0;
  // filter I and Q with independent history per rail, same coefficients (antialias.c) -
  // zero-initialized once, persists across calls (each call is one
  // ~10.7ms block, not a fresh signal).
  static struct antialias_state aa_i;
  static struct antialias_state aa_q;
  // 96kHz->48kHz decimation for usb_gadget.c's UAC2 gadget only (see
  // docs/dsp_design_notes/usb_uac_decimation_design.md) - independent
  // history/phase per rail, same as aa_i/aa_q above. hpsdr_p1.c keeps
  // getting native 96kHz IQ unchanged; only the USB path is decimated.
  static struct decim48k_state dec_i;
  static struct decim48k_state dec_q;

  (void)input_mic;
  if (n_samples > 4096)
    n_samples = 4096;

  if (!vfo_ready) {
    vfo_init_phase_table();
    // this init only matters if sound_process() is ever called before
    // main()'s own startup vfo_start()/radio_tune_to()
    vfo_start(&lo, RX_IF_FREQ_HZ, 0);
    vfo_ready = 1;
  }

  for (int n = 0; n < n_samples; n++) {
    int32_t s = input_rx[n];
    int lo_i, lo_q;
    vfo_read_iq(&lo, &lo_i, &lo_q);

    double rf = (double)s / 2147483648.0;
    rx_clip_check(rf);

    // mix to IQ
    i_samples[n] = rf * ((double)lo_i / 1073741824.0);
    q_samples[n] = rf * ((double)lo_q / 1073741824.0);

    // Anti-alias lowpass, applied to I and Q right after mixing (see
    // docs/dsp_design_notes/antialias_filter_design.md). Helps clean up
    // the self-image near the +-48kHz Nyquist edges, without touching the
    // real signal content well within the crystal filter's passband.
    i_samples[n] = antialias_apply(&aa_i, i_samples[n]);
    q_samples[n] = antialias_apply(&aa_q, q_samples[n]);
  }

  // hand the block's IQ to each consumer as its own copy - hpsdr_p1.c
  // (network) and usb_gadget.c (USB Audio Class gadget) don't know about
  // each other, and either can be active without the other
  hpsdr_send_iq(i_samples, q_samples, n_samples);

  // usb_gadget.c's UAC2 gadget is fixed at 48kHz (matches real UAC2
  // hosts like the QMX/Tab5 panadapter this was built to interoperate
  // with - see docs/dsp_design_notes/usb_uac_decimation_design.md),
  // but this block's i_samples[]/q_samples[] are still native 96kHz -
  // decim48k_apply() only emits a kept sample on every other call, so
  // uac_push_iq() is only called when both rails have one ready
  // (they always agree, since both are fed in lockstep every n here).
  for (int n = 0; n < n_samples; n++) {
    double out_i, out_q;
    int have_i = decim48k_apply(&dec_i, i_samples[n], &out_i);
    int have_q = decim48k_apply(&dec_q, q_samples[n], &out_q);
    if (have_i && have_q)
      uac_push_iq(out_i, out_q);
  }

  // keep local outputs silent
  memset(output_speaker, 0, n_samples * sizeof(int32_t));
  memset(output_tx, 0, n_samples * sizeof(int32_t));
}

/* ------------------------------------------------------------------ */
/*  Audio thread - capture -> sound_process() -> playback             */
/* ------------------------------------------------------------------ */
static void *audio_loop(void *arg) {
  (void)arg;

  int32_t cap_buf[MAX_FRAMES * CHANNELS];
  int32_t rx_buf[MAX_FRAMES];
  int32_t mic_buf[MAX_FRAMES];
  int32_t spk_buf[MAX_FRAMES];
  int32_t tx_buf[MAX_FRAMES];
  int32_t play_buf[MAX_FRAMES * CHANNELS]; // CW sidetone -> WM8731 DAC

  static struct xrun_tracker capture_xrun = {0};
  static struct xrun_tracker playback_xrun = {0};

  while (g_running) {
    snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, cap_buf, PERIOD_FRAMES);
    if (frames < 0) {
      xrun_note(&capture_xrun, "capture");
      if (xrun_recover(pcm_capture, (int)frames) < 0) {
        fprintf(stderr, "sound: capture recovery failed\n");
        break;
      }
      continue;
    }

    int n = (int)frames;
    if (n > MAX_FRAMES)
      n = MAX_FRAMES;

    for (int i = 0; i < n; i++) {
      rx_buf[i] = cap_buf[i * 2];
      mic_buf[i] = cap_buf[i * 2 + 1];
    }

    sound_process(rx_buf, mic_buf, spk_buf, tx_buf, n);

    // Once per audio block - checks the key, manages the CW keying
    // burst's hang timer, and asserts/releases PTT via radio_set_tx()
    // (see cw.c). Runs every iteration, TX or not, since this is what
    // actually notices the key going down in the first place.
    cw_poll_key();

    // Feed pcm_playback every block, TX or not, not just during a CW
    // burst - see docs/08_troubleshooting_and_bringup.md for why
    // (ALSA underrun detection is tied to the hardware clock, not to
    // whether writei() is called).
    if (pcm_playback) {
      if (cw_tx_active()) {
        // Per-band calibrated scale (see the TX_SAMPLE_HEADROOM
        // comment above) - looked up once per block, not per
        // sample, since freq_hdr doesn't change mid-block.
        double band_scale = hw_settings_tx_scale(freq_hdr);
        double amp = TX_SAMPLE_HEADROOM * TX_DRIVE * band_scale * TX_GAIN_CORRECTION;

        for (int i = 0; i < n; i++) {
          // cw_get_sample() owns the envelope advance for this
          // sample - must be called first. cw_get_tx_sample()
          // reads the same envelope position but at the
          // IF-shifted carrier that lands inside the crystal
          // filter's passband instead of producing two RF tones
          // (see cw.c's TX_IF_OFFSET_HZ comment).
          double sidetone = cw_get_sample();
          double tx_wave = cw_get_tx_sample();

          // R = the WM8731's PA-feeding channel - the IF-shifted
          // TX waveform, at the full wattmeter-calibrated amplitude.
          double raw_tx = tx_wave * amp;
          if (raw_tx > TX_SAMPLE_CLAMP)
            raw_tx = TX_SAMPLE_CLAMP;
          if (raw_tx < -TX_SAMPLE_CLAMP)
            raw_tx = -TX_SAMPLE_CLAMP;

          // L = local sidetone monitor only (on-board speaker),
          // at the sidetone pitch, at a fixed comfort level - see
          // SIDETONE_PEAK_AMPLITUDE above. Never reaches the PA,
          // and no longer moves when TX_GAIN_CORRECTION does.
          double raw_side = sidetone * SIDETONE_PEAK_AMPLITUDE;
          if (raw_side > TX_SAMPLE_CLAMP)
            raw_side = TX_SAMPLE_CLAMP;
          if (raw_side < -TX_SAMPLE_CLAMP)
            raw_side = -TX_SAMPLE_CLAMP;

          play_buf[i * 2] = (int32_t)raw_side;
          play_buf[i * 2 + 1] = (int32_t)raw_tx;
        }
      } else {
        memset(play_buf, 0, (size_t)n * 2 * sizeof(int32_t));
      }
      snd_pcm_sframes_t wframes = snd_pcm_writei(pcm_playback, play_buf, n);
      if (wframes < 0) {
        // Must check xrun_recover()'s return value here (unlike an
        // earlier version that didn't - see
        // docs/08_troubleshooting_and_bringup.md for the retry-
        // storm bug that caused) and disable playback gracefully
        // on real failure, rather than killing RX along with it.
        xrun_note(&playback_xrun, "playback");
        if (xrun_recover(pcm_playback, (int)wframes) < 0) {
          fprintf(stderr, "sound: playback recovery failed - disabling CW "
                          "sidetone/TX audio output (restart minibitx to "
                          "retry)\n");
          snd_pcm_close(pcm_playback);
          pcm_playback = NULL;
        }
      }
    }
  }

  return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */
int sound_thread_start(const char *device_name) {
  const char *dev = device_name ? device_name : "hw:0,0";

  pcm_capture = open_pcm(dev, SND_PCM_STREAM_CAPTURE);
  if (!pcm_capture)
    return -1;

  // Playback: CW sidetone output only (see cw.c) - RX IQ still goes
  // out over the network/UAC2, not through here. Not a hard failure
  // if it doesn't open; audio_loop() checks pcm_playback before
  // writing to it, so minibitx still runs (just without CW TX audio).
  pcm_playback = open_pcm(dev, SND_PCM_STREAM_PLAYBACK);
  if (!pcm_playback) {
    printf("sound: playback unavailable, CW sidetone output disabled\n");
  }

  g_running = 1;

  // Real-time priority, requested up front via pthread_attr_t so
  // pthread_create() fails fast if unavailable rather than silently
  // falling back later - see docs/08_troubleshooting_and_bringup.md
  // for why the audio thread needs SCHED_FIFO. Not fatal if it fails;
  // warns once and retries with default scheduling.
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  struct sched_param sch = {.sched_priority = sched_get_priority_max(SCHED_FIFO)};
  pthread_attr_setschedparam(&attr, &sch);

  int rc = pthread_create(&audio_thread, &attr, audio_loop, NULL);
  if (rc != 0) {
    fprintf(stderr,
            "sound: WARNING - failed to set audio thread to SCHED_FIFO (%s). "
            "Falling back to normal scheduling.\n",
            strerror(rc));
    rc = pthread_create(&audio_thread, NULL, audio_loop, NULL);
  }
  pthread_attr_destroy(&attr);

  if (rc != 0) {
    fprintf(stderr, "sound: pthread_create failed\n");
    snd_pcm_close(pcm_capture);
    pcm_capture = NULL;
    g_running = 0;
    return -1;
  }

  printf("sound: running (%s)\n", dev);
  return 0;
}

void sound_thread_stop(void) {
  if (!g_running)
    return;

  g_running = 0;
  pthread_join(audio_thread, NULL);

  if (pcm_capture) {
    snd_pcm_drop(pcm_capture);
    snd_pcm_close(pcm_capture);
  }
  if (pcm_playback) {
    snd_pcm_drop(pcm_playback);
    snd_pcm_close(pcm_playback);
  }
  pcm_capture = pcm_playback = NULL;

  printf("sound: stopped\n");
}
