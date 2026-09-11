// radio_hw.c

#include <stdio.h>
#include <stdint.h>
#include "gpio.h"
#include "i2c.h"
#include "radio_hw.h"

/* ---- INA260 power monitor (I2C address 0x40) -------------------------- */

#define INA260_ADDRESS   0x40
#define CONFIG_REGISTER  0x00
#define VOLTAGE_REGISTER 0x02
#define CURRENT_REGISTER 0x01
#define CONFIG_DEFAULT   0x6127 // Continuous mode, default averaging

/* ---- Boot-time GPIO setup ----------------------------------------------
 *
 * Each pin gets its own line-request handle from gpio.c, held for the
 * life of the process (there's no radio_hw_gpio_shutdown() - same
 * "opened once, never explicitly torn down" convention i2c.c already
 * uses for its own fd). Every later function in this file writes/reads
 * through these handles rather than a raw pin number - unlike wiringPi,
 * the character-device API has no "just pass the pin number again"
 * shortcut once a line has been requested.
 */

static int line_tx_line  = -1;
static int line_tx_power = -1;
static int line_ext_ptt  = -1;
static int line_lpf_a    = -1;
static int line_lpf_b    = -1;
static int line_lpf_c    = -1;
static int line_lpf_d    = -1;
static int line_cw_key   = -1;

int radio_hw_gpio_init(void)
{
	// Outputs are driven to their idle/RX-safe value (LOW/0) as part of
	// the request itself - gpio.c's character-device requests set the
	// initial output value atomically with claiming the line, so there's
	// no separate "configure, then write LOW" step (and no window where
	// the pin briefly holds whatever the kernel's own power-on/pinctrl
	// default was) the way the old pinMode()-then-digitalWrite() sequence
	// had.
	line_tx_line  = gpio_request_output(TX_LINE,  0, "minibitx-tx_line");
	line_tx_power = gpio_request_output(TX_POWER, 0, "minibitx-tx_power");
	line_ext_ptt  = gpio_request_output(EXT_PTT,  0, "minibitx-ext_ptt");
	line_lpf_a    = gpio_request_output(LPF_A,    0, "minibitx-lpf_a");
	line_lpf_b    = gpio_request_output(LPF_B,    0, "minibitx-lpf_b");
	line_lpf_c    = gpio_request_output(LPF_C,    0, "minibitx-lpf_c");
	line_lpf_d    = gpio_request_output(LPF_D,    0, "minibitx-lpf_d");

	// idle high; key closes to ground - matches wiringPi's PUD_UP before.
	line_cw_key   = gpio_request_input(CW_KEY, 1, "minibitx-cw_key");

	if (line_tx_line < 0 || line_tx_power < 0 || line_ext_ptt < 0 ||
	    line_lpf_a < 0 || line_lpf_b < 0 || line_lpf_c < 0 ||
	    line_lpf_d < 0 || line_cw_key < 0) {
		// gpio_request_output()/gpio_request_input() already logged
		// which pin and why.
		return -1;
	}

	return 0;
}

/* ---- Board hardware revision -------------------------------------------- */

int radio_hw_detect_version(void)
{
	uint8_t response[4];
	if (i2c_read_i2c_block_data(0x8, 0, 4, response) == -1)
		return SBITX_DE;
	else
		return SBITX_V2;
}

/* ---- Low-pass filter band switching --------------------------------------
 *
 * prev_lpf now tracks the selected BCM pin number (0 meaning "none", for
 * a frequency at or above 30MHz that no band below covers) rather than a
 * wiringPi pin number - same sentinel convention as before, just in the
 * new numbering. Unlike the old digitalWrite(lpf, HIGH) - which could be
 * (and, for out-of-range frequencies, was) called with pin 0 as a
 * harmless no-op against wiringPi's own numbering - there's no line
 * handle behind BCM pin 0 here, so the out-of-range case is now handled
 * explicitly instead of relying on that incidental behavior.
 */

static int prev_lpf = -1;
void set_lpf_40mhz(int frequency)
{
	int lpf = 0;        // BCM pin number, for the log line - 0 = none selected
	int line = -1;      // matching line handle to drive high, if any

	if (frequency < 5500000) {
		lpf = LPF_D; line = line_lpf_d;
	} else if (frequency < 10500000) {
		lpf = LPF_C; line = line_lpf_c;
	} else if (frequency < 18500000) {
		lpf = LPF_B; line = line_lpf_b;
	} else if (frequency < 30000000) {
		lpf = LPF_A; line = line_lpf_a;
	}

	if (lpf == prev_lpf)
	{
		return;
	}

	gpio_write(line_lpf_a, 0);
	gpio_write(line_lpf_b, 0);
	gpio_write(line_lpf_c, 0);
	gpio_write(line_lpf_d, 0);

	if (line >= 0)
		gpio_write(line, 1);

	prev_lpf = lpf;
	printf("LPF: selected pin %d for %d Hz\n", lpf, frequency);
}

/* ---- T/R relay and external PTT ------------------------------------------ */

void radio_hw_set_ptt(int on)
{
	gpio_write(line_ext_ptt, on ? 1 : 0);
}

void radio_hw_set_tx_relay(int on)
{
	gpio_write(line_tx_line, on ? 1 : 0);
}

int radio_hw_key_down(void)
{
	return gpio_read(line_cw_key) == 0;
}

/* ---- INA260 power monitor ------------------------------------------------ */

void read_voltage_current(float *voltage, float *current)
{
	uint8_t data_buffer[2]; // Buffer to hold raw register data

	// Explicitly set the register pointer to the voltage register
	if (i2c_write_i2c_block_data(INA260_ADDRESS, VOLTAGE_REGISTER, 0, NULL) < 0)
	{
		printf("Error setting voltage register pointer\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}

	// Read the voltage register (2 bytes)
	int e = i2c_read_i2c_block_data(INA260_ADDRESS, VOLTAGE_REGISTER, 2, data_buffer);
	if (e != 2)
	{
		printf("Error reading voltage register\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}
	uint16_t raw_voltage = (data_buffer[0] << 8) | data_buffer[1];
	*voltage = raw_voltage * 1.25e-3f; // Convert to volts (1.25 mV per LSB)

	// Explicitly set the register pointer to the current register
	if (i2c_write_i2c_block_data(INA260_ADDRESS, CURRENT_REGISTER, 0, NULL) < 0)
	{
		printf("Error setting current register pointer\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}

	// Read the current register (2 bytes)
	e = i2c_read_i2c_block_data(INA260_ADDRESS, CURRENT_REGISTER, 2, data_buffer);
	if (e != 2)
	{
		printf("Error reading current register\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}
	uint16_t raw_current = (data_buffer[0] << 8) | data_buffer[1];

	// Handle saturation or invalid value
	if (raw_current == 0xFFFF)
	{
		printf("Current measurement out of range or invalid\n");
		*current = 0.0f;
	}
	else
	{
		*current = raw_current * 1.25e-3f; // Convert to amps (1.25 mA per LSB)
	}
}

int radio_hw_ina260_configure(void)
{
	uint8_t config_data[2] = {
		(uint8_t)(CONFIG_DEFAULT >> 8),  // MSB
		(uint8_t)(CONFIG_DEFAULT & 0xFF) // LSB
	};
	if (i2c_write_i2c_block_data(INA260_ADDRESS, CONFIG_REGISTER, 2, config_data) < 0)
		return -1;
	return 0;
}
