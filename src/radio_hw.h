// radio_hw.h
//
// radio hardware control for minibitx: boot-time GPIO setup,
// LPF band switching, board-revision detection, and the INA260 power
// monitor.

#ifndef RADIO_HW_H
#define RADIO_HW_H

/* ---- GPIO pin assignments (BCM GPIO numbering, sBitx v2 hardware) -----
 *
 * These used to be wiringPi's own pin numbers (a different numbering
 * from BCM's), back when radio_hw.c drove them through wiringPi. Now that
 * radio_hw.c talks to gpio.c's character-device API instead - which
 * takes BCM offsets, matching /dev/gpiochip0 - these constants are BCM
 * numbers. The old->new mapping (and the live `gpio readall` capture on
 * real hardware it was verified against) is recorded in
 * docs/01_hardware_init_and_control.md; don't reuse the old numeric
 * values here as if they still meant the same physical pins - wiringPi's
 * numbering and BCM's numbering are two unrelated schemes that happen to
 * both be small integers.
 */

#define TX_LINE   23   // T/R relay control line
#define TX_POWER  16   // set once at boot, LOW; purpose unconfirmed in sbitx
#define EXT_PTT   12   // external PTT input/output line
#define LPF_A     24   // low-pass filter select lines, one active at a time
#define LPF_B     25
#define LPF_C     8    // shares a physical pin with SPI0's CE0 - unused as
                       // SPI on this board, so repurposing it as a plain
                       // GPIO output is safe (confirmed via `gpio readall`:
                       // it shows as OUT, not ALT0/SPI mode)
#define LPF_D     7    // shares a physical pin with SPI0's CE1 - same as
                       // LPF_C above
#define CW_KEY    4    // straight key input (active low - open = idle,
                       // closed to ground = key down)

/* ---- Board hardware revision ------------------------------------------ */

#define SBITX_DE  (0)  // original sBitx, no power/SWR bridge board present
#define SBITX_V2  (1)  // v2-and-later, power/SWR bridge board present

/* Requests TX_LINE, TX_POWER, EXT_PTT, and the four LPF select lines as
 * GPIO outputs (via gpio.c), driving them to their idle (LOW) state as
 * part of the same request, and requests CW_KEY as an input with its
 * pull-up enabled. Call once, before any other GPIO or radio_hw
 * function. Returns 0 on success, -1 if any of those line requests fail
 * (e.g. /dev/gpiochip0 missing, or a pin already claimed by something
 * else). */
int radio_hw_gpio_init(void);

/* Probes I2C address 0x8 (the power/SWR bridge board) to distinguish
 * original sBitx ("DE") hardware from v2-and-later. Returns SBITX_DE or
 * SBITX_V2. */
int radio_hw_detect_version(void);

/* Selects the low-pass filter appropriate for `frequency` (Hz) by driving
 * exactly one of the four LPF_x lines high and the rest low. No-op if the
 * frequency falls in the same filter's passband as the last call. */
void set_lpf_40mhz(int frequency);

/* Drives the external PTT line (EXT_PTT) high (on) or low (off). No
 * delay, no policy — see radio_set_tx() in radio.c for sequencing. */
void radio_hw_set_ptt(int on);

/* Drives the T/R relay control line (TX_LINE) high (on, transmit) or low
 * (off, receive). Same no-delay/no-policy contract as radio_hw_set_ptt(). */
void radio_hw_set_tx_relay(int on);

/* Reads the straight key (CW_KEY). Returns 1 if the key is down (pin
 * pulled low), 0 if up. No debounce - see cw.c, which polls this once
 * per audio block rather than trying to sample faster than that. */

int radio_hw_key_down(void);

/* Reads the INA260 power monitor's voltage (V) and current (A) registers
 * over I2C. On any I2C error, both outputs are set to 0.0. */
void read_voltage_current(float *voltage, float *current);

/* Writes the INA260's configuration register (continuous mode, default
 * averaging). Returns 0 on success, -1 on I2C failure. */
int radio_hw_ina260_configure(void);

#endif /* RADIO_HW_H */
