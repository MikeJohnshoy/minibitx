// ittibitx.c
// A tiny application that initializes the sbitx radio hardware,
// and allows a remote SDR application to control its operation over the network using
// a subset of openHPSDR Protocol 1.

#include "hpsdr_p1.h"
#include "sdr.h"
#include "si5351.h"
#include "sound.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Globals required by hpsdr_p1.c and routing
int freq_hdr = 7074000; // starting freq, until we get commanded from remote
int in_tx = 0;
int bfo_freq = 40035000;
// Local oscillator for RX quadrature mixing
static struct vfo lo;

// Minimal Tuning Function
void radio_tune_to(u_int32_t f) {
  freq_hdr = f;
  // Set Si5351 (Oscillator 2 usually used for RX in sBitx)
  // The IF offset is subtracted here if using an IF, adjust to your hardware needs.
  si5351bx_setfreq(2, f + bfo_freq - 24000);
  // Keep software LO in step with hardware tuning
  vfo_start(&lo, freq_hdr, lo.phase);

  // Call set_lpf_40mhz(f) here for bandpass filter switching
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

// The Audio Callback - Driven by the ALSA capture thread in sbitx_sound.c
void sound_process(int32_t *input_rx, int32_t *input_mic, int32_t *output_speaker,
                   int32_t *output_tx, int n_samples) {
  static double i_samples[4096];
  static double q_samples[4096];
  static int vfo_ready = 0;

  // Initialize LO once
  if (!vfo_ready) {
    vfo_init_phase_table();
    vfo_start(&lo, freq_hdr, 0);
    vfo_ready = 1;
  }

  /* Channel roles retained:
     input_rx  : left channel (RF baseband from codec)
     input_mic : right channel (reserved for future TX path)
     output_*  : muted here; TX/audio can be added later.
  */
  for (int n = 0; n < n_samples; n++) {
    int32_t s = input_rx[n]; // mono RX sample
    int lo_i, lo_q;
    vfo_read_iq(&lo, &lo_i, &lo_q); // fixed‑point LO

    double rf = (double)s / 2147483648.0; // normalize 32-bit
    double mix_i = rf * ((double)lo_i / 1073741824.0);
    double mix_q = rf * ((double)lo_q / 1073741824.0);

    i_samples[n] = mix_i;
    q_samples[n] = mix_q;
  }

  hpsdr_send_iq(i_samples, q_samples, n_samples);

  // Keep local outputs silent (RX-only streamer)
  memset(output_speaker, 0, n_samples * 2 * sizeof(int32_t));
  memset(output_tx, 0, n_samples * 2 * sizeof(int32_t));
}

// Barebones Setup for the WM8731 codec
void setup_audio_codec() {
  sound_mixer("hw:0", "Input Mux", 0);
  sound_mixer("hw:0", "Line", 1);
  sound_mixer("hw:0", "Mic", 0);
  sound_mixer("hw:0", "Master", 0); // Mute local speaker
}

int main(int argc, char **argv) {
  printf("Starting Minimal sBitx IQ Streamer...\n");

  // 1. Initialize Hardware (I2C & si5351 Clock), filters, and software vfo
  i2cbb_init();
  si5351bx_init();
  vfo_init_phase_table();
  vfo_start(&lo, freq_hdr, 0);
  radio_tune_to(freq_hdr);

  // 2. Initialize Networking (HPSDR Protocol 1)
  if (hpsdr_init() < 0) {
    fprintf(stderr, "Failed to bind HPSDR socket\n");
    return -1;
  }
  hpsdr_poll(); // Starts the listener thread for connection/tuning requests

  // 3. Initialize Audio
  setup_audio_codec();
  // This starts the background ALSA thread which repeatedly calls sound_process()
  sound_thread_start("hw:0,0");

  // 4. Main loop does nothing but keep the program alive
  while (1) {
    sleep(1);
  }

  return 0;
}
