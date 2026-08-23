# Elgato Game Capture HD60 S — USB protocol notes

Living document. Everything here is tagged by how we know it:

- **[OBSERVED]** — measured directly on hardware in this repo, reproducible.
- **[TEARDOWN]** — from published hardware teardown, not verified by us.
- **[UNKNOWN]** — not yet determined. Do not guess these in code.

The device is **not** UVC. `bDeviceClass = 255` (vendor specific) on every
interface, so no in-tree kernel driver will ever bind it.

---

## 1. Identity

| Field | Value | Source |
|---|---|---|
| idVendor | `0x0fd9` (Elgato Systems GmbH) | [OBSERVED] |
| idProduct | `0x005e` | [OBSERVED] |
| Product string | `Game Capture HD60 S` | [OBSERVED] |
| Serial | per-unit, e.g. `0314BC38FC` | [OBSERVED] |
| bcdDevice (fw rev) | `1f.4f` | [OBSERVED] |
| bcdUSB | `2.10` | [OBSERVED, at HS] |

Only the **original HD60 S** is `005e`. The HD60 S+ is different hardware with
a different PID and a hardware H.264 encoder; nothing here applies to it.

### Firmware
No host-side firmware upload is required. The device enumerates with its full
Elgato identity and string descriptors straight from a cold plug, meaning the
CYUSB3014 has already booted its image from onboard SPI flash. [OBSERVED]

This is a meaningful difference from the older Game Capture HD (`0fd9:0044`),
which *did* need a host firmware push. Do not port that logic here.

---

## 2. Hardware [TEARDOWN]

| Part | Role |
|---|---|
| Cypress/Infineon **CYUSB3014** (ARM926EJ-S) | USB 3.0 device controller, host-facing |
| Nuvoton **M031LD2AE** (Cortex-M0) | housekeeping MCU, peripherals + LED |
| **MST3367** | HDMI receiver (HDMI → parallel video), I2C slave `0x9c` |
| ITE **IT66121FN** | HDMI transmitter (passthrough out), I2C slave `0x9a` |
| Altera **MAX II** CPLD | GPIO expander, I2C slave `0x98` — *not* a datapath controller, see 7.6 |
| Winbond **W25Q32JV** 4 MB SPI flash | CYUSB3014 boot image + LED animation data @ `0x300000` |
| ITE **IT1504** | 16-ch LED driver |

The two ARM cores talk over **I2C**; the CYUSB3014 issues commands to the
Nuvoton MCU for peripheral control and status. This matters for us: some
register access from the host is expected to be *tunneled* through to the MCU
rather than serviced directly by the CYUSB3014.

---

## 3. USB 2.0 fallback personality [OBSERVED]

This is what the device presents when the SuperSpeed link fails to train.
`wTotalLength = 73`, 2 interfaces, 1 configuration, bus powered @ 100 mA.

```
Interface 0  (vendor specific)          — video/stream
  alt 0 : EP 0x83 IN  Isochronous  wMaxPacketSize 0x0000  (0 B — idle/bandwidth-free)
  alt 1 : EP 0x83 IN  Isochronous  wMaxPacketSize 0x0400  (1x1024 =  1024 B/µframe)
  alt 2 : EP 0x83 IN  Isochronous  wMaxPacketSize 0x1400  (3x1024 =  3072 B/µframe)
Interface 1  (vendor specific)          — control/status
  alt 0 : EP 0x81 IN  Interrupt    wMaxPacketSize 0x0040  (64 B, bInterval 1)
```

Note there is **no OUT endpoint at all** in this personality. Every host→device
command must therefore travel over **EP0 control transfers**.

### Bandwidth ceiling — why this mode cannot carry full-rate video

Isochronous HS service is once per microframe (`bInterval 1`), 8000/s:

| Alt | Bytes/µframe | Throughput |
|---|---|---|
| 1 | 1024 | 8.2 MB/s (66 Mbit/s) |
| 2 | 3072 | **24.6 MB/s (197 Mbit/s)** |

Uncompressed 1080p60 YUY2 is `1920*1080*2*60` = 248 MB/s ≈ **1990 Mbit/s**,
and 1080p60 RGB is worse. Alt 2 is ~10x short. So in USB 2.0 mode the device
can only be doing one of: heavily compressed output, a much lower
resolution/framerate, or refusing to stream video at all. [UNKNOWN] which.

**Consequence for the driver: the USB 2.0 personality is not the target.**

---

## 4. SuperSpeed personality [OBSERVED]

Not yet captured. The device has so far only linked at 480M on this host,
across several cables and four receptacles on two different xHCI controllers
(Renesas `0000:04:00.0` and `0000:08:00.3`).

An A/B swap settles where the fault is **not**. A USB 3 hub and the HD60 S were
exchanged between two ports on the same controller:

| Port | with the hub | with the HD60 S |
|---|---|---|
| `usb4-port3` | configured, 5000M | not attached |
| `usb4-port4` | configured, 5000M | not attached |

Both receptacles carry a genuine SuperSpeed link with another device and refuse
to with the HD60 S. Host controller and ports are therefore cleared, leaving
the cable or the device's own SuperSpeed pairs. Given several cables were tried
with an identical signature — USB 2 half `configured`, SS half `not attached` —
the device's USB-C receptacle is the leading suspect.

### Captured at last: the SuperSpeed descriptor set [OBSERVED]

The link finally trained at **5000 Mbit/s** (bus 4). The personality is
materially different from the USB 2.0 one:

| Interface | Alt | Endpoint | Type | Notes |
|---|---|---|---|---|
| 0 | 0 | `0x83` | iso | 0 bytes — idle |
| 0 | 1 | `0x83` | iso | 1x1024 |
| 0 | 2 | `0x83` | iso | 1x1024 + SS companion |
| 0 | 3 | `0x83` | iso | 1x1024 + SS companion, `wBytesPerInterval` 39936 |
| **0** | **4** | **`0x83`** | **BULK** | **`bMaxBurst` 15 = 16 packets/burst** |
| 1 | 0 | `0x81` | interrupt | 64 B, `bInterval` 7 |

**Alt 4 is the real capture path.** The USB 2.0 personality has only alts 0-2,
all isochronous — there is no bulk endpoint on High Speed at all. That is what
`bFunctionalitySupport = SuperSpeed` in the BOS descriptor means in practice.

`hd60s_stream_start` now reads the descriptor to decide: if the selected alt's
endpoint `0x83` is bulk it submits bulk URBs (burst-sized, 128 KB each), else
isochronous. Nothing is hard-coded to either personality.

Full dump in `docs/descriptors-superspeed.json`.

### The device does advertise SuperSpeed [OBSERVED]

Its BOS descriptor, read at High Speed, is unambiguous:

```
05 0f 16 00 02                    BOS, wTotalLength=22, 2 capabilities
07 10 02 02 00 00 00              USB 2.0 EXTENSION
0a 10 03 00 0e 00 03 00 00 00     SUPERSPEED_USB
```

Decoding the SuperSpeed capability:

| Field | Value | Meaning |
|---|---|---|
| `wSpeedsSupported` | `0x000e` | Full + High + **SuperSpeed 5G** |
| `bFunctionalitySupport` | `0x03` | **full functionality requires SuperSpeed** |
| `bmAttributes` | `0x00` | no LTM |

Two things follow, and both are load-bearing for the driver design:

1. The silicon's USB 3 stack is alive and requesting a 5 Gbit/s link. When the
   link trains at 480M the fault is in the cable or port, not the device.
2. `bFunctionalitySupport = 3` is the device *itself* declaring that its full
   function set is unavailable below SuperSpeed. This is direct evidence that
   the USB 2.0 personality is not a usable capture mode, independent of the
   bandwidth arithmetic in section 3.

`tools/hd60s-linkcheck.py` performs this decode and prints a verdict.

### Where the link actually fails [OBSERVED]

A USB 3 receptacle is two logical ports — the USB 2 half and the SuperSpeed
half — joined by a `peer` symlink in sysfs. On the port under test:

```
usb3-port1  state=configured    peer=usb4-port1     <- HD60 S attaches here
usb4-port1  state=not attached  peer=usb3-port1     <- SS half sees nothing
usb4-port4  state=configured                        <- SS trains fine on this controller
```

So the receptacle is wired for USB 3, and the controller demonstrably trains
SuperSpeed links on another of its ports. The USB 2 half configures while the
SuperSpeed half never attaches, which localises the fault to the SS
differential pairs — cable or connector, not the host controller and not the
device's protocol stack.

Expected once a SuperSpeed link trains, based on the CYUSB3014 design:
- a **BOS descriptor** advertising `SUPERSPEED_USB` capability
  (`wSpeedsSupported` bit 3),
- **bulk** IN endpoint(s) with SuperSpeed Endpoint Companion descriptors
  (`bMaxBurst`) replacing the isochronous EP 0x83,
- possibly a bulk **OUT** endpoint for commands, which would change the
  command transport away from EP0-only.

`tools/hd60s-watch.sh` auto-captures this the instant it appears.

Until then the streaming layer must not be finalised.

---

## 5. Control protocol

### 5.1 `bRequest 0xA0` — Cypress RAM read [OBSERVED]

The shipping firmware still answers the **standard Cypress FX3 vendor RAM
command**. This is not an Elgato command; it is the boot-loader interface the
CYUSB3014 exposes, left enabled in the production image.

```
bmRequestType 0xC0   (vendor, device-to-host, device recipient)
bRequest      0xA0
wValue        addr & 0xffff     <- LOW half
wIndex        addr >> 16        <- HIGH half
wLength       bytes to read
```

The half-ordering above is the FX3 boot loader convention and is inferred, not
yet directly confirmed — see 5.2 for the evidence.

Confirmed by reading address `0x00000000` twice through two independent code
paths (the EP0 sweep and `hd60s-dumpram`) and getting byte-identical results:

```
0000  28 f0 9f e5 28 f0 9f e5  28 f0 9f e5 28 f0 9f e5
0010  28 f0 9f e5 28 f0 9f e5  20 f1 1f e5 24 f0 9f e5
0020  fe ff ff ea fe ff ff ea  fe ff ff ea 1e ff 2f e1
0030  9c ea 01 40 2c 30 00 40  6c f2 01 40 38 30 00 40
```

That decodes as an **ARM32 exception vector table** — the CYUSB3014's I-TCM at
address 0:

| Offset | Word | Instruction | Vector |
|---|---|---|---|
| `0x00` | `e59ff028` | `LDR PC, [PC, #0x28]` | Reset |
| `0x04` | `e59ff028` | `LDR PC, [PC, #0x28]` | Undefined |
| `0x08` | `e59ff028` | `LDR PC, [PC, #0x28]` | SWI |
| `0x0c` | `e59ff028` | `LDR PC, [PC, #0x28]` | Prefetch abort |
| `0x10` | `e59ff028` | `LDR PC, [PC, #0x28]` | Data abort |
| `0x14` | `e59ff028` | `LDR PC, [PC, #0x28]` | (reserved) |
| `0x18` | `e51ff120` | `LDR PC, [PC, #-0x120]` | IRQ |
| `0x1c` | `e59ff024` | `LDR PC, [PC, #0x24]` | FIQ |
| `0x20` | `eafffffe` | `B .` | handler: spin |
| `0x2c` | `e12fff1e` | `BX LR` | handler: return |
| `0x30` | `0x4001ea9c` | — | branch target, SYSMEM |
| `0x34` | `0x4000302c` | — | branch target, SYSMEM |
| `0x38` | `0x4001f26c` | — | branch target, SYSMEM |
| `0x3c` | `0x40003038` | — | branch target, SYSMEM |

