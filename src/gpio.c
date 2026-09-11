// gpio.c - see gpio.h for scope and rationale.

#include "gpio.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// BCM2711 (Pi 4) exposes every header GPIO through this one chip - unlike
// some other SoCs/boards, there's no need to search /dev/gpiochip* for
// the right one here.
#define GPIO_CHIP_PATH "/dev/gpiochip0"

// Shared by gpio_request_output()/gpio_request_input(): opens the chip,
// issues a single-line GPIO_V2_GET_LINE_IOCTL request with the given
// flags (and, for outputs, an initial value attribute), and returns the
// resulting line request fd. The chip fd itself is only needed for the
// duration of this ioctl - once the kernel hands back a line request fd,
// that fd alone holds the line reserved, so the chip fd is closed before
// returning rather than leaked/kept open for no reason.
static int gpio_request_line(unsigned int bcm_gpio, uint64_t flags, int has_output_value,
                             int output_value, const char *consumer_label) {
  int chip_fd = open(GPIO_CHIP_PATH, O_RDWR | O_CLOEXEC);
  if (chip_fd < 0) {
    fprintf(stderr, "gpio: cannot open %s: %s\n", GPIO_CHIP_PATH, strerror(errno));
    return -1;
  }

  struct gpio_v2_line_request req;
  memset(&req, 0, sizeof(req));
  req.num_lines = 1;
  req.offsets[0] = bcm_gpio;
  strncpy(req.consumer, consumer_label, sizeof(req.consumer) - 1);
  req.config.flags = flags;

  if (has_output_value) {
    req.config.num_attrs = 1;
    req.config.attrs[0].mask = 1; // applies to offsets[0], the only requested line
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = output_value ? 1 : 0;
  }

  int ret = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
  int saved_errno = errno;
  close(chip_fd);
  if (ret < 0) {
    fprintf(stderr, "gpio: cannot request BCM%u ('%s'): %s\n", bcm_gpio, consumer_label,
            strerror(saved_errno));
    return -1;
  }

  return req.fd;
}

int gpio_request_output(unsigned int bcm_gpio, int initial_value, const char *consumer_label) {
  return gpio_request_line(bcm_gpio, GPIO_V2_LINE_FLAG_OUTPUT, 1, initial_value, consumer_label);
}

int gpio_request_input(unsigned int bcm_gpio, int pull_up, const char *consumer_label) {
  uint64_t flags = GPIO_V2_LINE_FLAG_INPUT;
  if (pull_up)
    flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
  return gpio_request_line(bcm_gpio, flags, 0, 0, consumer_label);
}

int gpio_write(int line, int value) {
  if (line < 0)
    return -1;

  struct gpio_v2_line_values vals;
  vals.mask = 1;
  vals.bits = value ? 1 : 0;
  if (ioctl(line, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
    fprintf(stderr, "gpio: write failed on line fd %d: %s\n", line, strerror(errno));
    return -1;
  }
  return 0;
}

int gpio_read(int line) {
  if (line < 0)
    return -1;

  struct gpio_v2_line_values vals;
  vals.mask = 1;
  if (ioctl(line, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
    fprintf(stderr, "gpio: read failed on line fd %d: %s\n", line, strerror(errno));
    return -1;
  }
  return (int)(vals.bits & 1);
}
