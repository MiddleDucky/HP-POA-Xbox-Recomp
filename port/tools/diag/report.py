#!/usr/bin/env python3
"""Reads a diagnostics report (run/reports/<name>, written by hppoa_diag.exe / Ctrl+Shift+R, hle_diag.c).

  report.py <name>                 summary: note, state, log problems, symbolised stacks, frames, flicker suspects
  report.py <name> --frames        each captured frame's passes (render targets in order, draw counts)
  report.py <name> --draw F:N      frame F's draw N: its line, textures and the full state accumulated to it
  report.py <name> --tex ADDR      every draw sampling texture ADDR (hex), per frame, with its content hash
  report.py <name> --log PATTERN   log lines matching a regular expression
  report.py --list                 the reports there are, newest last

<name> is the folder's name (20261020_101500) or a path. Stacks resolve against port/run/hppoa.exe; when that exe
was rebuilt after the report, the names may be wrong and it says so.
"""
import argparse
import collections
import difflib
import os
import re
import shutil
import subprocess
import sys

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
RUN = os.path.join(PORT, 'run')
REPORTS = os.path.join(RUN, 'reports')
EXE = os.path.join(RUN, 'hppoa.exe')

LOG_PROBLEM = re.compile(r'CRASH|EXCEPTION|ERROR|FATAL|ASSERT|unhandled|unimplemented|failed|NaN|deadlock|timeout|'
                         r'hung|stuck|abort|access violation', re.I)
# known harmless: infinite waits logged by the kernel, the host's Vulkan frame-generation layer
LOG_BENIGN = re.compile(r'timeout=INFINITE|lsfg-vk')


def find_report(name):
    if os.path.isdir(name):
        return os.path.abspath(name)
    p = os.path.join(REPORTS, name)
    if os.path.isdir(p):
        return p
    hits = sorted(d for d in os.listdir(REPORTS) if name in d) if os.path.isdir(REPORTS) else []
    if hits:
        return os.path.join(REPORTS, hits[-1])
    sys.exit(f'no report {name!r} under {REPORTS}')


