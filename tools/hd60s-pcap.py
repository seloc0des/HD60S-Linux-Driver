import struct,sys,collections
d=open('trial.pcapng','rb').read()
def u16(o): return struct.unpack_from('<H',d,o)[0]
def u32(o): return struct.unpack_from('<I',d,o)[0]
DEV=3
txns=[]  # (idx, bmr, br, wv, wi, wl, outdata)
off=0
while off+8<=len(d):
    bt=u32(off); bl=u32(off+4)
    if bl<12 or off+bl>len(d): break
    if bt==0x00000006:
        caplen=u32(off+20); p=off+28
        if p+28<=len(d):
            dev=u16(p+19); tr=d[p+22]; hlen=u16(p); stage=d[p+27]
            if dev==DEV and tr==2 and stage==0:
                pl=d[p+hlen:p+caplen]
                if len(pl)>=8:
                    bmr,br=pl[0],pl[1]; wv=struct.unpack_from('<H',pl,2)[0]
                    wi=struct.unpack_from('<H',pl,4)[0]; wl=struct.unpack_from('<H',pl,6)[0]
                    out=pl[8:8+wl] if not (bmr&0x80) else b''
                    txns.append((bmr,br,wv,wi,wl,out))
    off+=bl

def decode_write(wv,out):
    oh=out.hex()
    if wv==0x509c: return ("MST3367", "raw "+oh)
    if wv==0x5098: return ("CPLD", "raw "+oh)
    if wv==0x5088: return ("s88", "raw "+oh)
    if wv==0x5066 and out:
        b0=out[0]
        if b0 in (0xAA,0xAB): return ("MCU-CMD", oh)
        if b0&1: return ("read-setup", "s%02x cnt%d reg%02x"%(b0&0xfe,out[1] if len(out)>1 else 0,out[2] if len(out)>2 else 0))
        return ("I2C s%02x"%b0, "reg%02x="%out[1]+out[2:].hex() if len(out)>2 else oh)
    return ("wv%04x"%wv, oh)

writes=[t for t in txns if not (t[0]&0x80) and t[2] in (0x509c,0x5098,0x5088,0x5066) and t[5]]
# distinct writes preserving first-seen order, skipping pure read-setups
seen=set(); distinct=[]
for bmr,br,wv,wi,wl,out in writes:
    tgt,desc=decode_write(wv,out)
    if tgt=="read-setup": continue
    key=(wv,bytes(out))
    if key in seen: continue
    seen.add(key); distinct.append((tgt,desc,wv,out))
print("=== DISTINCT WRITES (config actions), first-seen order: %d ==="%len(distinct))
for tgt,desc,wv,out in distinct:
    print("  %-10s %s"%(tgt,desc))
