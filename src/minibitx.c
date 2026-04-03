// minibitx.c
// A tiny application that initializes the sbitx radio hardware,
// and allows a remote SDR application to control its operation over the network using
// a subset of openHPSDR Protocol 1.

#include "hpsdr_p1.h"
#include "si5351.h"
#include "ma_sound.h"
#include "vfo.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// globals
int freq_hdr = 7074000;   // current freq we are tuned to, until we get commanded from remote
int in_tx = 0;            // 0 = RX,  1 = TX
int bfo_freq = 40035000;  // center frequency of our crystal filter (40.035 mHz)
static struct vfo lo;     // LO for RX quadrature mixing values

// tuning
void radio_tune_to(u_int32_t f) {
  freq_hdr = f;
  // set Si5351 (oscillator 2 used for RX in sBitx)
  // the IF offset is subtracted here
  si5351bx_setfreq(2, f + bfo_freq - 24000);
  // keep software LO in step with hardware tuning
  vfo_start(&lo, freq_hdr, lo.phase);
  // call set_lpf_40mhz(f) here for bandpass filter switching
  printf("Tuned to: %d Hz\n", f);
}

// hpsdr_p1.c parses EP2 host commands and calls this to change frequency
void remote_execute(char *command) {
  if (strncmp(command, "freq ", 5) == 0) {
    int f = atoi(command + 5);
    if (f > 0) {
      radio_tune_to(f);
    }
  }
}

// driven by the ALSA capture thread in sbitx_sound.c
void sound_process(int32_t *input_rx, int32_t *input_mic, int32_t *output_speaker,
                   int32_t *output_tx, int n_samples) {
  static double i_samples[4096];
  static double q_samples[4096];
  static int vfo_ready = 0;

  // TEMP TEST: bypass audio input and generate a 1kHz IQ tone
  static double ph = 0.0;
  for (int n = 0; n < n_samples; n++) {
    ph += 2.0 * 3.141592653589793 * 1000.0 / 48000.0;
    if (ph >= 2.0 * 3.141592653589793) ph -= 2.0 * 3.141592653589793;
    i_samples[n] = 0.2 * sin(ph);
    q_samples[n] = 0.2 * cos(ph);
  }
  hpsdr_send_iq(i_samples, q_samples, n_samples);

  memset(output_speaker, 0, n_samples * 2 * sizeof(int32_t));
  memset(output_tx, 0, n_samples * 2 * sizeof(int32_t));
  return;
}

// barebones Setup for the WM8731 codec
void setup_audio_codec() {
  sound_mixer("hw:0", "Input Mux", 0);
  sound_mixer("hw:0", "Line", 1);
  sound_mixer("hw:0", "Mic", 0);
  sound_mixer("hw:0", "Master", 0); // Mute local speaker
}

int main(int argc, char **argv) {
  printf("Starting miniBitx IQ Streamer...\n");
  
  // Initialize wiringPi (must be done before any pinMode/digitalRead/Write)
  if (wiringPiSetupGpio() < 0) {            // or wiringPiSetup()
    fprintf(stderr, "Failed to init wiringPi\n");
    return -1;
  }

  // Initialize Hardware (I2C & si5351 Clock), filters, and software vfo
  i2cbb_init();
  si5351bx_init();
  vfo_init_phase_table();
  vfo_start(&lo, freq_hdr, 0);
  radio_tune_to(freq_hdr);

  // Initialize Networking (HPSDR Protocol 1)
  if (hpsdr_init() < 0) {
    fprintf(stderr, "Failed to bind HPSDR socket\n");
    return -1;
  }
  hpsdr_poll(); // Starts the listener thread for connection/tuning requests

  // Initialize Audio
  setup_audio_codec();
  // this starts the background miniaudio thread which repeatedly calls sound_process()
  sound_thread_start("hw:0,0");

  // main loop does nothing but keep the program alive
  while (1) {
    sleep(1);
  }

  return 0;
}
