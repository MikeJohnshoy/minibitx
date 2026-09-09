// vfo.c
//
// derived from Farhan's original vfo.c
// Modified and extended to enable I and Q outputs for quadrature mixing
// The vfo_read() is preserved but vfo_read_iq() is now preferred.

#include "vfo.h"
#include <math.h>

// we define one more lookup table entry than needed, so that each quadrant
// has both endpoints of a 90 degree range
static int phase_table[MAX_PHASE_COUNT];

// Must match the real ADC capture rate (sound.c's SAMPLE_RATE) - this is
// what vfo_start()'s phase-increment math uses to convert RX_IF_FREQ_HZ
// into an actual NCO rate. 
int sampling_freq = 96000;

// the only time we call this trig function is when we initialize the table
void vfo_init_phase_table() {
  for (int i = 0; i < MAX_PHASE_COUNT; i++) {
    double d = (M_PI / 2) * ((double)i) / ((double)MAX_PHASE_COUNT);
    phase_table[i] = (int)(sin(d) * 1073741824.0);
  }
}

void vfo_start(struct vfo *v, int frequency_hz, int start_phase) {
  v->phase_increment = (frequency_hz * 65536) / sampling_freq;
  v->phase = start_phase;
  v->freq_hz = frequency_hz;
}

// figure out what quadrant we are in and read the sine value
// for a given phase without advancing
// This function was pulled out of vfo_read() and
// put here to be shared by vfo_read() and vfo_read_iq()
static int vfo_lookup(int phase) {
  int i = 0;
  phase &= 0xffff;
  if (phase < 16384)
    i = phase_table[phase];
  else if (phase < 32768)
    i = phase_table[32767 - phase];
  else if (phase < 49152)
    i = -phase_table[phase - 32768];
  else
    i = -phase_table[65535 - phase];
  return i;
}

// this was the original function call for real-only mixing
// we should consider replacing calls to vfo_read() with calls to vfo_read_iq() instead
int vfo_read(struct vfo *v) {
  int i = vfo_lookup(v->phase);
  // increment the phase and modulo-65536
  v->phase += v->phase_increment;
  v->phase &= 0xffff;
  return i;
}

// provide both I (cosine) and Q (sine) outputs for quadrature mixing.
// cosine is simply the sine shifted forward by 90 degrees (16384 phase counts).
void vfo_read_iq(struct vfo *v, int *out_i, int *out_q) {
  // I channel = cos(phase) = sin(phase + 90 degrees)
  *out_i = vfo_lookup(v->phase + 16384);
  // Q channel = sin(phase)
  *out_q = vfo_lookup(v->phase);
  // increment the phase and modulo-65536
  v->phase += v->phase_increment;
  v->phase &= 0xffff;
}
