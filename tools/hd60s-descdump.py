#!/usr/bin/env python3
"""Dump and decode USB descriptors for the Elgato HD60 S straight from sysfs.

No libusb, no root, no deps. The point is to capture the device's *SuperSpeed*
personality the moment it enumerates on a USB 3.0 link -- the interface layout
differs completely from the USB 2.0 fallback, and we design against the former.

Usage:
    ./hd60s-descdump.py                 # find the HD60 S automatically
    ./hd60s-descdump.py --vid 0fd9 --pid 005e
    ./hd60s-descdump.py --json out.json # machine-readable, for diffing runs
"""

import argparse
import json
import os
import sys

SYSFS = "/sys/bus/usb/devices"

DT = {
    0x01: "DEVICE", 0x02: "CONFIG", 0x03: "STRING", 0x04: "INTERFACE",
    0x05: "ENDPOINT", 0x06: "DEVICE_QUALIFIER", 0x07: "OTHER_SPEED_CONFIG",
    0x0B: "INTERFACE_ASSOC", 0x0F: "BOS", 0x10: "DEVICE_CAPABILITY",
    0x30: "SS_EP_COMPANION", 0x31: "SSP_ISOC_EP_COMPANION",
}

XFER = {0: "Control", 1: "Isochronous", 2: "Bulk", 3: "Interrupt"}
SYNC = {0: "None", 1: "Async", 2: "Adaptive", 3: "Sync"}
USAGE = {0: "Data", 1: "Feedback", 2: "Implicit feedback", 3: "Reserved"}

# speed string -> human label
SPEEDS = {
    "1.5": "Low (1.5M)", "12": "Full (12M)", "480": "High (480M)",
    "5000": "SuperSpeed (5G)", "10000": "SuperSpeed+ (10G)",
    "20000": "SuperSpeed+ x2 (20G)",
}


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def find_devices(vid, pid):
    """Walk sysfs for nodes matching vid:pid. Returns list of sysfs paths."""
    hits = []
    if not os.path.isdir(SYSFS):
        return hits
    for name in sorted(os.listdir(SYSFS)):
        node = os.path.join(SYSFS, name)
        try:
            with open(os.path.join(node, "idVendor")) as f:
                v = f.read().strip()
            with open(os.path.join(node, "idProduct")) as f:
                p = f.read().strip()
        except OSError:
            continue  # interfaces and root hubs lack these; skip
        if v.lower() == vid.lower() and p.lower() == pid.lower():
            hits.append(node)
    return hits


def attr(node, name, default=""):
    try:
        with open(os.path.join(node, name)) as f:
            return f.read().strip()
    except OSError:
        return default


def parse_descriptors(blob):
    """Split the raw sysfs descriptor blob into a flat list of TLV records."""
    out, off = [], 0
    while off + 2 <= len(blob):
        blen = blob[off]
        if blen < 2 or off + blen > len(blob):
            break  # truncated or garbage; stop rather than guess
        out.append((blob[off + 1], blob[off:off + blen]))
        off += blen
    return out


def decode(dtype, d):
    """Return (label, list-of-(key,value)) for one descriptor."""
    name = DT.get(dtype, "UNKNOWN(0x%02x)" % dtype)
    f = []
    if dtype == 0x01:
        f = [("bcdUSB", "%x.%02x" % (d[3], d[2])),
             ("bDeviceClass", d[4]), ("bDeviceSubClass", d[5]),
             ("bDeviceProtocol", d[6]), ("bMaxPacketSize0", d[7]),
             ("idVendor", "0x%04x" % u16(d, 8)),
             ("idProduct", "0x%04x" % u16(d, 10)),
             ("bcdDevice", "%x.%02x" % (d[13], d[12])),
             ("bNumConfigurations", d[17])]
    elif dtype == 0x02:
        f = [("wTotalLength", u16(d, 2)), ("bNumInterfaces", d[4]),
             ("bConfigurationValue", d[5]),
             ("bmAttributes", "0x%02x" % d[7]),
             ("MaxPower", "%d mA" % (d[8] * 2))]
    elif dtype == 0x04:
        f = [("bInterfaceNumber", d[2]), ("bAlternateSetting", d[3]),
             ("bNumEndpoints", d[4]), ("bInterfaceClass", d[5]),
             ("bInterfaceSubClass", d[6]), ("bInterfaceProtocol", d[7])]
    elif dtype == 0x05:
        addr, attrs, mps = d[2], d[3], u16(d, 4)
        # bits 12:11 are additional transactions per microframe (HS iso/int only)
        mult = ((mps >> 11) & 3) + 1
        size = mps & 0x7FF
        f = [("bEndpointAddress", "0x%02x (EP %d %s)" %
              (addr, addr & 0x0F, "IN" if addr & 0x80 else "OUT")),
             ("TransferType", XFER.get(attrs & 3, "?")),
             ("SyncType", SYNC.get((attrs >> 2) & 3, "?")),
             ("UsageType", USAGE.get((attrs >> 4) & 3, "?")),
             ("wMaxPacketSize", "0x%04x (%dx%d = %d B)" %
              (mps, mult, size, mult * size)),
             ("bInterval", d[6])]
    elif dtype == 0x30:
        f = [("bMaxBurst", "%d (=%d pkts/burst)" % (d[2], d[2] + 1)),
             ("bmAttributes", "0x%02x" % d[3]),
             ("wBytesPerInterval", u16(d, 4))]
    elif dtype == 0x0B:
        f = [("bFirstInterface", d[2]), ("bInterfaceCount", d[3]),
             ("bFunctionClass", d[4])]
    if not f:
        f = [("raw", d.hex())]
    return name, f


