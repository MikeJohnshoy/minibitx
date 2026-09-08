CC      := gcc
# -march=native detects this machine's actual CPU (NEON on the Pi) at
# build time - minibitx should be built directly on the Pi
# it'll run on, but a binary built this way
# shouldn't be copied to a different Pi model; drop -march=native if
# that's ever needed. It's what lets antialias.c's branch-free FIR loop
# (see antialias.c) actually vectorize instead of just being eligible to.
CFLAGS  := -O3 -march=native -Wall -Wextra -std=gnu11
LDFLAGS := -lm -lasound -lpthread -ldl -lwiringPi
SRC := src/minibitx.c src/radio.c src/radio_hw.c src/hpsdr_p1.c src/usb_gadget.c src/status.c src/i2c.c \
     src/si5351v2.c src/sound.c src/vfo.c src/hamlib.c src/hw_settings.c src/antialias.c src/decim48k.c src/cw.c
OBJ := $(SRC:.c=.o)

all: minibitx

minibitx: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)
	# Grant two capabilities so minibitx doesn't need to run as root:
	#  - cap_sys_nice:      lets the audio thread get SCHED_FIFO (see
	#    sound.c's sound_thread_start()).
	#  - cap_dac_override:  bypasses the normal file-permission check so
	#    usb_gadget.c can mkdir/write under the root-owned
	#    /sys/kernel/config/usb_gadget/ configfs tree (see
	#    docs/usb_gadget_os_setup.md) - without this, running as an
	#    ordinary user gets "Permission denied" creating the gadget root
	#    even though the directory itself exists and dwc2/libcomposite
	#    are loaded correctly.
	# A fresh binary has neither capability - setcap has to be reapplied
	# after every relink, since it's a filesystem attribute on this
	# specific binary, not something that carries over. Leading '-' so a
	# missing/misconfigured sudo (e.g. no libcap2-bin, or a
	# non-interactive build) prints a warning and moves on instead of
	# failing the whole build - you'll just see the SCHED_FIFO fallback
	# warning and/or the gadget "Permission denied" again at runtime if
	# this line didn't actually take effect.
	-sudo setcap cap_sys_nice,cap_dac_override+ep $@

clean:
	rm -f $(OBJ) minibitx
