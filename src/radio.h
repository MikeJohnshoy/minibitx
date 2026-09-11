// radio.h

#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>
#include "vfo.h"

extern int freq_hdr;    // current frequency, Hz
extern int in_tx;       // 0 = RX, 1 = TX
extern int xtal_filter_center; // measured true center of the crystal
                                // filter, Hz - see radio.c
extern int bfo_freq;    // real clk1/BFO frequency used only while
                         // transmitting - deliberately NOT
                         // xtal_filter_center; see radio.c
extern struct vfo lo;   // software LO for RX quadrature mixing

#define RX_IF_FREQ_HZ 24000

// Tunes to f (Hz): sets the si5351 oscillator, restarts the software RX
// VFO, and selects the matching LPF.
void radio_tune_to(uint32_t f);

// Switches between receive (tx_on = 0) and transmit (tx_on = 1): drives
// EXT_PTT and TX_LINE in the correct order with a relay-settling delay
// between them, and updates in_tx.
void radio_set_tx(int tx_on);

// Parses one command string from a control surface (currently just
// * "freq NNN" from hpsdr_p1.c) and applies it.
void remote_execute(char *command);

#endif /* RADIO_H */
