#!/usr/bin/env python3
"""Pack upscaled sky pictures for the port's hd_textures folder, each named by the hash of the texture it replaces.

  make_hd_textures.py <level package> <folder of upscaled PNGs> <output hd_textures folder>

A PNG must be named after the texture: ar3_tex_sk_dementorsbattle_n_fr.png. Reads the player's own game files; the output is
derived from the game's art, so it stays on that PC and is never part of a release.
"""
import os, re, struct, subprocess, sys

def refpack(b):
    p = 6
    size = struct.unpack('>I', b[2:6])[0]
    o = bytearray()
    while len(o) < size:
        c = b[p]; p += 1
        if c < 0x80:
            c1 = b[p]; p += 1; lit = c & 3; cnt = ((c & 0x1C) >> 2) + 3; off = ((c & 0x60) << 3) + c1 + 1
        elif c < 0xC0:
            c1, c2 = b[p], b[p + 1]; p += 2; lit = (c1 >> 6) & 3; cnt = (c & 0x3F) + 4; off = ((c1 & 0x3F) << 8) + c2 + 1
        elif c < 0xE0:
            c1, c2, c3 = b[p], b[p + 1], b[p + 2]; p += 3; lit = c & 3; cnt = ((c & 0x0C) << 6) + c3 + 5
            off = ((c & 0x10) << 12) + (c1 << 8) + c2 + 1
        elif c < 0xFC:
            lit = ((c & 0x1F) + 1) * 4; cnt = off = 0
        else:
            lit = c & 3; cnt = off = 0
        o += b[p:p + lit]; p += lit
        for _ in range(cnt): o.append(o[-off])
        if c >= 0xFC: break
    return bytes(o)

FORMATS = {0x60: (0x0C, 8), 0x61: (0x0E, 16)}  # SHPX type: D3DFMT value on the Xbox, bytes per 4x4 block

def main(pkg, src, out):
    d = open(pkg, 'rb').read()
    os.makedirs(out, exist_ok=True)
    made = 0
    for m in re.finditer(rb'PKGH.{8}.{4}<textures>([A-Za-z0-9_]+)\.tga\0', d, re.S):
        name = m.group(1).decode()
        png = os.path.join(src, name + '.png')
        if not os.path.exists(png): continue
        q = d.find(b'PKGC', m.end()); raw = bytearray()
        while d[q:q + 4] == b'PKGC':
            ln = struct.unpack_from('<I', d, q + 4)[0]; c = d[q + 8:q + ln]
            raw += refpack(c) if c[1:2] == b'\xfb' else c
            q += ln
        off = struct.unpack_from('<I', raw, 0x14)[0]
        t = raw[off]; w, h = struct.unpack_from('<HH', raw, off + 4)
        fmt, block = FORMATS[t]
        n = (w + 3) // 4 * ((h + 3) // 4) * block
        hsh = 14695981039346656037
        for b in bytes(raw[off + 16:off + 16 + n]):
            hsh = ((hsh ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        info = subprocess.run(['magick', 'identify', '-format', '%w %h', png], capture_output=True, text=True, check=True).stdout.split()
        pw, ph = int(info[0]), int(info[1])
        px = subprocess.run(['magick', png, '-alpha', 'opaque', '-depth', '8', 'bgra:-'], capture_output=True, check=True).stdout
        path = os.path.join(out, f'{fmt:02X}_{w}x{h}_{hsh:016X}.bgra')
        with open(path, 'wb') as f:
            f.write(struct.pack('<4sII', b'HDT1', pw, ph)); f.write(px)
        print(name, f'{w}x{h} -> {pw}x{ph}', os.path.basename(path)); made += 1
    print(made, 'written to', out)

if __name__ == '__main__':
    main(*sys.argv[1:4])
