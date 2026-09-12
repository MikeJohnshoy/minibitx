#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* Start the full-duplex ALSA audio thread (capture + playback).
   device_name is the ALSA PCM name, e.g. "hw:0,0".
   Returns 0 on success, -1 on failure. */
int  sound_thread_start(const char *device_name);

/* Stop the audio thread and release ALSA devices. */
void sound_thread_stop(void);

/* ALSA mixer control for WM8731 codec hardware setup.
   Handles switches, volumes, and enumerated controls. */
void sound_mixer(char *card_name, char *element, int make_on);

/* Barebones WM8731 codec setup (input mux, levels, mute local speaker).
   Call once, after the ALSA devices are otherwise ready. */
void setup_audio_codec(void);

/* Mute (enable=0) or restore (enable=1, back to RX_CAPTURE_GAIN_PERCENT)
   the WM8731 'Capture' gain around a TX burst - see radio.c's
   radio_tx_apply(), which calls this on every TX/RX transition, and
   docs/dsp_design_notes/rx_gain_and_level_calibration.md for why this
   protects the ADC/DSP chain from TX energy. */
void sound_set_rx_capture(int enable);

#endif /* SOUND_H */
