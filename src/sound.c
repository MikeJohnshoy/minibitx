// sound.c
// Minimal ALSA full-duplex driver for minibitx.

#include "sound.h"
#include "radio.h"
#include "hpsdr_p1.h"
#include "usb_gadget.h"
#include "antialias.h"
#include "decim48k.h"
#include "cw.h"
#include "hw_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <alsa/asoundlib.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE      96000
#define CHANNELS         2        /* stereo: L = RX / R = Mic (capture) */
#define PERIOD_FRAMES    1024     /* frames per period (matches old cfg) */
#define MAX_FRAMES       4096

// WM8731 "Line" input level (ALSA percent, 0-100) - the one analog gain
// stage ahead of the ADC in the whole RX chain (no RF preamp on this
// board). Not bench-verified against real signal levels yet - see
// docs/dsp_design_notes/rx_gain_and_level_calibration.md. Pulled out to
// its own named constant (rather than a bare literal in
// setup_audio_codec() below) specifically so a bench sweep - try 80,
// rebuild, test; try a different value, rebuild, test - only ever
// touches this one line.
#define RX_LINE_GAIN_PERCENT 80

/* ------------------------------------------------------------------ */
/*  TX sample scaling - see hw_settings.h for the per-band 'scale'      */
/*  calibration table this is anchored against                         */
/* ------------------------------------------------------------------ */
//
// cw_get_sample() returns a value roughly in [-1, 1] (tone * envelope).
// Converting that to an int32 PCM sample used to be one flat, guessed
// constant (1e9) for every band - this replaced it with hw_settings.ini's
// real, bench-calibrated per-band 'scale' table (see hw_settings.h),
// so bands that need more drive (per real sbitx's own measurements of
// this exact board's PA gain rolling off toward 10m) actually get more,
// instead of every band getting the same guess.
//
// TX_DRIVE mirrors real sbitx's "drive" setting (0-100) - minibitx has
// no live UI/command to adjust it yet, so it's fixed at the same value
// (50) the hw_settings.ini scale table was itself calibrated against
// (bench-confirmed 5W on 40m at drive=50), so the table's numbers mean
// what they were measured to mean.
//
// TX_SAMPLE_HEADROOM is anchored so that the reference band (40m,
// HW_DEFAULT_TX_SCALE = 0.00115) reproduces the exact same PCM amplitude
// as the old flat 1e9 constant - i.e. today's already-tested 40m output
// level doesn't change. Other bands scale up or down from there
// following the real per-band ratios in the table. IMPORTANT: those
// ratios span roughly 14x across the table (0.00075 on 80m to 0.0107 on
// 10m), so on the high end this can legitimately call for several times
// more amplitude than 40m - TX_SAMPLE_CLAMP exists specifically to stop
// that from wrapping around int32 range rather than just clipping.
// Real sbitx's own numbers were bench-verified against a wattmeter; this
// mapping onto minibitx's completely different (non-FFT) sample pipeline
// has not been - treat the *shape* across bands as trustworthy (it's
// real measured data) and the *absolute* level as still needing a bench
// or on-air check, the same as the constant it replaces.
#define TX_DRIVE           50
#define TX_SAMPLE_HEADROOM (1000000000.0 / (TX_DRIVE * HW_DEFAULT_TX_SCALE))
#define TX_SAMPLE_CLAMP    2000000000.0   // stay well inside int32 range
#define TX_GAIN_CORRECTION 0.045
// Fixed PCM peak amplitude for the local sidetone monitor (left channel),
// independent of amp/TX_GAIN_CORRECTION - it used to be amp*SIDETONE_SCALE,
// which meant every time TX_GAIN_CORRECTION got re-bench-calibrated for RF
// power reasons (as it just was, 4.0 -> 0.045, moving the CW tone off the
// crystal filter's skirt and onto its passband - see cw.c's
// TX_IF_OFFSET_HZ), the sidetone volume silently rode along with it and
// needed re-tuning too. This is now a comfort setting only: 1e7 matches
// what SIDETONE_SCALE=0.005 sounded like against the old amp~=2e9
// (clipped) ceiling (2e9 * 0.005 = 1e7) - the level already confirmed
// comfortable on the bench - just no longer coupled to whatever TX_DRIVE
// happens to be calibrated to.
#define SIDETONE_PEAK_AMPLITUDE 10000000.0

