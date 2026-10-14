"""tess-gs 14.10.2026: декларации HS из DXIL (чего нет в SPIR-V: maxtessfactor, партиция pow2) и подписи контейнера.

Бит-код DXIL — LLVM 3.7, llvm-dis@15 читает его; часть DXIL содержит заголовок программы (24 байта), бит-код ищем по
магии. Декларации — тег 3 (HS) в списке тегов точки входа dx.entryPoints:
  !{pcfunc, i32 inputCP, i32 outputCP, i32 domain, i32 partitioning, i32 outputPrimitive, float maxTessFactor}.
"""
import re
import struct
import subprocess
import tempfile
from pathlib import Path

LLVM_DIS = '/opt/homebrew/opt/llvm@15/bin/llvm-dis'
DOMAIN = {1: 'isoline', 2: 'tri', 3: 'quad'}
PARTITION = {1: 'integer', 2: 'pow2', 3: 'odd', 4: 'even'}
OUTPUT = {1: 'point', 2: 'line', 3: 'tri_cw', 4: 'tri_ccw'}


def parts(data):
    n = struct.unpack_from('<I', data, 28)[0]
    out = {}
    for o in struct.unpack_from('<%dI' % n, data, 32):
        tag = data[o:o + 4].decode('latin1')
        size = struct.unpack_from('<I', data, o + 4)[0]
        out.setdefault(tag, data[o + 8:o + 8 + size])
    return out


def bitcode(data):
    p = parts(data).get('DXIL')
    if p is None:
        return None
    at = p.find(b'BC\xc0\xde')
    size = struct.unpack_from('<I', p, 20)[0]
    return p[at:at + size] if at >= 0 else None


def _node(ll, n):
    m = re.search(r'^%s = (?:distinct )?!\{(.*)\}$' % re.escape(n), ll, re.M)
    return m.group(1) if m else ''


def hull_declarations(container):
    data = Path(container).read_bytes()
    bc = bitcode(data)
    if bc is None:
        raise ValueError('no DXIL part')
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 'a.bc'
        src.write_bytes(bc)
        r = subprocess.run([LLVM_DIS, str(src), '-o', str(Path(d) / 'a.ll')], capture_output=True, text=True)
        if r.returncode:
            raise ValueError('llvm-dis: ' + r.stderr.strip()[:200])
        ll = (Path(d) / 'a.ll').read_text()
    ep = re.search(r'!dx\.entryPoints = !\{(![0-9]+)\}', ll).group(1)
    tags = _node(ll, _node(ll, ep).split(', ')[-1])
    m = re.search(r'i32 3, (![0-9]+)', tags)
    if not m:
        raise ValueError('no HS state tag')
    st = _node(ll, m.group(1))
    nums = re.findall(r'i32 (\d+)', st)
    mf = re.search(r'float ([0-9.e+-]+)', st)
    in_cp, out_cp, dom, part, prim = map(int, nums[:5])
    return dict(in_cp=in_cp, out_cp=out_cp, domain=DOMAIN[dom], partition=PARTITION[part], output=OUTPUT[prim],
                max_factor=float(mf.group(1)) if mf else None)


def signature(data, tag):
    """Элементы подписи ISG1/OSG1/PSG1 (или ISGN/OSGN/PCSG): name, index, sysvalue, type, register, mask."""
    p = parts(data)
    wide = tag in p
    chunk = p.get(tag) or p.get({'ISG1': 'ISGN', 'OSG1': 'OSGN', 'PSG1': 'PCSG'}[tag])
    if chunk is None:
        return []
    n = struct.unpack_from('<I', chunk, 0)[0]
    step = 32 if wide else 24
    out = []
    for k in range(n):
        e = 8 + k * step + (4 if wide else 0)
        name_at, index, sysvalue, ctype, reg = struct.unpack_from('<5I', chunk, e)
        mask = chunk[e + 20]
        name = chunk[name_at:chunk.find(b'\0', name_at)].decode('latin1')
        out.append(dict(name=name, index=index, sysvalue=sysvalue, type=ctype, register=reg, mask=mask,
                        component=(mask & -mask).bit_length() - 1 if mask else 0))
    return out


if __name__ == '__main__':
    import json, sys
    print(json.dumps(hull_declarations(sys.argv[1])))
    for tag in ('ISG1', 'OSG1', 'PSG1'):
        print(tag, json.dumps(signature(Path(sys.argv[1]).read_bytes(), tag)))
