#!/usr/bin/env python3
"""Recover public symbols (name -> section:offset) from a Microsoft PDB.

Not a real PDB parser. PDBs are MSF container files with paged streams, and a
proper reader is a lot of work -- but S_PUB32 records are self-describing and
small enough to survive a linear scan, which is plenty for locating functions
to disassemble.

S_PUB32 record layout:
    reclen : u16   (length after this field)
    rectyp : u16   == 0x110E
    flags  : u32
    off    : u32   offset within the section
    seg    : u16   1-based section index
    name   : NUL-terminated ASCII

Usage:
    ./pdbsyms.py CY3014.X64.PDB [-o pdb_syms.json]
"""

import argparse
import json
import re
import struct
import sys

S_PUB32 = 0x110E
NAME_RE = re.compile(rb'^[A-Za-z_?$@][\x20-\x7e]{2,}$')


def scan(data):
    syms, i, n = {}, 0, len(data)

    while i < n - 16:
        if data[i + 2] | (data[i + 3] << 8) != S_PUB32:
            i += 1
            continue

        reclen = data[i] | (data[i + 1] << 8)
        if not (14 <= reclen <= 4096) or i + 2 + reclen > n:
            i += 1
            continue

        _flags, off, seg = struct.unpack_from('<IIH', data, i + 4)
        if not (1 <= seg <= 32) or off >= 0x8000000:
            i += 1
            continue

        end = data.find(b'\0', i + 14, i + 2 + reclen)
        if end <= i + 14 or not NAME_RE.match(data[i + 14:end]):
            i += 1
            continue

        syms.setdefault(data[i + 14:end].decode('ascii'), (seg, off))
        i += 2 + reclen

    return syms


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('pdb')
    ap.add_argument('-o', '--out', default='pdb_syms.json')
    ap.add_argument('-g', '--grep', help='only print symbols containing this')
    args = ap.parse_args()

    with open(args.pdb, 'rb') as f:
        data = f.read()
    if not data.startswith(b'Microsoft C/C++'):
        print('warning: does not look like a PDB', file=sys.stderr)

    syms = scan(data)
    print('recovered %d public symbols' % len(syms))

    with open(args.out, 'w') as f:
        json.dump({k: list(v) for k, v in syms.items()}, f)
    print('wrote %s' % args.out)

    if args.grep:
        for k, (s, o) in sorted(syms.items()):
            if args.grep in k:
                print('  seg%-3d off 0x%08x  %s' % (s, o, k))


if __name__ == '__main__':
    main()
