#!/usr/bin/env python3
import sys, re, struct, argparse
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from macho_lib import MachO, demangle
from macho_unwind import func_starts
from capstone import *

def build(path):
    m=MachO(path)
    fs=func_starts(m)
    # add exported syms as starts
    for n,v in m.syms.items():
        s=m.sec_of(v)
        if s and s['name']=='__text': fs.append(v)
    fs=sorted(set(fs))
    dm=demangle(list(m.syms.keys()))
    a2s={}
    for n,v in m.syms.items():
        a2s.setdefault(v,dm.get(n,n))
    for a,n in m.addr2sym.items():
        a2s.setdefault(a,n)
    return m,fs,a2s

def fname(a2s,a):
    return a2s.get(a, 'sub_%x'%a)

def disas(m,fs,a2s,start,end=None,out=sys.stdout):
    import bisect
    i=bisect.bisect_right(fs,start)-1
    if end is None:
        j=bisect.bisect_right(fs,start)
        end = fs[j] if j<len(fs) else start+0x400
    fo=m.foff(start)
    md=Cs(CS_ARCH_X86,CS_MODE_64); md.syntax=CS_OPT_SYNTAX_INTEL; md.detail=False
    print('=== %s  (0x%x .. 0x%x)'%(fname(a2s,start),start,end),file=out)
    for ins in md.disasm(m.data[fo:fo+(end-start)],start):
        line='  %06x  %-24s %-7s %s'%(ins.address,ins.bytes.hex(),ins.mnemonic,ins.op_str)
        ann=[]
        mm=re.search(r'\[rip \+ (0x[0-9a-f]+)\]',ins.op_str)
        mm2=re.search(r'\[rip - (0x[0-9a-f]+)\]',ins.op_str)
        tgt=None
        if mm: tgt=ins.address+ins.size+int(mm.group(1),16)
        elif mm2: tgt=ins.address+ins.size-int(mm2.group(1),16)
        if tgt is not None:
            sec=m.sec_of(tgt)
            nm=a2s.get(tgt)
            if nm: ann.append('<%s>'%nm[:70])
            elif sec and sec['name'] in ('__cstring','__const','__objc_methname'):
                c=m.cstr(tgt)
                if c is not None: ann.append('"%s"'%c[:80])
                else: ann.append('%s+0x%x'%(sec['name'],tgt-sec['addr']))
            else:
                ann.append('0x%x%s'%(tgt,' ('+sec['name']+')' if sec else ''))
        mo2=re.match(r'^0x([0-9a-f]+)$',ins.op_str)
        if mo2 and ins.mnemonic in ('call','jmp'):
            t=int(mo2.group(1),16)
            ann.append('<%s>'%fname(a2s,t)[:70])
        if ann: line+='   ; '+' '.join(ann)
        print(line,file=out)

if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('file'); ap.add_argument('--addr'); ap.add_argument('--sym')
    ap.add_argument('--end'); ap.add_argument('--all',action='store_true')
    ap.add_argument('--list',action='store_true'); ap.add_argument('--grep')
    a=ap.parse_args()
    m,fs,a2s=build(a.file)
    if a.list:
        for v in fs:
            n=fname(a2s,v)
            if a.grep and a.grep.lower() not in n.lower(): continue
            print('0x%08x %s'%(v,n))
        sys.exit()
    if a.all:
        for i,v in enumerate(fs):
            e=fs[i+1] if i+1<len(fs) else None
            disas(m,fs,a2s,v,e)
        sys.exit()
    start=int(a.addr,0) if a.addr else None
    if start is None:
        cand=[v for v in fs if a.sym.lower() in fname(a2s,v).lower()]
        if not cand: sys.exit('no match')
        start=cand[0]
    disas(m,fs,a2s,start,int(a.end,0) if a.end else None)