The targets sit in `0x4000_0000`+, the FX3 SYSMEM window, which is where the
running firmware lives.

#### The read window is 255 bytes [OBSERVED]

Addressing is genuinely byte-granular — a read at `0x84` returns exactly the
4-byte-shifted content of a read at `0x80`, so this is real memory indexing and
not a canned buffer.

The handler enforces **`addr + len <= 0xFF`**. Single-byte reads:

| Address | Result |
|---|---|
| `0xf8`, `0xfb`, `0xfc`, `0xfd`, `0xfe` | ok |
| `0xff`, `0x100`, `0x101` | stall |

and varying the length from `0xf0`:

| Transfer | End | Result |
|---|---|---|
| `0xf0`+14 | `0xfe` | ok |
| `0xf0`+15 | `0xff` | ok |
| `0xf0`+16 | `0x100` | stall |

The check is on the **end** of the transfer, not the start, and over-long
requests are refused whole rather than truncated. So `0xc0`+16 succeeds while
`0xc0`+64 (ending at `0x100`) is rejected.

Readable range is therefore `0x00`–`0xFE`, **255 bytes**. Byte `0xFF` exists but
is unreachable: any transfer including it ends at `0x100` or beyond. That looks
like an off-by-one in the bounds check — `addr + len < 0x100` rather than
`<= 0x100`.

> **Correction.** This was previously documented as 252 bytes with the bound at
> `0xFC`. That came from probing on 4-byte alignment, where `0xfc`+4 ends at
> `0x100` and stalls — the alignment hid the real boundary. Single-byte probes
> show `0xfc`, `0xfd` and `0xfe` all read fine. `docs/captures/window-252.bin`
> keeps its filename but holds the first 252 of those 255 bytes.

**A full firmware dump over `0xA0` is still not possible.** The vendor command
handler lives in SYSMEM at `0x4000_0000`+, far outside a 255-byte window.

#### What the window contains [OBSERVED]

| Range | Content |
|---|---|
| `0x00`–`0x2f` | ARM exception vectors, plus `B .` spin and `BX LR` handlers |
| `0x30`–`0x4b` | seven SYSMEM branch targets |
| `0x4c`–`0x7f` | six `LDR PC,[PC,#-4]` + absolute-address trampoline pairs, targeting I-TCM |
| `0x80`–`0xfe` | uninitialised SRAM (see below) |

The trampoline targets are I-TCM offsets `0x113c`, `0x204`, `0x76c`, `0x158`,
`0x11a8`, `0xfd8` — all inside the 16 KB I-TCM but outside the readable window.

#### `0x80`–`0xfe` is uninitialised SRAM [OBSERVED]

The block looks statistically random and is constant across repeated reads
*within one power cycle*, so it is not a nonce, counter or RNG output. Across a
power cycle it changes — but only slightly:

| Measure | Value |
|---|---|
| bits differing across a power cycle | 52 / 992 (**5.2 %**) |
| bytes unchanged | 80 / 124 |
| bytes differing by 1 bit | 36 |
| bytes differing by 2 bits | 8 |
| `0x00`–`0x7f` (code) | byte-identical |

A freshly generated key or nonce would differ in roughly 50 % of bits. A few
percent is the signature of **uninitialised SRAM**: each cell has a
manufacturing bias that dominates its power-on state, while a metastable
minority flips run to run. This is the same effect SRAM PUFs exploit.

So the region carries no secret and no identity — it is simply memory nobody
wrote yet. Earlier `[UNKNOWN]` resolved.

#### `0xA0` also accepts writes [OBSERVED]

The OUT direction (`bmRequestType 0x40`) is live. Verified twice:

- an **idempotent** write at `0xfb` (read `0x94`, write `0x94` back) was
  accepted, changing nothing;
- a write of `0x5a` at `0xfc` was accepted and the read-back changed from
  `0x87` to `0x5a`.

So the device really does poke RAM, not merely ACK the transfer. The device
stayed healthy afterwards.

**Writes obey the same bound as reads** [OBSERVED]. A blind write at `0x100`,
just past the read window, **stalls**:

| Write target | Result |
|---|---|
| `0xfb` (idempotent) | accepted |
| `0xfc` (value `0x5a`) | accepted, read-back changed |
| `0x100` (value `0x5a`) | **stall** |

So `0xA0` here is a bounded 256-byte peek/poke window, **not** the Cypress FX3
boot loader. Elgato reimplemented the command with a hard bounds check rather
than leaving the stock handler in place.

**This closes the RAM-download route.** There is no way to place a flash reader
into memory and jump to it, so the firmware cannot be dumped over USB.

Note the FX3 boot protocol treats a **zero-length** `0xA0` as "jump to the
address in wValue/wIndex". `hd60s-writetest` refuses zero-length writes so this
cannot happen by accident.

### 5.2 A non-zero `wIndex` wedges EP0 [OBSERVED — HAZARD]

Every read attempted so far falls into a clean pattern:

| Requested addr | wValue | wIndex | Result |
|---|---|---|---|
| `0x00000000` | `0x0000` | `0x0000` | vector table, 64 B |
| `0x40000000` | `0x4000` | `0x0000` | 64 zero bytes |
| `0x00000040` | `0x0000` | `0x0040` | **timeout** |
| `0x40000040` | `0x4000` | `0x0040` | **timeout** |

Every success had `wIndex = 0`; every timeout had `wIndex != 0`. This is not an
address-range boundary — the tool was sending the address halves **swapped**.
Under the FX3 convention `wIndex` is the *high* half, so asking for `0x40` was
really asking for `0x0040_0000`: unmapped, no response, EP0 wedged.

It also explains the "`0x40000000` returns zeros" result: with the halves
swapped that request actually fetched `0x0000_4000`, the end of the 16 KB
I-TCM, which is plausibly zero-filled.

[UNKNOWN] The corrected ordering is **not yet confirmed on hardware** — the
device needs a physical replug between attempts and the test has not completed.
The first thing to do next is read `0x40` with `wValue=0x40, wIndex=0`.

#### Recovery behaviour

Once EP0 wedges, it stays wedged: subsequent transfers time out even at
address 0, which previously worked. `libusb_reset_device` did not bring it
back; the device fell off the bus and needed a **physical replug** (it is bus
powered, so unplugging power-cycles it).

No writes have ever been issued to the device, so there is no risk to the flash
image — a wedge costs a replug, not hardware.

`hd60s-dumpram` therefore **stops dead on the first timeout** rather than
continuing (continuing is what escalated a stall into a bus dropout), paces
transfers with `--delay` (default 5 ms), and offers `--reset` to attempt
recovery without a replug.

### 5.3 No Microsoft OS descriptors [OBSERVED]

Devices shipping Windows drivers often expose a Microsoft OS String Descriptor
at string index `0xEE`, which *names a vendor request code*. That would have
handed us a vendor `bRequest` without any blind probing. The HD60 S does not
have one — index `0xEE` stalls.

The device exposes only three strings (`Elgato Systems`, `Game Capture HD60 S`,
and the serial), so there is nothing to learn there either.

`DEVICE_QUALIFIER` and `OTHER_SPEED_CONFIG` are present and describe the
full-speed personality: a single interface with the same iso EP 0x83 at
0 / 64 / 128 bytes per frame, and no equivalent of interface 1. Nothing useful
for capture, but it confirms the device's descriptor set is complete and
conventional — the vendor protocol is not hidden in a nonstandard descriptor.

#### Where the firmware could still come from

The SYSMEM handler addresses (`0x4001ea9c`, `0x4000302c`, `0x4001f26c`,
`0x40003038`, `0x40003044`, `0x4001f270`, `0x4001f274`) are useful anchors *if*
the firmware image is obtained another way. Two routes that don't involve
poking the device:

1. **Elgato's firmware update package.** The image ships in the vendor updater;
   disassembling it offline carries zero risk to the hardware and would expose
   the vendor command dispatch directly. Best next avenue.
2. **`usbmon` capture** of the official driver on Windows/macOS (E5).

Reading the SPI flash in-circuit is a third route but needs a hardware
programmer on the pads.

### 5.4 `0xC0`, `0xC2`, `0xC3` in the vendor sweep [OBSERVED]

The EP0 sweep showed these returning 64 bytes each, but the payload was the
*same vector-table content* `0xA0` returned, with only the first one or two
bytes differing (`0xC2` -> `01`, `0xC3` -> `ff ff`). Since the sweep issues
`0xA0` first, the economical explanation is a **stale EP0 buffer** with a short
status value written over the front.

> **Update.** These are not "not real commands", as this section previously
> concluded. The vendor driver (5.9) uses `0xC1` for register access, `0xC2` on
> one init path, and `0xC5`/`0xC6` for MCU access. They are real — they simply
> do nothing useful when called with `wValue = 0`, which is all the sweep ever
> tried. See 5.8 for why that made the whole command set invisible.

### 5.5 Interrupt endpoint 0x81 is silent [OBSERVED]

Zero packets in 40 s, including across a physical HDMI connect and disconnect,
and zero in 20 s with a Nintendo Switch connected and outputting.

So EP 0x81 is **demand-driven, not free-running**: it reports only after some
vendor command arms it. The daemon therefore cannot rely on unsolicited status
events, and `hd60s-probe` listening alone will never produce data.

### 5.6 The isochronous endpoint sends nothing unprompted [OBSERVED]

Selecting a streaming alternate setting on interface 0 and submitting
isochronous IN transfers on EP 0x83 succeeds at the USB level but yields no
data whatsoever:

| Alt | Bytes/uframe | Duration | Packets serviced | Errored | Bytes |
|---|---|---|---|---|---|
| 1 | 1024 | 5.0 s | 32064 | 0 | **0** |
| 2 | 3072 | 6.0 s | 40064 | 0 | **0** |

Both alt settings are accepted, the host reserves the bandwidth, and packets are
serviced at roughly the full microframe rate with **zero errors** — the endpoint
is healthy. The device simply returns zero-length packets.

So video is not free-running: the stream must be **armed by a vendor command**
before anything appears on EP 0x83. Combined with 5.5 (EP 0x81 equally silent),
nothing at all happens on this device until the right control transfer is sent.

Both `hd60s-isotest` runs left the device healthy — reads at address 0 still
work afterwards, and the tool restores alt 0 (the zero-bandwidth idle setting)
on exit.

### 5.7 Input state is not exposed via a naive sweep [OBSERVED]

Sweeps taken with no HDMI input and with a Switch connected at 1080p are
**byte-identical** (`diff` empty). That is consistent with 5.1 and 5.3: the
only responding request is a RAM read of a fixed code region, which of course
does not change with input state. No status register has been located yet.

### 5.8 Why the blind sweeps found nothing [OBSERVED]

Superseded in part by 5.9, which establishes the protocol from the vendor
driver. Kept because the *reason* every probe failed is worth recording.