def bandwidth_note(records):
    """Rough per-alt-setting bandwidth so the USB2 vs USB3 gap is obvious."""
    notes, cur_if, cur_alt = [], None, None
    for dtype, d in records:
        if dtype == 0x04:
            cur_if, cur_alt = d[2], d[3]
        elif dtype == 0x05 and cur_if is not None:
            attrs, mps = d[3], u16(d, 4)
            ttype = attrs & 3
            mult = ((mps >> 11) & 3) + 1
            size = (mps & 0x7FF) * mult
            interval = d[6] or 1
            if ttype == 1:      # iso: one service per (2^(bInterval-1)) microframes
                per_s = 8000 / (2 ** (interval - 1))
                bps = size * per_s
                notes.append((cur_if, cur_alt, d[2], "iso",
                              "%.1f MB/s (%.0f Mbit/s)" % (bps / 1e6, bps * 8 / 1e6)))
            elif ttype == 2:
                notes.append((cur_if, cur_alt, d[2], "bulk",
                              "burst-limited, no reservation"))
    return notes


def main():
    ap = argparse.ArgumentParser(description="Decode HD60 S USB descriptors from sysfs")
    ap.add_argument("--vid", default="0fd9")
    ap.add_argument("--pid", default="005e")
    ap.add_argument("--json", metavar="FILE", help="also write decoded output as JSON")
    args = ap.parse_args()

    nodes = find_devices(args.vid, args.pid)
    if not nodes:
        print("No %s:%s device found. Is it plugged in?" % (args.vid, args.pid),
              file=sys.stderr)
        return 1

    doc = []
    for node in nodes:
        speed = attr(node, "speed")
        label = SPEEDS.get(speed, "%s M" % speed)
        print("=" * 68)
        print("Device : %s" % os.path.basename(node))
        print("Product: %s %s" % (attr(node, "manufacturer"), attr(node, "product")))
        print("Serial : %s" % attr(node, "serial"))
        print("Speed  : %s" % label)
        print("USB ver: %s   bcdDevice(fw): %s" %
              (attr(node, "version"), attr(node, "bcdDevice")))
        print("Power  : %s" % attr(node, "bMaxPower"))
        if speed in ("480", "12", "1.5"):
            print("  !! NOT SuperSpeed -- this is the USB 2.0 fallback personality.")
            print("     Bulk capture endpoints appear only on a 5G/10G link.")
        print("=" * 68)

        try:
            with open(os.path.join(node, "descriptors"), "rb") as f:
                blob = f.read()
        except OSError as e:
            print("cannot read descriptors: %s" % e, file=sys.stderr)
            continue

        records = parse_descriptors(blob)
        dev = {"node": os.path.basename(node), "speed": speed,
               "serial": attr(node, "serial"), "descriptors": []}

        indent = {0x01: 0, 0x02: 1, 0x04: 2, 0x05: 3, 0x30: 4, 0x31: 4}
        for dtype, d in records:
            name, fields = decode(dtype, d)
            pad = "  " * indent.get(dtype, 3)
            print("%s%s" % (pad, name))
            for k, v in fields:
                print("%s  %-22s %s" % (pad, k, v))
            dev["descriptors"].append(
                {"type": name, "raw": d.hex(),
                 "fields": {k: v for k, v in fields}})

        bw = bandwidth_note(records)
        if bw:
            print("\n--- streaming endpoint bandwidth ---")
            for iface, alt, ep, kind, note in bw:
                print("  if%d alt%d ep0x%02x  %-4s  %s" % (iface, alt, ep, kind, note))
            print("  (1080p60 uncompressed YUY2 needs ~3000 Mbit/s)")
        doc.append(dev)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(doc, f, indent=2)
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
