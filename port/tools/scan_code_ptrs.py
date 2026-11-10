#!/usr/bin/env python3
"""Seed candidates for indirect-call targets the disassembler did not reach.

Scans .rdata/.data (and code immediates are already handled by tools.disasm) for
dwords that point into an executable section, keeps those that are not already a
function start, and accepts them when they sit in a run of >=2 code pointers
(vtables, handler tables) or start at a 16-byte boundary after padding/ret.
Prints a JSON list in icall_seeds.json form.
usage: scan_code_ptrs.py XBE FUNCTIONS_JSON [--merge SEEDS_JSON]
"""
import json, struct, sys
import capstone

xbe = open(sys.argv[1], 'rb').read()
funcs = json.load(open(sys.argv[2]))
known = {int(f['start'], 16) for f in funcs}
spans = sorted((int(f['start'], 16), int(f['end'], 16)) for f in funcs)
base = struct.unpack_from('<I', xbe, 0x104)[0]
nsec = struct.unpack_from('<I', xbe, 0x11C)[0]
sh = struct.unpack_from('<I', xbe, 0x120)[0] - base
secs = []
for i in range(nsec):
    fl, va, vs, ra, rs, nameaddr = struct.unpack_from('<6I', xbe, sh + i * 56)
    no = nameaddr - base
    name = xbe[no:xbe.index(b'\0', no)].decode()
    secs.append((name, fl, va, vs, ra, rs))
CODE = {'.text', 'D3D', 'D3DX', 'XGRPH', 'DSOUND', 'XMV', 'XPP'}
code = [(va, va + rs, ra) for n, fl, va, vs, ra, rs in secs if n in CODE]
def in_code(a):
    for lo, hi, ra in code:
        if lo <= a < hi: return ra + a - lo
    return None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
def decodes(a):
    off = in_code(a)
    if off is None: return False
    n = 0
    for ins in md.disasm(xbe[off:off + 64], a):
        n += 1
        if ins.mnemonic in ('ret', 'jmp', 'call') or n >= 6: return True
    return False
def after_pad(a):
    off = in_code(a)
    if off is None or a & 0xF: return False
    p = xbe[off - 1]
    return p in (0xCC, 0x90, 0xC3) or xbe[off - 3] == 0xC2
def inside_func(a):
    import bisect
    i = bisect.bisect_right(spans, (a, 1 << 32)) - 1
    return i >= 0 and spans[i][0] < a < spans[i][1]

out = set()
for n, fl, va, vs, ra, rs in secs:
    if n not in ('.rdata', '.data'): continue
    vals = [struct.unpack_from('<I', xbe, ra + o)[0] for o in range(0, rs - 3, 4)]
    isc = [in_code(v) is not None for v in vals]
    for i, v in enumerate(vals):
        if not isc[i] or v in known: continue
        # neighbour in the same table is a known function (vtable / handler table)
        run = any(0 <= j < len(vals) and vals[j] in known for j in (i - 2, i - 1, i + 1, i + 2))
        if not decodes(v) or (inside_func(v) and not after_pad(v)): continue
        if after_pad(v) or (run and (v & 0xF) == 0):
            out.add(v)
seeds = []
if '--merge' in sys.argv:
    seeds = json.load(open(sys.argv[sys.argv.index('--merge') + 1]))
have = {int(s['start'], 16) for s in seeds}
for v in sorted(out - have):
    seeds.append({'start': '0x%08X' % v})
print(json.dumps(seeds, indent=1))
print(len(out), 'candidates', file=sys.stderr)