Third-party static analysis published by the
[hd60s-linux](https://github.com/Hydrogen2K/hd60s-linux) project reported these
request codes, and described them as `URB_FUNCTION_CLASS_INTERFACE`
(`bmRequestType 0xA1`/`0x21`):

| bRequest | Purpose (per that analysis) |
|---|---|
| `0xc0` | direct register read/write |
| `0xc1` | internal register transaction |
| `0xa0` | EEPROM reads |
| `0xa6` | debug / board-memory reads |
| `0xaa` | MCU proxy traffic |
| `0xc2`, `0xc6`, `0xc7`, `0xec` | PnP initialisation path |

The **codes are right**; the **transfer type is not**. Disassembling
`SendVendorCommandRequest` (5.9) shows URB function `0x17` =
`URB_FUNCTION_VENDOR_DEVICE`, i.e. `bmRequestType 0xC0`/`0x40`. Probing the
class space on real hardware confirms it — 256 codes each at `0xA1`, `0xA0` and
`0xC2` all stall, as does bank `0x98` under every code above.

#### The actual reason we missed it

Our EP0 sweep used `bmRequestType 0xC0` and did try `bRequest 0xC1`. Both were
correct. It failed on one parameter: **`wValue = 0`**.

`wValue` is the register selector, so `wValue = 0` asks for register `0x00` —
which does not exist. Measured:

| Request | Result |
|---|---|
| `0xC1`, `wValue 0x00`, wLength 1 | stall |
| `0xC1`, `wValue 0x00`, wLength 64 | stall |
| `0xC1`, `wValue 0x34`, wLength 1 | `0x01` |
| `0xC1`, `wValue 0x34`, wLength 64 | `0x01` (64 bytes returned) |

`wLength` is irrelevant — the device happily returns 64 bytes. Before the
handshake **only register `0x34` answers at all**, so a sweep had a 1-in-256
chance of stumbling on it even with the right request code.

The write direction is worse: value in the high byte of `wValue`, no data stage,
so it is invisible to any sweep that varies only `bRequest`.

**Lesson:** sweeping one setup-packet field while pinning the rest to zero
explores a line through the space, not the space. Where a device multiplexes a
register file through `wValue`, zero is very likely the one value that is not a
register.

### 5.9 Register protocol and the MCU handshake [OBSERVED — CONFIRMED ON HARDWARE]

Recovered by disassembling the vendor Windows driver, then **verified on the
real device**. See `docs/VENDOR-DRIVER.md` for how the driver was obtained and
analysed.

#### Transport

`CUSBBaseDevice::SendVendorCommandRequest(UCHAR bRequest, USHORT wValue,
USHORT wIndex, PVOID buf, ULONG len, UCHAR dir)` builds a `0x88`-byte URB with
function `0x17` = **`URB_FUNCTION_VENDOR_DEVICE`**, placing `bRequest` at URB
offset `0x81`, `wValue` at `0x82`, `wIndex` at `0x84`.

So these are **vendor / device-recipient** transfers — `bmRequestType 0xC0`
(IN) and `0x40` (OUT). Earlier notes in this file, following third-party
analysis, called them class-interface; that was wrong.

#### Register access — `bRequest 0xC1`

From the two thunks at `.text 0x62c00` (write) and `0x62c40` (read):

| Operation | bmRequestType | bRequest | wValue | wIndex | Data |
|---|---|---|---|---|---|
| read | `0xC0` | `0xC1` | `reg` | 0 | 1 byte IN |
| write | `0x40` | `0xC1` | `(value << 8) \| reg` | 0 | **none** |

The write carries its value in the **high byte of `wValue`** and has no data
stage at all. That is why every blind sweep with `wValue = 0` missed it, and
why the device looked completely inert for so long.

#### The MCU-presence handshake

Transcribed from `CheckIsMCUExist()` at `.text 0x19f80`:

```
if (idVendor == 0x04bb && (idProduct & 0xfff) == 0x543)
        probe = 0x33          # a different product
else
        probe = 0x39          # 0fd9:005e takes this branch

reg_write(probe, 0xC0)        # wValue 0xc039
reg_write(0x34,  0x41)        # wValue 0x4134
for i in 0..99:
        delay(10)
        if reg_read(probe) == 1:
                break         # MCU is up
```

**Verified on hardware.** Both writes are accepted and `reg 0x39` reads `1` on
the first poll. The register map changes as a result:

| Register | Before handshake | After |
|---|---|---|
| `0x34` | `0x00` | `0x01` |
| `0x39` | *stalls* | `0x01` |

Before the handshake only register `0x34` answers a read at all. That is the
gate every earlier probe ran into (5.8), and it is now open.

`./hd60s-reg --handshake` performs it and rescans.

**This is the vendor's intended path for our device.** An earlier revision of
this document claimed the handshake was gated on PID `0x0076` only. That was
wrong — it came from reading the *first* of two call sites.

`CY3014_HwInitialize` calls `CheckIsMCUExist` twice. The second call (VMA
`0x6fde2`) is reached through a switch on `(idProduct & 0xfff)`:

```
cmp ecx,0x532  je  hs
cmp ecx,0x4f   je  hs
cmp ecx,0x5e   je  hs        <- this device
cmp ecx,0x74   je  hs
...
hs: call CheckIsMCUExist
```

`0x5e` is explicitly in the list, so running the handshake on this device
matches what the vendor driver does.

#### Other init commands seen

`CY3014_HwInitialize` dispatches heavily on VID/PID (`0fd9:004f`, `005e`,
`0074`, `0076`, plus `04bb`, `1164`, `17ef` families). Two commands appear
before the MCU check:

| bRequest | Dir | Len | Notes |
|---|---|---|---|
| `0xC2` | IN | 1 | only when `(idProduct & 0xfff) == 0x531` — not this device |
| `0xEC` | OUT | 10 | payload begins `b8 22 .. .. .. c0 .. 00 .. ca`; issued on the path our device takes |

The `0xEC` payload is only partially recovered — several bytes of the 10-byte
buffer are not written in the visible basic block. [UNKNOWN]

#### The full command surface [OBSERVED]

`tools/vendor-cmdmap.py` finds every call to `SendVendorCommandRequest` in the
driver and recovers whichever arguments are constants. 273 call sites:

| bRequest | Sites | Role (from the symbols at each site) |
|---|---|---|
| `0xC0` | 214 | **I2C bridge** — `Get/SetSlaveDeviceRegister`, `SlaveDeviceBytes`. `wValue` = slave address, `wIndex` = register |
| `0xC1` | 33 | **FPGA/CPLD registers** — `Get/SetFpgaRegister`, `GSPI_*` |
| `0xC3` | 14 | GSPI word transfers |
| `0xC2` | 2 | init path, `CY3014_HwInitialize` |
| `0xC5` | 2 | MCU access, `CY3014_MCU_Exist` / `HwInitialize` |
| `0xA0` | 2 | EZ-USB data — the 255-byte window in 5.1 |

This refines 5.9: `0xC1` is not the general register interface, it is
specifically the FPGA/CPLD register file. The workhorse is `0xC0`, the I2C
bridge to the peripheral chips — the HDMI receiver, transmitter and clock
generator all hang off it.

`CY3014_GetSlaveDeviceRegister` (VMA `0x62ba0`) confirms the mapping:

```
movzx r9d, r8b      ; wIndex = register
movzx r8d, dl       ; wValue = slave address
mov   dl,  0xc0     ; bRequest
mov   DWORD [rsp+0x28], 1   ; one byte
mov   BYTE  [rsp+0x30], 1   ; IN
```

#### `EnableMCU` — the same shape, one bit different [OBSERVED]

`EnableMCU()` (VMA `0x1a590`) is nearly identical to the handshake:

| | reg `0x39` | reg `0x34` | poll |
|---|---|---|---|
| `CheckIsMCUExist` | `0xC0` | `0x41` | yes, until `0x39` reads 1 |
| `EnableMCU` | `0xC0` | `0x40` | no |

Only bit 0 of control register `0x34` differs. Verified: after `EnableMCU`,
reading `0x34` returns `0x00`; after the handshake it returns `0x01`. So the
read reflects the low bit of what was written.

`./hd60s-reg --enable-mcu` performs it.

#### The I2C bridge answers, but nothing is behind it yet [OBSERVED]

Reading register `0x00` from every even slave address `0x00`–`0xFE`, after the
handshake, returns **`0x33` every time** — and reading registers `0x00`–`0x0F`
from candidate HDMI addresses (`0x90`, `0x92`, `0x98`, `0x9A`, `0xB0`) also
returns `0x33` for all of them.

A successful control transfer here only means the *bridge* accepted the request;
it is not an I2C ACK. A constant reply across every address and register means
no peripheral is actually answering — the chips are presumably unpowered or the
I2C master is not enabled yet.

Running the handshake and `EnableMCU` first does **not** wake it, and the
readings are **not reproducible**:

| Run | Result |
|---|---|
| scan with no preceding handshake | all 128 addresses return `0x33` |
| scan right after handshake + `EnableMCU` | first 51 addresses return `0x01`, the rest `0x33` |
| 1158 reads of slave `0x90` over 3 s after `EnableMCU` | `0x33` throughout, never `0x01` |
| 9 fixed addresses read forward then backward | `0x33` every time, both orders |

The `0x01` block is therefore neither address-dependent nor time-dependent — it
is not a property of any I2C device. The most economical explanation is the
stale-EP0-buffer behaviour already documented in 5.4: the handshake's final poll
of register `0x39` returns `0x01` and leaves it in the buffer, and subsequent
reads echo it until something displaces it.

**Conclusion: the `0xC0` bridge is not returning I2C data at all on this device
in this state.** Do not read anything into the byte values.

**And the scan was probing the wrong half of the command.** See 5.10 — `0xC0`
serves two interfaces selected by `wValue`, and the useful one is `0x50xx`.

[UNKNOWN] What actually brings the peripherals up. Candidates from the symbol
table, none yet transcribed: `CY3014_PowerDownPeripherals` and its inverse, and
`CDevice::OnSetCustomDebugMcuAccessEnableProperty`.

`./hd60s-reg --i2c-scan` reports the uniform-reply case explicitly rather than
printing 128 false positives.

### 5.10 The MCU proxy — talking to the Nuvoton [OBSERVED — CONFIRMED ON HARDWARE]

`bRequest 0xC0` is **two interfaces**, selected by the range of `wValue`:

| `wValue` | Interface |
|---|---|
| `< 0x100` | direct I2C — slave address in `wValue`, register in `wIndex` |
| `0x50xx` | **MCU proxy** — tunnels a packet to the Nuvoton housekeeping MCU |

Both transports do `or di, 0x5000` on a command byte (VMA `0x627f0` for write,
`0x62880` for read):

```
write:  bmRequestType 0x40, bRequest 0xC0, wValue = 0x5000 | cmd, wIndex 0, data
read:   bmRequestType 0xC0, bRequest 0xC0, wValue = 0x5000 | cmd, wIndex 0, data
```

This is the "tunneled register form" the third-party analysis referred to but
did not publish.

#### Packet format

`0xAB` preamble, a length byte, then the command bytes.

#### `GetMCUROMVersion` — worked end to end

From VMA `0x1c240`:

```
write  wValue 0x5066 <- ab 03 12 34 57      (5 bytes)
delay  150 ms
read   wValue 0x5066 -> 3 bytes
```

**Result on hardware: `51 10 27`** — byte-for-byte what the vendor driver
compares against (`cmp bl,0x51` / `cmp bpl,0x10` / `cmp sil,0x27`). Repeatable
across runs.

This is the first exchange with real, independently-verifiable content: we sent
a command the driver sends, and the device replied with the value the driver
expects.

#### Behaviours established by experiment

| Test | Reply |
|---|---|
| read without writing first | `33 44 55` |
| write the correct command, then read | `51 10 27` |
| read 8 bytes instead of 3 | `51 10 27 ff ff ff ff ff` |
| write a **bogus** payload (`ab 03 ff ff ff`), then read | `33 44 55` |
| read back using a *different* cmd byte (`0x67`, `0x11`) | `51 10 27` |

Two things follow:

- **`33 44 55` is the "no valid reply" pattern**, not data. A rejected command
  produces it, which is how we know the MCU actually parses the packet rather
  than echoing.
- **The reply buffer is shared, not per-command.** After a successful exchange,
  any `cmd` in `wValue` reads the same bytes. So the reply must be read promptly,
  and a matching `cmd` on the read side proves nothing by itself.

The first exchange after the handshake sometimes returns the idle pattern, so
`hd60s-reg --mcu-version` retries.

#### Hazards — do not invoke

Two MCU functions in the driver are genuine brick risks and are **deliberately
not implemented** here:

| Symbol | VMA | Why |
|---|---|---|
| `Jump2LDROM` | `0x1c510` | puts the Nuvoton into its loader ROM, i.e. firmware-update mode |
| `UpdateAPROM` | `0x4b130` | rewrites the MCU application firmware |

`CommandMCUReset` (`0x19e50`) is milder but still resets the MCU.

### 5.11 I2C through the MCU — the peripherals are reachable [OBSERVED — CONFIRMED ON HARDWARE]

`i2c_access()` (VMA `0x19ca0`) assembles a packet whose **first byte becomes the
MCU command**:

| Slave | Form |
|---|---|
| `0x88`, `0x98`, `0x9c` | `wValue = 0x5000 \| slave`, payload direct |
| anything else | `wValue = 0x5066`, slave as the first payload byte |

The generic form is the one that reaches the peripheral chips:

```
write  wValue 0x5066 <- [slave|1] [count] [reg]
read   wValue 0x5066 -> count bytes
```

`slave|1` is the I2C read address. This matches the third-party description of a
`99 01 3b` payload for bank `0x98` register `0x3b`.

#### Devices found [OBSERVED]

After the handshake, scanning every even address and discarding `0xff` (idle
bus):

| Slave | reg `0x00` | Identification |
|---|---|---|
| `0x94` | `0x00` | regs read `00 d9 00 a0 0e 00 a0 00 60 42` |
| `0x98` | `0x1d` | regs read `1d 02 00 …` |
| **`0x9a`** | `0x54` | **ITE chip** — see below |
| `0x9c` | `0x02` | regs read `02 61 f5 02 01 00 08 04 03 28` |
| `0xa4` | `0x09` | |
| `0xaa` | `0x33` | |
| `0xd4` | `0x10` | |

Slave `0x9a` registers `0x00`–`0x03` read **`54 49 12 16`**:

| Reg | Value | Meaning |
|---|---|---|
| `0x00`–`0x01` | `0x4954` | vendor ID — **ITE Tech** (ASCII `"IT"`) |
| `0x02`–`0x03` | `0x1612` | device ID — **IT6612**, matching the driver's `CIT6612` class |

That is an independently verifiable identification: a chip ID read over a
protocol we reconstructed, agreeing with both the driver's class name and the
published teardown.

`./hd60s-reg --i2c-scan` and `--i2c-dump SLAVE` do this.

> **Supersedes the direct-I2C attempt.** Section 5.9 records a scan using
> `bRequest 0xC0` with `wValue < 0x100`, which returned a constant `0x33` at
> every address. That form is not real I2C on this device — the working path is
> the MCU proxy above. The earlier scan was probing the wrong half of `0xC0`.

### 5.12 The peripheral chips [OBSERVED]

Every `i2c_access` call site in the driver, with the slave constant recovered:

| Slave | Chip | Driver code |
|---|---|---|
| `0x64` | analog components | `CY3014_HwAnalogComponents*` |
| `0x88` | firmware register scanner | `CY3014_CtrlFirmwareRegisterScanner` |
| `0x94` | CS53L21 audio codec | `CY3014_CS53L21_*` |
| `0x98` | **CPLD** | `CY3014_CPLD_LoadGPIO / SaveGPIO / SetGPIO` |
| `0x9a` | **IT6612 HDMI transmitter** (passthrough) | `CY3014_ITE6612_*` — ID verified `54 49 12 16` |
| `0x9c` | **MST3367 HDMI receiver** | `CY3014_MST3367_*` |
| `0xaa` | firmware register poll | `CY3014_PollFirmwareRegisterScanner` |

`0x98` being the CPLD matches the Altera MAX II in the teardown, which formats
video into the FX3's GPIF-II bus.

#### MST3367 register pages

The receiver banks its register file. From the writer at VMA `0x65080`:

```
i2c_write(0x9c, 0x00, page)     ; register 0x00 selects the page
i2c_write(0x9c, reg,  value)    ; then the target register
```

That helper branches on the MCU-present flag (`[dev+0x63f0]`, set by
`CheckIsMCUExist`). With the MCU up — our case — it goes through `i2c_access`;
without it, it falls back to the direct `wValue = 0x9c` form. So the handshake
is a prerequisite for touching the receiver at all.

`./hd60s-reg --mst-dump PAGE` reads a page. Page `0x01` with no HDMI input:

```
00: 02 61 00 01 01 00 08 04 03 28 00 00 00 00 90 15
10: 06 93 5d 00 00 00 05 c0 c0 ff ff fc 1a 00 00 00
20: 00 00 26 00 00 a2 00 00 06 00 00 00 00 00 a1 00
30: 10 00 00 00 05 00 01 00 00 00 00 00 00 00 00 00
```

#### Receiver init is a flat register list

`MST3367_HwInitialize` (VMA `0x427b0`) is a run of `(page, reg, value)` writes
through `0x65080` — no table, just straight-line code with branches on device
variant. `tools/mst3367-init.py` extracts it: **73 writes, 56 fully literal**,
the rest computed at runtime and reported as `??` rather than guessed.

```bash
./tools/mst3367-init.py            # listing
./tools/mst3367-init.py --emit-c   # as a C table
```

**It is not a runnable sequence, and the tool now says so.** The function
branches on device variant and input mode, so a flat walk of the call sites
crosses mutually exclusive paths. Nine `(page, reg)` pairs are written with
conflicting values:

```
page 0x00 reg 0xad <- 0x00, 0x05      page 0x01 reg 0x19 <- 0x00, 0x02
page 0x00 reg 0xb3 <- 0x00, 0x14      page 0x01 reg 0x24 <- 0x40, 0x41
page 0x00 reg 0xb8 <- 0x00, 0x10      page 0x02 reg 0x07 <- 0x04, 0xf4
page 0x01 reg 0x17 <- 0x00, 0x02      page 0x02 reg 0x27 <- 0x00, 0xff
page 0x01 reg 0x18 <- 0x00, 0x02
```

Replaying the flat list would execute contradictory writes and misconfigure the
receiver. The control flow has to be resolved first.

#### Resolving the branch conditions [OBSERVED]

Both fields are set in `CDevice::OnDeviceStart` from the **Windows registry**,
each with a code default used when the value is absent — which is the normal
case on a stock install:

| Field | Registry value | Default | Set at |
|---|---|---|---|
| `[dev+0x8e60]` | `CustomCompanyS3Property` | **1** (`mov r15d,0x1`) | VMA `0x26b28` |
| `[dev+0x78f0]` | `CustomAnalogVideoInputBandwidthProperty` | **0** (`xor esi,esi`) | VMA `0x25c86` |

Applying the defaults to `MST3367_HwInitialize`:

```
0x427ce  cmp DWORD [rdi+0x8e60], 1     ; 1 == 1, equal
0x427d5  jne 0x4280c                   ; NOT taken
0x427d7  page 1 reg 0x17 = 0x00
0x427e7  page 1 reg 0x18 = 0x00
0x427f7  page 1 reg 0x19 = 0x00
0x42807  jmp 0x428bd                   ; skips the whole 0x78f0 block
```

So the `0x17/0x18/0x19 = 0x02` and `= 0x01` variants never execute, and the
three-way `0x78f0` dispatch (tested against 0, 1, 2 at `0x4280c`, `0x42845`,
`0x4287e`) is jumped over entirely. Four of the nine conflicts collapse.

That the second field is named *AnalogVideoInputBandwidth* also fits: the
MST3367 handles both analog and HDMI inputs, and the HD60 S is HDMI-only, so the
analog bandwidth path is dead code here.

[UNKNOWN] The remaining conflicts (`page 0x00` regs `0xad`/`0xb3`/`0xb8`,
`page 0x01` reg `0x24`, `page 0x02` regs `0x07`/`0x27`) sit behind other
conditions not yet traced.

#### Mode detection

`MST3367_HDMI_MODE_DETECT` (VMA `0x18ec0`) reads registers `0x55`, `0x57`–`0x5c`,
`0x5f`, `0x6a`, `0x6b`, plus page `0x02` register `0x29`. Its first test is
`(reg55 & 0x3c) == 0x3c`.

On this device with a live HDMI source connected, `reg 0x55` reads `0x01`, so
that test fails — **the receiver reports no detected mode**, which is expected
while it remains uninitialised. Register content does change when a source is
connected (page `0x01`: `0x28`/`0x29` go `06 00` -> `ce 07`, and `0x0e`/`0x0f`,
`0x10`, `0x34` change), so the chip is seeing *something*; it is just not locked.

[UNKNOWN] Signal detection therefore depends on completing the receiver init,
which depends on resolving the branch conditions above.

### 5.13 MST3367 addressing — a correction [OBSERVED]

Slave `0x9c` is one of `i2c_access`'s **special** slaves (`0x88`, `0x98`,
`0x9c`). Those take the slave value as the MCU command — `wValue = 0x509c` —
with a raw payload, **not** the generic command-`0x66` form used for every
other slave.

