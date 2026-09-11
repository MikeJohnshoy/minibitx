// gpio.h
//
// Direct-to-kernel GPIO access via the Linux GPIO character-device ioctl
// API (uAPI v2, <linux/gpio.h>) - replaces radio_hw.c's earlier
// dependency on wiringPi (unmaintained upstream, no Pi 5 support - not a
// concern for minibitx's Pi 4 target today, but the character-device API
// is the kernel's own current supported interface either way, not a
// workaround chosen just to drop a dependency). Same "let the kernel
// driver do it, no vendor library" precedent i2c.c already set when it
// replaced this project's old bit-banged I2C with plain ioctl() calls
// against /dev/i2c-N.
//
// One line request (and one open file descriptor) per GPIO line - this
// matches how radio_hw.c already treats each pin as its own independent
// handle (pinMode() + digitalWrite()/digitalRead() by pin number); there
// is no need here for the character-device API's multi-line atomic
// request/read/write support.
//
// All offsets passed in here are BCM GPIO numbers - the kernel's own
// numbering, and what /dev/gpiochip0 expects - NOT wiringPi's own pin
// numbers, which is what radio_hw.h's pin constants used to be before
// this rewrite. The BCM numbers radio_hw.h now uses were derived from a
// `gpio readall` capture on real sBitx v2 hardware (see
// docs/01_hardware_init_and_control.md) - if this is ever ported to a
// different board/pinout, that mapping needs re-deriving the same way,
// not assumed.

#ifndef GPIO_H
#define GPIO_H

// Requests bcm_gpio as an output line, driven immediately to
// initial_value (0 or nonzero) as part of the same request - there's no
// separate "set mode, then write" step the way wiringPi needed.
// consumer_label shows up in tools like `gpio readall` or
// /sys/kernel/debug/gpio, so future debugging can identify which pin is
// which without cross-referencing this source.
//
// Returns an opaque line handle (an fd, but callers should treat it as
// opaque) to pass to gpio_write(), or -1 on failure (already logged its
// own reason - most commonly /dev/gpiochip0 missing, or the offset
// already in use by something else).
int gpio_request_output(unsigned int bcm_gpio, int initial_value,
                         const char *consumer_label);

// Requests bcm_gpio as an input line, with the SoC's internal pull-up
// enabled if pull_up is nonzero (matches wiringPi's PUD_UP - there is no
// external pull-up on CW_KEY). Returns an opaque line handle (>= 0), or
// -1 on failure (already logged).
int gpio_request_input(unsigned int bcm_gpio, int pull_up,
                        const char *consumer_label);

// Drives a line previously returned by gpio_request_output() high
// (nonzero) or low (0). Returns 0 on success, -1 on failure (logs its
// own reason; a line handle from a failed request, or an otherwise
// invalid one, safely returns -1 rather than touching an arbitrary fd).
int gpio_write(int line, int value);

// Reads a line's current level (0 or 1). Returns -1 on failure, same
// invalid-handle safety as gpio_write().
int gpio_read(int line);

#endif /* GPIO_H */
