"""tess-gs 14.10.2026: понижение геометрической стадии (SPIR-V Geometry) в compute-тело с ЗАПИСЬЮ выдач.

Способ 3 сдачи 0083 (C.5, mesh-функция) в ABI прежнего пути: тело GS исполняется одной нитью на входной
примитив, каждый EmitVertex ДОСЛОВНО снимает копию всех выходов в запись k (k < maxvertexcount), EndPrimitive
ставит флаг «разрез после записи k». Сборку полос в треугольники/линии и запись в mesh делает обёртка
(gs_compose), повторяя формулы прежнего конвертера (dxbc_converter_gs.cpp). Выдачи сверх maxvertexcount
подавляются (насыщение, как в 0083). Потоки: только stream 0.

Преобразование SPIR-V:
  * Input/Output переменные -> Private (как lower_isoline), интерфейсные декорации сняты (роли — в abi);
  * новые Private-глобалы: rec_<o>: T_o[maxv] на каждый выход o, mr_gs_count: uint, mr_gs_cut: uint[maxv];
  * функции mr_gs_emit / mr_gs_cut; OpEmitVertex/OpEndPrimitive (и Stream-варианты для потока 0) -> вызовы;
  * точка входа: обёртка mr_isoline_compute (GLCompute, LocalSize 1) зовёт прежний main; режимы GS сняты.
"""
import struct
from collections import defaultdict


class Unsupported(ValueError):
    pass


IN_PRIM = {19: 'point', 20: 'line', 21: 'lineadj', 22: 'triangle', 23: 'triangleadj'}
OUT_TOPO = {27: 'point', 28: 'line', 29: 'triangle'}
VERTICES_PER_PRIMITIVE = {'point': 1, 'line': 2, 'lineadj': 4, 'triangle': 3, 'triangleadj': 6}
IO_DECOS = (11, 13, 14, 15, 16, 17, 18, 30, 31)   # BuiltIn, NoPerspective, Flat, Patch, Centroid, Sample, Invariant, Location, Component