This document previously described MST3367 access using the generic form. That
was wrong, and wrong in the worst way: the generic form is *accepted* and
returns plausible-looking data, but the page select does not take, so every read
comes from whatever window happened to be active.

Proof — regs `0x14`–`0x1b` after each page-select attempt:

| Page select | `14 15 16 17 18 19 1a 1b` |
|---|---|
| none | `00 00 00 c0 c0 ff ff ff` |
| `wValue 0x509c` payload `{00,00}` | `00 00 00 00 00 00 00 00` |
| `wValue 0x509c` payload `{00,01}` | `00 00 30 00 00 00 50 02` |
| `wValue 0x509c` payload `{00,02}` | `00 00 00 c0 c0 ff ff ff` |
| generic `cmd 0x66` `{9c,02,00,NN}` | unchanged for every NN |

Page 1's `0x16 = 0x30` and `0x1a = 0x50` are exactly what the extracted init
writes to those registers, which corroborates the window.

Correct forms:

```
page select : wValue 0x509c, payload { 0x00, page }
register write: wValue 0x509c, payload { reg, val }
register read : wValue 0x5066, payload { 0x9d, 0x01, reg }  then read 1 byte
```

Everything read from the MST3367 before this fix should be treated as
unreliable.

### 5.14 Hot-plug detect and EDID [OBSERVED — CONFIRMED ON HARDWARE]

HPD toward the source is **page 0 register `0xB7` bit 1**, from
`MST3367_TMDS_HOT_PLUG` (VMA `0x18ab0`), which routes `0fd9:005e` straight to a
read-modify-write of that bit.

