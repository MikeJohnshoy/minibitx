// sound.c
// Minimal ALSA full-duplex driver for minibitx.

#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <alsa/asoundlib.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE      48000
#define CHANNELS         2        /* stereo: L = RX / R = Mic (capture) */
#define PERIOD_FRAMES    1024     /* frames per period (matches old cfg) */
#define MAX_FRAMES       4096

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */
static snd_pcm_t *pcm_capture  = NULL;
static snd_pcm_t *pcm_playback = NULL;
static pthread_t  audio_thread;
static volatile int g_running = 0;

/* ------------------------------------------------------------------ */
/*  ALSA mixer helper — unchanged from sbitx / ma_sound.c              */
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
/*  ALSA PCM helpers                                                   */
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

    /* 4 periods ≈ 85 ms buffer — enough headroom for a Pi */
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

/* Recover from an ALSA xrun / suspend. Returns 0 on success. */
static int xrun_recover(snd_pcm_t *pcm, int err)
{
    if (err == -EPIPE) {                     /* underrun / overrun */
        fprintf(stderr, "sound: xrun, recovering\n");
        err = snd_pcm_prepare(pcm);
    } else if (err == -ESTRPIPE) {           /* suspended */
        while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
            usleep(10000);
        if (err < 0) err = snd_pcm_prepare(pcm);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/*  Audio thread — capture → sound_process() → playback               */
/* ------------------------------------------------------------------ */
static void *audio_loop(void *arg)
{
    (void)arg;

    int32_t cap_buf[MAX_FRAMES * CHANNELS];
    int32_t rx_buf[MAX_FRAMES];
    int32_t mic_buf[MAX_FRAMES];
    int32_t spk_buf[MAX_FRAMES];
    int32_t tx_buf[MAX_FRAMES];

    while (g_running) {
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, cap_buf, PERIOD_FRAMES);
        if (frames < 0) {
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
        // No playback write — output goes over HPSDR network
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
int sound_thread_start(const char *device_name)
{
    const char *dev = device_name ? device_name : "hw:0,0";

    pcm_capture = open_pcm(dev, SND_PCM_STREAM_CAPTURE);
    if (!pcm_capture) return -1;

    // No playback needed — IQ goes out over the network, not local audio
    pcm_playback = NULL;

    g_running = 1;
    if (pthread_create(&audio_thread, NULL, audio_loop, NULL) != 0) {
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
