"""tess-gs 14.10.2026: отражение пониженного тела для object/mesh-обёрток.

Параметры inner-функции у SPIRV-Cross — глобальные переменные, которых она касается (транзитивно), в порядке
возрастания их SPIR-V id. Поэтому соответствие «параметр ↔ запись/ресурс/встроенная» берётся из самого SPIR-V,
а типы и адресные пространства — из текста сигнатуры MSL. Имена не используются (SPIRV-Cross их переименовывает).
"""
import re
import struct

OP_NAME, OP_ENTRY, OP_DECORATE, OP_VARIABLE = 5, 15, 71, 59
OP_FUNCTION, OP_FUNCTION_END, OP_CALL = 54, 56, 57
OP_TYPE_POINTER, OP_TYPE_ARRAY = 32, 28
OP_STORE, OP_COPY_MEMORY, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN = 62, 63, 65, 66
DECO_BUILTIN, DECO_LOCATION, DECO_COMPONENT, DECO_PATCH = 11, 30, 31, 15
SC = {0: 'UniformConstant', 1: 'Input', 2: 'Uniform', 3: 'Output', 4: 'Workgroup', 5: 'CrossWorkgroup',
      6: 'Private', 7: 'Function', 9: 'PushConstant', 12: 'StorageBuffer'}
BUILTIN = {0: 'Position', 1: 'PointSize', 3: 'ClipDistance', 4: 'CullDistance', 6: 'PatchVertices', 7: 'PrimitiveId', 9: 'Layer', 10: 'ViewportIndex',
           8: 'InvocationId', 11: 'TessLevelOuter', 12: 'TessLevelInner', 13: 'TessCoord',
           42: 'VertexIndex', 43: 'InstanceIndex', 4424: 'BaseVertex', 4425: 'BaseInstance',
           27: 'LocalInvocationId', 29: 'LocalInvocationIndex', 26: 'WorkgroupId', 28: 'GlobalInvocationId'}


def instructions(data):
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    out, i = [], 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        out.append((op, list(words[i + 1:i + n])))
        i += n
    return out


def globals_by_function(data):
    """{имя функции (OpName): [id глобальных переменных, которых функция касается, транзитивно, по возрастанию]}."""
    ins = instructions(data)
    names = {a[0]: struct.pack('<%dI' % (len(a) - 1), *a[1:]).split(b'\0', 1)[0].decode() for op, a in ins if op == OP_NAME}
    globs = {a[1]: a[2] for op, a in ins if op == OP_VARIABLE and a[2] != 7}   # id -> storage class (не Function)
    # Таблица-константа (LUT): Private-массив с инициализатором, в который никто не пишет, — SPIRV-Cross выдаёт её
    # глобальной `constant`, а не параметром функции (fork_phase: 16 float). Такие id из списка параметров исключаем.
    pointee = {a[0]: a[2] for op, a in ins if op == OP_TYPE_POINTER}
    arrays = {a[0] for op, a in ins if op == OP_TYPE_ARRAY}
    chain_base = {a[1]: a[2] for op, a in ins if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN)}
    stored = set()
    for op, a in ins:
        if op in (OP_STORE, OP_COPY_MEMORY):
            x = a[0]
            while x in chain_base: x = chain_base[x]
            stored.add(x)
    lut = {a[1] for op, a in ins if op == OP_VARIABLE and a[2] == 6 and len(a) > 3
           and pointee.get(a[0]) in arrays and a[1] not in stored}
    for x in lut: globs.pop(x, None)
    funcs, cur, uses, calls = {}, None, {}, {}
    for op, a in ins:
        if op == OP_FUNCTION:
            cur = a[1]; uses[cur] = set(); calls[cur] = set()
        elif op == OP_FUNCTION_END:
            cur = None
        elif cur is not None:
            if op == OP_CALL:
                calls[cur].add(a[2])
            # все операнды, включая первый: OpStore %var и OpCopyMemory %var стоят в нулевой позиции
            for x in a:
                if x in globs:
                    uses[cur].add(x)
    def closure(f, seen=None):
        seen = seen or set()
        if f in seen: return set()
        seen.add(f)
        s = set(uses.get(f, ()))
        for c in calls.get(f, ()): s |= closure(c, seen)
        return s
    return {names.get(f, '%%%d' % f): sorted(closure(f)) for f in uses}, globs, names


