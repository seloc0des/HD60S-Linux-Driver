# Elgato HD60 S Linux driver -- userspace tools and daemon.
#
# Needs libusb-1.0 headers:
#   Debian/Ubuntu : sudo apt install libusb-1.0-0-dev
#   Fedora        : sudo dnf install libusb1-devel
#   Arch          : sudo pacman -S libusb

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -Isrc
LDFLAGS +=

LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0 2>/dev/null)

BINDIR  ?= /usr/local/bin
UDEVDIR ?= /etc/udev/rules.d

COMMON  := src/util.o
PROBE   := hd60s-probe
DUMP    := hd60s-dumpram
DAEMON  := hd60sd
WRTEST  := hd60s-writetest
ISOTEST := hd60s-isotest
OSDESC  := hd60s-osdesc
REGPROBE:= hd60s-regprobe
REG     := hd60s-reg

.PHONY: all clean install uninstall install-udev check-deps

all: check-deps $(PROBE) $(DUMP) $(DAEMON) $(WRTEST) $(ISOTEST) $(OSDESC) $(REGPROBE) $(REG)

check-deps:
	@pkg-config --exists libusb-1.0 || { \
	  echo "ERROR: libusb-1.0 development headers not found."; \
	  echo "  Debian/Ubuntu: sudo apt install libusb-1.0-0-dev"; \
	  echo "  Fedora:        sudo dnf install libusb1-devel"; \
	  echo "  Arch:          sudo pacman -S libusb"; \
	  exit 1; }

%.o: %.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -c $< -o $@

$(PROBE): tools/hd60s-probe.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

$(DUMP): tools/hd60s-dumpram.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

$(WRTEST): tools/hd60s-writetest.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

$(ISOTEST): tools/hd60s-isotest.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

$(OSDESC): tools/hd60s-osdesc.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

$(REGPROBE): tools/hd60s-regprobe.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

$(REG): tools/hd60s-reg.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

$(DAEMON): src/main.o src/sink.o src/testsrc.o src/statusframe.o src/device.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBUSB_LIBS)

# udev rule lets the logged-in desktop user talk to the device without sudo.
install-udev:
	install -m 0644 udev/99-elgato-hd60s.rules $(UDEVDIR)/99-elgato-hd60s.rules
	udevadm control --reload-rules && udevadm trigger
	@echo "Rule installed. Replug the HD60 S for it to take effect."

install: all install-udev
	install -m 0755 $(PROBE) $(BINDIR)/$(PROBE)
	install -m 0755 $(DUMP) $(BINDIR)/$(DUMP)
	install -m 0755 $(DAEMON) $(BINDIR)/$(DAEMON)
	install -m 0755 tools/hd60s-descdump.py $(BINDIR)/hd60s-descdump
	install -m 0755 tools/hd60s-watch.sh $(BINDIR)/hd60s-watch
	install -m 0755 tools/hd60s-linkcheck.py $(BINDIR)/hd60s-linkcheck

uninstall:
	rm -f $(BINDIR)/$(PROBE) $(BINDIR)/hd60s-descdump $(BINDIR)/hd60s-watch
	rm -f $(BINDIR)/hd60s-linkcheck
	rm -f $(UDEVDIR)/99-elgato-hd60s.rules
	udevadm control --reload-rules

clean:
	rm -f $(PROBE) $(DUMP) $(DAEMON) $(WRTEST) $(ISOTEST) $(OSDESC) $(REGPROBE) $(REG) src/*.o tools/*.o
