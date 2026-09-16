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

// Conventional +/-9.999kHz RIT range (matches common Icom/Kenwood-style
// rigs) - a UI/protocol bound only, not a hardware limit. The caller
// (currently just hamlib.c's J command) is responsible for range-
// checking a requested offset against this before calling
// radio_set_rit() - that function does not reject out-of-range values
// itself, same convention as radio_tune_to() trusting its own caller.
#define RIT_MAX_HZ 9999

// Tunes to f (Hz): sets the si5351 oscillator, restarts the software RX
// VFO, and selects the matching LPF. Also clears any RIT offset back to
// 0 - see radio_set_rit() below for why an offset dialed in against the
// old frequency shouldn't silently carry forward onto a new one.
void radio_tune_to(uint32_t f);

// Switches between receive (tx_on = 0) and transmit (tx_on = 1): drives
// EXT_PTT and TX_LINE in the correct order with a relay-settling delay
// between them, and updates in_tx.
void radio_set_tx(int tx_on);

// Sets the receive-only tuning offset, hz, added to freq_hdr when
// computing RX's clk2 - TX's own clk2 (radio_tx_apply(), radio.c) never
// sees it, so dialing in RIT never moves your transmit frequency, only
// where you're listening. Takes effect immediately if currently
// receiving; if mid-transmission, the stored value is applied the
// moment TX drops back to RX. Cleared to 0 by every radio_tune_to()
// call - a RIT offset has no defined meaning once you've retuned to a
// different frequency or band, so a fresh VFO frequency always starts
// RIT-free rather than silently inheriting whatever was dialed in
// before. See docs/04_remote_control_and_iq_output.md for the rigctld
// j/J commands that reach this, and radio.c's radio_set_rit() for why
// this is RX-only rather than something clk1 (the physical BFO) needs
// to know about at all.
void radio_set_rit(int hz);

// Returns the current RIT offset, hz (0 if none is applied).
int radio_get_rit(void);

// Parses one command string from a control surface (currently just
// * "freq NNN" from hpsdr_p1.c) and applies it.
void remote_execute(char *command);

#endif /* RADIO_H */
