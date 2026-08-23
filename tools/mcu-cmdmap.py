#!/usr/bin/env python3
"""Map the MCU proxy commands the CY3014 driver issues.

The MCU transports (VMA 0x627f0 write, 0x62880 read) take the command byte in
dl and a stack buffer in r8. Callers build the packet with a run of
`mov BYTE PTR [rsp+N], imm8` immediately before, so walking backwards from each
call recovers most packets literally.

Bytes written from registers rather than immediates show as '??' -- honest, not
a gap to fill by guessing.

Usage: ./mcu-cmdmap.py [--sys CY3014.X64.SYS] [--syms pdb_syms.json]
"""

import argparse
import bisect
import json
import os
import re
import subprocess
import sys

TEXT_VMA = 0x10370
MCU_WRITE = 0x627f0
MCU_READ = 0x62880
WINDOW = 60


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--sys', default='CY3014.X64.SYS')
    ap.add_argument('--syms', default='pdb_syms.json')
    args = ap.parse_args()

    if not os.path.exists(args.syms):
        sys.exit('need %s (run pdbsyms.py first)' % args.syms)
    syms = json.load(open(args.syms))
    addrs = sorted(set((TEXT_VMA + o, k) for k, (s, o) in syms.items() if s == 1))
    keys = [a for a, _ in addrs]

    def enclosing(a):
        i = bisect.bisect_right(keys, a) - 1
        return addrs[i][1] if i >= 0 else '?'

    out = subprocess.run(
        ['objdump', '-d', '--no-show-raw-insn', '-M', 'intel', args.sys],
        capture_output=True, text=True).stdout

    insns = []
    for line in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+(.*?)\s*$', line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2)))

    re_call = re.compile(r'^call\s+0x([0-9a-f]+)$')
    re_cmd = re.compile(r'^mov\s+dl,0x([0-9a-f]+)$')
    re_stk = re.compile(r'^mov\s+BYTE PTR \[rsp\+0x([0-9a-f]+)\],0x([0-9a-f]+)$')
    re_stkr = re.compile(r'^mov\s+BYTE PTR \[rsp\+0x([0-9a-f]+)\],[a-z0-9]+$')
    re_lea = re.compile(r'^lea\s+r8,\[rsp\+0x([0-9a-f]+)\]$')
    re_len = re.compile(r'^mov\s+r9d,0x([0-9a-f]+)$')
    re_lea9 = re.compile(r'^lea\s+r9d,\[r[a-z0-9]+\+0x([0-9a-f]+)\]$')

    sites = []
    for i, (addr, ins) in enumerate(insns):
        m = re_call.match(ins)
        if not m:
            continue
        tgt = int(m.group(1), 16)
        if tgt not in (MCU_WRITE, MCU_READ):
            continue

        cmd = base = length = None
        stack = {}
        for j in range(i - 1, max(-1, i - WINDOW), -1):
            _, p = insns[j]
            if p.startswith('call'):
                break
            mm = re_cmd.match(p)
            if mm and cmd is None:
                cmd = int(mm.group(1), 16)
            mm = re_lea.match(p)
            if mm and base is None:
                base = int(mm.group(1), 16)
            mm = re_len.match(p) or re_lea9.match(p)
            if mm and length is None:
                length = int(mm.group(1), 16)
            mm = re_stk.match(p)
            if mm:
                stack.setdefault(int(mm.group(1), 16), int(mm.group(2), 16))
            else:
                mm = re_stkr.match(p)
                if mm:
                    stack.setdefault(int(mm.group(1), 16), None)
        sites.append((addr, tgt, cmd, base, length, stack, enclosing(addr)))

    print('%d MCU proxy call site(s)\n' % len(sites))
    for addr, tgt, cmd, base, length, stack, sym in sites:
        kind = 'WRITE' if tgt == MCU_WRITE else 'READ '
        pkt = ''
        if base is not None and length:
            b = []
            for k in range(base, base + length):
                v = stack.get(k, 'skip')
                b.append('%02x' % v if isinstance(v, int) else '??')
            pkt = ' '.join(b)
        print('  0x%08x %s cmd=%s len=%s  [%s]' %
              (addr, kind,
               '0x%02x' % cmd if cmd is not None else '?',
               length if length is not None else '?', pkt))
        print('               in %s' % re.sub(r'^\?', '', sym)[:70])


if __name__ == '__main__':
    main()
