# Architecture

## Why userspace first

The HD60 S speaks an undocumented vendor protocol. Reverse-engineering it means
a lot of "send this, see what comes back" iteration, and doing that inside the
kernel means a malformed URB takes the whole machine down. So the protocol gets
discovered and stabilised in userspace with libusb, and only then ported to a
kernel V4L2 driver.

That ordering also means there's something usable at every stage rather than
nothing until the very end.

```
            ┌────────────────────────────────────────────┐
  stage 1   │ hd60s-descdump.py   sysfs, zero deps       │  DONE
            │ hd60s-watch.sh      catch enumerations     │  DONE
            └────────────────────────────────────────────┘
                              │
            ┌────────────────────────────────────────────┐
  stage 2   │ hd60s-probe        read-only EP0 + irq ep  │  WRITTEN,
            │                    → fills docs/PROTOCOL   │  needs libusb
            └────────────────────────────────────────────┘
                              │  (protocol known)
            ┌────────────────────────────────────────────┐
  stage 3   │ hd60sd             libusb capture daemon   │  BLOCKED on
            │   proto.c  state machine                   │  stage 2
            │   stream.c URB ring + frame reassembly     │
            │   out: v4l2loopback / raw fd / stdout      │
            └────────────────────────────────────────────┘
                              │  (streaming stable)
            ┌────────────────────────────────────────────┐
  stage 4   │ kernel/hd60s.ko    native V4L2 driver      │  FUTURE
            │   usb probe + vb2 queue + URB streaming    │
            └────────────────────────────────────────────┘
```

## Module split

| File | Responsibility |
|---|---|
| `src/hd60s.h` | IDs, endpoint constants, logging. No guessed protocol values. |
| `src/util.c` | hexdump, log level |
| `tools/hd60s-descdump.py` | sysfs descriptor decode; no deps, works unprivileged |
| `tools/hd60s-watch.sh` | polls for enumeration, snapshots descriptors + BOS |
| `tools/hd60s-probe.c` | read-only RE probe (see safety note below) |
| `src/proto.c` | *pending* — vendor command state machine |
| `src/stream.c` | *pending* — transfer ring, frame reassembly |

The transport layer is kept behind a narrow interface on purpose: the USB 2.0
personality uses isochronous transfers on EP 0x83 while SuperSpeed is expected
to use bulk, and the frame-reassembly code above it should not care which.

## Safety rules

1. **No host-to-device vendor requests until a capture proves them.** The
   CYUSB3014 boots from unprotected SPI flash. A wrong vendor write can leave
   the device unbootable, and there is no recovery path short of an SPI
   programmer on the pads.
2. **No invented constants.** If a `bRequest`/`wValue` is not in a real trace,
   it doesn't go in the source. `docs/PROTOCOL.md` marks every fact with how
   it's known.
3. **Read-only probing is fine.** Device-to-host EP0 requests either return
   data or stall; neither changes device state.

## Output path (stage 3)

`v4l2loopback` is the pragmatic target: the daemon writes frames into
`/dev/videoN` and OBS, ffmpeg, browsers and everything else just see a camera.
It costs one memcpy per frame versus a native kernel driver, which is
irrelevant next to not having a driver at all. Stage 4 removes it.