/* ------------------------------------------------------------------ */
/*  Module state                                                      */
/* ------------------------------------------------------------------ */
static snd_pcm_t *pcm_capture  = NULL;
static snd_pcm_t *pcm_playback = NULL;
static pthread_t  audio_thread;
static volatile int g_running = 0;

/* ------------------------------------------------------------------ */
/*  ALSA mixer helper                                                 */
/* ------------------------------------------------------------------ */
void sound_mixer(char *card_name, char *element, int make_on)
{
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
    }
    else if (snd_mixer_selem_has_capture_volume(elem)) {
        snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
        snd_mixer_selem_set_capture_volume_all(elem, make_on * max / 100);
    }
    else if (snd_mixer_selem_is_enumerated(elem))
        snd_mixer_selem_set_enum_item(elem, 0, make_on);

    snd_mixer_close(handle);
}

/* ------------------------------------------------------------------ */
/*  Codec hardware setup - barebones WM8731 init                      */
/* ------------------------------------------------------------------ */
void setup_audio_codec(void) {
  sound_mixer("hw:0", "Input Mux", 0);
  sound_mixer("hw:0", "Line", RX_LINE_GAIN_PERCENT);
  sound_mixer("hw:0", "Mic", 0);
  sound_mixer("hw:0", "Master", 0); // Mute local speaker
  sound_mixer("hw:0", "Output Mixer HiFi", 1);
  sound_mixer("hw:0", "Output Mixer Line Bypass", 0);
  sound_mixer("hw:0", "Output Mixer Mic Sidetone", 0);
}

