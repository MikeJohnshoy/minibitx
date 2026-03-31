CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
LDFLAGS := -lm -lfftw3 -lasound -lpthread
SRC := src/ittibitx.c src/hpsdr_p1.c src/i2cbb.c src/si5351v2.c src/sbitx_sound.c src/vfo.c
OBJ := $(SRC:.c=.o)

all: ittibitx

ittibitx: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJ) ittibitx