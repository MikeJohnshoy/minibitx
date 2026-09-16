// iq_stream.h
//
// A minimal, protocol-free UDP telemetry stream of raw baseband I/Q
// sample pairs - built so a simple network client (tools/rigctl_panel.py's
// spectrum display) can get real I/Q without implementing openHPSDR
// Protocol 1 (hpsdr_p1.c) or UAC2 (usb_gadget.c), both of which carry a
// lot of protocol machinery (discovery, start/stop, C&C registers, MOX,
// USB gadget setup) that a client only wanting "pairs of I/Q numbers for
// a spectrum plot" has no use for. See iq_stream.c's file header for the
// wire format and docs/dsp_design_notes/iq_stream_design.md for why this
// exists as a third, independent path rather than reusing the other two.
//
// Like hpsdr_p1.c and usb_gadget.c, this is fed its own copy of the I/Q
// from sound_process() and knows nothing about either of the other two -
// it can run alongside WSJT-X/Thetis on hpsdr_p1.c or a UAC2 host without
// disturbing either, which was the whole point of not just teaching
// hpsdr_p1.c's own single-client link a second listener.

#ifndef IQ_STREAM_H
#define IQ_STREAM_H

#define IQ_STREAM_PORT 4536

// Call once at startup (after hpsdr_init(), order doesn't otherwise
// matter - the two are fully independent). Returns 0 on success, -1 if
// the UDP socket couldn't be bound (not fatal to the caller - same
// "keep running on whatever subset of control surfaces came up"
// convention every other optional surface here follows).
int iq_stream_init(void);

// Starts the listener thread (catches subscribe datagrams) and the
// pacer thread (paces outbound packets to each active subscriber).
// Mirrors hpsdr_poll()'s job for its own two threads.
void iq_stream_poll(void);

// Call once per audio-thread block from sound_process(), same samples
// handed to hpsdr_send_iq() - producer side only, appends to a lock-free
// ring buffer and returns immediately; a no-op the instant no subscriber
// is active. Must stay fast and never block - runs on the real-time
// audio thread.
void iq_stream_send(const double *i_samples, const double *q_samples, int n);

// Stops both threads and closes the socket. Call after sound_thread_stop()
// so nothing is still trying to produce samples into a torn-down stream,
// same ordering hpsdr_stop() already follows.
void iq_stream_stop(void);

#endif /* IQ_STREAM_H */
