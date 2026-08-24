# elgato-hd60s - Linux driver for the Elgato Game Capture HD60 S

Userspace Linux support for the **original** Elgato Game Capture HD60 S
(`0fd9:005e`). Video and audio, straight into OBS. Not affiliated with or
supported by Elgato.

> **Status: working. 1920x1080 YUY2 at 60 fps with stereo audio.**
>
> There is no Linux driver for this device and it is not UVC, so it will never
> just show up as a webcam. The protocol here was recovered from Elgato's
> Windows driver (which ships with debug symbols), the macOS `libUB530.dylib`,
> the device's own MCU firmware, and USB captures of the vendor software, then
> verified on real hardware. Every fact in the docs is tagged by how it is
> known; nothing is invented.

The HD60 S+ is different hardware and is **not** covered.

## Requirements

- A **SuperSpeed (USB 3.x) link**. The device's USB 2.0 personality has no
  bulk endpoint at all, so a USB 2 cable or port physically cannot carry
  video. `tools/hd60s-linkcheck.py` will tell you which you have.
- `libusb`, `v4l2loopback` and a PipeWire or PulseAudio desktop:

```bash
sudo apt install libusb-1.0-0-dev v4l2loopback-dkms v4l-utils ffmpeg
make
sudo make install-udev   # device access without sudo; replug after
```

## Use it with OBS

```bash
./scripts/obs-start.sh
```

Then in OBS:

1. **Sources -> + -> Video Capture Device (V4L2)** and pick `Elgato HD60 S`.
   In the source's properties, **untick "Use Buffering"** unless you like
   latency. Restart OBS first if it was already open, so it rescans.
2. **Sources -> + -> Audio Output Capture** and pick `Elgato_HD60_S_Audio`.
   (It will not appear under Audio *Input* Capture; that list is microphones
   only.)
3. To hear the audio live, set that source's **Audio Monitoring** to
   "Monitor and Output" in Edit -> Advanced Audio Properties.

If there is no signal you get a colour-coded status card naming what the
receiver actually sees (red for no signal, amber for locked but no data,
green once bytes flow) instead of a black screen.

### Playing off the preview

The OBS preview adds a couple of frames of latency. If you play off the
monitor rather than a passthrough display, a low-latency viewer is snappier
than the OBS preview, and both can read the loopback at once:

```bash
mpv --profile=low-latency --untimed av://v4l2:/dev/video2
```

Play in the mpv window, let OBS do the recording.

### Audio notes

The device delivers 16-bit stereo PCM interleaved in the capture stream at an
odd ~71.6 kHz, and the exact rate drifts a little between sessions.
`obs-start.sh` plays it into a PipeWire null sink through ffmpeg's async
resampler, which tracks the real arrival rate, so pitch and sync hold without
tuning. If ffmpeg is missing it falls back to a fixed-rate `pacat` feed.

## USB 3.0 is required

The HD60 S must be on a **SuperSpeed** link. On a USB 2.0 link it falls back
to a reduced personality whose isochronous endpoint tops out at 3072
B/microframe = **197 Mbit/s**, against roughly 2000 Mbit/s for uncompressed
1080p60. The bulk capture endpoints only exist at 5 Gbit/s.

The device confirms this itself: its BOS descriptor reports
`bFunctionalitySupport = SuperSpeed`, meaning full functionality is
unavailable on a slower link. `hd60s-linkcheck.py` decodes that and warns
loudly when the link is degraded.

### If linkcheck says the SuperSpeed half is "not attached"

That means the receptacle is wired for USB 3 and the USB 2 pairs are making
contact, but the SuperSpeed pairs are not. Work through these in order; they
are sorted by effort, not likelihood:

1. **Flip the USB-C plug 180° at the device end.** USB-C carries two
   SuperSpeed pair sets (TX1/RX1 and TX2/RX2); a C-to-A cable wires one, and
   the device muxes to whichever the orientation selects. If one set of pins
   in the device receptacle is bent, worn or dirty, flipping uses the other
   set. Costs five seconds and fixes a genuinely common failure.
2. **Move to a port that is holding a SuperSpeed link right now.** linkcheck
   lists them. A port proven good this second beats a port labelled SS.
3. **Put a known-good USB 3 device on the same cable and port.** If it trains
   at 5000M, cable and port are cleared and the capture device's SuperSpeed
   side is the fault.

