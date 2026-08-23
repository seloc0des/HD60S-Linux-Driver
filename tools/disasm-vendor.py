#!/usr/bin/env python3
"""Disassemble a function of CY3014.X64.SYS by PDB symbol name or address.

Needs pdb_syms.json from pdbsyms.py alongside the .sys.

The .text mapping is  vma = TEXT_VMA + section_offset.  TEXT_VMA is calibrated
against SendVendorCommandRequest landing at 0x13fd0; note `objdump -h` reports
.text at 0x10380, and using that directly shifts everything by 0x10 and
misdecodes prologues.

Usage:
    ./disasm-vendor.py CheckIsMCUExist            # by symbol substring
    ./disasm-vendor.py 0x62c00 --len 0x50         # by raw address
"""

import argparse
import json
import os
import re
import subprocess
import sys

TEXT_VMA = 0x10370


def load_syms(path):
    with open(path) as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('target', help='symbol substring, or 0x-prefixed address')
    ap.add_argument('--sys', default='CY3014.X64.SYS')
    ap.add_argument('--syms', default='pdb_syms.json')
    ap.add_argument('--len', dest='length', default='0x100')
    args = ap.parse_args()

    syms = load_syms(args.syms) if os.path.exists(args.syms) else {}
    addr2sym = {TEXT_VMA + o: k for k, (s, o) in syms.items() if s == 1}
    length = int(args.length, 0)

    if args.target.startswith('0x'):
        start = int(args.target, 0)
        label = addr2sym.get(start, '(raw address)')
    else:
        hits = [(k, v) for k, v in syms.items() if args.target in k and v[0] == 1]
        if not hits:
            sys.exit('no .text symbol matching %r' % args.target)
        hits.sort(key=lambda kv: len(kv[0]))       # prefer the least decorated
        label, (_seg, off) = hits[0]
        start = TEXT_VMA + off

    print('== %s' % label)
    print('== VMA 0x%x, %d bytes\n' % (start, length))

    out = subprocess.run(
        ['objdump', '-d', '--no-show-raw-insn', '-M', 'intel',
         '--start-address', hex(start), '--stop-address', hex(start + length),
         args.sys],
        capture_output=True, text=True).stdout

    for line in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+(.*)$', line)
        if not m:
            continue
        addr, ins = int(m.group(1), 16), m.group(2)
        # annotate direct call/jmp targets we have names for
        cm = re.search(r'(?:call|jmp)\s+0x([0-9a-f]+)', ins)
        if cm:
            t = int(cm.group(1), 16)
            if t in addr2sym:
                ins += '   ; <%s>' % addr2sym[t][:58]
        print('  %08x  %s' % (addr, ins))


if __name__ == '__main__':
    main()
