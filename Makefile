CC      := gcc
# -march=native detects this machine's actual CPU (NEON on the Pi) at
# build time - minibitx should be built directly on the Pi
# it'll run on, but a binary built this way
# shouldn't be copied to a different Pi model; drop -march=native if
# that's ever needed. It's what lets antialias.c's branch-free FIR loop
# (see antialias.c) actually vectorize instead of just being eligible to.
# -Isrc/interfaces (alongside the implicit "search the including file's
# own directory" gcc already does for quoted #includes) is what lets
# src/interfaces/*.c keep saying #include "cw.h"/"rx_audio.h" etc.
# unqualified even though those headers live one directory up in plain
# src/ - and lets sound.c/minibitx.c (in src/) keep saying
# #include "hpsdr_p1.h" etc. unqualified even though those headers moved
# into src/interfaces/. Added when hpsdr_p1.c/usb_gadget.c/iq_stream.c/
# hamlib.c (the four modules that talk to an external SDR app - HPSDR
# Protocol 1, the UAC2/CAT USB gadget, the lightweight I/Q telemetry
# stream, and the rigctld server) moved out of plain src/ into their own
# subfolder, to keep them visually separate from the DSP/radio-control
# core (sound.c, rx_audio.c, radio.c, cw.c, vfo.c, ...) - see
# docs/04_remote_control_and_iq_output.md's intro. Not a real
# architectural boundary (nothing stops a src/interfaces file from
# including a core header or vice versa, same as before) - just where
# the file sits.
CFLAGS  := -O3 -march=native -Wall -Wextra -std=gnu11 -Isrc -Isrc/interfaces
LDFLAGS := -lm -lasound -lpthread -ldl
SRC := src/minibitx.c src/radio.c src/radio_hw.c src/interfaces/hpsdr_p1.c src/interfaces/usb_gadget.c src/i2c.c \
     src/si5351v2.c src/sound.c src/vfo.c src/interfaces/hamlib.c src/hw_settings.c src/antialias.c src/decim48k.c src/cw.c \
     src/rx_audio.c src/gpio.c src/interfaces/iq_stream.c
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