/* ------------------------------------------------------------------ */
/*  ALSA PCM helpers                                                  */
/* ------------------------------------------------------------------ */
static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir)
{
    snd_pcm_t *pcm = NULL;
    int err;

    if ((err = snd_pcm_open(&pcm, dev, dir, 0)) < 0) {
        fprintf(stderr, "sound: cannot open %s (%s): %s\n",
                dev, dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                snd_strerror(err));
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
                dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    printf("sound: opened %s %s @ %u Hz, period %lu, buffer %lu\n",
           dev, dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
           rate, (unsigned long)period, (unsigned long)buffer);

    return pcm;
}

/* Recover from an ALSA xrun / suspend. Returns 0 on success.
 *
 * A bare snd_pcm_prepare() is the standard one-shot fix for a fresh
 * -EPIPE and is usually enough - but on a device that's wedged for a
 * more persistent reason, prepare() can itself fail (return < 0), and
 * a caller that doesn't check that (see the playback fix note in
 * audio_loop() below) ends up calling snd_pcm_writei()/readi() again
 * immediately, hitting -EPIPE again immediately, calling this again -
 * an unbounded retry loop that prints "sound: xrun, recovering"
 * forever at full loop rate and never actually recovers, needing a
 * manual restart to clear (reported behavior this addresses). Try one
 * heavier fallback here - snd_pcm_drop() (discard whatever's left in
 * the ring buffer instead of assuming prepare() already put the device
 * in a clean state) then prepare() again - before giving up; some ALSA
 * drivers need that stronger reset to actually clear a stuck xrun.
 * Both call sites still need to check this function's return value and
 * stop retrying if it's still negative - fixing this function alone
 * isn't sufficient if a caller ignores a real failure.
 */
static int xrun_recover(snd_pcm_t *pcm, int err)
{
    if (err == -EPIPE) {                     /* underrun / overrun */
        err = snd_pcm_prepare(pcm);
        if (err < 0) {
            snd_pcm_drop(pcm);
            err = snd_pcm_prepare(pcm);
        }
    } else if (err == -ESTRPIPE) {           /* suspended */
        while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
            usleep(10000);
        if (err < 0) err = snd_pcm_prepare(pcm);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/*  xrun flood tracking                                                */
/* ------------------------------------------------------------------ */
//
// Reported: "sometimes I get the xrun flood immediately on startup,
// with no HPSDR consumer connected" - alongside a startup log showing
// "failed to set audio thread to SCHED_FIFO... falling back to normal
// scheduling". That's the real cause here, and it's a *different*
// failure mode than the one xrun_recover()'s drop+prepare fallback and
// the two call sites' return-value checks were built for. Those guard
// against the *device* getting stuck (prepare() itself failing) - but
// on an ordinary SCHED_OTHER thread, snd_pcm_prepare() succeeds fine
// every single time; the thread just isn't being scheduled promptly
// enough to feed the next period before it underruns again, so a
// "successful" recovery is immediately followed by another xrun,
// forever. Nothing in the existing checks ever fires (recovery keeps
// "succeeding"), so it prints and spins at the full ~93Hz loop rate
// indefinitely - which is exactly the flood reported, and a tight loop
// like that can itself worsen the CPU contention causing it. Rate-limit
// the logging and add a short breather once a flood is detected, plus
// a one-time hint pointing at the actual root cause (missing real-time
// scheduling privilege) instead of scrolling it off screen.
#define XRUN_FLOOD_WINDOW_NS   1000000000L   /* 1 second */
#define XRUN_FLOOD_THRESHOLD   10            /* xruns within the window = "flooding" */

struct xrun_tracker {
    int count;
    struct timespec window_start;
    int hint_shown;
};

static void xrun_note(struct xrun_tracker *t, const char *label)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_ns = (now.tv_sec - t->window_start.tv_sec) * 1000000000L
                     + (now.tv_nsec - t->window_start.tv_nsec);
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
            "rather than spinning at full rate.\n", label, XRUN_FLOOD_THRESHOLD);
        t->hint_shown = 1;
    }
    usleep(20000);   /* breather so this loop isn't itself pegging a core */
}

/* ------------------------------------------------------------------ */
/*  IQ mixing                                                         */
/* ------------------------------------------------------------------ */
#ifdef RX_GAIN_DIAG
// Temporary bench diagnostic for
// docs/dsp_design_notes/rx_gain_and_level_calibration.md - not compiled
// into normal builds (build with `make CFLAGS+=-DRX_GAIN_DIAG` to
// enable it for a bench session, plain `make` otherwise). Tracks the
// raw ADC sample - "rf" below, before any digital mixing or filtering -
// against full scale, since that's the one signal in the whole chain
// that reflects the WM8731 "Line" gain (RX_LINE_GAIN_PERCENT) directly,
// independent of anything downstream (IQ mixing, the anti-alias
// filter, decimation, or either output consumer's own scaling) - see
// that doc's Caveat 2. Reports peak and RMS in dBFS once per ~1 second
// of audio (96000 samples at this file's fixed 96kHz capture rate), so
// a bench session produces one line per second, tagged with the
// currently tuned frequency and Line setting so a captured log is
// self-describing without needing separate notes.
#define RXDIAG_WINDOW_SAMPLES 96000
static double rxdiag_peak = 0.0;
static double rxdiag_sumsq = 0.0;
static long rxdiag_count = 0;

