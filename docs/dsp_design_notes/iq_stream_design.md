# Lightweight I/Q telemetry stream (`iq_stream.c`)

Status: implemented, bench-verified (wire format, multi-subscriber
fan-out, subscriber timeout, and the Python client's FFT decode/scaling
all checked against the real source files below - see "Bench
verification"). Not yet confirmed on real hardware/over a real LAN (only
localhost so far) or against real RF (only a synthetic tone).

## Why a third I/Q path

minibitx already exports baseband I/Q two ways - openHPSDR Protocol 1 UDP
(`hpsdr_p1.c`) and USB Audio Class 2.0 (`usb_gadget.c`) - see
[`../04_remote_control_and_iq_output.md`](../04_remote_control_and_iq_output.md).
Both work fine for their intended audience (WSJT-X, Thetis, SDR Console,
a UAC2 host), but neither is a good fit for `tools/rigctl_panel.py`
wanting a live spectrum display:

- **HPSDR Protocol 1 is single-client.** `hpsdr_p1.c` keeps exactly one
  `stream_dest` address; a second UDP client sending its own START
  command just overwrites it - there's no rejection of a second
  connection, so a naive "make the panel a second HPSDR client" would
  silently steal the I/Q stream from whatever SDR app was already
  connected, rather than coexisting with it. Fixing that properly would
  mean teaching `hpsdr_p1.c` itself to fan out to several destinations,
  a change to code whose whole job today is being one specific,
  carefully-debugged protocol implementation (discovery, start/stop,
  C&C registers, MOX handling - see that file's own history).
- **Both carry protocol machinery a spectrum client has no use for.**
  HPSDR's discovery handshake, board/protocol identifiers, and C&C
  register layout, or UAC2's whole USB gadget/configfs/ALSA stack, are
  real complexity for a client that only wants "pairs of I/Q numbers,
  periodically, over the network."

So this is a third, independent path, explicitly scoped to be as simple
as possible on the wire, and explicitly designed to support several
simultaneous subscribers from the start - avoiding the single-client
problem outright rather than needing to reason about whether it matters
here too.

Like `hpsdr_p1.c` and `usb_gadget.c`, this is fed its own copy of the
I/Q from `sound_process()` and knows nothing about either of them - any
subset of the three can be active at once (a UAC2 host, WSJT-X on HPSDR,
and the spectrum panel here, all at the same time, none aware of the
others) - see `sound.c`'s own comment on this at the point all three are
called.

## Wire format

UDP, port 4536 (`IQ_STREAM_PORT`). No discovery, no start/stop
handshake: a client subscribes by sending *any* UDP datagram (content
ignored) to that port, and keeps receiving data packets as long as it
re-sends that "still here" datagram at least once every 5 seconds
(`SUBSCRIBER_TIMEOUT_SEC`) - a lapsed subscriber is simply dropped, no
explicit unsubscribe needed, so a client that crashes or is closed
without cleanup doesn't leak a slot forever. Up to 4 (`MAX_SUBSCRIBERS`)
clients can be subscribed at once, each getting an identical copy of the
same stream.

Data packet layout (all multi-byte fields big-endian, same convention
`hpsdr_p1.c` uses):

```
Offset  Size  Field
0       4     magic: 'I' 'Q' 'S' '1'
4       4     seq (uint32) - increments once per packet; a client can use
              gaps in this to notice dropped packets, but nothing on the
              server side acts on it
8       4     n_samples (uint32) - always 128 today (SAMPLES_PER_PACKET),
              sent explicitly so a client doesn't have to hardcode it
12      4*n   n_samples pairs of (int16 I, int16 Q), scaled so full-scale
              baseband amplitude reads close to full int16 range
```

128 samples/packet at the native 96kHz sample rate this stream carries
(no filtering or decimation, same as `hpsdr_p1.c`'s own outbound I/Q)
gives a packet every ~1.33ms, 524 bytes each - comfortably under any
normal MTU, no fragmentation risk, and a modest ~315kB/s per subscriber.

int16 rather than HPSDR's calibrated 24-bit scale: this stream's only
consumer is a visual spectrum display, which needs relative levels, not
hpsdr_p1.c's audience of SDR apps wanting calibrated absolute amplitude
for their own S-meter/noise-floor readouts.

## Real-time safety

Same pattern `hpsdr_p1.c` already established, for the same reason:
`iq_stream_send()` (called from `sound_process()`, on the real-time
audio thread) only ever appends to a lock-free single-producer/single-
consumer ring buffer and returns immediately - a dedicated pacer thread
drains it and sends one packet every 128/96000s, decoupled from the
audio thread's own bursty production pattern.

The subscriber list (who to send each packet to) is a separate piece of
state, touched only by the listener thread (adds/refreshes a subscriber
on each incoming datagram) and the pacer thread (reads it once per
packet, prunes expired entries) - both ordinary-priority threads, never
the audio thread. A plain mutex around that list is fine here; it would
not be fine around the sample ring buffer, which is exactly why that
part stays lock-free instead - see `hpsdr_p1.c`'s own comment on the
priority-inversion trap a mutex caused there when one side was the
real-time audio thread. Splitting these two pieces of state (lock-free
samples, mutex-protected subscriber list) rather than protecting
everything with one mutex is what makes both choices simultaneously
correct.

## Client side: `tools/rigctl_panel.py`'s `SpectrumClient`

Its own UDP socket, its own subscribe/keepalive traffic (re-sends a bare
datagram every second, comfortably under the server's 5s timeout), its
own receive thread - fully independent of `RigctlClient`'s TCP
connection to rigctld. If the UDP port isn't reachable (firewalled, or
an older minibitx build without `iq_stream.c`), the spectrum panel just
never gets data; the frequency/volume controls keep working regardless.

Decoding: each packet's `n_samples` (I, Q) pairs decode via
`np.frombuffer(data, dtype=">i2", ...)`, straight into a complex128
array (`I + jQ`) scaled back to roughly ±1.0. Samples accumulate into a
rolling buffer; whenever it holds at least `FFT_SIZE` (2048) samples,
the most recent 2048 are windowed (Hann) and FFT'd
(`np.fft.fftshift(np.fft.fft(...))`), giving 2048 bins spanning the full
±48kHz around dial center at 46.875 Hz/bin resolution - the same ±48kHz
range [`antialias_filter_design.md`](antialias_filter_design.md)'s
crystal-filter analysis and
[`rx_audio_demod_design.md`](rx_audio_demod_design.md)'s AGC-placement
work were both reasoning about, now actually visible on the panel.

dB calibration: dividing a bin's magnitude by `FFT_SIZE * mean(window)`
recovers a bin-centered input tone's true amplitude, so 0dB is
calibrated as "as loud as a single full-scale input tone would read" -
a normal dBFS-style reference, not an absolutely-calibrated S-meter (no
different from what any SDR app's own spectrum display does without a
signal generator to calibrate against).

The GUI redraws at a fixed ~15fps (`SPECTRUM_REDRAW_MS`) regardless of
how much faster the receive/FFT side actually runs - `get_latest()` just
hands back whatever's freshest each tick.

## Bench verification

All done against the real source files (`src/iq_stream.c` linked
unmodified into a small standalone C test harness feeding a synthetic
1kHz complex tone; `tools/rigctl_panel.py`'s `SpectrumClient` imported
directly, with `tkinter` stubbed out so it can run in a display-less
environment):

- **Wire format correctness:** every field (magic, seq, n_samples,
  sample count/pairs) matches the layout above across a real run;
  decoded sample magnitudes landed in the expected range for the
  injected tone's amplitude; zero sequence gaps over localhost.
- **Subscriber timeout:** a client that stops sending its keepalive
  datagram stops receiving packets within `SUBSCRIBER_TIMEOUT_SEC`, and
  the server logs the timeout - confirmed no further packets arrive for
  that client afterward.
- **Multi-subscriber fan-out:** three simultaneous subscribers each
  received an equal, healthy packet count over the same run - confirmed
  the same stream really does reach several clients at once, the whole
  point of not just adding a second HPSDR-protocol client.
- **End-to-end FFT correctness:** `SpectrumClient` pointed at the same
  synthetic-tone test server correctly located a 1kHz test tone at bin
  offset ~984Hz (one FFT bin's worth, 46.875Hz, of the true 1000Hz -
  as good as this resolution gets) and recovered its amplitude to
  within calibration expectations (a 0.5-amplitude tone read back at
  -6.6dB, matching the predicted 20·log10(0.5) ≈ -6.02dB almost
  exactly), with the noise floor elsewhere in the spectrum over 200dB
  down (no real noise source in the synthetic test, so this just
  confirms no spurious artifacts, not a real dynamic-range measurement).

Not yet done: a real bench/on-air run (real RF, real Wi-Fi/Ethernet
between the Pi and the panel's host, not localhost loopback), and
confirming the panel stays usable with a real minibitx process's other
load (audio thread, HPSDR, UAC2 all running at once) rather than the
standalone test harness's much lighter load.
