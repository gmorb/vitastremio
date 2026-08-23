#!/usr/bin/env python3
"""Catch use-before-definition and missing definitions in the Vita sources.

These files can't be compiled without vitasdk, so this class of error --
introduced by a bad search/replace deleting or reordering a function --
otherwise surfaces only as a failed build on the device. Cheap to check
statically: C requires a declaration before use.
"""
import re, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "..", "src")

DEF_RE   = re.compile(r'^\s*(?:static\s+)?[A-Za-z_][\w \*]*?\b(\w+)\s*\([^;]*?\)\s*$', re.M)
MACRO_RE = re.compile(r'^\s*#\s*define\s+(\w+)', re.M)
CALL_RE  = re.compile(r'\b(\w+)\s*\(')

KEYWORDS = {'if','for','while','switch','return','sizeof','defined','do','else'}

def header_decls():
    """Names prototyped in any src/*.h.

    A call before the definition is fine when a header already declared it,
    which is normal for a module's public API used internally. Without this
    the check reports the correct code as broken, and a linter that cries
    wolf gets switched off. """
    names = set()
    for fn in os.listdir(SRC):
        if not fn.endswith('.h'):
            continue
        for m in re.finditer(r'\b(\w+)\s*\([^;{]*\)\s*;',
                             open(os.path.join(SRC, fn)).read()):
            names.add(m.group(1))
    return names


PROTOTYPED = None


def check(path):
    global PROTOTYPED
    if PROTOTYPED is None:
        PROTOTYPED = header_decls()
    lines = open(path).read().split('\n')
    problems = []

    defined = {}          # name -> line index of definition
    for i, ln in enumerate(lines):
        m = re.match(r'^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(', ln)
        if m and not ln.strip().startswith(('//', '*', '/*')) and '=' not in ln.split('(')[0]:
            # a definition line ends with ) or ) { and the next non-blank is { or it ends in {
            tail = ln.rstrip()
            if tail.endswith(('{', ')')) and ';' not in tail:
                defined.setdefault(m.group(1), i)

    macros = {}
    for i, ln in enumerate(lines):
        m = MACRO_RE.match(ln)
        if m: macros.setdefault(m.group(1), i)

    for i, ln in enumerate(lines):
        stripped = ln.strip()
        if stripped.startswith(('//','*','/*','#')): continue
        for name in CALL_RE.findall(ln):
            if name in KEYWORDS: continue
            if name in PROTOTYPED:
                continue
            if name in defined and defined[name] > i and defined[name] != i:
                problems.append((i+1, "call to '%s' before its definition (line %d)"
                                 % (name, defined[name]+1)))
        for tok in re.findall(r'\b([A-Z][A-Z0-9_]{3,})\b', ln):
            if tok in macros and macros[tok] > i:
                problems.append((i+1, "uses macro %s defined later (line %d)"
                                 % (tok, macros[tok]+1)))
    return problems

def check_struct_members(path):
    """Flag P.<field> references whose field is not declared in vs_player.

    Renaming or removing a struct field leaves stale uses that only surface
    as a compile error on the device. Same class of miss as use-before-
    definition, so it belongs in the same guard.
    """
    src = open(path).read()
    m = re.search(r'typedef struct \{(.*?)\}\s*vs_player;', src, re.S)
    if not m:
        return []

    # Strip comments, then take every identifier in the struct body and
    # subtract type keywords. Over-collecting is the safe direction: a lint
    # that reports false positives gets ignored, which is worse than one
    # that occasionally misses. Comma-separated declarators like
    # "SceUID vthread, athread;" are the case a stricter pattern missed.
    body = re.sub(r'/\*.*?\*/', ' ', m.group(1), flags=re.S)
    body = re.sub(r'//[^\n]*', ' ', body)
    TYPES = {'volatile','const','unsigned','signed','static','struct','union',
             'int','long','short','char','float','double','void','SceUID',
             'SceUInt64','SceUInt32','vita2d_texture','frame_slot','vs_ring',
             'vs_conn','annexb_state','SceAvcdecCtrl','size_t'}
    fields = {t for t in re.findall(r'\b(\w+)\b', body)} - TYPES
    fields = {f for f in fields if not f.isdigit()}
    problems = []
    for i, ln in enumerate(src.split('\n')):
        st = ln.strip()
        if st.startswith(('//', '*', '/*', '#')):
            continue
        for name in re.findall(r'\bP\.(\w+)', ln):
            if name not in fields:
                problems.append((i + 1, "P.%s is not a member of vs_player" % name))
    return problems


bad = 0
for fn in sorted(os.listdir(SRC)):
    if not fn.endswith(('.c', '.h')): continue
    probs = check(os.path.join(SRC, fn))
    if fn == 'player.c':
        probs += check_struct_members(os.path.join(SRC, fn))
    if probs:
        bad += len(probs)
        print("%s:" % fn)
        seen = set()
        for line, msg in probs:
            if msg in seen: continue
            seen.add(msg)
            print("  line %-5d %s" % (line, msg))

print("\n%s (%d issue%s)" % ("FAILED" if bad else "PASSED", bad, "" if bad == 1 else "s"))
sys.exit(1 if bad else 0)
