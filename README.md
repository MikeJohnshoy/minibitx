# minibitx
Minimal set of code to initialize sbitx hardware and connect to external SDR software like SDRConsole.

•	main() initializes the hardware and spawns the network thread (hpsdr_poll) and audio thread (sound_thread_start).

•	The external SDR app discovers the radio via the UDP thread and sends a start stream command.

•	The sampling thread reads IF data from the audio chip, does a 24 kHz anti-aliasing filter and sends 48k samples per second to sound_process().

•	sound_process() performs complex mixing to baseband, passes the arrays of I and Q data into hpsdr_send_iq() and sends EP6 packets (hpsdr Protocol 1) out over the network.

•	The external SDR app handles all FFT processing, demodulation, AGC, and audio routing
