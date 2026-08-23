#!/usr/bin/env python3
"""Map every vendor command the CY3014 driver can issue.

Finds each call to CUSBBaseDevice::SendVendorCommandRequest and walks backwards
through the basic block recovering whichever arguments were set from constants.

The x64 calling convention puts them at:
    rcx = this
    dl  = bRequest        (mov dl, imm8)
    r8w = wValue          (mov r8d/r8w, imm  /  xor r8d,r8d)
    r9w = wIndex          (mov r9d/r9w, imm  /  xor r9d,r9d)
    [rsp+0x20] = buffer, [rsp+0x28] = length, [rsp+0x30] = direction

Direction 1 is IN (device-to-host), 0 is OUT. Arguments computed at runtime show
as '?' -- that is honest, not a gap to guess at.

Usage: ./vendor-cmdmap.py [--sys CY3014.X64.SYS] [--syms pdb_syms.json]
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

TEXT_VMA = 0x10370
WINDOW = 40          # instructions to look back from the call


def enclosing(addr, sorted_syms):
    """Nearest symbol at or before addr."""
    lo, hi = 0, len(sorted_syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if sorted_syms[mid][0] <= addr:
            best = sorted_syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--sys', default='CY3014.X64.SYS')
    ap.add_argument('--syms', default='pdb_syms.json')
    args = ap.parse_args()

    if not os.path.exists(args.syms):
        sys.exit('need %s (run pdbsyms.py first)' % args.syms)
    syms = json.load(open(args.syms))
    text = {TEXT_VMA + o: k for k, (s, o) in syms.items() if s == 1}
    sorted_syms = sorted(text.items())

    target = [a for a, k in text.items() if 'SendVendorCommandRequest' in k]
    if not target:
        sys.exit('SendVendorCommandRequest not in symbols')
    target = target[0]
    print('SendVendorCommandRequest at 0x%x\n' % target)

    out = subprocess.run(
        ['objdump', '-d', '--no-show-raw-insn', '-M', 'intel', args.sys],
        capture_output=True, text=True).stdout

    insns = []
    for line in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+(.*?)\s*$', line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2)))

    # patterns for the argument registers
    pats = {
        'req':   re.compile(r'^mov\s+dl,0x([0-9a-f]+)$'),
        'val':   re.compile(r'^mov\s+r8[dw],0x([0-9a-f]+)$'),
        'val0':  re.compile(r'^xor\s+r8d,r8d$'),
        'idx':   re.compile(r'^mov\s+r9[dw],0x([0-9a-f]+)$'),
        'idx0':  re.compile(r'^xor\s+r9d,r9d$'),
        'len':   re.compile(r'^mov\s+DWORD PTR \[rsp\+0x28\],0x([0-9a-f]+)$'),
        'len0':  re.compile(r'^mov\s+DWORD PTR \[rsp\+0x28\],e[a-z]{2}$'),
        'dir':   re.compile(r'^mov\s+BYTE PTR \[rsp\+0x30\],0x([0-9a-f]+)$'),
        'dir0':  re.compile(r'^mov\s+BYTE PTR \[rsp\+0x30\],bpl$'),
    }

    found = []
    for i, (addr, ins) in enumerate(insns):
        m = re.match(r'^call\s+0x([0-9a-f]+)$', ins)
        if not m or int(m.group(1), 16) != target:
            continue
        arg = {}
        for j in range(i - 1, max(-1, i - WINDOW), -1):
            _, prev = insns[j]
            if prev.startswith('call'):
                break                       # different call's setup
            for key, rx in pats.items():
                base = key.rstrip('0')
                if base in arg:
                    continue
                mm = rx.match(prev)
                if mm:
                    arg[base] = int(mm.group(1), 16) if mm.groups() else 0
        sym = enclosing(addr, sorted_syms)
        found.append((addr, sym[1] if sym else '?', arg))

    print('%d call site(s)\n' % len(found))
    by_req = defaultdict(list)
    for addr, sym, a in found:
        by_req[a.get('req')].append((addr, sym, a))

    for req in sorted(by_req, key=lambda r: (r is None, r)):
        label = '0x%02x' % req if req is not None else '(computed)'
        print('bRequest %s  -- %d site(s)' % (label, len(by_req[req])))
        for addr, sym, a in by_req[req][:12]:
            def f(k, w=4):
                v = a.get(k)
                return ('0x%0*x' % (w, v)) if v is not None else '?'
            print('    0x%08x  wValue=%-6s wIndex=%-6s len=%-6s dir=%-3s  %s'
                  % (addr, f('val'), f('idx'), f('len', 2), f('dir', 1),
                     re.sub(r'^\?|@@.*$', '', sym)[:52]))
        if len(by_req[req]) > 12:
            print('    ... %d more' % (len(by_req[req]) - 12))
        print()


if __name__ == '__main__':
    main()
