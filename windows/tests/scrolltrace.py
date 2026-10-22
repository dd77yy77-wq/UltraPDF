import sys,re
rows=[]
for l in open(sys.argv[1]):
    m=re.match(r'(\d+) sy=([\d.]+) miss=(\d+) blank=(\d+) ms=(\d+)',l)
    if m: rows.append(tuple(float(x) for x in m.groups()))
ys=[r[1] for r in rows]
mv=[i for i in range(1,len(rows)) if ys[i]!=ys[i-1]]
a,b=mv[0],mv[-1]
seg=rows[a:b+1]
print("%s: paints=%d scroll-frames=%d dur=%.1fs | frames w/ missing tiles: %d (%.0f%%), fully blank: %d | paint ms avg %.1f max %d"%(
 sys.argv[1].split('/')[-1],len(rows),len(seg),(seg[-1][0]-seg[0][0])/1000,sum(1 for r in seg if r[2]>0),100*sum(1 for r in seg if r[2]>0)/len(seg),sum(1 for r in seg if r[3]>0),sum(r[4] for r in seg)/len(seg),max(r[4] for r in seg)))
tail=rows[b:]
st=next((r[0]-rows[b][0] for r in tail if r[2]==0),None)
print("  after stop: settle ms =",st," paints after stop =",len(tail)," still-missing at end:",tail[-1][2])