With correct addressing the bit sets and holds (`0x00` -> `0x02`). With the old
broken addressing it silently refused — a good example of why the correction
above mattered.

**Asserting HPD gates the DDC path.** Before HPD, an I2C scan finds seven
devices and nothing at `0xa0`. After HPD, slave `0xa0` — the standard DDC/EDID
address — appears, and reads back a valid 128-byte EDID:

```
00: 00 ff ff ff ff ff ff 00 14 e1 01 00 00 00 00 00
10: 11 17 01 03 80 00 02 78 1a cf 74 a3 57 4c b0 23
...
50: 58 2c 25 00 c4 8e 21 00 00 9e 00 00 00 fc 00 48
60: 44 36 30 20 53 0a 20 20 20 20 20 20 0a 00 00 fd
```

| Field | Value |
|---|---|
| header | `00 FF FF FF FF FF FF 00` — valid |
| checksum | correct |
| manufacturer | `EGA` (Elgato) |
| product | `0x0001` |
| monitor name | **`HD60 S`** |
| extensions | 1 |

So the device holds its own EDID and serves it over DDC once HPD is asserted; a
Linux driver does not need to supply one. This also confirms the earlier trace
finding that `CY3014_SetStartTechEDID` is dead code here — it is gated on YUAN
VID `0x1164` and never runs for `0fd9:005e`.

`hd60sd --source device` now asserts HPD and validates the EDID on start.

[UNKNOWN] Even with HPD asserted and a valid EDID served, the source is still
not transmitting — mode-detect register `0x55` reads `0x03` (the driver's test
wants `(v & 0x3c) == 0x3c`) and H-total reads 0. So something beyond
sink-presentation is still required.

### 5.15 Receiver lock — horizontal achieved, vertical not [OBSERVED]

`scripts/hdmi-lock.hd60s` transcribes the page-2 front-end steps of
`MST3367_HDMI_INIT` (VMA `0x18c40`) with the canonical HPD-drop/re-lock
sequence. After running it against a Nintendo Switch at 1080p60:

| Registers | Value | Decoded | Stability |
|---|---|---|---|
| p0 `0x6a`/`0x6b` | `08 98` | H total = **2200** | stable across runs |
| p2 `0x28`/`0x29` | `80 07` | H active = **1920** | stable across runs |
| p0 `0x5b`/`0x5c` | varies | V total | **unstable** |
| p0 `0x55` | `0x82` | validity gate wants `& 0x3c == 0x3c` | not satisfied |

2200 x 1125 at 60 Hz is exactly 1080p60, and 2200/1920 cannot be measured
without a live TMDS clock. **The source is transmitting and the receiver has
horizontal lock.** Before any of this those registers read zero.

Vertical total fluctuates run to run, so vertical sync is not locking, which is
consistent with `0x55` never reaching `0x3c`.

#### The page-0x80 clock steps make it worse

`MST3367_HDMI_INIT` ends with writes to page `0x80` registers `0xCE`, `0xCF`,
`0xD0`. Those live on page `0x80` because step `0x18d2c` sets bit 7 of the page
register first — writing them on page `0x00` is a silent no-op, which is what
happened on the first attempt.

Applying them correctly *degraded* the lock: H total went from a stable 2200 to
858 and V total to 1091, neither a standard mode. The `0xD0`/`0xCF` pair encodes
a 3-bit field `{D0[1:0], CF[7]}` taken from `tbl[[dev+0x78f4] % 6]` with
`tbl = {0x06, 0x00, ...}`; assuming index 0 gives `v = 0x06`, so `D0 |= 0x03`
and `CF &= 0x7F`. The first attempt used the wrong OR mask (`0x00`).

[UNKNOWN] Whether the correct `v` fixes it, or whether these clock settings need
the pixel clock configured elsewhere first. `scripts/hdmi-lock.hd60s`
deliberately omits them; `scripts/hdmi-init.hd60s` keeps the full sequence for
comparison.

[UNKNOWN] What completes vertical lock. Endpoint `0x83` remains silent
throughout — no bytes in ~47000 isochronous packets.

### 5.16 CPLD access and the firmware scanner [OBSERVED]

The CPLD (Altera MAX II, slave `0x98`) is another special slave: writes go out
as `wValue 0x5098` with a raw `{reg, val}` payload. `CY3014_SetFpgaRegister` /
`GetFpgaRegister` are its byte accessors — misleadingly named, since they are
*not* the FX3's `bRequest 0xC1` register file.

Register map from `CY3014_HwReset`:

| Reg | Role | Bring-up value | Write takes? |
|---|---|---|---|
| `0x3B` | input route (`0x02` HDMI, `0x80` analog) | `0x02` | **no** |
| `0x20` | mode | `0x05` | yes |
| `0x00` | GPIO data port 0 | `0x01` | **no** (reads `0x1d`) |
| `0x10` | GPIO direction port 0 (0 = output) | `0xFE` | yes |
| `0x01` | GPIO data port 1 — HDMI hot-plug/enable lines | `\|= 0x03` | partly |
| `0x11` | GPIO direction port 1 | `0xFC` | yes |

For PID `0x005e` the FX3 `bRequest 0xC1` reg `0x2D` reset block at the head of
`HwReset` is skipped entirely, so a Linux driver must not replay it.

#### Firmware capability probes — trace confirmed on hardware

`CY3014_HwInitialize` probes the FX3's capability bitmask, and the trace found
that for PID `0x5e` the `0xC5` probe is deliberately skipped. Tested directly:

| Request | Result |
|---|---|
| `0xC7` wValue `0x0064` | accepted |
| `0xC5` wValue `0x0000` | **stalls** |
| `0xC6` wValue `0x0000` | accepted |

`0xC5` stalling and `0xC6` working is exactly what the disassembly predicts for
this PID — an independent confirmation of the trace.

A 56-byte scan table (14 four-byte `{reg, tag, mask, aux}` descriptors, per
`CY3014_FillScanRegisters`) uploads successfully with `bRequest 0xC6`,
wValue = period.

[UNKNOWN] Even so, the interrupt endpoint `0x81` produces nothing. The scanner's
results are supposed to arrive there.

### 5.17 The USB 2.0 link is the prime suspect for the silent endpoints

Both data endpoints are completely silent while the entire control plane works:

| Path | Status |
|---|---|
| EP0 control (registers, MCU, I2C, EDID) | fully working |
| Interrupt EP `0x81` | 0 packets, ever — including with the scanner enabled |
| Isochronous EP `0x83` | ~60000 packets serviced, **0 bytes**, 0 errors |

That combination — control plane perfect, both data endpoints mute — is what a
device looks like when its firmware has not started its DMA channels.

The device itself states the constraint: its BOS descriptor reports
`bFunctionalitySupport = SuperSpeed` (5.9), i.e. full functionality is
unavailable below 5 Gbit/s. The USB 2.0 personality reserves iso bandwidth of
3072 B/microframe = 197 Mbit/s against roughly 2000 Mbit/s for uncompressed
1080p60 — a tenth of what is needed.

[UNKNOWN] Whether the FX3 firmware refuses to start streaming on a High Speed
link, or whether some further configuration would start it anyway. This cannot
be settled from this host until the SuperSpeed link trains.

### 5.18 Corrections from the second decode pass [OBSERVED]

#### `MST3367_VideoFormatDetection` is not the blocker

This document previously named it as the main obstacle. That was wrong. Its real
bounds are `0x43d20`–`0x49119` (one `.pdata` entry), and it dispatches on
`[CDevice+0x7104]` — the registry DWORD `AnalogCrossbarVideoInputProperty`,
which the HD60 S INF sets to 0. Values 2/3 take the analog arm at `0x43d8c`;
0/1 take the HDMI arm at `0x47075`.

**101 of its 109 paged writes are below `0x4705e` and unreachable on the HDMI
arm.** That arm issues at most 5 writes and programs no timing at all — the
1080p60 geometry comes from immediates (`0x780`, `0x438`) after matching the
measured H active. So there is nothing there to transcribe.

#### USB 2.0 streaming is supported by the driver

The alternate-setting tier arithmetic (`0x6e73c`–`0x6e7a7`):

```
P      = width * height * fps           (default 0x76A7000 if zero)
scaled = P + ((P * 0x0B926FAB) >> 32)
T      = 0x3E8000     because HasSuperSpeedEndpoints (0x11990) returns 0
                      -- no 0x30 companion descriptors on this personality
scaled = 130,039,999 > T, and not SuperSpeed  ->  alt 2
```

Tier 3 is gated behind the SuperSpeed flag at `0x6e797`. So on a High Speed
link the driver selects **alt 2** and proceeds — it does not refuse to stream.
Our descriptor dump confirms alts 0/1/2 are all that exist, and `wTotalLength`
= 73 ≤ 0x70 means the bulk/quirk path at `0x6e8e6` is never entered.

**The 480M link is a quality ceiling, not a hard blocker.** This corrects 5.17,
which named it as the prime suspect for the silent endpoints.

#### `p0 0xB3` / `p2 0x27` are a mute pair

The receiver's no-signal path writes `0xFF` to both (`0x1940c`, `0x1941c`); the
success path writes `0x00` to both, in that order, as its **final** action
(`0x48d61` then `0x48d71`) — after the interface is already on its streaming
alt. `hd60sd --post-script` now runs the un-mute after SET_INTERFACE.

#### Active-window measurement engine

A signal check independent of the `0x55` gate (`0x48322`–`0x484b4`):

```
mst_write(0,0xE2,0x80); mst_write(0,0xE3,0x08); mst_write(0,0xE4,0x00)
mst_read(0,0x6C); mst_read(0,0x6D)          # latching dummy reads
delay ~50 ms
read p0 0xF3..0xFA
A=((F4&0x0F)<<8)|F3  B=((F6&0x0F)<<8)|F5
C=((F8&0x0F)<<8)|F7  D=((FA&0x0F)<<8)|F9
width = (B-A+1) & ~7   height = D-C+1
mst_write(0,0xE2,0x00)                       # disarm
```

**Measured on hardware: all of `0xF3`–`0xFA` read `0x00`, giving a 0 x 1
window.** With V total also 0, that means no DE and no vertical sync — there is
no valid signal arriving. Earlier in the same session the same registers gave a
clean 2200 x 1125, so the source stopped transmitting between the two
measurements, most likely after repeated HPD cycling.

This engine is the best go/no-go signal check available, because it does not
depend on the `0x55` gate whose bit semantics are still unknown.

### 5.19 The vertical-lock wall [OBSERVED]

With a live Nintendo Switch at 1080p60 and the full reconstructed bring-up
applied — including the corrections below — the receiver reaches a stable,
reproducible, *partial* lock:

| Measurement | Value | Verdict |
|---|---|---|
| p0 `0x6a`/`0x6b` | `08 98` = **2200** | exact 1080p60 H total |
| p2 `0x28`/`0x29` | `80 07` = **1920** | exact 1080p60 H active |
| p0 `0x5b`/`0x5c` | `00 00` | **V total absent** |
| active-window engine | `0 x 1` | **no DE** |
| p0 `0x14` | `0x00` | independent sync-status byte: nothing |
| p0 `0x55` | `0x02` | gate wants `(v & 0x3C) == 0x3C` |
| p2 `0x0E` | `0x00` | stream-open handshake waits for `& 0x07` — times out |
| ep `0x83` | 0 bytes / ~63000 packets | silent |

Horizontal timing is measured *exactly* right, so the front end is locked to the
pixel clock and line rate. Vertical sync and data-enable are entirely absent.

