"""
What recomp_manual.c defines.

Single source of truth, deliberately. Both the recompiler (deciding what NOT to
generate) and gen_dangling_stubs.py (deciding what still needs a stub) have to
agree on what counts as a definition. When they each had their own regex they
disagreed, and every disagreement is a link error: a definition missed by one
becomes "already defined", a definition invented by the other becomes
"unresolved external".
"""

import os
import re
import sys

# A definition is "void sub_XXXXXXXX(void)" followed by a body -- on the same
# line (`void sub_X(void) { esp += 4; return; }`) or the next. A declaration
# ends in ';' and must not match.
DEF_RE = re.compile(r"^void (\w*?(?:sub_)?([0-9A-Fa-f]{8}))\(void\)\s*(?:\{|$)", re.M)
_SUB_DEF_RE = re.compile(r"^void (sub_([0-9A-Fa-f]{8}))\(void\)\s*(?:\{|$)", re.M)
# recomp_manual.c wraps rather than replaces some functions: it defines sub_X
# itself and calls the generated body as sub_X_gen.
_WRAP_RE = re.compile(r"^extern void (sub_([0-9A-Fa-f]{8}))_gen\(void\);", re.M)


def strip_disabled(src):
    """Remove '#if 0 ... #endif' regions.

    recomp_manual.c keeps disabled overrides around as documentation. A
    definition inside one is not a definition; counting it makes the
    recompiler skip a function that then nothing defines.
    """
    out, depth = [], 0
    for line in src.splitlines(keepends=True):
        s = line.strip()
        if re.match(r"^#if\s+0\b", s):
            depth += 1
        elif depth and re.match(r"^#if(def|ndef)?\b", s):
            depth += 1  # nested conditional inside a disabled block
        elif depth and s.startswith("#endif"):
            depth -= 1
        elif depth == 0:
            out.append(line)
    return "".join(out)


def definition_names(path):
    """Names of functions actually defined (compiled) in `path`."""
    if not os.path.exists(path):
        return set()
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
    return {m.group(1) for m in _SUB_DEF_RE.finditer(live)}


_REF_RE = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b")

# An entry hook: `void sub_XXXXXXXX_enter(void)` defined by hand is called at
# the top of the generated sub_XXXXXXXX, before its first instruction. Unlike
# a _gen wrapper it is reached from direct callers too, and the function's own
# code stays generated: the way to trace, or hold, a function that is called
# directly without writing its body out by hand.
_ENTER_RE = re.compile(r"^void sub_([0-9A-Fa-f]{8})_enter\(void\)\s*(?:\{|$)", re.M)


def _paths(path):
    """One manual file, or several (--exclude-manual given more than once)."""
    return list(path) if isinstance(path, (list, tuple)) else [path]


def entry_hooks(path):
    """Addresses of functions the manual file(s) define an entry hook for."""
    if len(_paths(path)) > 1:
        return set().union(*(entry_hooks(p) for p in _paths(path)))
    path = _paths(path)[0]
    if not os.path.exists(path):
        return set()
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
    return {int(m.group(1), 16) for m in _ENTER_RE.finditer(live)}


# A wrapper macro: `#define NAME(param, ...)` whose body defines `void sub_##param(void)` (optionally extern "C") and
# calls `sub_##param##_gen`, as HP:PoA's SNEAK_WRAP, SFX_HOOK and HLE. Each use `NAME(XXXXXXXX, ...)` at a line's
# start defines sub_XXXXXXXX by hand and wraps its generated body, as a written-out wrapper does. A macro that only
# calls sub_##param##_gen (FALLBACK) defines nothing and is not one.
_MACRO_DEF_RE = re.compile(r"^#define\s+(\w+)\(\s*(\w+)[^)]*\)((?:[^\n]*\\\n)*[^\n]*)", re.M)


def wrapper_macro_uses(live):
    """Addresses defined (and wrapped) through wrapper macros in already-stripped source."""
    found = set()
    for m in _MACRO_DEF_RE.finditer(live):
        name, param, body = m.group(1), m.group(2), m.group(3)
        defines = re.search(r'void\s+sub_##' + re.escape(param) + r'\s*\(\s*void\s*\)', body)
        if not defines or ("sub_##" + param + "##_gen") not in body:
            continue
        for use in re.finditer(r"^\s*" + re.escape(name) + r"\(\s*([0-9A-Fa-f]{8})\b", live, re.M):
            found.add(int(use.group(1), 16))
    return found


def scan(path):
    """(skip, wrap, referenced) for functions the manual file handles.

    skip:       defined by hand -- gen must declare but not define them.
    wrap:       manual calls the generated body as sub_X_gen -- gen must still
                emit it, renamed.
    referenced: every sub_XXXXXXXX the hand-written code mentions at all. Those
                must keep the sub_ name in the generated output: recomp_manual.c
                declares and calls sub_00350C10, so if a naming pass renamed it
                to SwapCopy_D3D_..., gen defines that and nothing defines
                sub_00350C10. Correctness beats a prettier name.

    Several files (a list) give the union of their scans.
    """
    if len(_paths(path)) > 1:
        parts = [scan(p) for p in _paths(path)]
        return tuple(set().union(*(part[i] for part in parts)) for i in range(3))
    path = _paths(path)[0]
    if not os.path.exists(path):
        print(f"WARNING: {path} not found; excluding nothing.", file=sys.stderr)
        return set(), set(), set()
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
    skip = {int(m.group(2), 16) for m in _SUB_DEF_RE.finditer(live)}
    wrap = {int(m.group(2), 16) for m in _WRAP_RE.finditer(live)}
    macro_wraps = wrapper_macro_uses(live)
    skip |= macro_wraps
    wrap |= macro_wraps
    referenced = {int(m.group(1), 16) for m in _REF_RE.finditer(live)}
    return skip, wrap, referenced
