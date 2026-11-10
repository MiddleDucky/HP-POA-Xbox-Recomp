# direct call/jmp targets from one VA range into another, with the symbol name if known
import sys,struct,re,collections
xbe=open(sys.argv[1],'rb').read()
base=struct.unpack_from('<I',xbe,0x104)[0]
nsec=struct.unpack_from('<I',xbe,0x11C)[0]; shdr=struct.unpack_from('<I',xbe,0x120)[0]-base
secs=[struct.unpack_from('<5I',xbe,shdr+i*56)[1:] for i in range(nsec)]
def rd(va,n):
    for s in secs:
        if s[0]<=va<s[0]+s[3]:
            o=s[2]+va-s[0]; return xbe[o:o+min(n,s[0]+s[3]-va)]
    return b''
syms={}
for l in open(sys.argv[2]):
    m=re.match(r'(\S+) = 0x([0-9a-fA-F]+)',l)
    if m: syms[int(m.group(2),16)]=m.group(1)
f0,f1,t0,t1=[int(x,16) for x in sys.argv[3:7]]
code=rd(f0,f1-f0)
hits=collections.Counter(); first={}
for i in range(len(code)-5):
    if code[i]==0xE8 or code[i]==0xE9:
        tgt=(f0+i+5+struct.unpack_from('<i',code,i+1)[0])&0xffffffff
        if t0<=tgt<t1:
            hits[tgt]+=1; first.setdefault(tgt,f0+i)
for t in sorted(hits):
    print(f"{t:08X} {hits[t]:4d} from {first[t]:08X} {syms.get(t,'?')}")
