# Register baselines

Captured on a real HD60 S (`0fd9:005e`, serial `0314BC38FC`) after the MCU
handshake, over the MCU-proxied I2C path. Reference for spotting what changes
when the device is configured or when a source is connected.

Reproduce with:

```bash
./hd60s-reg --handshake --i2c-dump 0x9a     # or 0x98 / 0x94 / 0x9c
./hd60s-reg --mst-dump 0x01                 # receiver, page-selected
```

## I2C bus

Seven addresses respond; everything else reads `0xff` (idle bus).

| Slave | Chip |
|---|---|
| `0x94` | CS53L21 audio codec |
| `0x98` | CPLD (Altera MAX II) |
| `0x9a` | IT6612 HDMI transmitter — passthrough |
| `0x9c` | MST3367 HDMI receiver |
| `0xa4`, `0xaa`, `0xd4` | unidentified; `0xaa` is used by the firmware register poll |

## IT6612 HDMI transmitter — slave `0x9a`

```
00: 54 49 12 16 05 60 00 00 10 b2 f8 37 00 40 8c 18
10: 96 f1 6a ee 04 f8 83 a7 00 74 e0 03 03 00 ff 00
```

`0x00`–`0x01` = `0x4954`, ITE Tech (ASCII `"IT"`). `0x02`–`0x03` = `0x1612`,
IT6612. Register `0x0e` varies between reads, so it is live status.

## CPLD — slave `0x98`

```
00: 1d 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00
10: ff ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

## CS53L21 audio codec — slave `0x94`

```
00: 00 d9 00 a0 0e 00 a0 00 60 42 00 00 00 00 00 00
10: 00 00 00 00 00 88 00 00 00 00 7f 00 00 3f 00 00
```

## MST3367 HDMI receiver — slave `0x9c`, page `0x01`

> **STALE.** This dump was taken before the addressing fix in PROTOCOL.md 5.13.
> It was captured with the generic command-`0x66` form, which silently ignores
> page selects, so the window it shows is not page 1. Page addressing has since
> been verified correct by writing a distinct marker to each page and reading it
> back from all four — each page holds only its own marker. Re-capture before
> relying on the numbers below.

With a live HDMI source connected (Nintendo Switch, docked, powered on):

```
00: 02 61 00 01 01 00 08 04 03 28 00 00 00 00 00 00
10: 00 93 5d 00 00 00 05 c0 c0 ff ff fc 1a 00 00 00
20: 00 00 26 00 00 a2 00 00 ce 07 00 00 00 00 a1 00
30: 10 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00
40: 5a 44 02 00 18 00 00 00 d0 28 00 04 00 00 00 00
50: 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00
60: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
...  (0x60-0xff read as 00, except 0x98-0x9c = 40 08 40 40)
```

### What changes when a source is connected

Diffed against the same page with HDMI unplugged:

| Reg | No signal | Connected |
|---|---|---|
| `0x0e`–`0x0f` | `90 15` | `00 00` |
| `0x10` | `06` | `00` |
| `0x28`–`0x29` | `06 00` | `ce 07` |
| `0x34` | `05` | `00` |

So the receiver does react to the source. But the driver's own mode test —
`(page-reg 0x55 & 0x3c) == 0x3c` — still fails, because `0x55` reads `0x01`.
The chip sees something and has not locked, which is expected while it remains
uninitialised.