def lower(data):
    if len(data) < 20 or len(data) % 4:
        raise Unsupported('SPIRV length')
    w = list(struct.unpack('<%dI' % (len(data) // 4), data)); ins = []; i = 5
    if w[0] != 0x07230203:
        raise Unsupported('SPIRV magic')
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 65535
        if not n or i + n > len(w):
            raise Unsupported('instruction bounds')
        ins.append((op, list(w[i + 1:i + n]))); i += n
    entries = [x for op, x in ins if op == 15]
    if len(entries) != 1:
        raise Unsupported('single entry required')
    stage, main = entries[0][:2]
    if stage != 3:
        raise Unsupported('not a geometry shader (execution model %d)' % stage)
    modes = [x[1:] for op, x in ins if op == 16 and x[0] == main]
    in_prim = next((IN_PRIM[m[0]] for m in modes if m[0] in IN_PRIM), None)
    out_topo = next((OUT_TOPO[m[0]] for m in modes if m[0] in OUT_TOPO), None)
    maxv = next((m[1] for m in modes if m[0] == 26), None)
    invocations = next((m[1] for m in modes if m[0] == 0), 1)
    if in_prim is None or out_topo is None or not maxv:
        raise Unsupported('missing GS execution modes: in=%s out=%s maxv=%s' % (in_prim, out_topo, maxv))
    if maxv > 256:
        raise Unsupported('maxvertexcount %d > 256' % maxv)
    types = {x[0]: (op, x[1:]) for op, x in ins if 19 <= op <= 33}
    consts = {x[1]: (x[0], x[2:]) for op, x in ins if op == 43}
    deco = defaultdict(list)
    for op, x in ins:
        if op == 71:
            deco[x[0]].append(x[1:])
    vars_ = {x[1]: (x[0], x[2], x[3:]) for op, x in ins if op == 59}
    io = {v: x for v, x in vars_.items() if x[1] in (1, 3)}
    # потоки: только 0 — любая Stream-декорация с номером != 0 или Emit/EndStream с ненулевым потоком — отказ
    for v, ds in deco.items():
        for d in ds:
            if d[0] == 29 and d[1] != 0:
                raise Unsupported('output stream %d (only stream 0)' % d[1])
    nxt = w[3]; newdecl = []; newvars = []; newdeco = []; newnames = []   # newdecl: типы и константы в порядке создания

    def fresh():
        nonlocal nxt
        r = nxt; nxt += 1; return r
    cache = {(op, tuple(x)): i for i, (op, x) in types.items() if op != 32 or x[0] not in (1, 3)}

    def typ(op, x):
        k = (op, tuple(x))
        if k not in cache:
            n = fresh(); cache[k] = n; types[n] = (op, x); newdecl.append((op, [n] + x))
        return cache[k]
    uint = typ(21, [32, 0]); void = typ(19, []); ft = typ(33, [void]); boolt = typ(20, [])
    constants = {(t, tuple(x)): i for i, (t, x) in consts.items()}

    def constant(v):
        k = (uint, (v,))
        if k not in constants:
            n = fresh(); constants[k] = n; newdecl.append((43, [uint, n, v])); consts[n] = (uint, [v])
        return constants[k]

    def name(v, s):
        b = s.encode() + b'\0'; b += b'\0' * ((-len(b)) % 4)
        newnames.append((5, [v] + list(struct.unpack('<%dI' % (len(b) // 4), b))))
    all_iotypes = set()

    def mark(t):
        all_iotypes.add(t); op, a = types[t]
        if op in (23, 24): mark(a[0])
        elif op == 28: mark(a[0])
        elif op == 30:
            for c in a: mark(c)
    abi = []
    outputs = []
    for v, (ptr, storage, init) in io.items():
        if init:
            raise Unsupported('initialized interface')
        pointee = types[ptr][1][1]; mark(pointee)
        builtin = next((x[1] for x in deco[v] if x[0] == 11), None)
        rec = dict(variable=v, pointee=pointee, storage=storage, decorations=deco[v], builtin=builtin,
                   location=next((x[1] for x in deco[v] if x[0] == 30), None),
                   component=next((x[1] for x in deco[v] if x[0] == 31), 0), record_words=0)
        abi.append(rec)
        if storage == 3:
            outputs.append(rec)
    maxv_c = constant(maxv); zero = constant(0); one = constant(1)
    private_ptr = lambda t: typ(32, [6, t])
    # записи выдач
    for rec in outputs:
        arr = typ(28, [rec['pointee'], maxv_c])
        rv = fresh(); newvars.append((59, [private_ptr(arr), rv, 6])); name(rv, 'mr_gs_rec_%d' % rec['variable'])
        rec['record'] = rv; rec['record_type'] = arr
        abi.append(dict(variable=rv, pointee=arr, storage=100, decorations=[], builtin=None, location=None, component=0,
                        record_words=0, record_of=rec['variable']))
    cnt = fresh(); newvars.append((59, [private_ptr(uint), cnt, 6, zero])); name(cnt, 'mr_gs_count')
    abi.append(dict(variable=cnt, pointee=uint, storage=101, decorations=[], builtin=None, location=None, component=0, record_words=0))
    cut_arr = typ(28, [uint, maxv_c]); cut_null = fresh(); newdecl.append((46, [cut_arr, cut_null]))   # OpConstantNull
    cut = fresh(); newvars.append((59, [private_ptr(cut_arr), cut, 6, cut_null])); name(cut, 'mr_gs_cut')
    abi.append(dict(variable=cut, pointee=cut_arr, storage=102, decorations=[], builtin=None, location=None, component=0, record_words=0))
    # функции emit / cut
    def func(fname, body_fn):
        f = fresh(); name(f, fname); lbl = fresh()
        body = [(54, [void, f, 0, ft]), (248, [lbl])]
        body += body_fn()
        body += [(253, []), (56, [])]
        return f, body

    def emit_body():
        b = []
        def value(op, t, args):
            n = fresh(); b.append((op, [t, n] + args)); return n
        c = value(61, uint, [cnt])
        ok = value(176, boolt, [c, maxv_c])      # OpULessThan
        do, merge = fresh(), fresh()
        b += [(247, [merge, 0]), (250, [ok, do, merge]), (248, [do])]
        for rec in outputs:
            v = value(61, rec['pointee'], [rec['variable']])
            p = value(65, private_ptr(rec['pointee']), [rec['record'], c])
            b.append((62, [p, v]))
        c1 = value(128, uint, [c, one])
        b += [(62, [cnt, c1]), (249, [merge]), (248, [merge])]
        return b

    def cut_body():
        b = []
        def value(op, t, args):
            n = fresh(); b.append((op, [t, n] + args)); return n
        c = value(61, uint, [cnt])
        nz = value(171, boolt, [c, zero])        # OpINotEqual
        do, merge = fresh(), fresh()
        b += [(247, [merge, 0]), (250, [nz, do, merge]), (248, [do])]
        i = value(130, uint, [c, one])            # OpISub
        p = value(65, private_ptr(uint), [cut, i])
        b += [(62, [p, one]), (249, [merge]), (248, [merge])]
        return b
    f_emit, emit_fn = func('mr_gs_emit', emit_body)
    f_cut, cut_fn = func('mr_gs_cut', cut_body)
    # обёртка-ядро
    wrapper = fresh(); label = fresh(); name(wrapper, 'mr_isoline_compute')
    wrapperbody = [(54, [void, wrapper, 0, ft]), (248, [label]), (57, [void, fresh(), main]), (253, []), (56, [])]
    b = b'mr_isoline_compute\0'; b += b'\0' * ((-len(b)) % 4); entryname = list(struct.unpack('<%dI' % (len(b) // 4), b))
    interfaces = [] if w[1] < 0x10400 else list(vars_) + [x[1] for op, x in newvars]
    entry = (15, [5, wrapper] + entryname + interfaces)
    out = [] if any(op == 17 and x == [1] for op, x in ins) else [(17, [1])]
    names_done = deco_done = globals_done = False
    stream_ops = {220: 218, 221: 219}
    for op, x in ins:
        if op == 17 and x[0] in (2, 24, 54):   # Geometry, GeometryPointSize, GeometryStreams
            continue
        if op == 16 and x[0] == main:
            continue
        if op == 15:
            out += [entry, (16, [wrapper, 17, 1, 1, 1])]; continue
        if not names_done and (op in (71, 72) or 19 <= op <= 39 or op in (41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 59, 54)):
            out += newnames; names_done = True
        if not deco_done and (19 <= op <= 39 or op in (41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 59, 54)):
            out += newdeco; deco_done = True
        if op == 54 and not globals_done:
            out += newdecl + newvars; globals_done = True
        if op == 71 and x[0] in io and x[1] in IO_DECOS + (29,):
            continue
        if op == 72 and x[0] in all_iotypes and x[2] in IO_DECOS + (29,):
            continue
        if op == 32 and x[1] in (1, 3):
            x = [x[0], 6, x[2]]
        if op == 59 and x[1] in io:
            x = [x[0], x[1], 6] + x[3:]
        if op in stream_ops:
            if x[0] not in consts or consts[x[0]][1] != [0]:
                raise Unsupported('emit/cut on a non-zero stream')
            op = stream_ops[op]; x = []
        if op == 218:
            out.append((57, [void, fresh(), f_emit])); continue
        if op == 219:
            out.append((57, [void, fresh(), f_cut])); continue
        out.append((op, x))
    out += emit_fn + cut_fn + wrapperbody
    ww = w[:5]; ww[3] = nxt; ww[1] = max(ww[1], 0x10300)
    for op, x in out:
        ww += [((len(x) + 1) << 16) | op] + x
    meta = dict(source_stage='GS', physical_stage='CS', input_primitive=in_prim, output_topology=out_topo,
                max_vertices=maxv, invocations=invocations, vertices_per_primitive=VERTICES_PER_PRIMITIVE[in_prim],
                count_variable=cnt, cut_variable=cut, entry=wrapper, records=abi)
    return struct.pack('<%dI' % len(ww), *ww), meta


if __name__ == '__main__':
    import sys, json
    data = open(sys.argv[1], 'rb').read()
    spv, meta = lower(data)
    open(sys.argv[2], 'wb').write(spv)
    print(json.dumps({k: v for k, v in meta.items() if k != 'records'}, ensure_ascii=False))
