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

/* Diagnostic: prints an element's actual capabilities (playback/capture
   volume and switch, enumerated) and current value(s) to stderr. Not
   part of normal setup - a way to check ground truth on a given board
   when a control silently doesn't behave as expected. */
void sound_mixer_dump(char *card_name, char *element);

/* Barebones WM8731 codec setup (input mux, levels, mute local speaker).
   Call once, after the ALSA devices are otherwise ready. */
void setup_audio_codec(void);

/* Mute (enable=0) or restore (enable=1, back to RX_CAPTURE_GAIN_PERCENT)
   the WM8731 'Capture' gain around a TX burst - see radio.c's
   radio_tx_apply(), which calls this on every TX/RX transition, and
   docs/dsp_design_notes/rx_gain_and_level_calibration.md for why this
   protects the ADC/DSP chain from TX energy. */
void sound_set_rx_capture(int enable);

/* Sets the WM8731 'Master' control's LEFT channel only (0-100), the
   local speaker/headphone output - cw.c's TX sidetone and rx_audio.c's
   RX demod. 'Master' has independent L/R volume registers, and L/R go
   to two different physical destinations on this board (L: local audio
   amp, R: the mainboard's TX exciter feed - see sound_set_tx_drive()) -
   so this never needs to be muted/restored around a TX burst the way
   sound_set_rx_capture() does. setup_audio_codec() calls this once at
   startup; exposed publicly for a future real volume control. */
void sound_set_local_monitor(int percent);

/* Sets 'Master's RIGHT channel only (0-100) - the exciter feed. Called
   from radio.c's radio_tx_apply() with TX_MASTER_VOL while transmitting
   and 0 as the relay drops on every TX/RX transition; deliberately
   leaves the LEFT channel (sound_set_local_monitor(), above) untouched. */
void sound_set_tx_drive(int percent);

#endif /* SOUND_H */
