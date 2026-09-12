# RX Gain and Level Calibration: Using the 24 Bits Well

Status: done. The noise-floor/strong-signal check from §4 has been run
(dummy-load and on-air scans across 80m-10m, plus an SDR Console cross-
check on 40m FT8), and `RX_CAPTURE_GAIN_PERCENT` has been raised from
the kernel driver's accidental default to a deliberately chosen 70 - see
§6 for the data and reasoning. The temporary per-second bench diagnostic
that produced that data has been retired in favor of a permanent,
always-compiled clip guard (§7) - a lightweight safety net rather than a
recording instrument. Along the way, bench `amixer` inspection (2026-09)
also found that the analog gain stage this doc originally pointed at
(`'Line'`) is actually just an on/off switch, not a gain control - see
§3 for that corrected story. This still records the original design
discussion (2026-09) about how to approach setting/verifying RX gain, so
the reasoning and the right order of operations aren't lost.

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
sound_mixer("hw:0", "Capture", RX_CAPTURE_GAIN_PERCENT); // the real analog gain, 70% of max, by default
sound_mixer("hw:0", "Mic", 0);
```

`RX_CAPTURE_GAIN_PERCENT` (`sound.c`) is now 70 - raised from an initial
50 (chosen only to reproduce the kernel driver's accidental -12dB boot
default while the bench study was still running) once §6's data showed
real headroom to spare. It's a bench-informed choice, not yet a
wattmeter-grade calibration the way `TX_GAIN_CORRECTION` (0.045) is on
the TX side (`tx_power_calibration.md`) - see §6 for exactly what data
backs it and what it doesn't yet cover. If this setting is wrong in
either direction, any digital peak data collected on top of it is
measuring the consequences of that choice, not the radio itself:

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
     (70) needs to come down before anything else here is worth
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
- Whether `RX_CAPTURE_GAIN_PERCENT` (now 70) needs to move again -
  §6/§7 cover the data behind today's value and the permanent clip
  guard that watches for a future signal proving it wrong, but the
  underlying number hasn't been re-checked against a genuinely strong
  band opening yet (see §6's caveat about what this data does and
  doesn't cover).
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

## 6. Bench results and the chosen gain

The bench study used a temporary per-second diagnostic (since retired -
see §7) that tapped the raw ADC sample (`rf` in `sound_process()`,
before any digital mixing/filtering - see Caveat 2 above for why that's
the right point to measure) and printed peak/RMS dBFS once per second,
tagged with the tuned frequency and `Capture` setting.

At the kernel driver's accidental default (raw step 15, -12dB, what
`RX_CAPTURE_GAIN_PERCENT=50` had been deliberately set to reproduce
during the study - see §3), a scan across all HF bands (80m through
10m) found:

- **Dummy load (noise floor)**: remarkably consistent across every band
  - RMS -74.5 to -76.4dBFS, peak -59 to -62dBFS - with a handful of
    isolated spikes to -53 to -54dBFS on 10m/20m/40m that read as
    internally-generated noise (GPIO/SPI/PWM switching harmonics are
    the usual suspect on a Pi-hosted radio) rather than anything
    band-specific, since there's no antenna signal present to explain
    them.
- **Real antenna, FT8 sub-bands**: 20m and 10m came back indistinguishable
  from the dummy-load floor (those bands were simply quiet at capture
  time). 80m ran a bit hotter (RMS avg -72.7dBFS, peak avg -57.4dBFS).
  40m was the liveliest by far - RMS averaging -58.8dBFS with peaks
  averaging -46.3dBFS and the single loudest reading at **-41.7dBFS**,
  ~40dB below full-scale.
- **Cross-check via SDR Console** on that same 40m FT8 activity: FT8
  signals peaking around -65dBm rising out of a -110dBm noise floor (a
  45dB spread) - consistent with the dBFS data above, though SDR
  Console's absolute dBm scale is itself uncalibrated against a real RF
  reference here (Caveat 1), so the *spread* is the trustworthy number,
  not the specific dBm values.

40dB of unused headroom below full-scale, even on the busiest band
found, is well past "comfortably under full-scale" (§4) - the
too-conservative failure mode, not the too-hot one. `RX_CAPTURE_GAIN_PERCENT`
was raised from 50 to **70** (raw step 21, ~-3.0dB - a +9dB increase),
which by the same logic should land that -41.7dBFS peak around -33dBFS:
still well clear of clipping on everything observed, while using
noticeably more of the available range.

**What this data doesn't cover yet**: every band scan above happened to
catch fairly ordinary conditions - even 40m's "liveliest" reading was
described at the time as not necessarily the loudest realistic case
(a strong local station, a contest weekend, or a real band opening
could peak meaningfully higher). §4's strong-signal check is worth
re-running at 70% specifically looking for that louder case before
treating 70 as settled rather than "settled against what's been seen
so far." §7's clip guard is the safety net for whatever that check
finds.

## 7. The permanent clip guard

The per-second bench diagnostic above did its job and has been removed;
what replaced it in `sound.c` is a much smaller, always-compiled check
rather than a bench recorder - no periodic logging, no RMS
accumulation, nothing gated behind a build flag. It taps the same raw
`rf` sample and says something only on the rising edge of an actual
clipping episode:

```
sound: *** CLIPPING *** freq=7074000 capture=70% - RF front end is overdriving the ADC, consider lowering RX_CAPTURE_GAIN_PERCENT
```

Per-sample cost is one `fabs()` and one comparison - negligible next to
the mixing and FIR-filter arithmetic `sound_process()` already does for
every sample, so it costs nothing measurable to leave running in every
normal build, including on the Pi Zero 2W baseline. If it ever prints
during normal operation, that's the signal to come back to this doc and
reconsider `RX_CAPTURE_GAIN_PERCENT`.

## 8. Muted during TX

Comparing notes against `github.com/drexjj/sbitx` (a GUI-based sbitx
fork sharing this same WM8731 hardware) turned up one more thing worth
carrying over: its `tr_switch()` zeroes the `'Capture'` control the
moment TX begins and only restores it once the T/R relay has settled
back to RX, protecting the ADC/DSP chain from whatever bleeds into the
RX input during TX (relay leakage, PA harmonics, shared-ground
crosstalk). minibitx didn't do this - `RX_CAPTURE_GAIN_PERCENT` just sat
untouched through every TX/RX transition - so it now does too, via
`sound_set_rx_capture()` (`sound.c`), called from `radio_tx_apply()`
(`radio.c`) at the same two points sbitx does: muted first on the way
into TX (before PTT/the relay/either clock even changes), restored last
on the way back to RX (after the relay and clocks are already back).
See
[`03_tx_processing_pipeline.md`](../03_tx_processing_pipeline.md)
for where that sits in the TX sequence. Pure safety measure - no effect
on RX sensitivity or TX power.
