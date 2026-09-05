# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
PREFIX ?= /usr
DESTDIR ?=
KDIR ?= /lib/modules/$(shell uname -r)/build
VERSION := 0.2.0

WARNINGS := -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wvla \
	-Werror=implicit-function-declaration
HARDEN_CFLAGS := -fstack-protector-strong -fPIE \
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
HARDEN_LDFLAGS := -pie -Wl,-z,relro,-z,now

.PHONY: all module install check deb clean
all: build/gb10-fan

build/gb10-fan: src/gb10-fan.c Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 $(WARNINGS) $(HARDEN_CFLAGS) \
		$(LDFLAGS) $(HARDEN_LDFLAGS) -o $@ $< $(LDLIBS) -ldl

module:
	$(MAKE) -C kernel KDIR="$(KDIR)"

install: all
	install -d "$(DESTDIR)$(PREFIX)/sbin" \
		"$(DESTDIR)$(PREFIX)/src/nvfancontrol-$(VERSION)" \
		"$(DESTDIR)$(PREFIX)/lib/systemd/system" \
		"$(DESTDIR)$(PREFIX)/share/doc/gb10-fan"
	install -m 0755 build/gb10-fan "$(DESTDIR)$(PREFIX)/sbin/gb10-fan"
	install -m 0644 kernel/nvfancontrol.c kernel/Makefile packaging/dkms.conf \
		"$(DESTDIR)$(PREFIX)/src/nvfancontrol-$(VERSION)/"
	install -m 0644 packaging/gb10-fan.service \
		"$(DESTDIR)$(PREFIX)/lib/systemd/system/gb10-fan.service"
	install -m 0644 packaging/gb10-fan-floor.service \
		"$(DESTDIR)$(PREFIX)/lib/systemd/system/gb10-fan-floor.service"
	install -m 0644 README.md "$(DESTDIR)$(PREFIX)/share/doc/gb10-fan/README.md"
	install -m 0644 LICENSE "$(DESTDIR)$(PREFIX)/share/doc/gb10-fan/copyright"

# Compilation and shell syntax only; never run the governor or load a module.
check: all
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 $(WARNINGS) $(HARDEN_CFLAGS) \
		-fsyntax-only src/gb10-fan.c
	sh -n packaging/build-deb.sh
	sh -n packaging/postinst
	sh -n packaging/prerm
	sh -n packaging/postrm
	bash -n packaging/dkms.conf

deb:
	sh packaging/build-deb.sh

clean:
	rm -rf -- build
