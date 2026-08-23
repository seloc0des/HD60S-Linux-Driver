import sys,struct
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from macho_lib import MachO
class R:
    def __init__(s,p):
        s.m=MachO(p); s.d=s.m.data
    def foff(s,v):
        for x in s.m.sections:
            if x['off'] and x['addr']<=v<x['addr']+x['size']: return x['off']+(v-x['addr'])
    def sec(s,v):
        for x in s.m.sections:
            if x['addr']<=v<x['addr']+x['size']: return x['name']
    def vma(s,fo):
        for x in s.m.sections:
            if x['off'] and x['off']<=fo<x['off']+x['size']: return x['addr']+(fo-x['off'])
    def find_str(s,txt):
        i=s.d.find(txt.encode()+b'\0'); return s.vma(i) if i>=0 else None
    def refs(s,v):
        pat=struct.pack('<Q',v); out=[];i=0
        while True:
            i=s.d.find(pat,i)
            if i<0: break
            a=s.vma(i)
            if a is not None: out.append(a)
            i+=1
        return out
    def q(s,v): return struct.unpack_from('<Q',s.d,s.foff(v))[0]
    def vtable_for(s,name):
        sv=s.find_str(name)
        if sv is None: return None,None
        ti=[r-8 for r in s.refs(sv)]
        res=[]
        for t in ti:
            for r in s.refs(t):
                res.append((t,r))   # r = location of typeinfo ptr in vtable
        return sv,res
if __name__=='__main__':
    r=R(sys.argv[1])
    for nm in sys.argv[2:]:
        sv,res=r.vtable_for(nm)
        print('== %s  str=0x%x'%(nm,sv or 0))
        for ti,loc in res:
            print('   typeinfo 0x%x  referenced at 0x%x (%s)'%(ti,loc,r.sec(loc)))
            base=loc+8
            for k in range(60):
                v=r.q(base+k*8)
                sn=r.sec(v)
                if sn!='__text': break
                print('      +0x%03x  0x%x'%(k*8,v))
