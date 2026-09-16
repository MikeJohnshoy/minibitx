// hamlib.h - minimal rigctld-compatible TCP control server.
//
// Implements a tiny subset of Hamlib's plain-text rigctld wire protocol 

// Implements:
//    f/F (get/set frequency)
//    t/T (get/set PTT)
//    m/M (get/set mode (cosmetic only, minibitx has no onboard demod)
//    l/L AF (get/set audio volume, 0.0-1.0 - the one real hamlib "level";
//       backed by rx_audio.c's rx_audio_get_volume()/rx_audio_set_volume().
//       Every other hamlib level (RF, SQL, preamp, ...) has no minibitx
//       equivalent and gets an error reply, same as an unknown command.)
//    dump_state
//    chk_vfo
//    q/Q to close a connection cleanly
// Unknown commands get a PRT error reply

#ifndef HAMLIB_H
#define HAMLIB_H

// Starts the rigctld-compatible TCP server on the given port (4532 is
// the standard rigctld port) in a background thread. Returns 0 on
// success, -1 on failure (bind/listen error) - non-fatal, same pattern
// as hpsdr_init()/uac_init(): minibitx keeps running either way.
int hamlib_init(int port);

// Stops the server and closes the listening socket.
void hamlib_stop(void);

#endif /* HAMLIB_H */
