PREFIX ?= $(HOME)/.local
CFLAGS ?= -O2 -Wall -Wextra

all: evdev_coalesce.so

evdev_coalesce.so: evdev_coalesce.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl -lpthread

install: evdev_coalesce.so
	install -Dm755 parsec-steam-launch $(PREFIX)/bin/parsec-steam-launch
	install -Dm755 parsec-pad-proxy $(PREFIX)/bin/parsec-pad-proxy
	install -Dm755 evdev_coalesce.so $(PREFIX)/lib/parsec-evdev-fix/evdev_coalesce.so

uninstall:
	rm -f $(PREFIX)/bin/parsec-steam-launch $(PREFIX)/bin/parsec-pad-proxy
	rm -rf $(PREFIX)/lib/parsec-evdev-fix

clean:
	rm -f evdev_coalesce.so

.PHONY: all install uninstall clean