static void rxdiag_sample(double rf) {
    double a = fabs(rf);
    if (a > rxdiag_peak) rxdiag_peak = a;
    rxdiag_sumsq += rf * rf;
    rxdiag_count++;
    if (rxdiag_count >= RXDIAG_WINDOW_SAMPLES) {
        double rms = sqrt(rxdiag_sumsq / (double)rxdiag_count);
        // -240dBFS floor instead of -inf for a silent/all-zero window
        // (e.g. before the antenna/dummy load is even connected) - keeps
        // the log numeric and greppable rather than printing "-inf".
        double peak_dbfs = 20.0 * log10(rxdiag_peak > 1e-12 ? rxdiag_peak : 1e-12);
        double rms_dbfs  = 20.0 * log10(rms > 1e-12 ? rms : 1e-12);
        printf("rxgain: freq=%d line=%d%% peak=%.1fdBFS rms=%.1fdBFS%s\n",
               freq_hdr, RX_LINE_GAIN_PERCENT, peak_dbfs, rms_dbfs,
               rxdiag_peak >= 0.999 ? "  *** CLIPPING ***" : "");
        rxdiag_peak = 0.0;
        rxdiag_sumsq = 0.0;
        rxdiag_count = 0;
    }
}
#endif

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
    if (n_samples > 4096) n_samples = 4096;

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
#ifdef RX_GAIN_DIAG
        rxdiag_sample(rf);
