# PC tests: the ARM9 code compiled for the PC against a mock libnds, with a
# simulated ARM7. Needs a 32-bit capable gcc (pointers are 32-bit on the DS).
#   make test FILES="a.dsa b.dsa"      check streaming sample by sample
#   make screens-run CARD=/path/to/music   click through the UI, check it, screenshots in shots/

CC      ?= gcc
SRC     := ../../arm9/source
CFLAGS  := -m32 -std=gnu2x -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -fsanitize=address,undefined -Imock -I../../common/include -I$(SRC)
CORE    := hostsim.c $(SRC)/player.c $(SRC)/eq.c $(SRC)/dsa.c $(SRC)/util.c $(SRC)/i18n.c
HDRS    := $(wildcard $(SRC)/*.h ../../common/include/*.h mock/*.h mock/nds/*.h) hostsim.h

all: test_player test_eq screens

test_eq: test_eq.c $(SRC)/eq.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_eq.c $(SRC)/eq.c -lm

test_player: test_player.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_player.c $(CORE)

APP     := $(SRC)/search.c $(SRC)/keyboard.c $(SRC)/font.c $(SRC)/gfx.c $(SRC)/builtin_font.c $(SRC)/library.c $(SRC)/queue.c \
           $(SRC)/colorpick.c $(SRC)/palette.c $(SRC)/state.c $(SRC)/stats.c $(SRC)/eqprof.c $(SRC)/ui.c $(SRC)/viz.c $(SRC)/default_cover.c

screens: screens.c $(CORE) $(APP) $(SRC)/main.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ screens.c $(CORE) $(APP) -lm

test: test_player
	./test_player $(FILES)

screens-run: screens
	mkdir -p shots && ./screens $(CARD) shots

clean:
	rm -rf test_player test_eq screens shots

.PHONY: all test screens-run clean
