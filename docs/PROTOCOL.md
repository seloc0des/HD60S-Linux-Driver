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
| ITE **IT6802E** | HDMI receiver (HDMI → parallel video) |
| ITE **IT66121FN** | HDMI transmitter (passthrough out) |
| Altera **MAX II** CPLD | formats parallel video into the CYUSB3014 GPIF-II bus |
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

## 4. SuperSpeed personality [UNKNOWN]

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

**Caveat, and it matters.** In `CY3014_HwInitialize` (VMA `0x6fb10`) the call to
`CheckIsMCUExist` is guarded:

```
cmp WORD PTR [rax+0x8], 0xfd9     ; idVendor
jne skip
cmp WORD PTR [rax+0xa], 0x76      ; idProduct == 0x0076 (Rev. 4)
jne skip
call CheckIsMCUExist
```

So the vendor driver runs this handshake **only for PID `0x0076`**, not for our
`0x005e`. We ran it anyway and it had a real effect — the writes were accepted,
`0x39` began responding, and `0x34` changed `0x00` to `0x01`.

Two readings are possible and we cannot yet distinguish them: the sequence is
generic to the register block and Rev. 2 simply does not need it during the
vendor's init, or Rev. 2 reaches the same state through a different path.
[UNKNOWN] Do not assume we have replicated the vendor init for this device — we
have only shown that this particular sequence works on it.

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

#### Other request codes seen in the driver

| bRequest | Role |
|---|---|
| `0xC1` | register read/write (above) |
| `0xC5`, `0xC6` | MCU access, used by `CY3014_MCU_Exist` |
| `0xA0` | EZ-USB data (`CY3014_Get/SetEzusbData`) — the 255-byte window in 5.1 |

### 5.10 Still unknown

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