#endif

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
static void *audio_loop(void *arg)
{
    (void)arg;

    int32_t cap_buf[MAX_FRAMES * CHANNELS];
    int32_t rx_buf[MAX_FRAMES];
    int32_t mic_buf[MAX_FRAMES];
    int32_t spk_buf[MAX_FRAMES];
    int32_t tx_buf[MAX_FRAMES];
    int32_t play_buf[MAX_FRAMES * CHANNELS];   // CW sidetone -> WM8731 DAC

    static struct xrun_tracker capture_xrun  = {0};
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
        if (n > MAX_FRAMES) n = MAX_FRAMES;

        for (int i = 0; i < n; i++) {
            rx_buf[i]  = cap_buf[i * 2];
            mic_buf[i] = cap_buf[i * 2 + 1];
        }

        sound_process(rx_buf, mic_buf, spk_buf, tx_buf, n);

        // Once per audio block - checks the key, manages the CW keying
        // burst's hang timer, and asserts/releases PTT via radio_set_tx()
        // (see cw.c). Runs every iteration, TX or not, since this is what
        // actually notices the key going down in the first place.
        cw_poll_key();

        // Feed pcm_playback every block, TX or not - not just while a CW
        // burst is active. ALSA's underrun detection isn't tied to whether
        // writei() gets called; it's the hardware clock draining the ring
        // buffer against the software pointer. Only writing during a burst
        // meant the device sat with nothing arriving for however long the
        // key was up (seconds, easily), so its ~43ms buffer drained and it
        // underran on its own between every single burst - then the first
        // write of the next burst hit that stale underrun and had to
        // recover, which is exactly the "xrun, recovering" storm logged on
        // every key-down. Writing silence the rest of the time keeps the
        // device continuously running so it never gets the chance to idle
        // out - the same continuously-fed design real sbitx's own full
        // duplex audio path uses.
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
                    double tx_wave  = cw_get_tx_sample();

                    // R = the WM8731's PA-feeding channel - the IF-shifted
                    // TX waveform, at the full wattmeter-calibrated amplitude.
                    double raw_tx = tx_wave * amp;
                    if (raw_tx > TX_SAMPLE_CLAMP) raw_tx = TX_SAMPLE_CLAMP;
                    if (raw_tx < -TX_SAMPLE_CLAMP) raw_tx = -TX_SAMPLE_CLAMP;

                    // L = local sidetone monitor only (on-board speaker),
                    // at the sidetone pitch, at a fixed comfort level - see
                    // SIDETONE_PEAK_AMPLITUDE above. Never reaches the PA,
                    // and no longer moves when TX_GAIN_CORRECTION does.
                    double raw_side = sidetone * SIDETONE_PEAK_AMPLITUDE;
                    if (raw_side > TX_SAMPLE_CLAMP) raw_side = TX_SAMPLE_CLAMP;
                    if (raw_side < -TX_SAMPLE_CLAMP) raw_side = -TX_SAMPLE_CLAMP;

                    play_buf[i * 2]     = (int32_t)raw_side;
                    play_buf[i * 2 + 1] = (int32_t)raw_tx;
                }
            } else {
                memset(play_buf, 0, (size_t)n * 2 * sizeof(int32_t));
            }
            snd_pcm_sframes_t wframes = snd_pcm_writei(pcm_playback, play_buf, n);
            if (wframes < 0) {
                // BUG FIX (reported: "xrun flood, never recovers, have to
                // restart minibitx"): this used to call xrun_recover() and
                // throw away its return value, unlike the capture path
                // just above. If recovery ever genuinely failed here (not
                // just the ordinary one-shot xrun prepare() usually
                // clears), nothing noticed - the loop just came back
                // around, wrote to the still-broken device, got -EPIPE
                // again, called xrun_recover() again, forever, printing
                // "sound: xrun, recovering" at the full ~93Hz loop rate
                // with no backoff and no way out short of a restart.
                // Capture already breaks the whole thread on a real
                // failure (see above); do the equivalent here without
                // killing RX along with it - just stop touching the
                // playback device (no more sidetone/TX drive) and say so
                // once, clearly, instead of spinning silently.
                xrun_note(&playback_xrun, "playback");
                if (xrun_recover(pcm_playback, (int)wframes) < 0) {
                    fprintf(stderr,
                        "sound: playback recovery failed - disabling CW "
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
int sound_thread_start(const char *device_name)
{
    const char *dev = device_name ? device_name : "hw:0,0";

    pcm_capture = open_pcm(dev, SND_PCM_STREAM_CAPTURE);
    if (!pcm_capture) return -1;

    // Playback: CW sidetone output only (see cw.c) - RX IQ still goes
    // out over the network/UAC2, not through here. Not a hard failure
    // if it doesn't open; audio_loop() checks pcm_playback before
    // writing to it, so minibitx still runs (just without CW TX audio).
    pcm_playback = open_pcm(dev, SND_PCM_STREAM_PLAYBACK);
    if (!pcm_playback) {
        printf("sound: playback unavailable, CW sidetone output disabled\n");
    }

    g_running = 1;
    
    // Real-time priority: as an ordinary SCHED_OTHER thread the audio
    // thread competes with everything else on the system and can be
    // preempted long enough to miss an ALSA period. zbitx's sbitx_sound.c
    // hit underruns from the same cause and fixed it by bumping the audio
    // thread to SCHED_FIFO - do the same here, requested up front via
    // pthread_attr_t so pthread_create() itself fails fast (typically
    // EPERM) if we don't have the privilege, rather than the thread
    // silently falling back to normal scheduling well after this
    // function - and main()'s "ready to serve!" line - have already
    // returned. Not fatal if it fails (no root / no CAP_SYS_NICE / no
    // rtprio limit) - just warn here, synchronously, and retry with
    // default (SCHED_OTHER) attributes.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    struct sched_param sch = { .sched_priority = sched_get_priority_max(SCHED_FIFO) };
    pthread_attr_setschedparam(&attr, &sch);

    int rc = pthread_create(&audio_thread, &attr, audio_loop, NULL);
    if (rc != 0) {
        fprintf(stderr,
                "sound: WARNING - failed to set audio thread to SCHED_FIFO (%s). "
                "Falling back to normal scheduling.\n", strerror(rc));
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

void sound_thread_stop(void)
{
    if (!g_running) return;

    g_running = 0;
    pthread_join(audio_thread, NULL);

    if (pcm_capture)  { snd_pcm_drop(pcm_capture);  snd_pcm_close(pcm_capture);  }
    if (pcm_playback) { snd_pcm_drop(pcm_playback); snd_pcm_close(pcm_playback); }
    pcm_capture = pcm_playback = NULL;

    printf("sound: stopped\n");
}
