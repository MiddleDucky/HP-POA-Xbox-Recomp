#!/usr/bin/env python3
"""Fail the build when code reads a thread local variable through its plain global address.

On MinGW a thread local variable's symbol sits in .tls: it is the template every thread's copy starts from. Code must
reach the copy through the TLS slot (gs:0x58 + _tls_index). A `.refptr.<name>` access to such a symbol reads the
template instead, which never changes: MinGW GCC 16 emitted that for g_fp_stack at -O3, and the lifted x87 stack then
read zeros. This lists every .refptr use of a .tls symbol in the linked executable.
"""
import argparse
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("exe")
    a = ap.parse_args()

    lo = hi = None
    for line in subprocess.run([a.objdump, "-h", a.exe], capture_output=True, text=True, check=True).stdout.splitlines():
        f = line.split()
        if len(f) >= 4 and f[1] == ".tls":
            lo, hi = int(f[3], 16), int(f[3], 16) + int(f[2], 16)
    if lo is None:
        return 0
    tls = set()
    for line in subprocess.run([a.nm, a.exe], capture_output=True, text=True, check=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in "dDbB" and lo <= int(f[0], 16) < hi:
            tls.add(f[2])

    dis = subprocess.run([a.objdump, "-d", "--no-show-raw-insn", a.exe], capture_output=True, text=True,
                         check=True).stdout
    func, bad = "?", []
    ref = re.compile(r"<\.refptr\.([A-Za-z0-9_]+)>")
    for line in dis.splitlines():
        if line.endswith(">:"):
            func = line.split("<", 1)[1][:-2]
            continue
        m = ref.search(line)
        if m and m.group(1) in tls:
            bad.append(f"{func}: {line.strip()}")
    if bad:
        print(f"check_tls_refs: {len(bad)} read(s) of thread local variables through their global address:",
              file=sys.stderr)
        for b in bad[:50]:
            print("  " + b, file=sys.stderr)
        return 1
    print(f"check_tls_refs: {len(tls)} thread local variables, no global address reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
