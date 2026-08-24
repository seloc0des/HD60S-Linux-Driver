# Reverse-engineering experiment log

Run these in order. Record the result of each in `docs/PROTOCOL.md` with an
`[OBSERVED]` tag. An experiment that produces *nothing* is still a result  - 
write it down, because "the interrupt endpoint is silent at idle" constrains
the design just as much as a packet capture does.

All of these are read-only and safe on a 480M link.

---

## E1 - Is the interrupt endpoint silent at idle?

Nothing connected to the HD60 S's HDMI input.

```bash
./hd60s-probe --seconds 30
```

- **packets** → the device volunteers status unprompted. Record the cadence and
  the byte layout; the repeating fields are the status register.
- **zero packets** → EP 0x81 is demand-driven: it only reports after a vendor
  command or a real input event. Proceed to E2.

## E2 - Does HDMI hotplug generate an event?

The strongest signal available without a captured command set. Start the probe,
then physically connect an HDMI source to the **input** partway through, and
disconnect it before the end.

```bash
./hd60s-probe --seconds 40
```

Correlate packet timestamps against when you plugged. A packet arriving within
a few hundred ms of the plug event is a hotplug/signal-present notification,
and diffing the connected vs disconnected payloads isolates the bits that carry
signal-present, resolution, and HDCP state.

Repeat with different source resolutions (1080p60, 1080p30, 720p60). Bytes that
change with resolution are the timing/format fields.

## E3 - Which EP0 vendor requests are implemented?

```bash
./hd60s-probe --sweep --seconds 0
```

Sweeps `bRequest` 0x00–0xFF for vendor IN requests at device and interface
recipients. Codes that return data or a zero-length ACK are implemented; the
rest stall. This is device-to-host only - nothing is written.

Run it twice, with and without an HDMI source connected, and diff. Any request
whose payload changes with input state is a status register worth naming.

## E4 - Does the stream endpoint produce anything at 480M? (DONE)

```bash
./hd60s-isotest --alt 2 --seconds 5
./hd60s-isotest --alt 1 --seconds 4
```

Result: **no**. Both alt settings are accepted and packets are serviced at the
full microframe rate with zero errors, but every packet is zero-length - 40064
packets and 0 bytes at alt 2. The endpoint is healthy; the device just sends
nothing.

This is read-only and safe. It establishes that video must be armed by a vendor
command, and that the silence is not a bandwidth or alt-setting problem.

## E4b - RAM window via bRequest 0xA0 (DONE)

Result: byte-granular reads work but the handler caps transfers at
`addr + len <= 0xFC`. 252 bytes total, captured in
`docs/captures/window-252.bin`. Not enough for a firmware dump; see
`docs/PROTOCOL.md` 5.1.

## E6 - Standard descriptor space (DONE)

```bash
./hd60s-osdesc
```

Result: nothing. No Microsoft OS descriptor at string index `0xEE` (which would
have named a vendor request code), only three ordinary strings, and a
conventional `DEVICE_QUALIFIER` / `OTHER_SPEED_CONFIG` pair. The vendor protocol
is not reachable through the standard descriptor space.

## E7 - Does `0xA0` accept writes past the read window? (DONE)

```bash
./hd60s-writetest                                    # idempotent, safe
./hd60s-writetest --addr 0x100 --value 0x5a --force  # past the bound
```

Result: writes are accepted **within** the 255-byte window and **stall** outside
it. Same bound as reads. `0xA0` is a bounded peek/poke window, not the FX3 boot
loader, so a flash reader cannot be loaded and run. **This closes the on-device
firmware dump route.**

## E8 - Class-interface request space (DONE)

```bash
./hd60s-regprobe --sweep
./hd60s-regprobe --scan-bank 0x98 --req 0xc0
```

Result: nothing responds. All 256 codes at `bmRequestType` `0xA1`/`0xA0`/`0xC2`
stall, and bank `0x98` registers `0x00`-`0xff` stall under every request code
the Windows-driver analysis names.

This corroborates the third-party finding that register access is gated behind
an MCU-presence handshake. Read-only and safe.

## E9 - Register protocol + MCU handshake (DONE)

```bash
./hd60s-reg --handshake
```

Recovered from the vendor driver and verified on hardware. See
`docs/PROTOCOL.md` 5.9. This superseded E5 -- no usbmon trace was needed.

## E5 - Full command set (SUPERSEDED by E9)

The vendor command set cannot be derived by probing alone -- E4b established
that the RAM read cannot reach the SYSMEM where the handler lives. Two routes
remain, in order of preference:

1. **Elgato's firmware update image**, disassembled offline. Zero hardware risk,
   and the SYSMEM anchors in PROTOCOL.md 5.1 give a starting point.
2. **`usbmon` capture** of the official Windows/macOS driver, replayed and
   diffed.

Until one of those lands, the driver cannot stream.

---

## Recording results

Keep raw output - don't summarise into prose and throw the bytes away:

```bash
./hd60s-probe --seconds 40 2>&1 | tee docs/captures/e2-hotplug-$(date +%F-%H%M).log
```
