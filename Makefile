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
SRC = src/main.c src/util.c src/compat.c src/serial.c src/rig.c src/rigsrv.c src/web.c src/menu.c src/audio.c src/sstv.c
OBJ = $(SRC:.c=.o)

ft891ctl: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c src/ft891ctl.h src/compat.h src/net.h
	$(CC) $(CFLAGS) -c -o $@ $<

# A weboldal a programba épül be (.incbin), ezért ettől is függ
src/web.o: web/index.html
src/menu.o: src/menus.def

test: ft891ctl
	python3 tests/run_tests.py

# Windows-változat Linuxon fordítva: sudo apt install mingw-w64
# A hang és a HTTPS ebben a változatban még nincs benne.
WINCC ?= x86_64-w64-mingw32-gcc
windows: ft891ctl.exe
ft891ctl.exe: $(SRC) src/ft891ctl.h src/compat.h src/net.h src/menus.def web/index.html
	$(WINCC) -O2 -Wall -Wextra -std=gnu11 -D_WIN32_WINNT=0x0601 -D__USE_MINGW_ANSI_STDIO=1 \
		-o $@ $(SRC) -static -lpthread -lws2_32 -liphlpapi -lsetupapi -lbcrypt -lm

clean:
	rm -f src/*.o ft891ctl ft891ctl.exe

.PHONY: test clean windows