#### Ruled out

- **`p0 0x0E` bit 3** would make `0x5b`/`0x5c` report sync *width* rather than V
  total. Measured `0x40` — bit clear. Not the cause.
- **`p0 0x54` bit 4** is clear (`0x20`), which is correct.
- **`MST3367_VideoFormatDetection`** — 101 of its 109 writes are unreachable on
  the HDMI arm (5.18).
- **The USB 2.0 link** — the tier arithmetic selects alt 2 and proceeds (5.18).

#### A structural bug this exposed

`MST3367_HDMI_INIT` step 7 is a read-modify-write of register `0x00` — the
**page register itself** — setting bit 7, and that bit must stay set across the
following `0xCE`/`0xCF` writes (`0x18D11`–`0x18D8E`).

`hd60s_mst_write(page, reg, val)` re-selects the page before every access, so it
*destroys the hold* and can never reproduce this. `hd60s_mst_raw_write/read` and
the `mst_raw` / `mst_rawrmw` script ops exist for exactly this case. Fixing it
made the sequence execute correctly — but did not change the lock state.

#### What is genuinely still unknown

The decode passes were explicit about their own limits, and these are the gaps:

- **The bit semantics of `p0 0x55`.** Not derivable from the binary — the driver
  only ever tests `& 0x3C`, never assigns meaning to individual bits.
- **`bRequest 0xC2`.** A zero-length OUT issued unconditionally with its result
  discarded; no other driver in the package uses it and the PDB has no name for
  it. Semantics undeterminable from this image.
- **The `0xEC` 10-byte payload.** `B8 22 ?? ?? ?? C0 ?? 00 ?? CA`, with five
  bytes left as uninitialised stack by the vendor driver. Structure unknown.
- **The EDID upload inside `CY3014_SetStartTechEDID`** — payloads assembled at
  runtime, not decoded. Judged not to be the blocker since the source does
  transmit, but it is a real gap in the replay.

Getting past this most likely needs a `usbmon` capture of the working Windows
driver against this exact unit, so the successful register sequence can be
diffed against ours. Static analysis has been taken about as far as it goes.

### 5.20 CORRECTION: the receiver was never locked [OBSERVED]

Earlier sections of this document, and several session reports, claimed the
receiver had achieved horizontal lock on a live 1080p60 source, citing
`p0 0x6a/0x6b` = 2200 and `p2 0x28/0x29` = 1920 as exact 1080p60 timing that
"cannot be measured without a live TMDS clock".

**That claim does not survive testing.** Those registers are latched or static,
not live measurements:

| Condition | H total | H active |
|---|---|---|
| baseline | 2200 | 1920 |
| during TMDS/PLL reset (`p2 0x07 = 0xF4`), held 350 ms | 2200 | 1920 |
| just after release | 2200 | 1920 |
| **HPD de-asserted 1.5 s** (source should stop transmitting) | 2200 | 1920 |
| HPD re-asserted 2.5 s | 2200 | 1920 |

Nothing moves them. A live measurement would collapse when the TMDS clock is
removed. Neither our scripts nor the bring-up writes `0x6a`/`0x6b` or
`p2 0x28`/`0x29`, so these are chip-held values that happen to contain plausible
1080p60 constants.

They did change earlier in the session (`06 00` -> `ce 07`, and once `2020/1740`),
so they are not immutable — but they do not track the source, and they are not
evidence of lock.

**Corrected status: the receiver shows no evidence of ever having locked.** Every
independent indicator agrees and always has:

| Indicator | Value |
|---|---|
| V total (`p0 0x5b/0x5c`) | 0 |
| active-window engine (`p0 0xF3`-`0xFA`) | all zero |
| `p0 0x14` (independent sync status) | `0x00` |
| `p2 0x0E` (stream handshake) | `0x00` |
| `p0 0x55` gate | `0x02`, wants `& 0x3C == 0x3C` |
| `p0 0x01` bit 2 (HDMI mode) | clear |
| ep `0x83` | 0 bytes |

The earlier reading — "partial lock, horizontal good, vertical missing" — was
wrong. There is no partial lock. There is no lock.

#### What this rules out

It removes the vertical-sync line of enquiry as the primary problem. A receiver
with genuine H lock and no V lock is an unusual, specific fault; a receiver with
nothing is the ordinary case of "no signal reaching the front end". The question
is not *why does vertical fail* but *why does nothing arrive*.

[UNKNOWN] Whether the source ever transmits into this device. We have never had
an independent confirmation — no display was ever connected to the passthrough
output, so every judgement about the source has come through the same
uninitialised receiver we are trying to debug.

### 5.21 HPD reaches the hardware; TMDS never arrives [OBSERVED]

A differential capture of all 768 MST3367 registers (pages 0, 1, 2) with HPD
asserted for 3 s versus de-asserted for 3 s. Exactly three registers differ:

| Register | HPD high | HPD low | Note |
|---|---|---|---|
| `p0 0xb7` | `0x02` | `0x00` | the bit we write — expected |
| `p2 0x0e` | `0x10` | `0x00` | the stream-open handshake register |
| `p2 0x0f` | `0x15` | `0x00` | |

Two things follow.

**Our HPD control genuinely reaches the hardware.** `p2 0x0e`/`0x0f` are not
written by us and they track the HPD state, so the receiver observes the change.
That validates the whole control chain end to end: USB -> MCU proxy -> I2C ->
MST3367 -> a physical line the front end can see.

**But no TMDS ever arrives.** `p2 0x0e` reads `0x10` — bit 4 set, bits 0-2
clear, and the driver's stream-open handshake (`0x6E452`-`0x6E474`) waits for
`(p2 0x0e & 0x07) != 0`. Not one timing, sync or status register outside those
three moves. If a source were transmitting, HPD removal would collapse the clock
and something would change; nothing does.

Combined with 5.20 (the H values are static, not measurements), the conclusion is
that **the front end never receives a signal**, and our HPD assertion — while
electrically effective at the register level — does not result in the source
transmitting.

[UNKNOWN] Whether the cause is upstream (the source never starts) or at the
physical layer (the sink presentation is incomplete in some way the registers do
not show). This cannot be settled from the host: every observation we can make
comes through the same receiver.

**The decisive test is external**: connect a display to the HD60 S's HDMI
passthrough output. If it shows the source, the source transmits and the fault
is in our configuration. If it shows nothing, the source is not transmitting and
no register sequence will change that.

### 5.22 HPD is ACTIVE LOW — receiver now locks [OBSERVED — CONFIRMED]

**The single most consequential error in this project.** Page 0 register `0xB7`
bit 1 is **active low**: clearing it *asserts* hot-plug detect, setting it
*de-asserts*. This document and the driver code had it backwards throughout.

Measured directly, sampling while toggling the bit:

| `0xB7` bit 1 | V total | H total | `0x55` | gate `& 0x3c == 0x3c` |
|---|---|---|---|---|
| set (`0x02`) | 996 | 1483 | `0x02` | fails |
| **clear (`0x00`)** | **1125** | **2200** | **`0x7f`** | **PASSES** |

1125 x 2200 at 60 Hz is exactly 1080p60. With the bit clear the receiver
achieves full lock and the vendor driver's own validity test succeeds.

The disassembly said so all along: `MST3367_HwInitialize` ends with an
unconditional `p0 0xB7 = 0x00` at VMA `0x43735`, and a trace explicitly flagged
that "Windows leaves 0xB7 = 0x00 on this device". That was overridden here on a
mistaken empirical reading — early in the session, setting the bit appeared to
make the EDID EEPROM at slave `0xa0` become visible, and that coincidence was
taken as proof of polarity. It was not.

**Correction to 5.14 and 5.20.** 5.14 claimed asserting bit 1 gated the DDC path;
5.20 concluded the receiver had never locked. Both were consequences of driving
HPD backwards. With the polarity fixed the receiver locks reliably, and
`hd60sd --diagnose` reports:

```
  p0 0x55=7f (gate wants & 0x3c == 0x3c)   V total = 1125
  VERDICT: SIGNAL PRESENT -- V total=1125, gate 0x55=7f PASSES.
```

#### The two problems are now cleanly separated

With a locked receiver reporting a valid 1080p60 signal, endpoint `0x83` still
delivers **0 bytes in ~80000 serviced packets**. Receiver lock and the FX3
datapath are therefore independent problems, and only the second remains.

`p2 0x0E` still reads `0x00`, so the vendor driver's stream-open handshake
(`0x6E452`-`0x6E474`, which waits for `(p2 0x0E & 0x07) != 0`) would still time
out. That is now the sharpest remaining lead.

### 5.23 With the receiver locked, both data endpoints are still silent [OBSERVED]

This is the cleanest evidence the project has produced, because for the first
time the receiver side is provably correct.

State with a live 1080p60 source and the full bring-up applied:

| | |
|---|---|
| `p0 0x55` | `0x7f` — validity gate **passes** |
| V total | 1125 |
| H total | 2200 |
| H active (p2 `0x28/0x29`) | 1920 |
| stream enable `p1 0x24` | `0x41` |
| mute pair `p0 0xB3` / `p2 0x27` | `0x00` / `0x00` — un-muted |
| scanner regs `p2 0x11`/`0x12` | `0x7a` / `0x11` — real values |
| CPLD `0x20` / `0x10` | `0x05` / `0xfe` |

Everything the vendor driver configures is configured, and the receiver reports
a valid, correctly-measured 1080p60 signal.

Yet:

| Endpoint | Result |
|---|---|
| iso `0x83` | 0 bytes in ~80000 serviced packets, 0 errors |
| interrupt `0x81` | 0 packets, even after uploading a scan table with `0xC6` |

#### What was tried

Poking every plausible CPLD datapath register while streaming — `0x3b = 0x02`
(HDMI route), `0x20` with and without bit 4, `0x20 = 0x75`, `0x00`, `0x10 =
0xFC`/`0xFE`, port-1 `0x01`/`0x11`, and `0x21`-`0x24` — produced **zero bytes in
every case**.

The firmware register scanner (`bRequest 0xC6` with a 56-byte descriptor table,
both `wIndex` forms and two scan periods) is accepted but produces nothing on the
interrupt endpoint, even though the registers it scans now hold valid data.

#### The interrupt endpoint is the tell

EP `0x81` needs no meaningful bandwidth — 64 bytes per millisecond. If the FX3
firmware were running its data-producing logic at all, the scanner should report
there regardless of link speed. It does not.

**Control plane perfect, both data endpoints mute, zero errors** is the signature
of firmware that has not started its DMA channels.

This revives the reading that 5.18 retracted. That retraction was based on the
alternate-setting tier arithmetic selecting alt 2 on a High Speed link — but that
only shows which alt the *host driver* picks, not that the *device firmware* will
stream. The two are different claims and this document conflated them.

Elgato's published requirement for this model is USB 3.0, and the device's own
BOS descriptor reports `bFunctionalitySupport = SuperSpeed`.

[UNKNOWN] Whether any reduced-rate mode exists that would stream over High Speed.
No vendor command to select capture resolution has been identified.

### 5.24 Still unknown

- `bRequest` / `wValue` / `wIndex` values for any operation
- the MCU-presence handshake reportedly required before register access works
- the "tunneled register" form used after that handshake
- register map (input status, EDID, resolution, HDCP state, LED)
- video packet framing, pixel format, resolution negotiation
- audio format and whether audio is muxed into the video endpoint

