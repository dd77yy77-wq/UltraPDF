import struct, sys
def sp(d, off):
    t,hs,sz,sc,stc,fl,ss,sts = struct.unpack_from('<HHIIIIII', d, off)
    offs = struct.unpack_from('<%dI'%sc, d, off+28)
    out=[]
    for o in offs:
        p = off+ss+o
        if fl & 0x100:
            l=d[p]; p+=1
            if l&0x80: l=((l&0x7f)<<8)|d[p]; p+=1
            bl=d[p]; p+=1
            if bl&0x80: bl=((bl&0x7f)<<8)|d[p]; p+=1
            out.append(d[p:p+bl].decode('utf-8'))
        else:
            l=struct.unpack_from('<H',d,p)[0]; p+=2
            out.append(d[p:p+2*l].decode('utf-16le'))
    return out, fl, (sc,stc,ss,sts), sz
def dump(path):
    d=open(path,'rb').read()
    print('root', struct.unpack_from('<HHI', d, 0), len(d))
    off=8
    strings=None
    while off<len(d):
        t,hs,sz=struct.unpack_from('<HHI',d,off)
        if t==0x0001:
            strings,fl,info,_=sp(d,off); print('stringpool flags=0x%x'%fl, info, len(strings)); 
            for i,s in enumerate(strings): print('  ',i,repr(s))
        elif t==0x0180:
            n=(sz-8)//4; print('resmap', [hex(x) for x in struct.unpack_from('<%dI'%n,d,off+8)])
        elif t in (0x0100,0x0101):
            ln,cm,pre,uri=struct.unpack_from('<IIII',d,off+8); print('ns', hex(t), 'line',ln,'prefix',strings[pre],'uri',strings[uri&0xffffffff])
        elif t==0x0102:
            ln,cm,ns,name,astart,asz,ac,idi,ci,si = struct.unpack_from('<IIIIHHHHHH',d,off+8)
            print('<%s>'%strings[name], 'line',ln,'astart',astart,'asz',asz,'ac',ac, 'hdr',hs,'size',sz)
            ao=off+16+astart
            for k in range(ac):
                ans,an,raw,vs,r0,vt,vd=struct.unpack_from('<IIIHBBI',d,ao+k*20)
                nm=strings[an]; 
                print('    attr ns=%s name=%s raw=%s size=%d type=0x%02x data=0x%x'%(ans if ans==0xffffffff else strings[ans], nm, raw if raw==0xffffffff else repr(strings[raw]), vs, vt, vd))
        elif t==0x0103:
            ln,cm,ns,name=struct.unpack_from('<IIII',d,off+8); print('</%s>'%strings[name])
        else: print('chunk',hex(t))
        off+=sz
dump(sys.argv[1])
