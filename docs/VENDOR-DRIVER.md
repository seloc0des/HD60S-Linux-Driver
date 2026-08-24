# Recovering the protocol from the vendor driver

The HD60 S protocol was not reverse-engineered by probing the device - probing
found almost nothing, because the register write encodes its value in the high
byte of `wValue` and sends no data stage, which no blind sweep would hit.

It came from Elgato's own Windows driver, which ships **with debug symbols**.

## Obtaining it

Official Elgato URL, no third-party mirror:

```bash
curl -L -o gc.msi \
  https://edge.elgato.com/egc/windows/egcw/3.70/final/GameCaptureSetup_3.70.56.3056_x64.msi
# sha256 403b03518ac5170e52238fdbc5ce757b9901b16e123101ec5db2c46d640ae949
7z x gc.msi -o./x
7z x x/DRVInstaller.exe -o./x/drv
```

That yields, among ~69 driver files:

| File | Role |
|---|---|
| `GameCaptureHD60S.inf` | binds `USB\VID_0FD9&PID_005E` = "GameCapture HD60 S (Rev. 2)" |
| `CY3014.X64.SYS` | the kernel driver (4.0 MB) - CY3014 is the CYUSB**3014** |
| `CY3014.X64.PDB` | **debug symbols, 3.5 MB** |
| `CY3014.*.AX` | DirectShow filters |

The INF confirms the whole family: PID `004F`, `005E` (Rev. 2 - this device),
`0074` (Rev. 3), `0076` (Rev. 4).

## Symbols

The PDB is MSF 7.00. A full parser is not needed: scanning for `S_PUB32`
records (`rectyp 0x110E`, then `flags:u32, off:u32, seg:u16, name:asciiz`)
recovers thousands of names with section-relative addresses.

`tools/pdbsyms.py` does this and writes `pdb_syms.json`.

Segment 1 is `.text`. The mapping to VMA is **`vma = 0x10370 + off`**, calibrated
against `SendVendorCommandRequest`, which lands at `0x13fd0`. (`objdump -h`
reports `.text` VMA `0x10380`; the extra `0x10` shows up as misdecoded
prologues if you use it directly.)

## Disassembling

```bash
objdump -d --no-show-raw-insn -M intel \
  --start-address 0x19f80 --stop-address 0x1a0a0 CY3014.X64.SYS
```

`tools/disasm-vendor.py` wraps this, resolving symbol names and annotating call
targets.

## The functions that mattered

| Symbol | VMA | What it gave us |
|---|---|---|
| `CUSBBaseDevice::SendVendorCommandRequest` | `0x13fd0` | URB layout, `URB_FUNCTION_VENDOR_DEVICE` (`0x17`) |
| *(thunk)* | `0x62c00` | register **write**: `0xC1`, `wValue=(val<<8)\|reg`, no data |
| *(thunk)* | `0x62c40` | register **read**: `0xC1`, `wValue=reg`, 1 byte IN |
| `CheckIsMCUExist` | `0x19f80` | **the MCU-presence handshake** |
| `CY3014_MCU_Exist` | `0x62e20` | MCU access via `0xC5` / `0xC6` |
| `CY3014_HwInitialize` | `0x6fb10` | full hardware init (not yet transcribed) |

## Legal note

This is interoperability reverse engineering: reading a publicly distributed
driver to make hardware the user owns work on their own operating system. No
vendor code is copied into this project - only the wire protocol is described,
which is not itself copyrightable.
