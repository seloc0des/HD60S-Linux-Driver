#!/usr/bin/env python3
"""Diagnose why the HD60 S isn't on a SuperSpeed link.

The HD60 S needs 5 Gbit/s. On a USB 2.0 link it falls back to a personality
whose isochronous endpoint caps out around 197 Mbit/s -- roughly a tenth of
what uncompressed 1080p60 needs -- and the bulk capture endpoints don't exist
at all. This tells you whether the device, the cable, or the port is at fault.

No dependencies, no root.
"""

import os
import sys

SYSFS = "/sys/bus/usb/devices"
VID, PID = "0fd9", "005e"


def rd(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def rdb(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return b""


def find_device():
    for name in sorted(os.listdir(SYSFS)):
        node = os.path.join(SYSFS, name)
        if rd(os.path.join(node, "idVendor")).lower() == VID and \
           rd(os.path.join(node, "idProduct")).lower() == PID:
            return node
    return None


def parse_bos(blob):
    """-> (advertises_superspeed, functionality_support_speed, caps)"""
    ss, fs, caps = False, None, []
    o = 0
    while o + 2 <= len(blob):
        ln = blob[o]
        if ln < 2 or o + ln > len(blob):
            break
        t = blob[o + 1]
        if t == 0x10:  # DEVICE CAPABILITY
            ct = blob[o + 2]
            caps.append(ct)
            if ct == 0x03 and ln >= 10:  # SUPERSPEED_USB
                speeds = blob[o + 4] | (blob[o + 5] << 8)
                ss = bool(speeds & 0x08)
                fs = blob[o + 6]
        o += ln
    return ss, fs, caps


def port_peer_state(node):
    """For the port the device sits on, report its SuperSpeed peer.

    A USB 3 receptacle is two logical ports: the USB 2 half and the SuperSpeed
    half, linked by a 'peer' symlink. If the USB 2 half is 'configured' while
    the peer is 'not attached', the SS differential pairs aren't connecting --
    that is cable or connector, not the controller.
    """
    base = os.path.basename(node)            # eg "3-1"
    if "-" not in base:
        return None
    bus, _, port = base.partition("-")
    if not port.isdigit():
        return None                           # device sits behind a hub

    root = os.path.realpath(os.path.join(SYSFS, "usb" + bus))
    pdir = os.path.join(root, "%s-0:1.0" % bus, "usb%s-port%s" % (bus, port))
    if not os.path.isdir(pdir):
        return None

    info = {"port": os.path.basename(pdir),
            "state": rd(os.path.join(pdir, "state"), "?"),
            "peer": None, "peer_state": None}
    peer = os.path.realpath(os.path.join(pdir, "peer")) \
        if os.path.islink(os.path.join(pdir, "peer")) else None
    if peer:
        info["peer"] = os.path.basename(peer)
        info["peer_state"] = rd(os.path.join(peer, "state"), "?")
    return info


def all_ss_ports():
    """Every SuperSpeed port on the host and whether anything is on it."""
    out = []
    for name in sorted(os.listdir(SYSFS)):
        if not name.startswith("usb"):
            continue
        node = os.path.join(SYSFS, name)
        try:
            if int(rd(os.path.join(node, "speed"), "0")) < 5000:
                continue
        except ValueError:
            continue
        bus = name[3:]
        root = os.path.realpath(node)
        pbase = os.path.join(root, "%s-0:1.0" % bus)
        if not os.path.isdir(pbase):
            continue
        for d in sorted(os.listdir(pbase)):
            if not d.startswith("usb%s-port" % bus):
                continue
            out.append((d, rd(os.path.join(pbase, d, "state"), "?")))
    return out


def superspeed_roots():
    """Root hubs capable of 5G or better, with their PCI parent."""
    out = []
    for name in sorted(os.listdir(SYSFS)):
        if not name.startswith("usb"):
            continue
        node = os.path.join(SYSFS, name)
        try:
            spd = int(rd(os.path.join(node, "speed"), "0"))
        except ValueError:
            continue
        if spd >= 5000:
            pci = os.path.basename(os.path.realpath(os.path.join(node, "..")))
            nports = rd(os.path.join(node, "maxchild"), "?")
            kids = [d for d in os.listdir(SYSFS)
                    if d.startswith(name[3:] + "-") and "-" in d and ":" not in d]
            out.append((name, spd, pci, nports, len(kids)))
    return out


FUNC = {1: "Full speed", 2: "High speed", 3: "SuperSpeed"}


def main():
    node = find_device()
    if not node:
        print("HD60 S (%s:%s) is not plugged in." % (VID, PID))
        return 2

    speed = rd(os.path.join(node, "speed"))
    print("=" * 62)
    print("HD60 S at %s  --  link speed %s Mbit/s" % (os.path.basename(node), speed))
    print("=" * 62)

    ss, fs, caps = parse_bos(rdb(os.path.join(node, "bos_descriptors")))

    print("\nDevice capability (from its own BOS descriptor):")
    if not caps:
        print("  no BOS exposed -- cannot confirm SuperSpeed capability")
    else:
        print("  advertises SuperSpeed 5G : %s" % ("YES" if ss else "NO"))
        if fs is not None:
            print("  full functionality needs : %s" % FUNC.get(fs, "speed code %d" % fs))

    try:
        linked = int(speed)
    except ValueError:
        linked = 0

    print("\nVerdict:")
    if linked >= 5000:
        print("  OK -- SuperSpeed link. This is the personality the driver targets.")
        return 0

    if ss:
        print("  The DEVICE is fine: it advertises SuperSpeed and is asking for it.")
        print("  The link is still training at %s Mbit/s, so the fault is between" % speed)
        print("  the device and the host controller -- cable or port.")
        if fs == 3:
            print("  The device states full functionality requires SuperSpeed, so")
            print("  capture is not expected to work in this mode at all.")
    else:
        print("  Device does not advertise SuperSpeed. Either its USB 3 side is")
        print("  faulty, or it never negotiated a link that would expose it.")

    pp = port_peer_state(node)
    if pp:
        print("\nPhysical port analysis:")
        print("  USB 2 half   : %s  state=%s" % (pp["port"], pp["state"]))
        if pp["peer"]:
            print("  SuperSpeed   : %s  state=%s" % (pp["peer"], pp["peer_state"]))
            if pp["peer_state"] == "not attached":
                print("  -> This receptacle IS wired for USB 3, but its SuperSpeed")
                print("     half sees nothing while the USB 2 half is connected.")
                print("     The SS differential pairs are not making a link:")
                print("     that is the cable or a connector, not the controller.")
        else:
            print("  SuperSpeed   : NO PEER -- this receptacle is USB 2 only.")
            print("  -> Move to a port that has a SuperSpeed peer.")

    ss_ports = all_ss_ports()
    live = [p for p, st in ss_ports if st == "configured"]
    if live:
        print("\n  SuperSpeed ports currently carrying a link: %s" % ", ".join(live))
        print("  Those are proven-good. Try the HD60 S directly in one of them")
        print("  (unplug whatever is there first) to rule the port out entirely.")

    print("\nSuperSpeed-capable root hubs on this host:")
    roots = superspeed_roots()
    if not roots:
        print("  none found -- this machine has no USB 3 controller?!")
    for name, spd, pci, nports, kids in roots:
        print("  %-6s %5d Mbit/s  %d port(s)  pci %s  (%d device(s) attached)"
              % (name, spd, int(nports) if nports.isdigit() else 0, pci, kids))
    print("\n  A SuperSpeed link shows up as a device on one of THOSE buses.")
    print("  If the HD60 S keeps landing on a 480M bus, its SS pairs aren't")
    print("  reaching the host.")

    print("""
Next step -- isolate cable vs port vs device:

  1. Take a known-good USB 3 device (a USB 3 flash drive is ideal).
  2. Plug it into the SAME port with the SAME cable.
     -> also links at 480M : the cable or port is the problem.
     -> links at 5000M     : cable and port are fine, so the HD60 S's
                             SuperSpeed side is the suspect.

Most USB-C cables sold are USB 2.0 only, and many are charge-only. Use the
cable that shipped with the device, into a SuperSpeed USB-A port (usually
blue, or marked SS).
""")
    return 1


if __name__ == "__main__":
    sys.exit(main())
