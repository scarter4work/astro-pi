#!/usr/bin/env python3
"""Read a NUKEX_DUMP_VOXELS file and report the model race's win rates."""
import struct, sys, collections

NAMES = {0:"Student-t", 1:"GMM", 2:"Contamination", 3:"KDE/other"}

def read(path):
    recs=[]
    with open(path,'rb') as f:
        magic,ver,stride = struct.unpack('<III', f.read(12))
        assert magic==0x4456584E, f"bad magic {magic:#x}"
        while True:
            b=f.read(4)
            if len(b)<4: break
            n=struct.unpack('<I',b)[0]
            vals=f.read(4*n); wts=f.read(4*n)
            fb=f.read(288)
            if len(fb)<288: break
            fl=struct.unpack('<36d', fb)
            recs.append((n,fl))
    return stride,recs

def main(path,label):
    stride,recs=read(path)
    if not recs:
        print(f"{label}: no records"); return
    win=collections.Counter(); ch_win=collections.defaultdict(collections.Counter)
    close=0; conv=collections.Counter()
    for n,f in recs:
        w=int(round(f[25])); ch=int(round(f[32]))
        win[w]+=1; ch_win[ch][w]+=1
        for i,k in ((3,0),(9,1),(19,2)):
            if f[i]>0.5: conv[k]+=1
        aiccs=sorted(a for a,c in ((f[8],f[3]),(f[18],f[9]),(f[24],f[19])) if c>0.5)
        if len(aiccs)>1 and (aiccs[1]-aiccs[0])<2.0: close+=1
    t=len(recs)
    print(f"=== {label} ===")
    print(f"  sampled voxel-channel fits: {t:,}  (1 in {stride})")
    print("  winner:")
    for k in sorted(win): print(f"    {NAMES.get(k,k):<14} {win[k]:>8,}  {100*win[k]/t:5.1f}%")
    print("  converged (of sampled):")
    for k in sorted(conv): print(f"    {NAMES.get(k,k):<14} {conv[k]:>8,}  {100*conv[k]/t:5.1f}%")
    print(f"  dAICc < 2 vs runner-up: {close:,}  {100*close/t:.1f}%   <- indistinguishable by the code's own authority")
    if len(ch_win)>1:
        print("  by channel:")
        for ch in sorted(ch_win):
            sub=ch_win[ch]; st=sum(sub.values())
            bits=" ".join(f"{NAMES.get(k,k)} {100*v/st:.1f}%" for k,v in sorted(sub.items()))
            print(f"    ch {ch}: n={st:,}  {bits}")
    print()

if __name__=="__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv)>2 else sys.argv[1])
