import struct
def func_starts(mo):
    sec=None
    for s in mo.sections:
        if s['name']=='__unwind_info': sec=s
    if not sec: return []
    d=mo.data; base=sec['off']
    ver,cesOff,cesCnt,perOff,perCnt,idxOff,idxCnt=struct.unpack_from('<7I',d,base)
    starts=set()
    idx=[]
    for i in range(idxCnt):
        fo,so,lo=struct.unpack_from('<3I',d,base+idxOff+i*12)
        idx.append((fo,so,lo))
    for i,(fo,so,lo) in enumerate(idx):
        if so==0: continue
        kind=struct.unpack_from('<I',d,base+so)[0]
        if kind==2:
            k,entOff,entCnt=struct.unpack_from('<IHH',d,base+so)
            for j in range(entCnt):
                e=struct.unpack_from('<I',d,base+so+entOff+j*8)[0]
                starts.add(e)
        elif kind==3:
            k,entOff,entCnt,encOff,encCnt=struct.unpack_from('<IHHHH',d,base+so)
            for j in range(entCnt):
                e=struct.unpack_from('<I',d,base+so+entOff+j*4)[0]
                starts.add(fo+(e&0xffffff))
    return sorted(starts)