def read(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return None


# ── symbols ────────────────────────────────────────────────────────────────

class Symbols:
    def __init__(self):
        nm = shutil.which('x86_64-w64-mingw32-nm') or shutil.which('nm')
        self.addrs, self.names = [], []
        if not nm or not os.path.exists(EXE):
            return
        out = subprocess.run([nm, '-n', '-C', EXE], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split(None, 2)
            if len(parts) == 3 and parts[1] in 'tTwW':
                self.addrs.append(int(parts[0], 16))
                self.names.append(parts[2])

    def name(self, a):
        import bisect
        i = bisect.bisect_right(self.addrs, a) - 1
        if i < 0 or a - self.addrs[i] > 0x100000:
            return None
        return f'{self.names[i]}+{a - self.addrs[i]:x}'


def describe(sym, a, base):
    if base and base != 0x140000000 and a >= base:
        a = a - base + 0x140000000  # nm's addresses assume the preferred base
    if 0x140000000 <= a < 0x150000000:
        n = sym.name(a)
        if n:
            return n
    return f'{a:x}' + (' (wine/system)' if a >= 0x6f0000000000 else '')


def stacks(rep, sym, base):
    out = []
    text = read(os.path.join(rep, 'stacks.txt'))
    if text:
        groups = collections.OrderedDict()
        for line in text.splitlines():
            m = re.match(r'thread (\d+): (.*)', line)
            if not m:
                continue
            frames = [int(x, 16) for x in m.group(2).split()]
            names = [describe(sym, a, base) for a in frames]
            # the interesting part: our own frames (system waits at the top say only "waiting")
            own = [n for n in names if not n.endswith('(wine/system)')]
            key = tuple(own[:6])
            groups.setdefault(key, []).append(m.group(1))
        out.append('stacks (threads with the same top frames together; system frames left out):')
        for key, tids in groups.items():
            out.append(f'  {len(tids)} thread(s) {",".join(tids[:6])}{"..." if len(tids) > 6 else ""}:')
            for n in key or ('(only system frames: idle in a wait)',):
                out.append(f'      {n}')
        rest = [l for l in text.splitlines() if l and not l.startswith(('thread ', '#'))]
        if rest:
            out.append('  locks / I/O workers:')
            out += ['    ' + l for l in rest[:40]]
    ext = read(os.path.join(rep, 'stacks_external.txt'))
    if ext:
        per = collections.defaultdict(collections.Counter)
        for line in ext.splitlines():
            m = re.match(r'thread (\d+): (\S+) \S+ (.*)', line)
            if m:
                per[m.group(1)][describe(sym, int(m.group(2), 16), base)] += 1
        out.append('threads sampled from outside (hung game): instruction pointer, times seen of 10')
        for tid, c in per.items():
            out.append(f'  {tid}: ' + ', '.join(f'{n} x{k}' for n, k in c.most_common(3)))
    return out


# ── frames ─────────────────────────────────────────────────────────────────

DRAW = re.compile(r'^(\d{4}) draw rt (\w+) ds (\w+) prim (\d+) count (\d+) vs (\w+) ps (\w+)')
TEX = re.compile(r'\| t(\d) (\w+) fmt (\w+) (\d+x\d+)([^|]*)')


class Draw:
    __slots__ = ('i', 'line', 'rt', 'prim', 'count', 'vs', 'ps', 'tex', 'st', 'state', 'sub')

    def key(self):
        return (self.rt, self.vs, self.ps, self.prim, self.count)


def parse_frame(path):
    draws, passes = [], []
    state = {}
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        m = DRAW.match(line)
        if m:
            d = Draw()
            d.i, d.rt, d.prim, d.count, d.vs, d.ps = (int(m.group(1)), m.group(2), int(m.group(4)),
                                                     int(m.group(5)), m.group(6), m.group(7))
            d.line, d.st, d.sub, d.state = line, {}, [], dict(state)
            d.tex = {}
            for t in TEX.finditer(line):
                h = re.search(r'hash (\w+)', t.group(5))
                d.tex[int(t.group(1))] = (t.group(2), t.group(3), t.group(4), h.group(1) if h else None,
                                          'target' in t.group(5))
            draws.append(d)
            cur = d
            if not passes or passes[-1][0] != d.rt:
                passes.append([d.rt, 0])
            passes[-1][1] += 1
            continue
        if line.startswith('     st') and cur is not None:
            for kv in line[7:].split():
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    cur.st[k] = v
                    state[k] = v
            cur.state = dict(state)
            continue
        if line.startswith('     ') and cur is not None:
            cur.sub.append(line.strip())
        elif not m:
            cur = None
            if ' clear rt ' in line:
                passes.append(['clear ' + line.split(' clear rt ')[1].split()[0], 0])
    return draws, passes


def frame_dirs(rep):
    return sorted(os.path.join(rep, d) for d in os.listdir(rep)
                  if d.startswith('frame_') and os.path.exists(os.path.join(rep, d, 'frame.txt')))


def load_frames(rep):
    out = []
    for fd in frame_dirs(rep):
        draws, passes = parse_frame(os.path.join(fd, 'frame.txt'))
        out.append((os.path.basename(fd), draws, passes))
    return out


def flicker(frames, limit=25):
    """draws that line up across frames (same target, shaders, primitive, count) but sample other textures or
    texture contents, or change state, from frame to frame"""
    if len(frames) < 2:
        return ['flicker check: needs 2 or more frames']
    base_name, base, _ = frames[0]
    tex_diff = collections.OrderedDict()
    st_diff = collections.Counter()
    st_example = {}
    for name, draws, _ in frames[1:]:
        sm = difflib.SequenceMatcher(a=[d.key() for d in base], b=[d.key() for d in draws], autojunk=False)
        for blk in sm.get_matching_blocks():
            for k in range(blk.size):
                a, b = base[blk.a + k], draws[blk.b + k]
                for s in set(a.tex) | set(b.tex):
                    ta, tb = a.tex.get(s), b.tex.get(s)
                    if not ta or not tb or ta[4] or tb[4]:
                        continue  # render targets change every frame
                    if ta[0] != tb[0] or (ta[3] and tb[3] and ta[3] != tb[3]):
                        tex_diff.setdefault((a.key(), s), []).append(
                            f'{base_name} #{a.i} t{s} {ta[0]} hash {ta[3]}  vs  {name} #{b.i} t{s} {tb[0]} hash {tb[3]}')
                for k2 in set(a.state) | set(b.state):
                    if a.state.get(k2) != b.state.get(k2):
                        st_diff[(a.key(), k2)] += 1
                        st_example.setdefault((a.key(), k2), f'{base_name} #{a.i} {k2}={a.state.get(k2)}  vs  '
                                                            f'{name} #{b.i} {k2}={b.state.get(k2)}')
    out = [f'flicker check over {len(frames)} frames (draws matched to {base_name} by target/shaders/count):']
    if not tex_diff:
        out.append('  no matched draw sampled another texture or other texture contents')
    for (key, s), ex in list(tex_diff.items())[:limit]:
        out.append(f'  TEXTURE CHANGES rt {key[0]} vs {key[1]} ps {key[2]} count {key[4]}: ' + ex[0] +
                   (f'  (+{len(ex) - 1} more)' if len(ex) > 1 else ''))
    # state that differs in few matched draws is the suspicious kind (a whole-frame difference is camera/time)
    few = [(k, n) for k, n in st_diff.items() if not k[1].startswith('vp')]
    few.sort(key=lambda kn: kn[1])
    if few:
        out.append(f'  state differing between matched draws: {len(few)} (draw, state) pairs; first ones:')
        for (key, k2), n in few[:limit]:
            out.append(f'    {st_example[(key, k2)]}   [rt {key[0]} vs {key[1]} count {key[4]}, {n}x]')
    return out


# ── main ───────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('name', nargs='?')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--frames', action='store_true')
    ap.add_argument('--draw')
    ap.add_argument('--tex')
    ap.add_argument('--log')
    a = ap.parse_args()
    if a.list or not a.name:
        for d in sorted(os.listdir(REPORTS)) if os.path.isdir(REPORTS) else []:
            note = (read(os.path.join(REPORTS, d, 'note.txt')) or '').strip().splitlines()
            print(f'{d}  {note[0][:90] if note else ""}')
        return
    rep = find_report(a.name)
    state = read(os.path.join(rep, 'state.txt')) or ''
    m = re.search(r'image base (\w+)', state)
    base = int(m.group(1), 16) if m else 0

    if a.log:
        rx = re.compile(a.log)
        for lg in ('last_run.log', 'hppoa_run.log', 'xbox_kernel.log'):
            for n, line in enumerate((read(os.path.join(rep, lg)) or '').splitlines(), 1):
                if rx.search(line):
                    print(f'{lg}:{n}: {line}')
        return
    if a.draw:
        f, n = a.draw.split(':')
        frames = load_frames(rep)
        name, draws, _ = frames[int(f)]
        d = next((d for d in draws if d.i == int(n)), None)
        if not d:
            sys.exit(f'{name} has no draw {n}')
        print(f'{name} {d.line}')
        for s in d.sub:
            print('   ', s)
        print('  full state (accumulated):', ' '.join(f'{k}={v}' for k, v in sorted(d.state.items())))
        return
    if a.tex:
        want = a.tex.upper().lstrip('0X').zfill(8)
        for name, draws, _ in load_frames(rep):
            for d in draws:
                for s, t in d.tex.items():
                    if t[0].upper() == want:
                        print(f'{name} #{d.i} t{s} hash {t[3]} rt {d.rt} vs {d.vs} ps {d.ps} count {d.count}')
        return

    print(f'== report {os.path.basename(rep)}')
    for f in ('note.txt', 'report.txt', 'state.txt', 'files.txt'):
        t = read(os.path.join(rep, f))
        if t:
            print(f'-- {f}')
            print(t.rstrip() if f != 'state.txt' else '\n'.join(t.splitlines()[:7]))
    m = re.search(r'built (\S+ \S+)', state)
    if m and os.path.exists(EXE):
        import time
        now = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(os.path.getmtime(EXE)))
        if now != m.group(1):
            print(f'!! hppoa.exe was rebuilt since the report ({m.group(1)} -> {now}): stack names may be wrong')

    print('-- logs: problem lines (last 25) and the tail')
    for lg in ('last_run.log', 'hppoa_run.log'):
        t = read(os.path.join(rep, lg))
        if not t:
            continue
        lines = t.splitlines()
        bad = [f'{lg}:{n}: {l}' for n, l in enumerate(lines, 1) if LOG_PROBLEM.search(l) and not LOG_BENIGN.search(l)]
        print(f'{lg}: {len(lines)} lines, {len(bad)} problem lines')
        for l in bad[-25:]:
            print('  ' + l[:220])
        for l in lines[-8:]:
            print('  | ' + l[:220])

    print('--', end=' ')
    sym = Symbols()
    print('\n'.join(stacks(rep, sym, base)) or 'no stacks')

    frames = load_frames(rep)
    print(f'-- frames: {len(frames)}')
    for name, draws, passes in frames:
        targets = collections.Counter(d.rt for d in draws)
        print(f'  {name}: {len(draws)} draws, {len(targets)} targets, {len(passes)} passes')
        if a.frames:
            for p in passes:
                print(f'      {p[0]}: {p[1]} draws')
    for l in flicker(frames):
        print(l)


if __name__ == '__main__':
    main()
