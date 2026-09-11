# RX Gain and Level Calibration: Using the 24 Bits Well

Status: **proposed** - not started. This records a design discussion
(2026-09) about how to approach setting/verifying RX gain, before any
code exists for it, so the reasoning and the right order of operations
aren't lost before someone picks it up.

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
that actually matters here: the WM8731 codec's Line input level, fixed
in `setup_audio_codec()` (`sound.c`):

```c
sound_mixer("hw:0", "Input Mux", 0);
sound_mixer("hw:0", "Line", 80);  // 80% of max
sound_mixer("hw:0", "Mic", 0);
```

`80` has no documented derivation behind it - it reads as a
reasonable-sounding default, not a bench-verified operating point the
way `TX_GAIN_CORRECTION` (0.045) was wattmeter-calibrated on the TX
side (`tx_power_calibration.md`). If that setting is wrong in either
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
   - Regularly slamming into full-scale codes → `Line` (80) needs to
     come down before anything else here is worth trusting.
   - Never getting anywhere close to full-scale even on the loudest
     signal found → there's room to raise `Line` and buy back real
     bits.
3. **Only then**, once 1-2 confirm the analog stage is in a sane place,
   does band-by-band peak logging over time become meaningful - it's
   fine-tuning against a validated baseline rather than measuring
   around a moving target.

## 5. What's not decided yet

- Whether any per-band digital gain actually gets applied downstream
  (a fixed table like TX's `scale`, something adaptive, or nothing at
  all if the analog stage alone turns out sufficient across all bands)
  is an open question - this doc only covers how to validate the
  starting point and gather trustworthy data, not what to do with it.
- No code exists yet for the noise-floor/strong-signal checks above or
  for band-by-band peak logging itself - both would most naturally be
  a temporary diagnostic build (or a debug flag), not something wired
  into every day operation of `sound_process()`.