Note that a cable being sold as "USB 3.0", or having a coloured insert, says
nothing about whether all four SuperSpeed conductors actually make contact.
`not attached` is a measurement; the label is a claim.

## How it works

`hd60sd` is a libusb daemon. It runs the MCU handshake, brings up the HDMI
receiver with a text bring-up script (`scripts/stream-cold.hd60s`), enables
audio (`scripts/audio-block.hd60s`), then streams the bulk endpoint. Video
lines and audio records are interleaved with per-record markers; the framer
demuxes them, frames go to a v4l2loopback node paced by the device's own
clock, and PCM goes down a FIFO to the PipeWire sink. Audio writes are
non-blocking so a stalled consumer can never stall video.

The full protocol, including how each fact was established, is in
[docs/PROTOCOL.md](docs/PROTOCOL.md). The reverse-engineering trail, wrong
turns included, is in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).

### The register protocol

Recovered from the vendor driver, verified on hardware:

| Op | bmRequestType | bRequest | wValue | Data |
|---|---|---|---|---|
| read | `0xC0` | `0xC1` | `reg` | 1 byte IN |
| write | `0x40` | `0xC1` | `(value << 8) \| reg` | none |

The write puts its value in the **high byte of `wValue`** with no data stage,
which is why blind sweeps never found it. Before the handshake only register
`0x34` responds at all.

## Poking at the hardware yourself

```bash
# 1. Is the link healthy? Start here; most problems are a USB 2.0 cable
./tools/hd60s-linkcheck.py

# 2. Descriptor dump: no dependencies, no root
./tools/hd60s-descdump.py

# 3. Watch for (re)enumeration and snapshot each one
./tools/hd60s-watch.sh

# 4. Listen to what the device volunteers on its interrupt endpoint
./hd60s-probe --seconds 30

# 5. Find which EP0 vendor requests are implemented (read-only, safe)
./hd60s-probe --sweep --seconds 0

# 6. Register access + the MCU handshake (the interesting part)
./hd60s-reg                  # scan registers, reads only
./hd60s-reg --handshake      # run the handshake, then rescan
./hd60s-reg --reg 0x39       # read one register

# 7. Signal diagnosis in plain language
./hd60sd --source device --script scripts/probe.hd60s --sink raw:/dev/null --seconds 1 --diagnose
```

Bring-up sequences live in text files (`scripts/*.hd60s`), not in the binary,
so iterating on the init never means a rebuild:

```
fpga_write REG VAL                     FPGA/CPLD register file (bRequest 0xC1)
fpga_read  REG
i2c_write  SLAVE REG VAL               via the MCU proxy
i2c_read   SLAVE REG
mst_write  PAGE REG VAL                MST3367 receiver, page-selected
mst_read   PAGE REG
delay      MS
poll_mst   PAGE REG MASK WANT [TRIES]
expect_mst PAGE REG MASK WANT
```

## The synthetic path

The output half is testable without hardware:

```bash
# synthetic colour bars into a file, then check with ffmpeg
./hd60sd --source test --frames 90 --sink raw:/tmp/test.yuv
ffmpeg -f rawvideo -pix_fmt yuyv422 -s 1280x720 -i /tmp/test.yuv -frames:v 1 /tmp/f.png

# straight into a v4l2loopback node, so OBS/ffmpeg/browsers see a camera
sudo modprobe v4l2loopback
./hd60sd --source test --sink v4l2:/dev/video0
```

## Safety

The device boots from unprotected SPI flash, so a stray vendor write can
brick it. `hd60s-probe` is **strictly read-only**: every EP0 transfer it
issues is device-to-host. Keep it that way. Do not write to the MCU or SPI
flash, and read [docs/PROTOCOL.md](docs/PROTOCOL.md) section 10 before
sending anything the scripts don't already send.

## Layout

```
src/        the daemon (device, framer, sinks)
scripts/    bring-up scripts + obs-start.sh
tools/      link/descriptor diagnostics (python, no build needed)
docs/       protocol, architecture, experiment log
udev/       device permissions rule
kernel/     notes toward an eventual kernel driver
```

## License and credits

GPL-3.0, see [LICENSE](LICENSE).

Reverse engineered and written by Sean Elovirta
([seloc0des](https://github.com/seloc0des)).

The symbol map used during reverse engineering is not shipped; regenerate it
from Elgato's own driver package with `tools/pdbsyms.py` if you need it.

Elgato and Game Capture are trademarks of their respective owners; this
project is independent interoperability work.
