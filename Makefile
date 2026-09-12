CC      := gcc
# -march=native detects this machine's actual CPU (NEON on the Pi) at
# build time - minibitx should be built directly on the Pi
# it'll run on, but a binary built this way
# shouldn't be copied to a different Pi model; drop -march=native if
# that's ever needed. It's what lets antialias.c's branch-free FIR loop
# (see antialias.c) actually vectorize instead of just being eligible to.
CFLAGS  := -O3 -march=native -Wall -Wextra -std=gnu11
LDFLAGS := -lm -lasound -lpthread -ldl
SRC := src/minibitx.c src/radio.c src/radio_hw.c src/hpsdr_p1.c src/usb_gadget.c src/i2c.c \
     src/si5351v2.c src/sound.c src/vfo.c src/hamlib.c src/hw_settings.c src/antialias.c src/decim48k.c src/cw.c \
     src/gpio.c
OBJ := $(SRC:.c=.o)

all: minibitx

minibitx: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)
	# Grant capabilities so minibitx doesn't need to run as root:
	#  - cap_sys_nice:      lets the audio thread get SCHED_FIFO
	#  - cap_dac_override:  bypasses the normal file-permission check so
	#    usb_gadget.c can mkdir/write under the root-owned
	#    /sys/kernel/config/usb_gadget/ configfs tree
	-sudo setcap cap_sys_nice,cap_dac_override+ep $@

clean:
	rm -f $(OBJ) minibitx
