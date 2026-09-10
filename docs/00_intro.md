# 00 — minibitx introduction

## What this is

minibitx runs on the Raspberry Pi inside an sbitx radio, in place of the
`sbitx` software that shipped with it. It brings the hardware up, exposes
a rigctld-compatible control port, and streams baseband I/Q out over
HPSDR Protocol 1 (UDP) and/or a USB Audio Class 2 gadget. An external SDR
application — SDR Console, Quisk, WSJT-X's rig-control layer, etc. — does
everything downstream of that: FFT display and waterfall, demodulation of
whatever mode is in use, filtering, and audio routing.

The sBitx code base grew to support multiple operating modes with a powerful
user interface. My goal was to strip out everything but what was needed to make the
hardware work right, and leave as much as possible of the signal processing and user-interface
to the growing collection of high quality SDR applications.  

## What this is not

minibitx has no onboard demodulation, no waterfall, and no mode logic.
`m`/`M` (mode get/set) exist on the rigctld control port purely so a
client's mode selector doesn't error out — minibitx doesn't act on the
value in any way. There is also no dependency on the original sbitx
codebase at runtime; minibitx was built by extracting the minimum set of
functions from sbitx needed to let an external SDR app drive the
hardware, and runs stand-alone.

## Status

Receive works: antenna to baseband I/Q, streamed over HPSDR and/or USB
audio, remotely controlled by HPSDR Protocol 1 or tunable via rigctld. 

Transmit:  a simple CW waveform (with Blackman-Harris shaping) controlled
from a straight key on the sbitx key input.

## How the rest of these docs are organized

Roughly bottom-up, following the signal and control paths through the
code:

- [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md) —
  bringing up the GPIO lines, the si5351 oscillator, the I2C bus, and the
  WM8731 audio codec before any signal processing can happen.
- [`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) — the
  receive signal chain itself, antenna to baseband I/Q.
- [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) — the
  transmit side: what exists, what's planned.
- [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md)
  — how external software tunes/keys the radio and receives the I/Q it
  produces.
- [`05_process_and_threading_model.md`](05_process_and_threading_model.md)
  — how `main()` brings all of the above up, and the thread structure
  that keeps it running.
- [`dsp_design_notes/`](dsp_design_notes/) — standalone design write-ups
  for DSP work that's been analyzed but not yet wired into the code.
- [`07_build_and_deployment.md`](07_build_and_deployment.md) — building
  minibitx and the kernel/OS pieces it depends on.
- [`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md)
  — hardware bring-up gotchas that don't fit neatly elsewhere.
- `10_`–`12_` — guides for using minibitx with specific kinds of external
  software (digital modes, general-coverage receive, a CW transceiver).

Documents in the `0x` range describe how minibitx works internally;
documents numbered `10` and up describe how to use it.
