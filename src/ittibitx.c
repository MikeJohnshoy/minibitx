// ittibitx.c
// A tiny application that initializes the hardware in a sbitx,
// and allows a remote SDR application to control its operation over the network using
// a subset of openHPSDR Protocol 1.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "sdr.h"
#include "sound.h"
#include "si5351.h"
#include "hpsdr_p1.h"

// Globals required by hpsdr_p1.c and routing
int freq_hdr = 7074000;
int in_tx = 0; 
int bfo_freq = 40035000;

// Minimal Tuning Function
void radio_tune_to(u_int32_t f) {
    freq_hdr = f;
    // Set Si5351 (Oscillator 2 usually used for RX in sBitx)
    // The IF offset is subtracted here if using an IF, adjust to your hardware needs.
    si5351bx_setfreq(2, f + bfo_freq - 24000); 
    
    // Call set_lpf_40mhz(f) here if you still want automatic bandpass filter switching
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
void sound_process(int32_t *input_rx, int32_t *input_mic, 
                   int32_t *output_speaker, int32_t *output_tx, 
                   int n_samples) 
{
    static double i_samples[4096];
    static double q_samples[4096];

    // 1. Extract I and Q from the ALSA input
    // The WM8731 gives us interleaved stereo (Left=I, Right=Q)
    for(int i = 0; i < n_samples; i++) {
        // Convert to double. Scale down if your hpsdr_p1.c expects normalized floats.
        i_samples[i] = (double)input_rx[i * 2] / 200000000.0;
        q_samples[i] = (double)input_rx[i * 2 + 1] / 200000000.0;
    }

    // 2. Ship the raw IQ baseband over the network
    hpsdr_send_iq(i_samples, q_samples, n_samples);

    // 3. Keep local outputs muted (RX only)
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

    // 1. Initialize Hardware (I2C & Clock)
    i2cbb_init();
    si5351bx_init();
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
    while(1) {
        sleep(1);
    }

    return 0;
}
