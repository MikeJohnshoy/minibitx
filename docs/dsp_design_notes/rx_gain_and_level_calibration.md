# RX Gain and Level Calibration: Using the 24 Bits Well

Status: a temporary diagnostic build exists (§6) implementing the
noise-floor/strong-signal check from §4; one quiet-band capture has been
taken on 40m (§6). Along the way, bench `amixer` inspection (2026-09)
found that the analog gain stage this doc originally pointed at
(`'Line'`) is actually just an on/off switch, not a gain control - see
§3 for the corrected story and the resulting code fix
(`RX_LINE_INPUT_ON` / `RX_CAPTURE_GAIN_PERCENT` in `sound.c`). This
still records the original design discussion (2026-09) about how to
approach setting/verifying RX gain, so the reasoning and the right order
of operations aren't lost.

## 1. Background

The RX chain's IQ eventually gets packed as 24-bit PCM for
`usb_gadget.c`'s UAC2 gadget (`uac_writer_thread()`, scaling a
`[-1, 1]`-range double by `8388607.0` = 2²³-1 before packing 3
little-endian bytes per sample - see
[`usb_uac_decimation_design.md`](usb_uac_decimation_design.md) for the
rest of that path). The natural question once real audio was flowing
end-to-end (WSJT-X decoding FT8 - see
[`../usb_gadget_os_setup.md`](../usb_gadget_os_setup.md) §9/§11) is
whether that 24-bit range is being used well: not clipping on strong
signals, not sitting so far under full-scale that real dynamic range is
being thrown away.

One idea discussed for approaching this: log peak I/Q amplitude per
band over some time period, and use that data to inform a per-band
digital gain/level scheme - the RX equivalent of what
[`tx_power_calibration.md`](tx_power_calibration.md) already did for TX
drive. Two caveats came up immediately that are worth recording before
that work starts, since they change what the logged data can actually
be used for.

## 2. Caveat 1: these numbers are relative, not absolute, without a separate calibration step

Nothing in the RX pipeline ties a digital sample value to a real-world
RF quantity. In `sound_process()` (`sound.c`), the raw ADC sample is
normalized purely against the ADC's own full-scale reference:

```c
double rf = (double)s / 2147483648.0;   // fraction of full-scale, no dB/dBm attached
```

Everything downstream of that (the IQ mix, the anti-alias filter, the
48kHz decimation, the 24-bit pack for USB) stays in that same
"fraction of full-scale" space. So peak-logging by band tells you real,
useful *relative* things - which bands run hotter, how close to
full-scale you get, how it varies over a day - but nothing in dBm or µV
at the antenna connector.

Getting an absolute number requires one deliberate calibration step
outside this pipeline: inject a known level from a signal generator
into the antenna port (a common reference point is -73 dBm, "S9"), read
the resulting peak digital amplitude, and that pairing becomes a single
dB-per-code conversion factor. Worth checking that factor holds at more
than one frequency/level before trusting it broadly - `tx_power_calibration.md`
§2 already ran into a real case on this same board family where gain
wasn't flat across HF, so RX shouldn't be assumed flat either without
checking.

If all that's actually needed is "am I using the bits well," the
relative-only data from simple peak logging may be entirely sufficient
and the calibrated-signal-generator step can be skipped. It's worth
being explicit about which goal is in play before starting, though,
since the two produce data that looks superficially similar but answers
different questions.

## 3. Caveat 2: this assumes the analog gain ahead of the ADC is already set correctly

Peak-logging happens entirely downstream of the one analog gain stage
that actually matters here. That stage turned out *not* to be what it
first looked like: `'Line'` (the WM8731 input-select control this doc
originally pointed at) is actually just an on/off switch
(`amixer -c 0 sget 'Line'` reports `Capabilities: cswitch` only - no
volume or dB range at all). The real continuous gain control is a
separate ALSA simple-mixer element, `'Capture'`
(`Capabilities: cvolume`, raw range 0-31), which maps onto the WM8731's
actual line-input attenuator (-34.5dB to +12dB in 1.5dB steps per the
codec datasheet). Before this fix, `setup_audio_codec()` never set
`'Capture'` explicitly at all - it simply ran at whatever the kernel's
`wm8731` driver defaulted to on boot (confirmed by bench measurement:
step 15 of 31, i.e. 48%, -12.00dB). `setup_audio_codec()` now sets both
explicitly:

```c
sound_mixer("hw:0", "Input Mux", 0);
sound_mixer("hw:0", "Line", RX_LINE_INPUT_ON);        // on/off switch, not a gain
sound_mixer("hw:0", "Capture", RX_CAPTURE_GAIN_PERCENT); // the real analog gain, 50% of max, by default
sound_mixer("hw:0", "Mic", 0);
```

`RX_CAPTURE_GAIN_PERCENT` (`sound.c`) is deliberately set to 50, which
`sound_mixer()`'s percent-to-raw conversion (`percent * max / 100`,
integer division) turns into exactly `50*31/100 = 15` - the same step
the driver was defaulting to, chosen so the diagnostic data already
captured against that default (§6) stays valid. It has no other
documented derivation behind it - it reads as a reasonable-sounding
default, not a bench-verified operating point the way
`TX_GAIN_CORRECTION` (0.045) was wattmeter-calibrated on the TX side
(`tx_power_calibration.md`). If that setting is wrong in either
direction, any digital peak data collected on top of it is measuring
the consequences of that choice, not the radio itself:

- **Too hot**, and a strong signal or a busy band clips inside the
  WM8731's own ADC before any digital sample exists to log. No amount
  of downstream digital gain scheme can undo that - it can only decide
  how much of an already-damaged signal to keep.
- **Too conservative**, and only the top handful of the 24 available
  bits ever get exercised, quietly throwing away real dynamic range and
  burying weak signals a little deeper in quantization noise than they
  need to be.

## 4. Recommended order of operations

Rather than starting straight into per-band peak logging, validate the
Line-in setting first, independently of it:

1. **Noise-floor check.** Terminate or disconnect the antenna and look
   at the resulting peak/RMS digital level with nothing but the
   receiver's own noise present. That level, relative to full-scale,
   is the actual usable headroom available above the noise floor - it
   should be a meaningful number of dB below full-scale, not a handful
   of counts above zero.
2. **Strong-signal check.** With the antenna connected, find the
   loudest realistic signal available (a strong local station, a busy
   contest weekend, or a signal generator if one's on hand) and check
   where its peaks land:
   - Regularly slamming into full-scale codes → `RX_CAPTURE_GAIN_PERCENT`
     (50) needs to come down before anything else here is worth
     trusting.
   - Never getting anywhere close to full-scale even on the loudest
     signal found → there's room to raise it and buy back real bits.
3. **Only then**, once 1-2 confirm the analog stage is in a sane place,
   does band-by-band peak logging over time become meaningful - it's
   fine-tuning against a validated baseline rather than measuring
   around a moving target.

Remember the real ceiling here is the WM8731 ADC's own usable dynamic
range/noise floor, not the literal 144dB a 24-bit container could
theoretically hold - the goal is "strongest realistic signal sits
comfortably under full-scale, noise floor sits comfortably above the
quantization floor," not "hit every last bit."

## 5. What's not decided yet

- Whether any per-band digital gain actually gets applied downstream
  (a fixed table like TX's `scale`, something adaptive, or nothing at
  all if the analog stage alone turns out sufficient across all bands)
  is an open question - this doc only covers how to validate the
  starting point and gather trustworthy data, not what to do with it.
- Whether `RX_CAPTURE_GAIN_PERCENT` itself needs to change from 50 -
  that's exactly what §6's diagnostic build and the bench session it
  enables are for.
- Whether this analog setting should ever become runtime-adjustable
  (a live CAT/rigctl "RF gain" control) rather than a fixed,
  bench-calibrated constant re-set at compile time, the way
  `TX_GAIN_CORRECTION` already is on the TX side. Current thinking
  leans toward keeping it fixed, in line with minibitx's minimal-
  onboard-controls philosophy - a live analog gain control has no level
  meter to react to today, and a coarse ALSA mixer step is a clumsier
  lever than a downstream digital multiplier would be if per-band
  differences ever turn out to matter. Not a final decision, just the
  current lean.

## 6. The diagnostic build

`sound.c` has a bench-only instrumentation path, compiled in only with
`-DRX_GAIN_DIAG`:

```
make CPPFLAGS=-DRX_GAIN_DIAG
```

(`CPPFLAGS`, not `CFLAGS` - a plain `make CFLAGS+=...` on the command
line replaces this Makefile's own `CFLAGS` line entirely rather than
adding to it, silently dropping `-O3 -march=native -Wall -Wextra
-std=gnu11` in the process; `CPPFLAGS` is untouched by the Makefile, so
passing it this way only adds the diagnostic define.) Rebuild plain
`make` afterward to return to a normal binary - the two shouldn't be
mixed up, since the diagnostic build's `printf`s are not something to
leave running in normal operation.

It taps the raw ADC sample (`rf` in `sound_process()`, before any
digital mixing/filtering - see Caveat 2 above for why that's the right
point to measure), and once per second of audio (96000 samples at the
fixed 96kHz capture rate) prints one line:

```
rxgain: freq=7030000 capture=50% peak=-8.3dBFS rms=-42.1dBFS
```

- `freq` and `capture` are just the currently tuned dial frequency and
  the compiled-in `RX_CAPTURE_GAIN_PERCENT`, included so a captured log
  is self-describing without needing separate notes.
- `peak`/`rms` are in dBFS (0 = full-scale); a window that ever actually
  hits full-scale gets a trailing `*** CLIPPING ***` marker so it's
  unambiguous when scanning a captured log.
- Redirect/tee console output to a file per test run to build up a
  record, e.g. `./minibitx | tee rxgain_20m_dummyload_line80.log` -
  naming each file by band/condition/gain setting keeps a multi-run
  sweep straightforward to compare afterward.

Suggested first session, matching the two-sided check in §4: on a dummy
load (no antenna signal - the noise-floor check), tune so the FT8
sub-band sits at the center of the IF passband rather than its edge (so
the anti-alias filter's full margin is available), and let it run for a
minute or so on 20m, then repeat on 40m - all at today's default
`RX_CAPTURE_GAIN_PERCENT` (50) as the baseline before trying anything
else. Follow with the strong-signal side of the check (real antenna,
active band) the same way, same two bands, same starting gain, before
considering whether 50 needs to move.

The quiet-band capture already collected on 40m (2026-09, `'Line'`
believed at the time to be the gain control, actually at its on/off
default while `'Capture'` sat at the kernel driver's undocumented boot
default of step 15/48%/-12dB) remains directly comparable to future
runs at `RX_CAPTURE_GAIN_PERCENT=50`, since that constant was chosen
specifically to reproduce that exact same -12dB operating point on
purpose rather than by driver accident. It doesn't need to be redone -
it can stand as the first data point in the sweep.
