#!/usr/bin/env python3
"""Extract the MST3367 HDMI-receiver init sequence from the vendor driver.

MST3367_HwInitialize is a flat run of calls to the page-register writer at VMA
0x65080, whose x64 arguments are:

    rcx = device
    dl  = page      (register 0x00 on slave 0x9c selects it)
    r8b = register
    r9b = value

so each call site is a literal (page, reg, value) triple. Values loaded from
registers rather than immediates are reported as '??' rather than guessed.

Usage:
    ./mst3367-init.py                       # MST3367_HwInitialize
    ./mst3367-init.py --func MST3367_HDMI_INIT
    ./mst3367-init.py --emit-c              # as a C table
"""

import argparse
import json
import os
import re
import subprocess
import sys

TEXT_VMA = 0x10370
WRITER = 0x65080
WINDOW = 12


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--sys', default='CY3014.X64.SYS')
    ap.add_argument('--syms', default='pdb_syms.json')
    ap.add_argument('--func', default='MST3367_HwInitialize')
    ap.add_argument('--len', dest='length', default='0x900')
    ap.add_argument('--emit-c', action='store_true')
    args = ap.parse_args()

    if not os.path.exists(args.syms):
        sys.exit('need %s (run pdbsyms.py first)' % args.syms)
    syms = json.load(open(args.syms))

    hits = [(k, v) for k, v in syms.items()
            if args.func in k and v[0] == 1 and not k.startswith('??_C')]
    if not hits:
        sys.exit('no .text symbol matching %r' % args.func)
    hits.sort(key=lambda kv: len(kv[0]))
    name, (_seg, off) = hits[0]
    start = TEXT_VMA + off
    length = int(args.length, 0)

    out = subprocess.run(
        ['objdump', '-d', '--no-show-raw-insn', '-M', 'intel',
         '--start-address', hex(start), '--stop-address', hex(start + length),
         args.sys],
        capture_output=True, text=True).stdout

    insns = []
    for line in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+(.*?)\s*$', line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2)))

    # dl = page, r8b = reg, r9b = value; xor rNd,rNd means zero
    pats = {
        'page':  re.compile(r'^mov\s+dl,0x([0-9a-f]+)$'),
        'reg':   re.compile(r'^mov\s+r8b,0x([0-9a-f]+)$'),
        'val':   re.compile(r'^mov\s+r9b,0x([0-9a-f]+)$'),
        'val0':  re.compile(r'^xor\s+r9d,r9d$'),
        'reg0':  re.compile(r'^xor\s+r8d,r8d$'),
        'page0': re.compile(r'^xor\s+edx,edx$'),
    }

    seq = []
    for i, (addr, ins) in enumerate(insns):
        m = re.match(r'^call\s+0x([0-9a-f]+)$', ins)
        if not m or int(m.group(1), 16) != WRITER:
            continue
        a = {}
        for j in range(i - 1, max(-1, i - WINDOW), -1):
            _, p = insns[j]
            if p.startswith('call'):
                break
            for key, rx in pats.items():
                base = key.rstrip('0')
                if base in a:
                    continue
                mm = rx.match(p)
                if mm:
                    a[base] = int(mm.group(1), 16) if mm.groups() else 0
        seq.append((addr, a.get('page'), a.get('reg'), a.get('val')))

    def f(v):
        return '0x%02x' % v if v is not None else '??'

    if args.emit_c:
        print('/* %s -- extracted from the vendor driver, %d writes */'
              % (name, len(seq)))
        print('static const struct { uint8_t page, reg, val; } mst3367_init[] = {')
        for _addr, p, r, v in seq:
            if None in (p, r, v):
                print('\t/* skipped, computed at runtime: page=%s reg=%s val=%s */'
                      % (f(p), f(r), f(v)))
                continue
            print('\t{ 0x%02x, 0x%02x, 0x%02x },' % (p, r, v))
        print('};')
        return

    print('%s: %d register write(s)\n' % (name, len(seq)))
    known = sum(1 for _a, p, r, v in seq if None not in (p, r, v))
    for addr, p, r, v in seq:
        print('  0x%08x  page %s  reg %s = %s' % (addr, f(p), f(r), f(v)))
    print('\n  %d/%d fully literal' % (known, len(seq)))


if __name__ == '__main__':
    main()
