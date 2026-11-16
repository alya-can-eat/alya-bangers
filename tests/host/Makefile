# PC tests: the ARM9 code compiled for the PC against a mock libnds, with a
# simulated ARM7. Needs a 32-bit capable gcc (pointers are 32-bit on the DS).
#   make test FILES="a.dsa b.dsa"      check streaming sample by sample
#   make screens CARD=/path/to/music   render screenshots into shots/

CC      ?= gcc
SRC     := ../../arm9/source
CFLAGS  := -m32 -std=gnu2x -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -fsanitize=address,undefined -Imock -I../../common/include -I$(SRC)
CORE    := hostsim.c $(SRC)/player.c $(SRC)/dsa.c $(SRC)/util.c

all: test_player screens

test_player: test_player.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^

screens: screens.c $(CORE) $(SRC)/font.c $(SRC)/gfx.c $(SRC)/browser.c $(SRC)/builtin_font.c $(SRC)/main.c
	$(CC) $(CFLAGS) -o $@ screens.c $(CORE) $(SRC)/font.c $(SRC)/gfx.c $(SRC)/browser.c $(SRC)/builtin_font.c

test: test_player
	./test_player $(FILES)

screens-run: screens
	mkdir -p shots && ./screens $(CARD) shots

clean:
	rm -rf test_player screens shots

.PHONY: all test screens-run clean