def decorations(data):
    ins = instructions(data)
    d = {}
    for op, a in ins:
        if op == OP_DECORATE:
            d.setdefault(a[0], []).append(a[1:])
    return d


def msl_signature(text, function):
    """[(адресное пространство или '', тип, имя)] параметров static inline функции `function` в MSL."""
    m = re.search(r'static inline __attribute__\(\(always_inline\)\)\s*\n?\s*\w+ ' + re.escape(function) + r'\(([^)]*)\)', text)
    if not m:
        raise ValueError('no inner function %s in MSL' % function)
    params = []
    depth, cur = 0, ''
    for ch in m.group(1):
        if ch == '<': depth += 1
        if ch == '>': depth -= 1
        if ch == ',' and depth == 0:
            params.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip(): params.append(cur.strip())
    out = []
    for p in params:
        p = p.replace('__restrict', '').strip()
        space = ''
        for s in ('threadgroup', 'thread', 'constant', 'const device', 'device'):
            if p.startswith(s + ' '):
                space, p = s, p[len(s) + 1:].strip(); break
        mm = re.match(r'^(.*?)\s*&?\s*(\w+)$', p)
        typ, name = mm.group(1).strip(), mm.group(2)
        out.append((space, typ, name))
    return out


def describe(data, text, function='main0', abi=None):
    """abi: словарь lower() (ABI.json) — роли записей (Location/Component/BuiltIn/Patch, storage Input=1/Output=3)
    берутся из него: после понижения переменные стали Private и декораций интерфейса не несут."""
    """Соединить параметры MSL функции с глобальными SPIR-V (по порядку id). Возвращает список словарей."""
    per_func, globs, names = globals_by_function(data)
    # SPIRV-Cross переименовывает SPIR-V `main` в `main0` (чтобы не спорить с C-main)
    key = function if function in per_func else ('main' if function == 'main0' and 'main' in per_func else function)
    if key not in per_func:
        raise ValueError('нет функции %s; есть: %s' % (function, sorted(per_func)))
    ids = per_func[key]
    params = msl_signature(text, function)
    if len(ids) != len(params):
        raise ValueError('%s: %d глобальных в SPIR-V против %d параметров в MSL' % (function, len(ids), len(params)))
    deco = decorations(data)
    out = []
    for gid, (space, typ, name) in zip(ids, params):
        ds = deco.get(gid, [])
        item = dict(id=gid, storage=SC.get(globs[gid], str(globs[gid])), space=space, type=typ, name=name,
                    spirv_name=names.get(gid))
        for d in ds:
            if d[0] == DECO_BUILTIN: item['builtin'] = BUILTIN.get(d[1], str(d[1]))
            if d[0] == DECO_LOCATION: item['location'] = d[1]
            if d[0] == DECO_COMPONENT: item['component'] = d[1]
            if d[0] == DECO_PATCH: item['patch'] = True
            if d[0] == 34: item['set'] = d[1]
            if d[0] == 33: item['binding'] = d[1]
        if abi:
            rec = next((r for r in abi['records'] if r['variable'] == gid), None)
            if rec:
                item['role'] = 'input' if rec['storage'] == 1 else 'output' if rec['storage'] == 3 else str(rec['storage'])
                item['words'] = rec['record_words']
                for d in rec['decorations']:
                    if d[0] == DECO_BUILTIN: item['builtin'] = BUILTIN.get(d[1], str(d[1]))
                    if d[0] == DECO_LOCATION: item['location'] = d[1]
                    if d[0] == DECO_COMPONENT: item['component'] = d[1]
                    if d[0] == DECO_PATCH: item['patch'] = True
        out.append(item)
    return out


if __name__ == '__main__':
    import sys, json
    spv, msl = sys.argv[1:3]
    fn = sys.argv[3] if len(sys.argv) > 3 else 'main0'
    abi = json.load(open(sys.argv[4])) if len(sys.argv) > 4 else None
    for it in describe(open(spv, 'rb').read(), open(msl).read(), fn, abi):
        print(json.dumps(it, ensure_ascii=False))
