#!/usr/bin/env python3
"""Disassembler for PoA's compiled level scripts (data/misc/*.hpb).

Interpreter: sub_001B1A90 steps one instruction through the 95-entry table at
0x30C578. Code is 16-bit words: an opcode, then its operands.

File: u16 n1, u16 n2, a table of globals not yet decoded, then u16 function count, then each function:
  u32 name length, name (with NUL), u32 word count, words,
  u16 ?, u16 constant count, constants: u16 type (0 bool, 3 float, 4 int, 5 pair, 6 triple, 7 and 8 text), u32 value or text length, a second u32 for a pair.

Usage: hpb_dis.py file.hpb [function-name-substring]
"""
import struct
import sys

# operands per opcode, from the number of code reads in each handler
OPERANDS = {0x00: 1, 0x01: 1, 0x11: 5, 0x1B: 3, 0x24: 1, 0x2E: 1, 0x2F: 3}
for o in range(0x1C, 0x24):
    OPERANDS[o] = 3
for o in list(range(0x07, 0x11)):
    OPERANDS[o] = 2
for o in list(range(0x12, 0x1B)) + list(range(0x25, 0x2E)):
    OPERANDS[o] = 1
for o in (0x31, 0x33, 0x37, 0x38, 0x39, 0x3A, 0x3C, 0x3F, 0x49, 0x4C, 0x56, 0x57, 0x59, 0x5A, 0x5B, 0x5D, 0x5E):
    OPERANDS[o] = 2


# from the handlers; push types by the helper each one ends with
NAMES = {0x00: "jmp", 0x01: "jf", 0x02: "call", 0x06: "start", 0x24: "frame", 0x07: "native>bool", 0x0A: "native>float",
         0x0B: "native>int", 0x0F: "native", 0x10: "native>vec", 0x11: "native5", 0x1A: "method", 0x1B: "method3",
         0x25: "pop_bool", 0x27: "pop_int", 0x28: "pop_float", 0x29: "pop_int2", 0x2D: "pop_text", 0x2C: "pop_text2",
         0x30: "and", 0x32: "eq_bool", 0x33: "load_bool", 0x34: "not", 0x35: "ne_bool", 0x36: "or", 0x3B: "fadd",
         0x3D: "fdiv", 0x3F: "load_float", 0x44: "fmul", 0x46: "fsub", 0x47: "fneg", 0x48: "iadd", 0x4A: "idiv",
         0x4B: "ieq", 0x4C: "load_int", 0x4D: "igt", 0x4F: "ilt", 0x52: "ine", 0x53: "imul", 0x54: "isub", 0x55: "ineg",
         0x5B: "load_vec", 0x5D: "load_obj", 0x5E: "load_text"}
LOADS = {0x33, 0x3F, 0x4C, 0x5B, 0x5D, 0x5E}


def parse_from(b, p):
    nf = struct.unpack_from("<H", b, p)[0]
    p += 2
    funcs = []
    for _ in range(nf):
        n = struct.unpack_from("<I", b, p)[0]
        p += 4
        if n > 200:
            raise ValueError("name length")
        name = b[p:p + n].rstrip(b"\0").decode("latin1")
        p += n
        w = struct.unpack_from("<I", b, p)[0]
        p += 4
        code = struct.unpack_from("<%dH" % w, b, p)
        p += 2 * w
        a, nref = struct.unpack_from("<HH", b, p)
        p += 4
        refs = []
        for _ in range(nref):
            t, v = struct.unpack_from("<HI", b, p)
            p += 6
            if t in (7, 8):   # text: v is its length
                refs.append((t, b[p:p + v].rstrip(b"\0").decode("latin1")))
                p += v
            elif t == 3:
                refs.append((t, struct.unpack("<f", struct.pack("<I", v))[0]))
            elif t == 5:   # two values
                refs.append((t, (v, struct.unpack_from("<I", b, p)[0])))
                p += 4
            elif t == 6:   # three values
                refs.append((t, (v,) + struct.unpack_from("<II", b, p)))
                p += 8
            elif t in (0, 4):
                refs.append((t, v))
            else:
                raise ValueError("constant type %d" % t)
        funcs.append((name, code, a, refs))
    return funcs, p


def parse(path):
    """The globals table's size is not decoded yet: functions start where a parse reaches the end of the file exactly."""
    b = open(path, "rb").read()
    for p in range(4, len(b) - 12):
        nf, n = struct.unpack_from("<HI", b, p)
        if not 0 < nf < 5000 or not 2 <= n < 200 or p + 6 + n > len(b) or b[p + 6 + n - 1] != 0:
            continue
        name = b[p + 6:p + 6 + n - 1]
        if not name or not all(32 <= c < 127 for c in name):
            continue
        try:
            funcs, end = parse_from(b, p)
        except (struct.error, ValueError, UnicodeDecodeError):
            continue
        if end == len(b):
            return funcs, p, len(b)
    return [], 0, len(b)


def walk(code):
    i = 0
    out = []
    while i < len(code):
        op = code[i]
        n = OPERANDS.get(op, 0)
        out.append((i, op, code[i + 1:i + 1 + n]))
        i += 1 + n
    return out, i == len(code)


if __name__ == "__main__":
    funcs, end, size = parse(sys.argv[1])
    want = sys.argv[2] if len(sys.argv) > 2 else None
    print("# %d functions, parsed to %d of %d" % (len(funcs), end, size))
    for name, code, a, refs in funcs:
        if want and want not in name:
            continue
        ins, ok = walk(code)
        print("%s: %d words, a=%d, %d refs%s" % (name, len(code), a, len(refs), "" if ok else " (walk ran over)"))
        for i, op, args in ins:
            note = ""
            if op in LOADS and args[0] == 1 and args[1] < len(refs):
                note = "  ; %r" % (refs[args[1]][1],)
            print("  %04X %-12s %s%s" % (i, NAMES.get(op, "op%02X" % op), " ".join("%X" % x for x in args), note))
