import struct, re, subprocess, bisect
from capstone import *

class MachO:
    def __init__(self, path):
        self.data=d=open(path,'rb').read()
        ncmds=struct.unpack_from('<I',d,16)[0]
        off=32; self.sections=[]; self.syms={}
        self.eoff=self.esize=0
        self.indirect=[]; self.dysym=None; self.symtab=[]
        for _ in range(ncmds):
            cmd,cs=struct.unpack_from('<II',d,off)
            if cmd==0x19:
                nsects=struct.unpack_from('<I',d,off+64)[0]; so=off+72
                for i in range(nsects):
                    sn=d[so:so+16].rstrip(b'\0').decode()
                    sg=d[so+16:so+32].rstrip(b'\0').decode()
                    a,sz=struct.unpack_from('<QQ',d,so+32)
                    fo,align,reloff,nreloc,flags=struct.unpack_from('<5I',d,so+48)
                    r1,r2=struct.unpack_from('<II',d,so+68)
                    self.sections.append(dict(seg=sg,name=sn,addr=a,size=sz,off=fo,flags=flags,r1=r1,r2=r2))
                    so+=80
            elif cmd==0x80000022 or cmd==0x22:
                f=struct.unpack_from('<10I',d,off+8); self.eoff,self.esize=f[8],f[9]
                self.dyldinfo=f
            elif cmd==0x02:
                symoff,nsyms,stroff,strsize=struct.unpack_from('<4I',d,off+8)
                self.stroff=stroff
                for i in range(nsyms):
                    o=symoff+i*16
                    strx,typ,sect,desc=struct.unpack_from('<IBBH',d,o)
                    val,=struct.unpack_from('<Q',d,o+8)
                    e=d.find(b'\0',stroff+strx)
                    nm=d[stroff+strx:e].decode('ascii','replace')
                    self.symtab.append((nm,typ,sect,val))
                    if val and (typ&0x0e)==0x0e: self.syms.setdefault(nm,val)
            elif cmd==0x0b:
                f=struct.unpack_from('<18I',d,off+8)
                self.indirectoff,self.nindirect=f[16],f[17]
            off+=cs
        if self.esize: self._trie(self.eoff,b'')
        self.addr2sym={}
        for k,v in self.syms.items(): self.addr2sym.setdefault(v,k)
        self._stubs()
        self._binds()
    def _uleb(self,o):
        d=self.data; v=0;sh=0
        while True:
            b=d[o];o+=1;v|=(b&0x7f)<<sh
            if not b&0x80: return v,o
            sh+=7
    def _sleb(self,o):
        d=self.data; v=0;sh=0
        while True:
            b=d[o];o+=1;v|=(b&0x7f)<<sh; sh+=7
            if not b&0x80:
                if b&0x40: v-= (1<<sh)
                return v,o
    def _trie(self,node,prefix):
        d=self.data; o=node
        term,o=self._uleb(o)
        if term:
            end=o+term
            flags,o2=self._uleb(o); addr,o3=self._uleb(o2)
            if prefix: self.syms.setdefault(prefix.decode('ascii','replace'),addr)
            o=end
        n=d[o];o+=1
        for _ in range(n):
            e=d.find(b'\0',o); lbl=d[o:e]; o=e+1
            ch,o=self._uleb(o)
            self._trie(self.eoff+ch,prefix+lbl)
    def _stubs(self):
        # map stub address -> undefined symbol name via indirect symbol table
        d=self.data
        for s in self.sections:
            t=s['flags']&0xff
            if t in (0x08,0x06,0x07):  # symbol stubs / lazy / non-lazy ptr
                esz = s['r2'] if t==0x08 else 8
                if esz==0: continue
                n=s['size']//esz
                for i in range(n):
                    idx=struct.unpack_from('<I',d,self.indirectoff+(s['r1']+i)*4)[0]
                    if idx>=len(self.symtab): continue
                    nm=self.symtab[idx][0]
                    self.addr2sym.setdefault(s['addr']+i*esz,nm)
    def _binds(self):
        pass
    def foff(self,vma):
        for s in self.sections:
            if s['addr']<=vma<s['addr']+s['size'] and s['off']:
                return s['off']+(vma-s['addr'])
        return None
    def sec_of(self,vma):
        for s in self.sections:
            if s['addr']<=vma<s['addr']+s['size']: return s
        return None
    def cstr(self,vma):
        f=self.foff(vma)
        if f is None: return None
        e=self.data.find(b'\0',f)
        if e<0 or e-f>200: return None
        try: return self.data[f:e].decode('ascii')
        except: return None

def demangle(names):
    if not names: return {}
    p=subprocess.run(['c++filt'],input='\n'.join(n[1:] if n.startswith('__Z') else n for n in names),
                     capture_output=True,text=True)
    return dict(zip(names,p.stdout.strip().split('\n')))