The interrupt endpoint 0x81 shows activity without any vendor command being
sent, so it is plausibly an unsolicited status/hotplug channel (HDMI signal
present, resolution change). [UNKNOWN] — needs capture to confirm.

### How these get filled in
1. `usbmon` capture of the official driver on Windows/macOS talking to the
   device, then replay/diff. This is the only reliable route — there is no
   public datasheet.
2. Cautious host-side probing of EP0 with benign `GET_` style requests, logging
   which `bRequest` values return data vs stall. Never write blindly: a wrong
   vendor write can brick the SPI flash image.

**Rule for this repo: no invented constants.** If a value is not in a capture,
it does not go in the code.

---

## 6. HDCP

HDMI sources that assert HDCP will not be capturable, and nothing in this
driver attempts to defeat that. Consoles generally allow disabling HDCP for
game output; that is the supported path.

## 7. Where the data stops [OBSERVED]

Established 2026-08-23, with the receiver locked to a 1080p60 Switch over a
SuperSpeed link. This section supersedes any earlier speculation about the
endpoint being misconfigured.

### 7.1 The USB side is healthy; the FX3 is starved

Logging `libusb`'s transfer status (rather than only the byte count) separates
three cases that all previously looked like "0 bytes":

| alt | endpoint | status | meaning |
|---|---|---|---|
| 4 | bulk | `TIMED_OUT`, actual 0 of 131072 | device NAKs forever -- nothing queued |
| 1 | iso | `COMPLETED`, every packet len 0 | device sends empty packets every interval |

Neither is an error, a STALL, or a babble. The endpoint is armed and the host
controller is happy. The FX3 simply has no data to hand over.

### 7.2 The FX3's DMA channel *is* being built [OBSERVED]

`bRequest 0xA0` reaches the FX3's peripheral space, not just its RAM. Reads are
byte-wide, and FX3 peripheral registers only answer 32-bit accesses, so **only
the low byte of each 32-bit register is recoverable** -- every word reads back as
`0x000000xx`. That is still enough to diff.

Dumping 4 KB of each block while idle and again while streaming on alt 4:

| block | base | idle -> streaming |
|---|---|---|
| PIB (GPIF) | `0xE0014000` | 14 bytes change; `+0x00` `0x20`->`0x00`, `+0x04` ->`0x6c`, `+0x2c` ->`0x30`, `+0x12c` `0x01`->`0xf1` |
| PIB sockets | `0xE0018000` | socket 0 `+0x00` `0xff`->`0x10`, socket 1 `+0x80` `0xff`->`0x11` |
| UIB | `0xE0030000` | unchanged |
| UIB ingress sockets | `0xE0038000` | socket 3 `+0x180` `0xff`->`0x10`, plus `+0x190`, `+0x990` |

`0xff` in a socket's descriptor field is the "no descriptor / unused" encoding.
Both the GPIF-side producer sockets and the USB-side consumer socket go from
unused to a valid descriptor number when we `SET_INTERFACE`. So the FX3 firmware
is creating and arming the DMA channel exactly as it should.

**Conclusion: the GPIF is armed and waiting for pixels that never arrive.** The
break is upstream of the FX3 -- in the CPLD, or on the receiver's output side.
It is not in our USB setup, which is where the previous several days of effort
went.

### 7.3 The CPLD sees nothing [OBSERVED]

Toggling HPD in software (`p0 0xB7` bit 1 -- active low) makes the source stop
and restart, giving a clean before/during/after differential over 128 registers:

- **All 64 CPLD registers `0x00`-`0x3F` are byte-identical** with video present
  and absent. Not one bit moves.
- The MST3367's registers track the video perfectly: `p0 0x55` `0x7f`<->`0x83`,
  `p2 0x28/0x29` (H active) `0x0780`<->`0x2608`, `p0 0x56`, `0x58`, `0x5e`, `0x5f`
  all follow.

Setting bit 4 of CPLD `0x20` after `SET_INTERFACE` (the bring-up script clears it
at line 103-104 having set it at line 86) reads back correctly as `0x15` and
changes nothing.

### 7.4 `p2 0x0E` is read-to-clear [OBSERVED] -- CORRECTION

Earlier notes treat `p2 0x0E` reading `0x00` as evidence that nothing is
happening, and the vendor's wait for `(v & 0x07) != 0` as therefore doomed. That
reading was wrong: the register is a **read-to-clear latch**.

```
first read after bring-up writes : 0x90
every subsequent read            : 0x00
after HPD de-assert              : 0x80   (input lost latched)
after HPD re-assert              : 0x90
```

So it is an event latch on input transitions, and every previous `0x00` was a
drained latch rather than a dead register.

### 7.5 Receiver output registers do not survive an input transition [OBSERVED]

Three page-2 registers set during bring-up are cleared by an input transition
and do **not** come back when the input returns:

| reg | after bring-up | after HPD cycle |
|---|---|---|
| `p2 0x0D` | `0x15` | `0x01` |
| `p2 0x10` | `0x07` | `0x00` |
| `p2 0x16` | `0x04` | `0x00` |

`p2 0x12` bit 7 sets while there is no input (`0x11` <-> `0x91`), so it reads as
a "no signal" status bit.

This matters because it means a bring-up performed before the source is stable
gets partially undone. It is consistent with the vendor driver re-running
`MST3367_HwInitialize` continuously during capture. Running our bring-up twice
with a 3 s gap does not by itself produce data.

### 7.6 The CPLD is a GPIO expander, not the video path [OBSERVED]

An earlier note in section 2 described the Altera MAX II as "formats parallel
video into the GPIF-II bus". Nothing supports that and two things contradict it.

Sweeping all 256 registers of slave `0x98`, only **nine** respond:

| reg | value |
|---|---|
| `0x00` | `0x1d` |
| `0x01` | `0x02` |
| `0x10` | `0xfe` |
| `0x11` | `0xff` |
| `0x20` | `0x05` |
| `0xf0`-`0xf3` | `54 45 53 54` = ASCII **"TEST"** |

None of the 256 changes when video appears or disappears. And the vendor driver
only ever reaches this slave through three functions, all of them GPIO:
`CY3014_CPLD_LoadGPIO`, `CY3014_CPLD_SaveGPIO`, `CY3014_CPLD_SetGPIO`.

So the CPLD is a GPIO latch with an ID signature. Poking its registers to find a
"datapath enable" — which earlier sessions spent a long time doing — was never
going to work. The video path from the receiver to the GPIF is not under its
register control.

### 7.7 The receiver is an MST3367 [OBSERVED] -- CORRECTION

Section 2 previously listed the receiver as an ITE IT6802E. The string
`IT6802` appears **zero** times in `CY3014.X64.SYS` and zero times in its PDB,
while `MST3367` appears 14 and 73 times respectively. Our MST3367 register map
also produces a genuine lock with correct timings (V total 1125, H total 2200,
H active 1920) on real hardware, which it could not do against the wrong part.

The transmitter really is an ITE part: `IT66` appears 136 times in the PDB.

### 7.8 What `0xA0` can and cannot see [OBSERVED]

`bRequest 0xA0` reads FX3 RAM and FX3 peripheral space alike, but not equally:

- **RAM** (`0x00000000` ITCM, `0x40000000` SYSMEM) returns all four bytes of
  every word. Address `0x00000000` reads `e59ff028` = `ldr pc, [pc, #0x28]`,
  the ARM926 exception vector table.
- **Peripherals** (`0xE00xxxxx`) return only the **low byte** of each 32-bit
  register; bytes 1-3 always read zero. The peripheral decodes 32-bit accesses
  only and the handler is doing byte-wise copies.

This is a real limit on the diagnostic. FX3 socket registers keep their
interesting bits high — `SCK_STATUS` bit 31 `SCK_ENABLE`, bit 25 `SCK_GO_ENABLE`
— so we can see a socket's descriptor number change but not whether the socket
is actually enabled.

Socket layout confirmed against the observed data (sockets are `0x80` apart):

| offset | register |
|---|---|
| `0x00` | `SCK_DSCR` (`0xff` = unused) |
| `0x04` | `SCK_SIZE` |
| `0x08` | `SCK_COUNT` |
| `0x0C` | `SCK_STATUS` |
| `0x10` | `SCK_INTR` |
| `0x14` | `SCK_INTR_MASK` |
| `0x20` | `DSCR_BUFFER` |
| `0x24` | `DSCR_SYNC` |
| `0x28` | `DSCR_CHAIN` |

## 8. The FX3 side is complete and correct [OBSERVED]

### 8.1 The firmware's code is unreadable, its data is not

Dumping all 512 KB of SYSMEM via `0xA0` gives one contiguous refusal:

| range | size | result |
|---|---|---|
| `0x40000000`-`0x40002F40` | 12 KB | reads fine — DMA descriptors |
| `0x40003000`-`0x40023000` | **128 KB** | every read STALLs |
| `0x4002301C`-`0x40080000` | 372 KB | reads fine — heap and buffers |

The boundaries are sharp and repeatable (`0x40002F00` readable, `0x40003000`
stalls, `0x40023100` readable again). There is **no ARM code anywhere in the
readable regions** — zero `stmfd sp!` prologues, zero `bx lr`. So the firmware
image sits in that 128 KB hole and the `0xA0` handler will not read it out.

The readable part does carry the string
`Copyright (c) 1996-2008 Express Logic Inc. * ThreadX ARM9/RVDS Version`,
confirming this is a stock Cypress FX3 SDK firmware on ThreadX.

**Consequence:** the FX3's firmware cannot be disassembled from the device. The
MCU firmware shipped in the vendor package is the only decodable firmware we have.

> Note: `hd60s-dumpram` pads unmapped chunks with `0xFF`, not `0x00`. A dump is
> not all-real data — check for `0xFF` runs before drawing conclusions from it.

### 8.2 The video DMA channel is built exactly as it should be [OBSERVED]

`0x40000000` holds the FX3's DMA descriptor array, 16 bytes per entry
(`buffer`, `sync`, `chain`, `size`), 755 live descriptors. Two classes matter:

| sync | count | buffer size | meaning |
|---|---|---|---|
| `0xc100c303` | 6 | 16384 | PIB (GPIF) socket 0 → UIB socket 3 |
| `0xc101c303` | 6 | 16384 | PIB (GPIF) socket 1 → UIB socket 3 |

Decoding `sync` as consumer in the low half, producer in the high half:
`cons_sck = sync & 0xFF`, `cons_ip = (sync >> 8) & 0x3F`,
`prod_sck = (sync >> 16) & 0xFF`, `prod_ip = (sync >> 24) & 0x3F`, with
IP block IDs `0` LPP, `1` PIB, `2` SIB, `3` UIB, `4` UIBIN, `0x3F` CPU.

That gives **producer = PIB (GPIF) sockets 0 and 1, consumer = UIB socket 3 =
endpoint `0x83` IN**, with twelve 16 KB buffers — two producer sockets
ping-ponging into one USB IN endpoint. That is textbook FX3 video capture.

It also matches the live register diff of 7.2 exactly: the sockets that were
handed valid descriptors on `SET_INTERFACE` are PIB 0, PIB 1 and UIB 3 — the
same three this table describes.

### 8.3 Conclusion: the fault is on the GPIF's input pins

Everything downstream of the GPIF is confirmed good:

- the DMA channel is constructed correctly for 1080p capture
- both producer sockets and the consumer socket are armed on `SET_INTERFACE`
- the USB endpoint is healthy (it NAKs on bulk, sends empty iso packets)
- the receiver is locked with correct timings

