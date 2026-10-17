import struct, sys, zipfile
def refs(d):
    ssz,so=struct.unpack_from('<II',d,56); tsz,to=struct.unpack_from('<II',d,64); psz,po=struct.unpack_from('<II',d,72)
    fsz,fo=struct.unpack_from('<II',d,80); msz,mo=struct.unpack_from('<II',d,88)
    def uleb(p):
        r=0;s=0
        while True:
            b=d[p];p+=1;r|=(b&0x7f)<<s;s+=7
            if not b&0x80:return r,p
    S=[]
    for i in range(ssz):
        o=struct.unpack_from('<I',d,so+4*i)[0]; _,p=uleb(o); e=d.index(b'\0',p); S.append(d[p:e].decode('utf-8','replace'))
    T=[S[struct.unpack_from('<I',d,to+4*i)[0]] for i in range(tsz)]
    P=[]
    for i in range(psz):
        sh,rt,po2=struct.unpack_from('<III',d,po+12*i)
        params=[]
        if po2:
            n=struct.unpack_from('<I',d,po2)[0]; params=[T[struct.unpack_from('<H',d,po2+4+2*k)[0]] for k in range(n)]
        P.append('('+''.join(params)+')'+T[rt])
    M=set()
    for i in range(msz):
        c,p,n=struct.unpack_from('<HHI',d,mo+8*i); M.add((T[c],S[n],P[p]))
    F=set()
    for i in range(fsz):
        c,t,n=struct.unpack_from('<HHI',d,fo+8*i); F.add((T[c],S[n],T[t]))
    return M,F
if __name__=='__main__':
    mine=open(sys.argv[1],'rb').read()
    ref=zipfile.ZipFile(sys.argv[2]).read('classes.dex')
    mm,mf=refs(mine); rm,rf=refs(ref)
    ours=set(c for c,_,_ in mm if c.startswith('Lcom/dd/shchitok'))
    print('classes defined by us:', len(ours))
    bad=0
    for c,n,p in sorted(mm):
        if c.startswith('Lcom/dd/shchitok') or c.startswith('Ljava/lang/Object') : continue
        status='OK (in UltraPDF dex)' if (c,n,p) in rm else '???'
        if status=='???': bad+=1
        print('%-22s %s.%s%s'%(status[:22],c,n,p))
    for c,n,t in sorted(mf):
        if c.startswith('Lcom/dd/shchitok'): continue
        print('FIELD', 'OK' if (c,n,t) in rf else '???', c,n,t)
    print('unverified method refs:', bad)
