# FT-891 vezérlő - csak gcc, make és pthread kell hozzá
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wshadow -std=gnu11 -D_GNU_SOURCE
LDFLAGS ?= -pthread -lm
# A HTTPS-hez kell, nélküle a telefon mikrofonja nem érhető el: sudo apt install libssl-dev
HAVE_SSL := $(shell printf '\043include <openssl/ssl.h>\n' | $(CC) -E -x c - >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_SSL),1)
CFLAGS  += -DHAVE_SSL
LDFLAGS += -lssl -lcrypto
endif
SRC = src/main.c src/util.c src/rig.c src/rigsrv.c src/web.c src/menu.c src/audio.c src/sstv.c
OBJ = $(SRC:.c=.o)

ft891ctl: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c src/ft891ctl.h
	$(CC) $(CFLAGS) -c -o $@ $<

# A weboldal a programba épül be (.incbin), ezért ettől is függ
src/web.o: web/index.html
src/menu.o: src/menus.def

test: ft891ctl
	python3 tests/run_tests.py

clean:
	rm -f src/*.o ft891ctl

.PHONY: test clean
