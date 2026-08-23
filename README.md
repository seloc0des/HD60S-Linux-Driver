# elgato-hd60s — Linux driver for the Elgato Game Capture HD60 S

Userspace-first Linux support for the **original** Elgato Game Capture HD60 S
(`0fd9:005e`). Not affiliated with or supported by Elgato.

> **Status: protocol recovered, capture not yet implemented.**
>
> The register protocol and the MCU-presence handshake have been recovered from
> Elgato's own Windows driver (which ships with debug symbols) and **verified on
> real hardware** — see [docs/PROTOCOL.md](docs/PROTOCOL.md) §5.9 and
> [docs/VENDOR-DRIVER.md](docs/VENDOR-DRIVER.md).
>
> What works: register read/write, the handshake, and the whole output pipeline.
> What does not: starting a video stream. Every fact is tagged by how it is
> known; nothing here is invented.

The HD60 S+ is different hardware and is **not** covered.

## Why a driver is needed at all

The device reports `bDeviceClass = 255` (vendor specific) on every interface.
It is not UVC, so no in-tree kernel driver can bind it and it will never just
show up as a webcam.

## Quick start

```bash
# 1. Is the link healthy? Start here -- most problems are a USB 2.0 cable
./tools/hd60s-linkcheck.py

# 2. Descriptor dump -- no dependencies, no root
./tools/hd60s-descdump.py

# 3. Watch for (re)enumeration and snapshot each one
./tools/hd60s-watch.sh

# 4. Build the RE probe
sudo apt install libusb-1.0-0-dev     # Fedora: libusb1-devel  Arch: libusb
make

# 5. Device access without sudo, then replug
sudo make install-udev

# 6. Listen to what the device volunteers on its interrupt endpoint
./hd60s-probe --seconds 30

# 7. Find which EP0 vendor requests are implemented (read-only, safe)
./hd60s-probe --sweep --seconds 0

# 8. Register access + the MCU handshake (the interesting part)
./hd60s-reg                  # scan registers, reads only
./hd60s-reg --handshake      # run the handshake, then rescan
./hd60s-reg --reg 0x39       # read one register
```

### The register protocol

Recovered from the vendor driver, verified on hardware:

| Op | bmRequestType | bRequest | wValue | Data |
|---|---|---|---|---|
| read | `0xC0` | `0xC1` | `reg` | 1 byte IN |
| write | `0x40` | `0xC1` | `(value << 8) \| reg` | none |

The write puts its value in the **high byte of `wValue`** with no data stage,
which is why blind sweeps never found it. Before the handshake only register
`0x34` responds at all.

## USB 3.0 is required

The HD60 S must be on a **SuperSpeed** link. On a USB 2.0 link it falls back to
a reduced personality whose isochronous endpoint tops out at 3072 B/microframe
= **197 Mbit/s**, against roughly 2000 Mbit/s for uncompressed 1080p60. The
bulk capture endpoints only exist at 5 Gbit/s.

The device confirms this itself: its BOS descriptor reports
`bFunctionalitySupport = SuperSpeed`, meaning full functionality is
unavailable on a slower link. `hd60s-linkcheck.py` decodes that and warns
loudly when the link is degraded.

### If linkcheck says the SuperSpeed half is "not attached"

That means the receptacle is wired for USB 3 and the USB 2 pairs are making
contact, but the SuperSpeed pairs are not. Work through these in order — they
are sorted by effort, not likelihood:

1. **Flip the USB-C plug 180° at the device end.** USB-C carries two
   SuperSpeed pair sets (TX1/RX1 and TX2/RX2); a C-to-A cable wires one, and
   the device muxes to whichever the orientation selects. If one set of pins in
   the device receptacle is bent, worn or dirty, flipping uses the other set.
   Costs five seconds and fixes a genuinely common failure.
2. **Move to a port that is holding a SuperSpeed link right now.** linkcheck
   lists them. A port proven good this second beats a port labelled SS.
3. **Put a known-good USB 3 device on the same cable and port.** If it trains
   at 5000M, cable and port are cleared and the capture device's SuperSpeed
   side is the fault.

Note that a cable being sold as "USB 3.0", or having a coloured insert, says
nothing about whether all four SuperSpeed conductors actually make contact.
`not attached` is a measurement; the label is a claim.

## The capture daemon

`hd60sd` carries the output half of the pipeline. It is finished and tested;
the *input* half does not exist yet, because the vendor command set that starts
a stream is undocumented.

```bash
# synthetic colour bars into a file, then check with ffmpeg
./hd60sd --source test --frames 90 --sink raw:/tmp/test.yuv
ffmpeg -f rawvideo -pix_fmt yuyv422 -s 1280x720 -i /tmp/test.yuv -frames:v 1 /tmp/f.png

# straight into a v4l2loopback node, so OBS/ffmpeg/browsers see a camera
sudo modprobe v4l2loopback
./hd60sd --source test --sink v4l2:/dev/video0

# --source device is rejected on purpose until the protocol is known
./hd60sd --source device
```

Building it this way means that when the protocol does land, only the USB side
is unproven -- the frame plumbing, pixel format and v4l2 path are already
known good.

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
