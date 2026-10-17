import struct, sys
def walk(d, off, end, depth=0, out=print):
    while off < end:
        t,hs,sz = struct.unpack_from('<HHI', d, off)
        out('  '*depth + f'@{off} type=0x{t:04x} hdr={hs} size={sz}')
        if t in (0x0002, 0x0200):   # table, package contain chunks after header
            walk(d, off+hs, off+sz, depth+1, out)
        off += sz
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    t,hs,sz=struct.unpack_from('<HHI',d,0)
    print(f'root type=0x{t:04x} hdr={hs} size={sz} len={len(d)}')
    walk(d, hs if t==0x0002 else 8, len(d)) if t==0x0002 else None
    if t==0x0003:
        # xml: walk chunks flat
        off=8
        while off<len(d):
            tt,h,s=struct.unpack_from('<HHI',d,off); print(f'@{off} 0x{tt:04x} hdr={h} size={s}'); off+=s
