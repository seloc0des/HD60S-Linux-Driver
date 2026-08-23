#!/usr/bin/env python3
"""Disassemble a Mach-O x86-64 binary by symbol name.

Exists because the macOS build of Elgato's Game Capture software ships
libUB530.dylib -- a second, independently written implementation of the same
HD60 S protocol, and one that kept its C++ symbol names. Comparing it against
the Windows driver disambiguates things neither binary settles alone.

Ubuntu's objdump has no Mach-O support and llvm-objdump/otool are absent, so
this parses the container itself and disassembles with capstone.

Usage:
    ./machodis.py FILE --list                     # symbols
    ./machodis.py FILE --list --grep MST3367
    ./machodis.py FILE --sym SetupForVideoFormat  # disassemble by name
    ./machodis.py FILE --addr 0x1234 --len 0x200
"""

import argparse
import re
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_DYLD_EXPORTS_TRIE = 0x80000033
N_TYPE, N_SECT = 0x0E, 0x0E


def demangle(name):
    """Enough Itanium demangling to make method names readable."""
    m = re.match(r'^_?_Z(N?)(.*)$', name)
    if not m:
        return name
    rest, out = m.group(2), []
    while rest:
        d = re.match(r'^(\d+)', rest)
        if not d:
            break
        n = int(d.group(1))
        start = d.end()
        out.append(rest[start:start + n])
        rest = rest[start + n:]
    return '::'.join(out) if out else name


class MachO:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        magic, = struct.unpack_from('<I', self.data, 0)
        if magic != MH_MAGIC_64:
            sys.exit('not a 64-bit little-endian Mach-O (magic 0x%08x)' % magic)
        _cpu, _sub, _ft, ncmds, _sz, _fl, _res = struct.unpack_from('<7I', self.data, 4)
        self.sections = []
        self.symbols = {}
        off = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<II', self.data, off)
            if cmd == LC_SEGMENT_64:
                nsects, = struct.unpack_from('<I', self.data, off + 64)
                so = off + 72
                for _i in range(nsects):
                    sname = self.data[so:so + 16].rstrip(b'\0').decode('ascii', 'replace')
                    seg = self.data[so + 16:so + 32].rstrip(b'\0').decode('ascii', 'replace')
                    addr, size = struct.unpack_from('<QQ', self.data, so + 32)
                    foff, = struct.unpack_from('<I', self.data, so + 48)
                    self.sections.append((seg, sname, addr, size, foff))
                    so += 80
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY, LC_DYLD_EXPORTS_TRIE):
                if cmd == LC_DYLD_EXPORTS_TRIE:
                    eoff, esize = struct.unpack_from('<II', self.data, off + 8)
                else:
                    # rebase, bind, weak_bind, lazy_bind, export -- ten u32 total
                    fields = struct.unpack_from('<10I', self.data, off + 8)
                    eoff, esize = fields[8], fields[9]
                if esize:
                    self._read_trie(eoff, eoff, esize)
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, _strsize = struct.unpack_from('<4I', self.data, off + 8)
                for i in range(nsyms):
                    o = symoff + i * 16
                    strx, typ, _sect, _desc = struct.unpack_from('<IBBH', self.data, o)
                    value, = struct.unpack_from('<Q', self.data, o + 8)
                    if not value or (typ & N_TYPE) != N_SECT:
                        continue
                    end = self.data.find(b'\0', stroff + strx)
                    nm = self.data[stroff + strx:end].decode('ascii', 'replace')
                    if nm:
                        self.symbols.setdefault(nm, value)
            off += cmdsize

    def _uleb(self, o):
        v, sh = 0, 0
        while True:
            b = self.data[o]
            o += 1
            v |= (b & 0x7F) << sh
            if not (b & 0x80):
                return v, o
            sh += 7

    def _read_trie(self, base, node, size, prefix=b''):
        """Walk the dyld export trie; exported symbols live here, not in symtab."""
        if node >= base + size:
            return
        o = node
        terminal, o = self._uleb(o)
        if terminal:
            end = o + terminal
            _flags, o2 = self._uleb(o)
            addr, _ = self._uleb(o2)
            name = prefix.decode('ascii', 'replace')
            if name:
                self.symbols.setdefault(name, addr)
            o = end
        nchild = self.data[o]
        o += 1
        for _ in range(nchild):
            e = self.data.find(b'\0', o)
            label = self.data[o:e]
            o = e + 1
            child, o = self._uleb(o)
            self._read_trie(base, base + child, size, prefix + label)

    def text(self):
        for seg, sec, addr, size, foff in self.sections:
            if seg == '__TEXT' and sec == '__text':
                return addr, size, foff
        sys.exit('no __TEXT,__text section')

    def file_off(self, vma):
        for _seg, _sec, addr, size, foff in self.sections:
            if addr <= vma < addr + size:
                return foff + (vma - addr)
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('file')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--grep')
    ap.add_argument('--sym')
    ap.add_argument('--addr')
    ap.add_argument('--len', dest='length', default='0x300')
    args = ap.parse_args()

    mo = MachO(args.file)

    if args.list:
        rows = sorted(mo.symbols.items(), key=lambda kv: kv[1])
        for nm, va in rows:
            d = demangle(nm)
            if args.grep and args.grep.lower() not in d.lower() and \
               args.grep.lower() not in nm.lower():
                continue
            print('  0x%08x  %s' % (va, d[:96]))
        print('  (%d symbol(s))' % len(rows))
        return

    if args.addr:
        start = int(args.addr, 0)
    elif args.sym:
        hits = [(n, v) for n, v in mo.symbols.items()
                if args.sym.lower() in demangle(n).lower() or args.sym.lower() in n.lower()]
        if not hits:
            sys.exit('no symbol matching %r' % args.sym)
        hits.sort(key=lambda kv: len(kv[0]))
        print('== %s' % demangle(hits[0][0]))
        start = hits[0][1]
    else:
        sys.exit('need --list, --sym or --addr')

    length = int(args.length, 0)
    fo = mo.file_off(start)
    if fo is None:
        sys.exit('address 0x%x not in any section' % start)

    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OPT_SYNTAX_INTEL
    except ImportError:
        sys.exit('capstone not installed: python3 -m pip install --user capstone')

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.syntax = CS_OPT_SYNTAX_INTEL
    addr2sym = {v: demangle(k) for k, v in mo.symbols.items()}
    print('== VMA 0x%x, %d bytes\n' % (start, length))
    for ins in md.disasm(mo.data[fo:fo + length], start):
        line = '  %08x  %-7s %s' % (ins.address, ins.mnemonic, ins.op_str)
        m = re.match(r'^0x([0-9a-f]+)$', ins.op_str)
        if m and ins.mnemonic in ('call', 'jmp'):
            t = int(m.group(1), 16)
            if t in addr2sym:
                line += '   ; <%s>' % addr2sym[t][:60]
        print(line)


if __name__ == '__main__':
    main()
