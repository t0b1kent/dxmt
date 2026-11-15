#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later. Copyright (c) 2026 Timur Ravilov.
"""TSO64 12.11.2026: compiler wrapper that removes loads of PAIRS of 128-bit registers (ldp q) from the generated code: under Apple hardware
TSO on M1 such a load costs +4.4..6 ns, a single ldr q costs nothing extra. usage (as CC): ldpfilter.py <real compiler> <args...>
Compile steps (-c ... -o OUT) run as: real -S -> rewrite the assembly -> real -c. Everything else is passed through unchanged.
Rewrite (same registers, same memory, same order of effects on registers; loads only, stores untouched):
  ldp qA, qB, [base, #o]        -> ldr qA, [base, #o] ; ldr qB, [base, #o+16]
  ldp qA, qB, [base], #i        -> ldr qB, [base, #16] ; ldr qA, [base], #i
  ldp qA, qB, [base, #i]!       -> ldr qA, [base, #i]! ; ldr qB, [base, #16]
and the SEH unwind directive that follows inside an epilogue (.seh_save_any_reg_p / _px) is split the same way.
TSO64_LDPFILTER=0 disables the rewrite (the round trip through assembly stays, to prove it is neutral); TSO64_LDPFILTER_LOG=<file> appends
one line per object: <out> <pairs rewritten> <pairs kept>."""
import os, re, subprocess, sys
real, args = sys.argv[1], sys.argv[2:]
SRC = ('.c', '.cc', '.cpp', '.cxx', '.m', '.mm')
srcs = [a for a in args if a.endswith(SRC) and not a.startswith('-')]
if '-c' not in args and '-shared' in args and 'arm64ec-w64-mingw32' in os.path.basename(real) and '-nostartfiles' not in args \
        and os.environ.get('TSO64_LDPFILTER', '1') != '0':
    # Link of an ARM64EC DLL: the driver puts dllcrt2.o BEFORE the user's objects, and its (prebuilt, pair-loading) copies of the COMDAT
    # entry thunks ($ientry_thunk$cdecl$i8$i8, ...i8i8i8) win over ours. Same inputs, startup objects moved after the user's objects.
    lib = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(real))), 'arm64ec-w64-mingw32', 'lib')
    if not os.path.isdir(lib):   # INTEGRATE111 12.11: llvm-mingw 20260908 keeps the (hybrid) ARM64EC startup objects in aarch64-w64-mingw32/lib
        lib = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(real))), 'aarch64-w64-mingw32', 'lib')
    last = max(i for i, a in enumerate(args) if a.endswith(('.obj', '.o')) and not a.startswith('-'))
    args = args[:last + 1] + [os.path.join(lib, 'dllcrt2.o'), os.path.join(lib, 'crtbegin.o')] + args[last + 1:] + [os.path.join(lib, 'crtend.o'), '-nostartfiles']
    os.execv(real, [real] + args)
if '-c' not in args or '-o' not in args or len(srcs) != 1 or '-E' in args or '-S' in args:
    os.execv(real, [real] + args)
out = args[args.index('-o') + 1]; tmp = out + '.tso64.s'
a1 = list(args); a1[a1.index('-c')] = '-S'; a1[a1.index('-o') + 1] = tmp
rc = subprocess.run([real] + a1).returncode
if rc: sys.exit(rc)
LDP = re.compile(r'^(\s*)ldp\s+q(\d+),\s*q(\d+),\s*\[(sp|x\d+)(?:,\s*#(-?\d+))?\](!)?(?:,\s*#(-?\d+))?\s*((?://|;).*)?$')
SEH = re.compile(r'^(\s*)\.seh_save_any_reg_(p|px)\s+q(\d+),\s*(-?\d+)\s*$')
def ld(q, base, off, pre=False, post=None):
    if post is not None: return 'ldr\tq%d, [%s], #%d' % (q, base, post)
    if pre: return 'ldr\tq%d, [%s, #%d]!' % (q, base, off)
    if off == 0: return 'ldr\tq%d, [%s]' % (q, base)
    if off > 0 and off % 16 == 0 and off <= 65520: return 'ldr\tq%d, [%s, #%d]' % (q, base, off)
    if -256 <= off <= 255: return 'ldur\tq%d, [%s, #%d]' % (q, base, off)
    return None
