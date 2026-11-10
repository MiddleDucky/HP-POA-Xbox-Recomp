import sys,struct,capstone
xbe=open(sys.argv[1],'rb').read()
base=struct.unpack_from('<I',xbe,0x104)[0]
nsec=struct.unpack_from('<I',xbe,0x11C)[0]; shdr=struct.unpack_from('<I',xbe,0x120)[0]-base
secs=[]
for i in range(nsec):
    fl,va,vs,ra,rs=struct.unpack_from('<5I',xbe,shdr+i*56)
    secs.append((va,vs,ra,rs))
def rd(va,n):
    for s in secs:
        if s[0]<=va<s[0]+s[3]:
            o=s[2]+va-s[0]; return xbe[o:o+n]
    return b''
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
a=int(sys.argv[2],16); n=int(sys.argv[3]) if len(sys.argv)>3 else 40
cnt=0
for ins in md.disasm(rd(a,n*16),a):
    print(f"{ins.address:08X}  {ins.mnemonic} {ins.op_str}")
    cnt+=1
    if cnt>=n: break
