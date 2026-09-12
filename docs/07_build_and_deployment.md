# 07 — build and deployment

Status: stub.

## Scope

- Build: `Makefile`, the single `minibitx` binary, the sources it's
  linked from, and its library dependencies (`libasound`, `pthread`,
  `libm`, `libdl`). GPIO (`src/gpio.c`) talks straight to the kernel's
  character-device API (`/dev/gpiochip0`) - no separate GPIO library to
  link.
- The `update` script: what it does (`git stash` / `git pull` from
  `$HOME/minibitx`) and when to use it versus a manual pull.
- Kernel/OS dependencies minibitx assumes are already in place:
  `dtoverlay=audioinjector-wm8731-audio` for the codec, the
  `i2c-rtc-gpio` overlay the si5351 rides on, and membership in the
  `gpio` group for `/dev/gpiochip0` access (the same group requirement
  wiringPi's `/dev/gpiomem` access always had - not a new one this
  brought in).
- USB gadget (UAC2 + CDC-ACM) support is *not* one of those
  already-in-place dependencies - it needs its own OS-level setup
  (`dtoverlay=dwc2, dr_mode=peripheral`, `libcomposite`, and powering the
  Pi via GPIO instead of USB-C). See
  [`usb_gadget_os_setup.md`](dsp_design_notes/usb_gadget_OS_setup.md).
- Deployment: running under a plain terminal versus systemd/journald —
  see [`05_process_and_threading_model.md`](05_process_and_threading_model.md)
  for how minibitx reports operational state either way (per-command
  console echoes, not a periodic status line).
- `credits` — upstream sbitx code this project is based on.