Nothing is left between them except the parallel video bus from the MST3367 into
the FX3's GPIF-II pins. Either the receiver is not driving it, or the GPIF state
machine is not being clocked or triggered.

Since the FX3's own code is unreadable (8.1) and the CPLD is only a GPIO latch
(7.6), the remaining sources of truth are the MCU firmware and `libUB530.dylib`.

## 9. Starting the stream [OBSERVED]

The full sequence, verified end to end on hardware. Implemented by
`scripts/stream.hd60s`.

### 9.1 What was actually missing

Two things, and both had been sitting in plain sight:

1. **`bRequest 0xC6` with an empty payload is the DISARM form.** The bring-up
   script had been ending with `vendor_out 0xC6 0x0000 0x0100` — wLength 0 —
   which explicitly stops the register scanner. The arm form carries a list of
   4-byte scan descriptors. Sending the real list makes the interrupt endpoint
   `0x81` produce its first ever packet.
2. **`p0 0xB0` bit 0 is the receiver's video output enable.** Every init table
   in both the MCU firmware and the Windows driver leaves it clear (`0x24`).
   It is set only once a video format has been recognised. For a progressive HD
   format the value is `0x25` (colour-space mode 2 in bits 5:4, enable in bit 0).

Writing `p0 0xB0 = 0x25` starts the pixels immediately.

### 9.2 The arm command

```
bmRequestType 0x40, bRequest 0xC6, wValue 0x0064, wIndex 0x0101, wLength 40
```

Payload is ten 4-byte records `{reg, tag|page, mask, slave}`; `tag = b1 >> 6`,
where tag 2 = MST3367 (page = `b1 & 0x3f`) and tag 1 = raw I2C to `slave`.

```
55 80 3C 00   6A 80 0F 00   6B 80 FE 00   01 81 07 00   0B 82 DF 00
0C 82 3F 00   0E 82 08 00   48 82 60 00   9B 82 F0 00   0E 40 80 9A
```

The disarm form is the same request with `wValue 0x0000, wIndex 0x0100` and no
payload. The vendor brackets every host-side register access with
disarm/arm, so the scanner and direct I2C access are mutually exclusive.

### 9.3 The output block for 1080p60

Applied after the receiver reports lock (`p0 0x55 & 0x3C == 0x3C`):

| register | value | meaning |
|---|---|---|
| `p1 0x24` | `0x40` | EDID-SRAM control (**not** stream enable; `0x41` = EDID disabled) |
| `p0 0xAB` | `&= 0x7F` | clear the CSC-RAM write gate |
| `p0 0xAD` | `0x05` | |
| `p0 0xB1` | `0xC0` | |
| `p0 0xB2` | `0x00` | |
| `p0 0xB3` | `0x00` | video unmute |
| `p0 0xB4` | `0x54` | |
| `p0 0xB5` | `0x0C` | |
| `p0 0xAE` | `\|= 0x04` | |
| `p2 0x07` | `0xF4` then `0x04` | front-end reset pulse the vendor always issues |
| **`p0 0xB0`** | **`0x25`** | **video output enable — this is what starts the data** |

### 9.4 The wire format [OBSERVED]

The device sends whole frames, blanking included, as a flat byte stream:

```
line  = 3840 bytes YUY2 (1920 px) + 4-byte marker  FF 00 00 KK
KK    = 0x80  active line
      = 0xAB  blanking line
frame = 1080 active + 45 blanking = 1125 lines x 3844 = 4 324 500 bytes
```

The 1125 is exactly the V total the receiver reports, and the 45 blanking lines
are `Y = 0x10` throughout. Because every line is self-labelling, a receiver can
resync from anywhere in the stream — `src/framer.c` does exactly that, then
strips the marker, drops blanking, and emits 1920x1080 YUY2.

Measured: ~233 MB/s, 331 frames in 6 s, one resync at startup.

### 9.5 Throughput matters

1080p60 at this frame size is ~259 MB/s. With 8 URBs of 128 KB the daemon only
sustained 73 MB/s, dropped most of the stream, and — because the drops broke
continuity — the frame period could not be measured at all. 32 URBs of 256 KB
fixed it. If frame periodicity ever looks absent again, suspect the queue depth
before suspecting the device.

### 9.6 Why we use alt 4 (bulk) and the vendor usually does not

The Windows driver picks its alternate setting from a width x height x fps
bandwidth formula and, for 1080p60 on a normal Intel/AMD/Renesas xHCI, selects
**alt 2** — isochronous, 32768 B/uframe. It reaches alt 4 (bulk) only for a
hard-coded host-controller blacklist (ASMedia `1b21:1042/1142/1242`, Etron
`1b6f:7023`, VIA `1106:3432`) or a registry override. There is also a
descending alt-4-to-0 retry loop in `CUSBBaseDevice::CreatePipe`, but it only
fires if the interface is still on alt 0.

We use alt 4 by default anyway, because on Linux it is simply better:

- bulk is lossless and flow-controlled, so a late URB costs latency, not pixels
- it needs no bandwidth reservation, so it cannot fail to start because some
  other device already booked the bus
- measured: 4168 frames in 70 s (59.5 fps), 18 GB, one resync at startup

`--alt N` still selects any of 1, 2, 3 (isochronous) or 4 (bulk); the driver
reads the active config descriptor and submits the right transfer type either
way, so neither personality is hard-coded.

### 9.7 Caveat: the shipped PDB does not match the shipped SYS

`CY3014.X64.PDB` and `CY3014.X64.SYS` in the vendor package have **mismatched
GUIDs**, and symbol addresses drift by up to `0x1A0`. Several confident-looking
conclusions earlier in this project came from reading a symbol name at an
address that belongs to a different function.

If you need exact addresses, `MZ0380.X64.PDB` matches `eMZ0380.X64.SYS`
byte-for-byte and carries the same MST3367 source. Prefer it.

Two corrections that came out of doing so:

- `p1 0x24`/`0x25`/`0x26`/`0x27` is a key/EDID RAM block (control, address,
  address, auto-incrementing data), **not** a stream enable. Its running value
  is `0x40`; `0x41` is the "disabled" state.
- The `p2 0x0E` poll at `0x6E452` is a diagnostic settling wait around a
  deliberate HDCP BKSV wipe. Both loops fall through on timeout with no error
  and no state change, so that register was never gating the datapath.

### 9.8 CPLD reads must use the proxy form, not the raw form

The MCU's raw pass-through for slaves `0x88`, `0x98` and `0x9C`
(`wValue = 0x5000 | slave`) is **write-only**: the handler sets the reply length
to zero, so a following IN returns the MCU's stale TX preset `33 44 55` rather
than register data. Anything reading the CPLD that way is reading a constant.

Ours does not. `hd60s_cpld_read` goes through the generic proxy
(`wValue 0x5066`, payload `[slave|1, count, reg]`) and only `hd60s_cpld_write`
uses the raw form, which is what that form is for. Two independent checks
confirm the reads are real rather than a fixed preset:

- write/readback tracks: `0x20 = 0x05` reads `0x05`, and after `|= 0x10` it
  reads `0x15`
- `0xF0` reads `0x54`, which is precisely the value the vendor's own CPLD
  watchdog compares against before re-running the MCU handshake

So the survey in 7.6 stands: nine live registers, four of them ASCII `TEST`,
none of them reacting to video.

## 10. Audio: where it is not [OBSERVED]

The receiver has audio. MST3367 `p2 0x9B` reads `0x40`, and `libUB530.dylib`'s
`GetAudioSampleRate` (vtable+0x40 -> `0x9184`) decodes that register's high
nibble as `0xC0` -> 32000, `0x00` -> 44100, anything else -> 48000. So the
source is locked at 48 kHz. `p2 0x0B` bit 6 (audio event) is set. That same
register is one of the ten records in the `0xC6` scan list we send
(`9B 82 F0 00`), so the vendor watches it the same way.

The USB stream nevertheless carries video and nothing else. This was measured in
the driver rather than sampled, because every earlier assumption here turned out
to be wrong:

| Hiding place | Instrument | Result |
|---|---|---|
| A third line type | count `FF 00 00 KK` with unknown `KK` | **0** across 16 GB |
| Blanking payload | compare every blanking line against flat `10 80` fill | one non-flat line per frame, and it is itself uniform (`26 83 26 7c` x960) — a flat colour |
| The 4-byte marker slot | capture the raw slot on every marker miss | only ever flat fill, never varying data |
| A different alt | alt 2 iso vs alt 4 bulk | identical stream |
| A second endpoint | descriptors | none; EP `0x81` is 64 B per 8 ms ≈ 8 KB/s, far under the 192 KB/s needed |
| A second DMA channel | FX3 descriptor table at `0x40000000` | only GPIF sck 0/1 -> UIB sck 3, plus 256-byte CPU channels |

The marker slot was the best candidate on arithmetic — 4 bytes x 1125 lines x
60 fps = 270 KB/s against 192 KB/s needed — and it is simply unused.

### 10.1 What has been swept, with video live

An earlier CPLD sweep concluded "no effect", but it ran before `p0 0xB0` was
found, i.e. against a dead stream, and proved nothing. Repeated properly:

- all 256 CPLD registers, and all 16 GPIO lines driven low one at a time on a
  freshly reset device: **0 odd markers, 0 change**, and video never lost sync
  (1 resync across 5529 frames)
- the MST3367 audio output block (`p2 0x17/0x19/0x1A/0x1B/0x22/0x27`): no change
- `0xC2` and `0xC7` are fixed: `0xC7` only ever takes `wValue 0x64`, from a
  re-init helper at `0xd06` that runs `0xC2(0)`, `0xC7(0x64)`, then the `0xC1`
  MCU handshake. Neither is a mode selector.
- `0xEC` is a one-shot. Its builder `sub_a815` takes two parameters (payload
  bytes 5 and 7) but has no callers at all — direct, tail-call or via a pointer
  table. Initialize emits it inline with `(0xC0, 0x00)`.

> A reading of 24858 resyncs during one sweep looked like audio breaking frame
> sync. It was not: the sweep had wedged the device. After a USB reset the
> baseline is 456 frames with 1 resync. Reset before trusting any sweep result.

### 10.2 The unexplored surface: `bRequest 0xC3` (GSPI)

`0xC3` is a generic SPI interface — `CY3014_GSPI_INIT`, `GSPI_READ_WORD`,
`GSPI_READ_WORD_SW`, `GSPI_WRITE_WORD`, 14 call sites — and
`CY3014_OnControlPanelAnalysisThread` reads `wIndex 0x0006` and `0x001f` from it
*during capture*. We had never issued it. Reading it back (vendor IN, `0xC3`,
`wValue 0x0000`, `wIndex` = register, 2 bytes) a real register file answers:

```
0x00-0x02 0000   0x03 ff01   0x04 ffff   0x05-0x06 0000
0x07 ff0f        0x08 ffff   0x09-0x0a 0000   0x0b ff07
0x0c and up ffff (unmapped)
```

Twelve live registers, with the three interesting ones four apart.

> **Do not write here blind.** The board carries a Winbond W25Q32JV holding the
> FX3 boot image, and `CY3014_GSPI_*` is how this driver family reaches SPI
> flash on other products. A wrong write corrupts the boot image and bricks the
> device permanently — not a replug. Establish what is on the bus for PID
> `005e` before writing anything.