SECT = re.compile(r'^(\s*\.section\s+)([^",\s]*[#\x80-\xff][^,\s]*)(,.*)$')   # INTEGRATE111 13.11: also section names with non-ASCII bytes (rpcrt4 has a function with a Cyrillic name; -ffunction-sections prints its section unquoted)
lines = [SECT.sub(r'\1"\2"\3', x) for x in open(tmp, encoding='latin-1').read().split('\n')]
# ARM64EC: the mutual anti-dependency pair (X = "#X" / "#X" = X), printed before every use of X, cannot be parsed back in place
# ("assembler label can not be undefined"); once per symbol at the end of the file it assembles to the same symbol table.
_body = []; _blocks = []; _seen = set(); _i = 0
while _i < len(lines):
    if _i + 3 < len(lines) and lines[_i].strip().startswith('.weak_anti_dep') and ' = ' in lines[_i + 1] and lines[_i + 2].strip().startswith('.weak_anti_dep') and ' = ' in lines[_i + 3]:
        _a, _b = lines[_i + 1].split(' = '); _c, _d = lines[_i + 3].split(' = ')
        if _c == _b and _d == _a:
            if _a not in _seen: _seen.add(_a); _blocks += lines[_i:_i + 4]
            _i += 4; continue
    _body.append(lines[_i]); _i += 1
lines = _body + _blocks; res = []; done = kept = 0; i = 0
enabled = os.environ.get('TSO64_LDPFILTER', '1') != '0'
while i < len(lines):
    l = lines[i]; m = LDP.match(l) if enabled else None
    if not m: res.append(l); i += 1; continue
    ind, a, b, base = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
    off = int(m.group(5) or 0); pre = bool(m.group(6)); post = m.group(7); post = int(post) if post is not None else None
    seh = SEH.match(lines[i + 1]) if i + 1 < len(lines) else None
    if post is not None:
        first, second = (ld(b, base, 16), ld(a, base, 0, post=post)) if -256 <= post <= 255 else (None, None); so = [(b, '', 16), (a, '_x', post)]
    elif pre:
        first, second = (ld(a, base, off, pre=True), ld(b, base, 16)) if -256 <= off <= 255 else (None, None); so = [(a, '_x', -off), (b, '', 16)]
    else:
        first, second = ld(a, base, off), ld(b, base, off + 16); so = [(a, '', off), (b, '', off + 16)]
    if first is None or second is None or (seh and (int(seh.group(3)) != a or base != 'sp')):
        res.append(l); kept += 1; i += 1; continue
    if seh:
        want = 'px' if (pre or post is not None) else 'p'
        if seh.group(2) != want: res.append(l); kept += 1; i += 1; continue
        res += [ind + first, '%s.seh_save_any_reg%s\tq%d, %d' % (seh.group(1), so[0][1], so[0][0], abs(so[0][2])),
                ind + second, '%s.seh_save_any_reg%s\tq%d, %d' % (seh.group(1), so[1][1], so[1][0], abs(so[1][2]))]
        i += 2
    else:
        res += [ind + first, ind + second]; i += 1
    done += 1
open(tmp, 'w', encoding='latin-1').write('\n'.join(res))
a2 = []; skip = 0
for x in args:
    if skip: skip -= 1; continue
    if x in ('-MF', '-MQ', '-MT'): skip = 1; continue
    if x == '-x': skip = 1; continue
    if x in ('-xc', '-xc++', '-xobjective-c', '-xobjective-c++'): continue
    if x in ('-MD', '-MMD'): continue
    a2.append(tmp if x == srcs[0] else x)
rc = subprocess.run([real] + a2 + ['-Wno-unused-command-line-argument']).returncode
fb = 0
if rc != 0:   # INTEGRATE111 13.11: the round trip through assembly failed - compile this object directly (as without the wrapper) and say so in the log
    rc = subprocess.run([real] + args).returncode; fb = 1
log = os.environ.get('TSO64_LDPFILTER_LOG')
if log:
    with open(log, 'a') as f: f.write('%s %d %d rc=%d%s\n' % (out, done, kept, rc, ' FALLBACK-DIRECT' if fb else ''))
if rc == 0 and not os.environ.get('TSO64_LDPFILTER_KEEP'): os.unlink(tmp)
sys.exit(rc)
