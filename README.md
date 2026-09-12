# minibitx — an experimental test bed

minibitx is a project to experiment with software from the sbitx codebase. 
The 'mini' in minibitx means we're assembling the minimal set of code necessary to configure and operate the sbitx hardware with the best performance possible.
Code for each required function has been pulled from the sbitx baseline and added to minibitx.
minibitx can now be compiled and run on the Rpi-4 in the sbitx to demonstrate and test performance.
Mature, highly developed external Software Defined Radio (SDR) applications are being used with minibitx to find the upper limit of the sbitx processing chain.
Lessons learned in this project can be folded back into sbitx or used in other projects.

minibitx currently has the receive processing pipeline shown below in working order though it will continue to be reviewed and refined. Note that this pipeline is largely "baked into" the sbitx hardware.

```
  Antenna
     |
     v
  Low Pass Filter (LPF) bank (select one)
     |
     v
  Mixer 1  <---  clk2, si5351 RX LO (varies with tuning)
     |            mixes received signal to center of crystal filter
     v
  Crystal filter centered at ~40.0124 MHz, based on hardware spec 
     |
     v
  Mixer 2  <---  clk1, si5351 (FIXED, never varies, shifts output
     |           of crystal filter to 24kHz baseband   
     v
  Low IF, centered at RX_IF_HZ (24000 Hz)
     |
     v
  ADC / wm8731 audio codec (sound.c, 96 kHz sample rate) gain is settable?
     |            need to examine how this gain setting affects dynamic range
     v            (used as IF gain in sbitx?)
  Software VFO (vfo.c, "lo" in radio.c) <--- FIXED at RX_IF_HZ (24000 Hz)
     |            sound.c: sound_process() calls vfo_read_iq() per sample
     v            converts real value A/D output to analytic I&Q at baseband
  Baseband I/Q (centered at 0 Hz)
     |
     v
  Anti-alias FIR (antialias.c, 21 taps, applied separately to I and Q)
     |
     v
  handed to interface software (hpsdr_p1, USB audio out, or simple network
            interface) to work with external applications
```
A transmit processing pipeline also exists (just imagine the reverse of the process abovve), currently for CW transmission only.

A secondary minibitx objective is to replace code dependent on deprecated libraries, so wiringPi has been replaced with libgpio.  The 'bit banging' code used for i2c bus was replaced with i2c support built into the kernel.  

minibitx is quite small - most of the code is in the interface software that passes data through various protocols (hpsdr protocol 1, USB audio and control gadget, and UDP interface) to external SDR applications.

TO DO: 

1. simple/efficient/low-overhead packet interface for IQ in and out

## Building

```
make
```

Produces a single `minibitx` binary from the sources in `src/`. Requires
`libasound`, and the usual `pthread`/`libm`/`libdl` (see
`Makefile` for the exact link line).

## Running

```
./minibitx
```

Brings up the radio hardware, starts the audio and network threads, and
listens for control connections — a rigctld-compatible server on TCP
4532, and an HPSDR Protocol 1 UDP listener. A composite USB gadget provides I&Q data as audio and a CAT control interface.  Additional interfaces will be experimented with.

## Documentation

The `docs/` folder has the detailed breakdown of how minibitx works,
organized roughly from the hardware up:

| Doc | Content |
|---|---|
| [`docs/00_intro.md`](docs/00_intro.md) | Project scope, status, and a map of the rest of the docs |
| [`docs/01_hardware_init_and_control.md`](docs/01_hardware_init_and_control.md) | GPIO, si5351/I2C, WM8731 codec bring-up |
| [`docs/02_rx_processing_pipeline.md`](docs/02_rx_processing_pipeline.md) | Antenna to baseband I/Q, stage by stage |
| [`docs/03_tx_processing_pipeline.md`](docs/03_tx_processing_pipeline.md) | What TX support exists today and what's still missing |
| [`docs/04_remote_control_and_iq_output.md`](docs/04_remote_control_and_iq_output.md) | rigctld, HPSDR control/IQ, USB Audio Class output |
| [`docs/05_process_and_threading_model.md`](docs/05_process_and_threading_model.md) | Startup sequence and thread structure |
| [`docs/dsp_design_notes/`](docs/dsp_design_notes/) | DSP work (e.g. the anti-alias FIR) and other design docs |
| [`docs/07_build_and_deployment.md`](docs/07_build_and_deployment.md) | Build, kernel/overlay dependencies, deployment notes |
| [`docs/08_troubleshooting_and_bringup.md`](docs/08_troubleshooting_and_bringup.md) | Hardware bring-up gotchas |
| [`docs/10_external_digital_modes_wsjtx.md`](docs/10_external_digital_modes_wsjtx.md) | Using minibitx with WSJT-X and similar digital-mode apps |
| [`docs/11_general_coverage_sdr_receiver.md`](docs/11_general_coverage_sdr_receiver.md) | Using minibitx as a general-coverage SDR receiver |
| [`docs/12_simple_cw_transceiver.md`](docs/12_simple_cw_transceiver.md) | Building a simple CW transceiver around minibitx |

## Credits

- Inspired by Ashhar Farhan's (VU2ESE) original sbitx code
- Code was based on JJ's 64-bit repository at https://github.com/drexjj/sbitx
- hpsdrsim.c from the piHPSDR project 
