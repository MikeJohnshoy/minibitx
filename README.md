# minibitx    -- an experimental test bed
STATUS  Compiles, establishes connection with SDRConsole, but no IQ data being passed yet ...

Minimal set of code to initialize sbitx hardware and connect to external SDR software, like quisk or SDRConsole.

•	main() initializes the hardware and spawns the network thread (hpsdr_poll) and audio thread (sound_thread_start).

•	The external SDR app discovers the radio via the UDP thread and sends a start stream command.

•	The audio thread reads IF data from the audio chip at 48k samples per second, does a 24 kHz anti-aliasing filter and sends 48k samples per second to sound_process().

•	sound_process() performs complex mixing to baseband, passes the arrays of I and Q data to hpsdr_send_iq() for hpsdr Protocol 1 over the network.

•	The external SDR app handles all FFT processing, demodulation of various signal types, and audio routing
