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

# parsec-nat-follow runs as root (system service). Use with sudo.
install-nat-follow:
	install -Dm755 parsec-nat-follow $(DESTDIR)/usr/local/bin/parsec-nat-follow
	install -Dm644 parsec-nat-follow.service $(DESTDIR)/etc/systemd/system/parsec-nat-follow.service

uninstall-nat-follow:
	rm -f $(DESTDIR)/usr/local/bin/parsec-nat-follow $(DESTDIR)/etc/systemd/system/parsec-nat-follow.service

clean:
	rm -f evdev_coalesce.so

.PHONY: all install uninstall install-nat-follow uninstall-nat-follow clean
