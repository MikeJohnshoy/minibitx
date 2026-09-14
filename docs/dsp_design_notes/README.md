# DSP design notes

Home for DSP design notes — measured data, derivations,
and concrete specs to build against, kept separate from the
"how the shipped code works" docs elsewhere in `docs/`.

Each doc in this folder should open with a `Status:` line stating whether
it's proposed, in progress, or implemented, following the convention
already used in `antialias_filter_design.md`.

## Contents

- [`antialias_filter_design.md`](antialias_filter_design.md) — measured
  crystal filter response and the FIR anti-alias filter spec derived
  from it for the RX chain. Status: implemented (`antialias.c`).
- [`tx_power_calibration.md`](tx_power_calibration.md) — why sbitx's
  per-band TX scale table doesn't transfer to minibitx's pipeline
  unchanged, and a step-by-step wattmeter procedure (one QRP CW
  frequency per band) to re-derive per-band values for a flat 5W.
  Status: done - all 9 bands bench-confirmed in the 4.7-5.5W target
  window.
- [`usb_uac_decimation_design.md`](usb_uac_decimation_design.md) — the
  96kHz->48kHz decimating lowpass that makes `usb_gadget.c`'s UAC2
  gadget actually deliver the 48kHz it adve# DSP design notes

Home for DSP design notes — measured data, derivations,
and concrete specs to build against, kept separate from the
"how the shipped code works" docs elsewhere in `docs/`.

Each doc in this folder should open with a `Status:` line stating whether
it's proposed, in progress, or implemented, following the convention
already used in `antialias_filter_design.md`.

## Contents

- [`antialias_filter_design.md`](antialias_filter_design.md) — measured
  crystal filter response and the FIR anti-alias filter spec derived
  from it for the RX chain. Status: implemented (`antialias.c`).
- [`tx_power_calibration.md`](tx_power_calibration.md) — why sbitx's
  per-band TX scale table doesn't transfer to minibitx's pipeline
  unchanged, and a step-by-step wattmeter procedure (one QRP CW
  frequency per band) to re-derive per-band values for a flat 5W.
  Status: done - all 9 bands bench-confirmed in the 4.7-5.5W target
  window.
- [`usb_uac_decimation_design.md`](usb_uac_decimation_design.md) — the
  96kHz->48kHz decimating lowpass that makes `usb_gadget.c`'s UAC2
  gadget actually deliver the 48kHz it advertises, cascaded after
  `antialias_filter_design.md`'s own filter. Status: implemented
  (`decim48k.c`), bench-verified numerically and now against a real
  UAC2 host too (WSJT-X decoding FT8).
- [`rx_gain_and_level_calibration.md`](rx_gain_and_level_calibration.md)
  — how to approach checking/tuning RX gain now that IQ audio actually
  reaches a real UAC2 host: why raw peak levels are relative-only
  without a separate calibrated-signal-generator step, why that has to
  wait on confirming the WM8731 `Line` input level (currently an
  undocumented fixed 80%) is itself set correctly, and the recommended
  noise-floor/strong-signal checks to do first. Status: proposed - a
  design discussion recorded ahead of any code.
- [`rx_audio_demod_design.md`](rx_audio_demod_design.md) — the local CW
  audio monitor (`rx_audio.c`): the product-detector-plus-BFO design, why
  a fixed output gain couldn't work once real signal levels were
  measured on the bench (and the AGC that replaced it), the WM8731
  `Master` L/R independence fix that came out of the same debugging
  session, the two-stage (v3) filtering that replaced v1/v2's single
  combined filter - a wide complex image-reject bandpass decoupled from
  a separate, narrow post-demod selectivity filter - and, following an
  on-air report of a 3kHz-away CW signal still being clearly audible, the
  two-step chase to fix it: first a 4-section cascaded biquad (an
  improvement, but a resonator cascade's skirt never gets truly steep no
  matter how many sections), then replacing that outright with a fixed
  8-pole elliptic (Cauer) design - the same math a real crystal ladder
  filter's synthesis uses, landing in a real CW crystal filter's shape-
  factor range - deliberately not runtime-adjustable, plus the group-
  delay/ring-time cost that bought, and a settling-time fix to the test
  harness needed to measure the improvement correctly.
  Status: implemented; first on-air CW copy confirmed (2026-09) against
  v1's filter, v2's image-reject filter also on-air confirmed (signals
  fade below ~500Hz sidetone pitch, matching the bench prediction), v3's
  stage split and the 4-section cascade are both bench-verified
  numerically but not yet re-confirmed on air.rtises, cascaded after
  `antialias_filter_design.md`'s own filter. Status: implemented
  (`decim48k.c`), bench-verified numerically and now against a real
  UAC2 host too (WSJT-X decoding FT8).
- [`rx_gain_and_level_calibration.md`](rx_gain_and_level_calibration.md)
  — how to approach checking/tuning RX gain now that IQ audio actually
  reaches a real UAC2 host: why raw peak levels are relative-only
  without a separate calibrated-signal-generator step, why that has to
  wait on confirming the WM8731 `Line` input level (currently an
  undocumented fixed 80%) is itself set correctly, and the recommended
  noise-floor/strong-signal checks to do first. Status: proposed - a
  design discussion recorded ahead of any code.
- [`rx_audio_demod_design.md`](rx_audio_demod_design.md) — the local CW
  audio monitor (`rx_audio.c`): the product-detector-plus-BFO design, why
  a fixed output gain couldn't work once real signal levels were
  measured on the bench (and the AGC that replaced it), the WM8731
  `Master` L/R independence fix that came out of the same debugging
  session, the two-stage (v3) filtering that replaced v1/v2's single
  combined filter - a wide complex image-reject bandpass decoupled from
  a separate, narrow, runtime-adjustable post-demod selectivity filter -
  and, following an on-air report of a 3kHz-away CW signal still being
  clearly audible, why that selectivity filter needed to become a
  4-section cascaded biquad (plus the bandwidth-correction math that
  keeps `rx_audio_set_filter_bw()` meaning what it says, and a
  settling-time fix to the test harness needed to measure it correctly).
  Status: implemented; first on-air CW copy confirmed (2026-09) against
  v1's filter, v2's image-reject filter also on-air confirmed (signals
  fade below ~500Hz sidetone pitch, matching the bench prediction), v3's
  stage split and the 4-section cascade are both bench-verified
  numerically but not yet re-confirmed on air.
