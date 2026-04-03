CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
LDFLAGS := -lm -lasound -lpthread -ldl
SRC := src/minibitx.c src/hpsdr_p1.c src/i2cbb.c src/si5351v2.c src/ma_sound.c src/vfo.c
OBJ := $(SRC:.c=.o)

all: minibitx

minibitx: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJ) minibitx
