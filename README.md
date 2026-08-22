# elgato-hd60s — Linux driver for the Elgato Game Capture HD60 S

Userspace-first Linux support for the **original** Elgato Game Capture HD60 S
(`0fd9:005e`). Not affiliated with or supported by Elgato.

> **Status: reverse engineering.** Descriptor tooling works today. There is no
> working capture path yet, because the vendor protocol is undocumented and
> hasn't been captured. See [docs/PROTOCOL.md](docs/PROTOCOL.md) for exactly
> what is known versus guessed — nothing here is invented.

The HD60 S+ is different hardware and is **not** covered.

## Why a driver is needed at all

The device reports `bDeviceClass = 255` (vendor specific) on every interface.
It is not UVC, so no in-tree kernel driver can bind it and it will never just
show up as a webcam.

## Quick start

```bash
# 1. Descriptor dump -- no dependencies, no root
./tools/hd60s-descdump.py

# 2. Watch for (re)enumeration and snapshot each one
./tools/hd60s-watch.sh

# 3. Build the RE probe
sudo apt install libusb-1.0-0-dev     # Fedora: libusb1-devel  Arch: libusb
make

# 4. Device access without sudo, then replug
sudo make install-udev

# 5. Listen to what the device volunteers on its interrupt endpoint
./hd60s-probe --seconds 30

# 6. Find which EP0 vendor requests are implemented (read-only, safe)
./hd60s-probe --sweep --seconds 0
```

## USB 3.0 is required

The HD60 S must be on a **SuperSpeed** link. On a USB 2.0 link it falls back to
a reduced personality whose isochronous endpoint tops out at 3072 B/microframe
= **197 Mbit/s**, against roughly 2000 Mbit/s for uncompressed 1080p60. The
bulk capture endpoints only exist at 5 Gbit/s.

`hd60s-descdump.py` warns loudly when the link is degraded. If it says
`NOT SuperSpeed`, fix the cable or port before anything else — most USB-C
cables sold are USB 2.0 only, and some are charge-only with no data pairs at
all. Use the cable that shipped with the device, into a SuperSpeed port.

## Safety

The device boots from unprotected SPI flash, so a stray vendor write can brick
it. `hd60s-probe` is **strictly read-only** — every EP0 transfer it issues is
device-to-host. Keep it that way.

## Layout

```
docs/     protocol + hardware notes, captured enumerations
src/      shared code and (pending) daemon
tools/    descriptor dump, enumeration watcher, RE probe
udev/     device access rule
kernel/   future native V4L2 module
```

## Contributing a protocol capture

The fastest way to unblock this is a `usbmon` trace of the official driver
driving the device. If you can capture one, open an issue with the raw trace —
that is the missing piece, not more code.

## License

GPL-2.0-only, so the protocol work can be reused by a future in-tree driver.
