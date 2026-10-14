"""tess-gs 14.10.2026: object/mesh-функции собственного пути для тесселяции по ABI прежнего пути слоя.

Object = VS (выборка вершин кодом, как у переведённых VS) + HS (hull_main / patch_main пониженного тела) + раскладка
payload прежнего пути + обрезка факторов fmin(maxTF) с сохранением NaN + генерация workload'ов тесселятора.
Mesh = положения области (get_domain_location_<партиция>) + тело DS + сборка примитивов (generatePrimitive<…>).
Тесселятор — текст air_tessellation_own.metal (прежний путь, без asm-имён): совпадение положений и порядка
примитивов — по построению. Всё, что здесь выдумано, — только плоские копии между записями и payload.

Варианты индексов (нет / u16 / u32) — константа функции 130 (spvIndexKind), слой делает три PSO из одной библиотеки.
Входы вершин — константы функции 4*loc+{0,1,2,3}, как у переведённых VS (слой подставляет раскладку сам).
"""
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tess_body    # noqa: E402
import tess_dxil    # noqa: E402

TESSELLATOR = (HERE / 'air_tessellation_own.metal').read_text()
VERTEX_PULL = (HERE.parent.parent / 'admission/translator/abi_vertex_pull.metal').read_text()
PRELUDE = (HERE.parent.parent / 'admission/translator/abi_prelude.metal').read_text()
INDEX_KIND_CONSTANT = 130
WORKLOAD_WORDS = 14          # sizeof(TessMeshWorkload) / 4 = 56 / 4


class ComposeError(Exception):
    pass


# ---------------------------------------------------------------- типы

def parse_type(t):
    """'spvUnsafeArray<float4, 32>' -> ('float4', 32, True); 'float3' -> ('float3', 1, False)."""
    m = re.match(r'spvUnsafeArray<(.+), (\d+)>$', t.strip())
    if m:
        return m.group(1).strip(), int(m.group(2)), True
    return t.strip(), 1, False


def scalar_of(elem):
    m = re.match(r'^(float|uint|int|half|short|ushort|bool)(\d)?$', elem)
    if not m:
        raise ComposeError('unsupported element type ' + elem)
    return m.group(1), int(m.group(2) or 1)


SWZ = 'xyzw'


# ---------------------------------------------------------------- факторы прежнего пути

def integer_factor(factor, partition):
    import math
    if partition == 'integer':
        return int(math.ceil(min(max(factor, 1.0), 64.0)))
    if partition == 'pow2':
        f = int(min(max(factor, 1.0), 64.0))
        return 1 if f == 1 else 1 << (32 - (f - 1).bit_length() + (f - 1).bit_length())  # заглушка ниже
    if partition == 'odd':
        f = int(math.ceil(min(max(factor, 1.0), 63.0)))
        return f if f & 1 else f + 1
    if partition == 'even':
        f = int(math.ceil(min(max(factor, 2.0), 64.0)))
        return f + 1 if f & 1 else f
    raise ComposeError('partition ' + partition)


def pow2_factor(factor):
    f = int(min(max(factor, 1.0), 64.0))
    if f == 1:
        return 1
    return 1 << ((f - 1).bit_length())


def max_workloads(factor_int, domain, max_factor):
    import math
    if domain == 'isoline':
        return int(math.ceil(min(max(max_factor, 1.0), 64.0)))
    if domain == 'tri':
        return int(math.ceil((factor_int - 1) / 4.0)) * 3 + (factor_int & 1)
    return int(math.ceil((factor_int - 1) / 4.0)) * 4 + (factor_int & 1)


def mesh_limits(factor_int, output):
    mep = factor_int + 1
    if output == 'point':
        return 2 * mep, 2 * mep, 'point'
    if output == 'line':
        return mep, mep - 1, 'line'
    v = (mep + 2 - (mep & 1)) * 2 + 1
    return v, v, 'triangle'


def next_pow2(n):
    return 1 if n <= 1 else 1 << (n - 1).bit_length()


def validator(decl, factor_int, tpp, hs_regs, pc_scalars, pc_regs, vs_regs, ds_regs, max_potential_factor):
    """Копия ValidateTessellationReflection (d3d12_pipeline_graphics.cpp:906) — предсказание допуска слоя
    и выбор числа патчей в группе. Возвращает dict с payload/mesh_bytes/split/ok/why."""
    patches = 32 // tpp
    f = factor_int
    isoline = decl['domain'] == 'isoline'
    import math
    workloads = int(math.ceil(decl['max_factor'])) if isoline else ((f - 1 + 3) // 4) * (4 if decl['domain'] == 'quad' else 3) + (f & 1)
    slots = f + 1 if isoline else ((f + 1 + 2 - ((f + 1) & 1)) * 2) + 1
    control = 16 * hs_regs * decl['out_cp'] * patches
    patch = 4 * pc_scalars * patches
    payload = (control + patch + 4 + workloads * patches * 56 + 15) & ~15
    mesh_bytes = slots * (16 * ds_regs + 16)
    split = f > max_potential_factor or mesh_bytes > 32768
    # предел множителя в раздельном режиме — как считает прежний конвертер (dxbc_converter.cpp, max_split_factor)
    max_split_factor = 0
    for ff in range(1, 65):
        if ((2 * (ff + 1) + 7) & ~7) * ds_regs * 16 > 32768:
            break
        max_split_factor = ff
    why = []
    if split:
        if isoline or decl['output'] not in ('tri_cw', 'tri_ccw') or f > max_split_factor:
            why.append('split_unsupported')
        else:
            slots = 2 * (f + 1)
            mesh_bytes = ((slots + 7) & ~7) * 16 * ds_regs
    mesh_groups = workloads * patches * (2 if split else 1)
    shared = 16 * vs_regs * decl['in_cp'] * patches + control + 16 * pc_regs * patches + 16
    if payload > 16384: why.append('payload>16384')
    if mesh_bytes > 32768: why.append('mesh_bytes>32768')
    if slots > 256: why.append('slots>256')
    if mesh_groups > 1024: why.append('mesh_groups>1024')
    if shared > 32768: why.append('shared>32768')
    return dict(patches=patches, workloads=workloads, slots=slots, payload=payload, mesh_bytes=mesh_bytes, split=split,
                max_split_factor=max_split_factor, mesh_groups=mesh_groups, shared=shared, ok=not why, why=why)


# ---------------------------------------------------------------- связка по подписям

def sem_map(elements):
    """(register, component) -> (name, index) по подписи контейнера; системные значения тоже."""
    return {(e['register'], e['component']): (e['name'].upper(), e['index']) for e in elements}


def key_of(rec):
    return (rec.get('location'), rec.get('component', 0))


def link(producer, consumer, prod_sig, cons_sig, what):
    """Для каждого потребителя (записи с location) — запись производителя с той же семантикой.
    Сначала по (location, component) при совпадении семантик, иначе по семантике, иначе — отказ."""
    out = {}
    pmap = {key_of(r): r for r in producer if 'location' in r}
    psem = {}
    for r in producer:
        if 'location' in r:
            s = prod_sig.get(key_of(r))
            if s:
                psem[s] = r
    for c in consumer:
        if 'location' not in c:
            continue
        s = cons_sig.get(key_of(c))
        p = pmap.get(key_of(c))
        if p is not None and (s is None or prod_sig.get(key_of(p)) == s):
            out[c['id']] = p
        elif s in psem:
            out[c['id']] = psem[s]
        else:
            raise ComposeError('%s: no producer for consumer %s loc=%s sem=%s' % (what, c['name'], key_of(c), s))
    return out


def qualify(expr, ns, names):
    for n in names:
        expr = re.sub(r'(?<![\w:])' + re.escape(n) + r'\b', ns + '::' + n, expr)
    return expr


def struct_names(body_text):
    return re.findall(r'^struct (\w+)\n\{', body_text, re.M)


def is_resource(p):
    return 'role' not in p and p['storage'] not in ('Private', 'Workgroup', 'Function')


def assign_position_registers(params, sig_elems, role, what):
    """SV_Position в SPIR-V — встроенная без Location; в payload и для связки ей даётся регистр подписи контейнера
    (как в регистровой модели прежнего пути). Запись помечается pos_builtin и дальше считается расположенной."""
    pos = next((e for e in sig_elems if e['name'].upper() == 'SV_POSITION'), None)
    for p in params:
        if p.get('builtin') == 'Position' and p.get('role') == role and 'location' not in p:
            if pos is None:
                raise ComposeError('%s: SV_Position builtin without a signature register' % what)
            p['location'], p['component'], p['pos_builtin'] = pos['register'], 0, True


def located(p):
    return 'location' in p and ('builtin' not in p or p.get('pos_builtin'))


def buffer_extra(stage, fn):
    users = stage['buffer_users']
    out = []
    for n in (0, 1):
        lst = users.get(n, users.get(str(n), []))
        if fn in lst:
            out.append('spvDescriptorSet%d' % n)
    return out


def uses_set1(*stages):
    return any(stage['buffer_users'].get(1, stage['buffer_users'].get('1', [])) for stage in stages)


def set1_struct(*stages):
    for stage in stages:
        s = stage['descriptor_set_structs'].get(1)
        if s:
            return s
    return 'struct spvDescriptorSetBuffer1\n{\n    ulong r0;\n};\n'


# ---------------------------------------------------------------- object

def compose_object(vs, hs, decl, vs_sig, hs_sig, ds_regs_hint=0):
    icp, ocp = decl['in_cp'], decl['out_cp']
    tpp = next_pow2(max(icp, ocp))
    partition = decl['partition']
    factor_int = pow2_factor(decl['max_factor']) if partition == 'pow2' else integer_factor(decl['max_factor'], partition)
    maxw = max_workloads(factor_int, decl['domain'], decl['max_factor'])
    vs_main, hs_hull, hs_patch = vs['params']['main0'], hs['params'].get('hull_main'), hs['params'].get('patch_main')
    ds_regs_guess = ds_regs_hint if ds_regs_hint else 1
    # Патчей в группе — столько, чтобы payload уложился в 16 КБ Metal (формула валидатора слоя):
    # больше лан на патч (лишние простаивают), меньше патчей и workload'ов в одной группе.
    hs_regs_guess = max([p.get('location', -1) for p in hs['params']['main0'] if p.get('role') == 'output' and not p.get('patch') and 'builtin' not in p] + [0]) + 1
    while True:
        v = validator(decl, factor_int, tpp, hs_regs_guess, 6, 2, 8, ds_regs_guess, factor_int)
        if v['payload'] <= 16384 or tpp >= 32:
            break
        tpp *= 2
    ppg = 32 // tpp
    hs_main = hs['params']['main0']
    split = bool(hs_hull and hs_patch)
    if not split and not (ocp == 1 or ocp == tpp):
        raise ComposeError('HS body is not split into hull_main/patch_main and ocp %d != threads per patch %d' % (ocp, tpp))
    hs_all = {p['id']: p for p in hs_main}
    assign_position_registers(vs_main, vs_sig['OSG1'], 'output', 'VS')
    assign_position_registers(hs_main, hs_sig['ISG1'], 'input', 'HS in')
    assign_position_registers(hs_main, hs_sig['OSG1'], 'output', 'HS out')
    for lst in (hs_hull or [], hs_patch or []):
        for p in lst:
            q = hs_all.get(p['id'])
            if q is not None and q.get('pos_builtin'):
                p.update(q)
    vs_out = [p for p in vs_main if p.get('role') == 'output']
    vs_in = [p for p in vs_main if p.get('role') == 'input' and 'location' in p]
    hs_params = (hs_hull + hs_patch) if split else hs_main
    hs_in = [p for p in hs_params if p.get('role') == 'input' and located(p)]
    hs_in_bi = [p for p in hs_params if p.get('role') == 'input' and 'builtin' in p and not p.get('pos_builtin')]
    hs_cp_out = [p for p in hs_all.values() if p.get('role') == 'output' and not p.get('patch') and located(p)]
    hs_pc_out = [p for p in hs_all.values() if p.get('role') == 'output' and p.get('patch') and 'builtin' not in p]
    hs_tlo = next((p for p in hs_all.values() if p.get('builtin') == 'TessLevelOuter'), None)
    hs_tli = next((p for p in hs_all.values() if p.get('builtin') == 'TessLevelInner'), None)
    if hs_tlo is None:
        raise ComposeError('HS without TessLevelOuter')
    # регистры и скаляры payload
    hs_regs = max([p['location'] for p in hs_cp_out] + [-1]) + 1
    pc_layout = {}            # id записи HS -> первый слот pc
    pc_scalars = 6
    for p in sorted(hs_pc_out, key=key_of):
        elem, n, arr = parse_type(p['type'])
        s, w = scalar_of(elem)
        pc_layout[p['id']] = pc_scalars
        pc_scalars += w * n
    vs_regs = max([p.get('location', -1) for p in vs_out] + [-1]) + 1
    # связки
    vs_sem = sem_map(vs_sig['OSG1']); hs_isem = sem_map(hs_sig['ISG1'])
    hs_in_src = link(vs_out, hs_in, vs_sem, hs_isem, 'VS->HS')
    for p in hs_in_bi:
        if p['builtin'] not in ('InvocationId', 'PrimitiveId', 'PatchVertices'):
            raise ComposeError('HS reads builtin %s: not served' % p['builtin'])
    # ---- текст
    L = []
    emit = L.append
    emit('// tess-gs: object-функция (VS + HS) собственного пути; раскладка payload прежнего пути слоя.')
    emit('#pragma clang diagnostic ignored "-Wmissing-prototypes"')
    emit('#pragma clang diagnostic ignored "-Wmissing-braces"')
    emit('#pragma clang diagnostic ignored "-Wunused-variable"')
    emit('#include <metal_stdlib>')
    emit('#include <simd/simd.h>')
    emit('using namespace metal;')
    emit(PRELUDE)
    emit(vs['unsafe_array'] or hs['unsafe_array'])
    emit(VERTEX_PULL)
    emit(TESSELLATOR)
    qwords = max(vs['root_qwords'], hs['root_qwords'])
    emit('struct spvDescriptorSetBuffer0\n{\n' + ''.join('    ulong r%d;\n' % i for i in range(max(qwords, 1))) + '};')
    set1 = uses_set1(vs, hs)
    if set1:
        emit(set1_struct(vs, hs))
    emit('namespace vs_body {\n' + vs['body'] + '\n}')
    emit('namespace hs_body {\n' + hs['body'] + '\n}')
    emit('struct Payload {')
    emit('    uint4 cp[%d][%d][%d];' % (ppg, ocp, max(hs_regs, 1)))
    emit('    uint pc[%d][%d];' % (ppg, pc_scalars))
    emit('    int batched_start;')
    emit('    int workloads[%d];' % (maxw * ppg * WORKLOAD_WORDS))
    emit('};')
    payload_bytes = 16 * ppg * ocp * max(hs_regs, 1) + 4 * ppg * pc_scalars + 4 + maxw * ppg * 56
    payload_bytes = (payload_bytes + 15) & ~15
    # константы функции
    for p in sorted(vs_in, key=key_of):
        loc = p['location']
        for k, nm in enumerate(('entry', 'offset', 'format', 'step')):
            emit('constant uint spvVin%d_%s [[function_constant(%d)]];' % (loc, nm, 4 * loc + k))
    emit('constant uint spvIndexKind [[function_constant(%d)]];' % INDEX_KIND_CONSTANT)
    uses0 = bool(vs['resources'] or hs['resources'])
    emit('[[object]] void main0_object(object_data Payload& payload [[payload]], mesh_grid_properties mgp,')
    emit('    uint2 tid [[thread_position_in_threadgroup]], uint2 tgid [[threadgroup_position_in_grid]],')
    emit('    constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]],')
    if set1:
        emit('    constant spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]],')
    emit('    constant spvVertexBufferEntry* spvVertexBuffers [[buffer(2)]],')
    emit('    device const uint* spvIndexBuffer [[buffer(3)]], constant uint* spvDrawArguments [[buffer(4)]])')
    emit('{')
    emit('    threadgroup int workload_count;')
    for p in vs_out:
        elem, n, arr = parse_type(p['type'])
        emit('    threadgroup %s vs_out_%d[%d][%d];' % (elem, p['id'], ppg, icp))
    for p in list(hs_cp_out) + list(hs_pc_out) + [hs_tlo] + ([hs_tli] if hs_tli else []):
        if p['space'] == 'threadgroup':
            emit('    threadgroup %s hs_out_%d[%d];' % (p['type'], p['id'], ppg))
    emit('    const uint cp_id = tid.x, patch_slot = tid.y;')
    emit('    const uint batched_patch_start = tgid.x * %du;' % ppg)
    emit('    const uint patch_id = batched_patch_start + patch_slot;')
    emit('    const uint instance_id = tgid.y;')
    emit('    const uint control_point_index = patch_id * %du + cp_id;' % icp)
    emit('    const uint patch_count = spvDrawArguments[0] / %du;' % icp)
    emit('    if (cp_id == 0u && patch_slot == 0u) workload_count = 0;')
    emit('    const bool active_patch = patch_id < patch_count;')
    # ---- VS
    emit('    if (cp_id < %du && active_patch) {' % icp)
    emit('        uint vid;')
    emit('        if (spvIndexKind == 1u) vid = uint(((device const ushort*)spvIndexBuffer)[spvDrawArguments[2] + control_point_index]);')
    emit('        else if (spvIndexKind == 2u) vid = spvIndexBuffer[spvDrawArguments[2] + control_point_index];')
    emit('        else vid = control_point_index;')
    emit('        const uint base_vertex = spvIndexKind ? spvDrawArguments[3] : spvDrawArguments[2];')
    emit('        const uint base_instance = spvIndexKind ? spvDrawArguments[4] : spvDrawArguments[3];')
    emit('        const uint vertex_index = vid + base_vertex;')
    emit('        const uint instance_index = instance_id + base_instance;')
    vs_names = struct_names(vs['body'])
    args = []
    for p, arg in zip(vs_main, vs['call_args']):
        if is_resource(p):
            args.append(qualify(arg, 'vs_body', vs_names)); continue
        if arg.startswith('spvDescriptorSet'):
            args.append(arg); continue
        nm = 'vs_%d' % p['id']
        elem, n, arr = parse_type(p['type'])
        if p.get('role') == 'input' and 'location' in p:
            s, w = scalar_of(elem)
            loc = p['location']
            pull = 'spvPull(spvVertexBuffers, spvVin%d_entry, spvVin%d_offset, spvVin%d_format, spvVin%d_step, vertex_index, instance_index, base_instance)' % (loc, loc, loc, loc)
            swz = '' if w == 4 else '.' + SWZ[:w]
            emit('        %s %s = as_type<%s>(%s%s);' % (elem, nm, elem, pull, swz))
        elif p.get('builtin') == 'VertexIndex':
            emit('        uint %s = vertex_index;' % nm)
        elif p.get('builtin') == 'BaseVertex':
            emit('        uint %s = base_vertex;' % nm)
        elif p.get('builtin') == 'InstanceIndex':
            emit('        uint %s = instance_index;' % nm)
        elif p.get('builtin') == 'BaseInstance':
            emit('        uint %s = base_instance;' % nm)
        elif p.get('role') == 'output':
            emit('        %s %s = {};' % (p['type'], nm))
        else:
            raise ComposeError('VS param not handled: %s' % p)
        args.append(nm)
    if not vs.get('empty'):
        emit('        vs_body::main0(%s);' % ', '.join(buffer_extra(vs, 'main0') + [a for a in args if not a.startswith('spvDescriptorSet')]))
    for p in vs_out:
        emit('        vs_out_%d[patch_slot][cp_id] = vs_%d;' % (p['id'], p['id']))
    emit('    }')
    emit('    threadgroup_barrier(mem_flags::mem_threadgroup);')
    # ---- HS: локали
    hs_names = struct_names(hs['body'])
    declared = {}   # id -> выражение аргумента (hull_main и patch_main делят одни переменные: запоминать ВЫРАЖЕНИЕ)

    def hs_local(p):
        nm = 'hs_%d' % p['id']
        if p['id'] in declared:
            return declared[p['id']]
        declared[p['id']] = hs_local_emit(p, nm)
        return declared[p['id']]

    def hs_local_emit(p, nm):
        elem, n, arr = parse_type(p['type'])
        if p.get('role') == 'input' and p['id'] in hs_in_src:
            src = hs_in_src[p['id']]
            emit('    %s %s = {};' % (p['type'], nm))
            if arr:
                emit('    for (uint c = 0; c < %du; ++c) %s[c] = vs_out_%d[patch_slot][c];' % (icp, nm, src['id']))
            else:
                emit('    %s = vs_out_%d[patch_slot][0];' % (nm, src['id']))
        elif p.get('builtin') == 'InvocationId' or ('role' not in p and p['type'] == 'uint' and p['space'] == 'thread'):
            emit('    uint %s = cp_id;' % nm)
        elif p.get('builtin') == 'PrimitiveId':
            emit('    uint %s = patch_id;' % nm)
        elif p.get('builtin') == 'PatchVertices':
            emit('    uint %s = %du;' % (nm, icp))
        elif p.get('role') == 'output':
            if p['space'] == 'threadgroup':
                return 'hs_out_%d[patch_slot]' % p['id']
            emit('    %s %s = {};' % (p['type'], nm))
        else:
            raise ComposeError('HS param not handled: %s' % p)
        return nm

    kernel_args = [a for a in hs['call_args'] if not a.startswith('spvDescriptorSet')]   # позиционно = main0

    def hs_call(fn, params):
        a = []
        for p in params:
            if is_resource(p):
                # материализованное выражение берём из вызова ядра (позиционно, по id)
                idx = next(i for i, q in enumerate(hs_main) if q['id'] == p['id'])
                a.append(qualify(kernel_args[idx], 'hs_body', hs_names))
            else:
                a.append(hs_local(p))
        return '%s::%s(%s)' % ('hs_body', fn, ', '.join(buffer_extra(hs, fn) + a))

    if split:
        emit('    if (active_patch && cp_id < %du) %s;' % (ocp, hs_call('hull_main', hs_hull)))
        emit('    threadgroup_barrier(mem_flags::mem_threadgroup);')
        emit('    if (active_patch && cp_id == 0u) %s;' % hs_call('patch_main', hs_patch))
    else:
        # тело без разделения фаз: оркестратор сам ведёт фазы (барьер внутри, если он есть); все %d лан патча
        emit('    if (active_patch && cp_id < %du) %s;' % (ocp, hs_call('main0', hs_main)))
    emit('    threadgroup_barrier(mem_flags::mem_threadgroup);')
    # ---- CP outputs -> payload
    emit('    if (active_patch && cp_id < %du) {' % ocp)
    for p in hs_cp_out:
        elem, n, arr = parse_type(p['type'])
        s, w = scalar_of(elem)
        src = ('hs_out_%d[patch_slot][cp_id]' % p['id']) if p['space'] == 'threadgroup' else ('hs_%d[%s]' % (p['id'], 'cp_id' if arr else '0'))
        for i in range(w):
            comp = p.get('component', 0) + i
            val = src if w == 1 else '%s.%s' % (src, SWZ[i])
            emit('        payload.cp[patch_slot][cp_id][%d].%s = as_type<uint>(%s);' % (p['location'], SWZ[comp], val))
    emit('    }')
    # ---- patch constants, clamp, workload (lane 0 of each patch)
    emit('    if (active_patch && cp_id == 0u) {')
    def pc_src(p):
        return ('hs_out_%d[patch_slot]' % p['id']) if p['space'] == 'threadgroup' else ('hs_%d' % p['id'])
    emit('        const float max_factor = %sf;' % repr(float(decl['max_factor'])))
    emit('        float tf[6];')
    for i in range(4):
        emit('        { float v = %s[%d]; float c = fmin(v, max_factor); tf[%d] = isnan(v) ? v : c; }' % (pc_src(hs_tlo), i, i))
    for i in range(2):
        if hs_tli:
            emit('        { float v = %s[%d]; float c = fmin(v, max_factor); tf[%d] = isnan(v) ? v : c; }' % (pc_src(hs_tli), i, 4 + i))
        else:
            emit('        tf[%d] = 0.0f;' % (4 + i))
    emit('        for (uint i = 0; i < 6u; ++i) payload.pc[patch_slot][i] = as_type<uint>(tf[i]);')
    for p in hs_pc_out:
        elem, n, arr = parse_type(p['type'])
        s, w = scalar_of(elem)
        base = pc_layout[p['id']]
        for j in range(n):
            for i in range(w):
                src = pc_src(p) + ('[%d]' % j if arr else '')
                val = src if w == 1 else '%s.%s' % (src, SWZ[i])
                emit('        payload.pc[patch_slot][%d] = as_type<uint>(%s);' % (base + j * w + i, val))
    part = {'integer': 'integer', 'pow2': 'pow2', 'odd': 'odd', 'even': 'even'}[partition]
    if decl['domain'] == 'quad':
        emit('        gen_workload_quad_%s(int(patch_slot), &workload_count, payload.workloads, tf[4], tf[5], tf[0], tf[1], tf[2], tf[3]);' % part)
    elif decl['domain'] == 'tri':
        emit('        gen_workload_triangle_%s(int(patch_slot), &workload_count, payload.workloads, tf[3], tf[0], tf[1], tf[2]);' % part)
    else:
        emit('        gen_workload_isoline_%s(int(patch_slot), &workload_count, payload.workloads, tf[0], tf[1]);' % part)
    emit('    }')
    emit('    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_object_data);')
    emit('    if (cp_id == 0u && patch_slot == 0u) {')
    emit('        mgp.set_threadgroups_per_grid(uint3(uint(workload_count) * %du, 1u, 1u));' % (2 if v['split'] else 1))   # раздельный режим: две mesh-группы на workload
    emit('        payload.batched_start = int(batched_patch_start);')
    emit('    }')
    emit('}')
    info = dict(icp=icp, ocp=ocp, threads_per_patch=tpp, patch_per_group=ppg, factor_int=factor_int, max_workloads=maxw,
                split=bool(v['split']), max_split_factor=v['max_split_factor'],
                hs_regs=max(hs_regs, 1), pc_scalars=pc_scalars, pc_layout={str(k): v for k, v in pc_layout.items()},
                vs_regs=max(vs_regs, 1), payload_bytes=payload_bytes, vertex_inputs=sorted(p['location'] for p in vs_in),
                hs_cp_out=[dict(id=p['id'], loc=p['location'], comp=p.get('component', 0), type=p['type']) for p in hs_cp_out],
                hs_pc_out=[dict(id=p['id'], loc=p['location'], comp=p.get('component', 0), type=p['type'], slot=pc_layout[p['id']]) for p in hs_pc_out])
    return '\n'.join(L) + '\n', info


# ---------------------------------------------------------------- mesh

def compose_mesh(hs, ds, decl, obj, hs_sig, ds_sig):
    icp, ocp = decl['in_cp'], decl['out_cp']
    partition, domain, output = decl['partition'], decl['domain'], decl['output']
    factor_int = obj['factor_int']
    mv, mp, topo = mesh_limits(factor_int, output)
    split = bool(obj.get('split'))
    if split:
        if topo != 'triangle':
            raise ComposeError('split mode requires triangle output')
        mv, mp = 2 * (factor_int + 1), 2 * factor_int   # как у прежнего конвертера: 2*max_edge_point, 2*factor_int
    ds_main = ds['params']['main0']
    assign_position_registers(ds_main, ds_sig['ISG1'], 'input', 'DS in')
    ds_in = [p for p in ds_main if p.get('role') == 'input' and not p.get('patch') and located(p)]
    ds_pin = [p for p in ds_main if p.get('role') == 'input' and p.get('patch') and 'builtin' not in p]
    ds_out = [p for p in ds_main if p.get('role') == 'output' and 'builtin' not in p]
    ds_pos = next((p for p in ds_main if p.get('role') == 'output' and p.get('builtin') == 'Position'), None)
    ds_clip = next((p for p in ds_main if p.get('role') == 'output' and p.get('builtin') == 'ClipDistance'), None)
    ds_cull = next((p for p in ds_main if p.get('role') == 'output' and p.get('builtin') == 'CullDistance'), None)
    if ds_pos is None:
        raise ComposeError('DS without SV_Position output')
    # SV_CullDistance: Metal не отсекает по нему; прежний путь выдаёт его обычным varying (`SV_CullDistance<reg>`)
    # и не отсекает тоже (dxbc_signature.cpp: cull_distance_scalars + DefineOutput user). Делаем так же.
    # связки HS cp out -> DS cp in; HS pc out -> DS pc in
    hs_all = {p['id']: p for p in hs['params']['main0']}
    hs_cp_out = [hs_all[int(x['id'])] for x in obj['hs_cp_out']]
    hs_pc_out = [hs_all[int(x['id'])] for x in obj['hs_pc_out']]
    cp_src = link(hs_cp_out, ds_in, sem_map(hs_sig['OSG1']), sem_map(ds_sig['ISG1']), 'HS->DS')
    pc_src = link(hs_pc_out, ds_pin, sem_map(hs_sig['PSG1']), sem_map(ds_sig['PSG1']), 'HS->DS patch') if ds_pin else {}
    ds_regs = max([p['location'] for p in ds_out] + [-1]) + 2    # + регистр SV_Position, как у прежнего пути
    L = []
    emit = L.append
    emit('// tess-gs: mesh-функция (DS) собственного пути; тесселятор прежнего пути слоя.')
    emit('#pragma clang diagnostic ignored "-Wmissing-prototypes"')
    emit('#pragma clang diagnostic ignored "-Wmissing-braces"')
    emit('#pragma clang diagnostic ignored "-Wunused-variable"')
    emit('#include <metal_stdlib>')
    emit('#include <simd/simd.h>')
    emit('using namespace metal;')
    emit(PRELUDE)
    emit(ds['unsafe_array'] or hs['unsafe_array'])
    emit(TESSELLATOR)
    qwords = max(hs['root_qwords'], ds['root_qwords'])
    emit('struct spvDescriptorSetBuffer0\n{\n' + ''.join('    ulong r%d;\n' % i for i in range(max(qwords, 1))) + '};')
    set1 = uses_set1(ds)
    if set1:
        emit(set1_struct(ds))
    emit('namespace ds_body {\n' + ds['body'] + '\n}')
    emit('struct Payload {')
    emit('    uint4 cp[%d][%d][%d];' % (obj['patch_per_group'], ocp, obj['hs_regs']))
    emit('    uint pc[%d][%d];' % (obj['patch_per_group'], obj['pc_scalars']))
    emit('    int batched_start;')
    emit('    int workloads[%d];' % (obj['max_workloads'] * obj['patch_per_group'] * WORKLOAD_WORDS))
    emit('};')
    emit('struct MeshVertex {')
    emit('    float4 position [[position]];')
    members = {}
    for p in sorted(ds_out, key=key_of):
        elem, n, arr = parse_type(p['type'])
        if arr:
            raise ComposeError('arrayed DS output not served: %s' % p['name'])
        comp = p.get('component', 0)
        user = 'locn%d' % p['location'] + ('_%d' % comp if comp else '')
        members[p['id']] = 'm_%d' % p['id']
        emit('    %s m_%d [[user(%s)]];' % (elem, p['id'], user))
    clip_n = 0
    if ds_clip is not None:
        elem, clip_n, arr = parse_type(ds_clip['type'])
        emit('    float clip [[clip_distance]] [%d];' % clip_n)   # MSL: атрибут перед размерностью массива
    cull_n = 0
    if ds_cull is not None:
        elem, cull_n, arr = parse_type(ds_cull['type'])
        for i in range(cull_n):
            emit('    float cull_%d [[user(SV_CullDistance_%d)]];' % (i, i))
    if output == 'point':
        emit('    float point_size [[point_size]];')
    emit('};')
    verts_per_prim = {'triangle': 3, 'line': 2, 'point': 1}[topo]
    if cull_n:
        # честный SV_CullDistance (семантика D3D) под константой функции 131: примитив отбрасывается, если по какому-то
        # индексу отсечения ВСЕ его вершины отрицательны. Индексы, которые пишет тесселятор, записываются прокси-типом.
        emit('struct MeshPrimitive { bool culled [[primitive_culled]]; };')
        emit('constant bool spvHonestCull [[function_constant(131)]];')
        emit('using MeshT = metal::mesh<MeshVertex, MeshPrimitive, %d, %d, topology::%s>;' % (mv, mp, topo))
        emit('struct CullMesh {')
        emit('    MeshT mesh; threadgroup uint* indices; threadgroup uint* total;')
        emit('    void set_index(uint i, uint v) { mesh.set_index(i, v); indices[i] = v; }')
        emit('    void set_vertex(uint i, MeshVertex v) { mesh.set_vertex(i, v); }')
        emit('    void set_primitive_count(uint n) { mesh.set_primitive_count(n); *total = n; }')
        emit('};')
    else:
        emit('using MeshT = metal::mesh<MeshVertex, void, %d, %d, topology::%s>;' % (mv, mp, topo))
    emit('[[mesh]] void main0_mesh(object_data const Payload& payload [[payload]], MeshT mesh,')
    emit('    uint tid [[thread_index_in_threadgroup]], uint gid [[threadgroup_position_in_grid]],')
    emit('    constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]' + (',' if set1 else ')'))
    if set1:
        emit('    constant spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]])')
    emit('{')
    emit('    object_data int* data = (object_data int*)payload.workloads;')
    if cull_n:
        emit('    threadgroup uint cull_indices[%d];' % (verts_per_prim * mp))
        emit('    threadgroup uint cull_total;')
        emit('    threadgroup float cull_vals[%d][%d];' % (mv, cull_n))
        emit('    if (tid == 0u) cull_total = 0u;')
    emit('    const int workload_index = int(gid);')
    emit('    const int patch_index = get_domain_patch_index(workload_index%s, data);' % (' >> 1' if split else ''))
    emit('    const uint patch_id = uint(payload.batched_start + patch_index);')
    # входы DS, не зависящие от вершины: контрольные точки и патч-константы
    ds_names = struct_names(ds['body'])
    pre, per = [], []
    args = []
    for p, arg in zip(ds_main, ds['call_args'][(1 if ds['call_args'] and ds['call_args'][0].startswith('spvDescriptorSet') else 0):]):
        if is_resource(p):
            args.append(qualify(arg, 'ds_body', ds_names)); continue
        nm = 'ds_%d' % p['id']
        elem, n, arr = parse_type(p['type'])
        s, w = scalar_of(elem)
        if p['id'] in cp_src:
            src = cp_src[p['id']]
            if arr and n != ocp:
                raise ComposeError('DS input patch size %d != HS output control points %d' % (n, ocp))
            pre.append('    %s %s = {};' % (p['type'], nm))
            cnt = n if arr else 1
            for c in range(cnt):
                parts = []
                for i in range(w):
                    comp = src.get('component', 0) + i
                    parts.append('as_type<%s>(payload.cp[patch_index][%d][%d].%s)' % (s, c, src['location'], SWZ[comp]))
                val = parts[0] if w == 1 else '%s(%s)' % (elem, ', '.join(parts))
                pre.append('    %s%s = %s;' % (nm, '[%d]' % c if arr else '', val))
        elif p.get('builtin') == 'TessLevelOuter':
            pre.append('    %s %s = {};' % (p['type'], nm))
            for i in range(min(n, 4)):
                pre.append('    %s[%d] = as_type<float>(payload.pc[patch_index][%d]);' % (nm, i, i))
        elif p.get('builtin') == 'TessLevelInner':
            pre.append('    %s %s = {};' % (p['type'], nm))
            for i in range(min(n, 2)):
                pre.append('    %s[%d] = as_type<float>(payload.pc[patch_index][%d]);' % (nm, i, 4 + i))
        elif p['id'] in pc_src:
            src = pc_src[p['id']]
            slot = obj['pc_layout'][str(src['id'])]
            pre.append('    %s %s = {};' % (p['type'], nm))
            cnt = n if arr else 1
            for c in range(cnt):
                parts = ['as_type<%s>(payload.pc[patch_index][%d])' % (s, slot + c * w + i) for i in range(w)]
                val = parts[0] if w == 1 else '%s(%s)' % (elem, ', '.join(parts))
                pre.append('    %s%s = %s;' % (nm, '[%d]' % c if arr else '', val))
        elif p.get('builtin') == 'PrimitiveId':
            pre.append('    uint %s = patch_id;' % nm)
        elif p.get('builtin') == 'PatchVertices':
            pre.append('    uint %s = %du;' % (nm, ocp))
        elif p.get('builtin') == 'TessCoord':
            per.append('        %s %s = %s;' % (elem, nm, 'tess_coord' if w == 3 else 'tess_coord.xy'))
        elif p.get('role') == 'output':
            per.append('        %s %s = {};' % (p['type'], nm))
        else:
            raise ComposeError('DS param not handled: %s' % p)
        args.append(nm)
    L.extend(pre)
    emit('    for (uint base = 0u; ; base += 32u) {')
    emit('        const int thread_index = int(base + tid);')
    emit('        domain_location loc = get_domain_location_%s%s(workload_index, thread_index, data);' % ('split_' if split else '', partition))
    emit('        if (loc.active) {')
    emit('            const float3 tess_coord = float3(loc.uv.x, loc.uv.y, 1.0f - (loc.uv.x + loc.uv.y));')
    for line in per:
        emit('    ' + line)
    emit('            ds_body::main0(%s);' % ', '.join(buffer_extra(ds, 'main0') + args))
    emit('            MeshVertex v;')
    emit('            v.position = ds_%d;' % ds_pos['id'])
    for p in ds_out:
        emit('            v.%s = ds_%d;' % (members[p['id']], p['id']))
    if ds_clip is not None:
        for i in range(clip_n):
            emit('            v.clip[%d] = ds_%d[%d];' % (i, ds_clip['id'], i))
    if ds_cull is not None:
        for i in range(cull_n):
            emit('            v.cull_%d = ds_%d[%d];' % (i, ds_cull['id'], i))
    if output == 'point':
        emit('            v.point_size = 1.0f;')
    emit('            mesh.set_vertex(uint(thread_index), v);')
    for i in range(cull_n):
        emit('            cull_vals[thread_index][%d] = ds_%d[%d];' % (i, ds_cull['id'], i))
    emit('        }')
    emit('        if (!loc.iterate) break;')
    emit('    }')
    gen = {'tri_cw': 'generatePrimitiveTriangle', 'tri_ccw': 'generatePrimitiveTriangleCCW', 'line': 'generatePrimitiveLine',
           'point': 'generatePrimitivePoint'}[output]
    if split:
        gen = {'tri_cw': 'generatePrimitiveTriangleSplit', 'tri_ccw': 'generatePrimitiveTriangleSplitCCW'}[output]
    if cull_n:
        emit('    CullMesh proxy{mesh, cull_indices, &cull_total};')
        emit('    if (tid == 0u) %s(workload_index, data, proxy);' % gen)
        emit('    threadgroup_barrier(mem_flags::mem_threadgroup);')
        emit('    if (spvHonestCull) {')
        emit('        for (uint p = tid; p < cull_total; p += 32u) {')
        emit('            bool culled = false;')
        for i in range(cull_n):
            emit('            culled = culled || (%s);' % ' && '.join('cull_vals[cull_indices[%du * p + %du]][%d] < 0.0f' % (verts_per_prim, k, i) for k in range(verts_per_prim)))
        emit('            MeshPrimitive mp; mp.culled = culled;')
        emit('            mesh.set_primitive(p, mp);')
        emit('        }')
        emit('    }')
    else:
        emit('    if (tid == 0u) %s(workload_index, data, mesh);' % gen)
    emit('}')
    info = dict(ds_regs=ds_regs, mesh_vertices=mv, mesh_primitives=mp, topology=topo, clip=clip_n, cull=cull_n,
                ds_out=[dict(id=p['id'], loc=p['location'], comp=p.get('component', 0), type=p['type']) for p in ds_out])
    return '\n'.join(L) + '\n', info


# ---------------------------------------------------------------- описания для слоя

def description(kind, entry, root_key, qwords, resources, decl, obj, mesh, vin_lines):
    lines = ['format=4', 'kind=%s' % kind, 'entry=%s' % entry, 'root=%s' % (root_key or 'none'),
             'root_qwords=%d' % qwords, 'resources=%d' % resources, 'variant=none',
             'tess_domain=%s' % decl['domain'], 'tess_partition=%s' % decl['partition'], 'tess_output=%s' % decl['output'],
             'tess_max_factor=%r' % float(decl['max_factor']), 'tess_in_cp=%d' % decl['in_cp'], 'tess_out_cp=%d' % decl['out_cp'],
             'tess_threads_per_patch=%d' % obj['threads_per_patch'], 'tess_hs_out_regs=%d' % obj['hs_regs'],
             'tess_patch_scalars=%d' % obj['pc_scalars'], 'tess_patch_regs=%d' % max(1, (obj['pc_scalars'] + 3) // 4),
             'tess_vs_out_regs=%d' % obj['vs_regs'], 'tess_payload_bytes=%d' % obj['payload_bytes'],
             'tess_ds_out_regs=%d' % mesh['ds_regs'], 'tess_max_potential_factor=%d' % obj['factor_int'],
             'tess_split=%d' % int(bool(obj.get('split'))), 'tess_max_split_factor=%d' % obj.get('max_split_factor', 0),
             'tess_cull_constant=%d' % int(mesh.get('cull', 0) > 0),
             'tess_mesh_vertices=%d' % mesh['mesh_vertices'], 'tess_mesh_primitives=%d' % mesh['mesh_primitives']]
    lines.append('vertex_inputs=%d' % len(vin_lines))
    lines.extend(vin_lines)
    return '\n'.join(lines) + '\n'


def compose(vs_path, hs_path, ds_path, outdir, rts0=None):
    outdir = Path(outdir); outdir.mkdir(parents=True, exist_ok=True)
    vs = tess_body.body(vs_path, 'VS', outdir / 'vs', rts0)
    hs = tess_body.body(hs_path, 'HS', outdir / 'hs', rts0)
    ds = tess_body.body(ds_path, 'DS', outdir / 'ds', rts0)
    decl = tess_dxil.hull_declarations(hs_path)
    sig = {}
    for name, path in (('vs', vs_path), ('hs', hs_path), ('ds', ds_path)):
        data = Path(path).read_bytes()
        sig[name] = {tag: tess_dxil.signature(data, tag) for tag in ('ISG1', 'OSG1', 'PSG1')}
    ds_regs_hint = max([p.get('location', -1) for p in ds['params']['main0'] if p.get('role') == 'output' and 'builtin' not in p] + [-1]) + 2
    obj_text, obj = compose_object(vs, hs, decl, sig['vs'], sig['hs'], ds_regs_hint)
    mesh_text, mesh = compose_mesh(hs, ds, decl, obj, sig['hs'], sig['ds'])
    # решение о раздельном режиме зависит от НАСТОЯЩЕГО числа регистров DS (известно только после mesh):
    # если догадка object-стороны разошлась — второй проход с точным числом
    check = validator(decl, obj['factor_int'], obj['threads_per_patch'], obj['hs_regs'], obj['pc_scalars'],
                      max(1, (obj['pc_scalars'] + 3) // 4), obj['vs_regs'], mesh['ds_regs'], obj['factor_int'])
    if bool(check['split']) != bool(obj.get('split')):
        obj_text, obj = compose_object(vs, hs, decl, sig['vs'], sig['hs'], mesh['ds_regs'])
        mesh_text, mesh = compose_mesh(hs, ds, decl, obj, sig['hs'], sig['ds'])
    (outdir / 'object.metal').write_text(obj_text)
    (outdir / 'mesh.metal').write_text(mesh_text)
    # предсказание допуска слоя по его же формуле
    admission = validator(decl, obj['factor_int'], obj['threads_per_patch'], obj['hs_regs'], obj['pc_scalars'],
                          max(1, (obj['pc_scalars'] + 3) // 4), obj['vs_regs'], mesh['ds_regs'], obj['factor_int'])
    if admission['payload'] != obj['payload_bytes']:
        raise ComposeError('payload size %d differs from the validator formula %d' % (obj['payload_bytes'], admission['payload']))
    vs_isg = {e['register']: e for e in sig['vs']['ISG1']}
    vin = []
    for loc in obj['vertex_inputs']:
        e = vs_isg.get(loc)
        if e is None:
            raise ComposeError('no VS input signature element for register %d' % loc)
        vin.append('vin=%d %d %s' % (loc, e['index'], e['name']))
    root_key = vs['root_key'] or hs['root_key'] or ds['root_key']
    qwords = max(vs['root_qwords'], hs['root_qwords'], ds['root_qwords'])
    (outdir / 'object.desc').write_text(description('vshs', 'main0_object', root_key, qwords, vs['resources'] + hs['resources'], decl, obj, mesh, vin))
    (outdir / 'mesh.desc').write_text(description('hsds', 'main0_mesh', root_key, qwords, hs['resources'] + ds['resources'], decl, obj, mesh, []))
    summary = dict(decl=decl, object=obj, mesh=mesh, root_key=root_key, qwords=qwords, admission=admission,
                   resources=dict(vs=vs['resources'], hs=hs['resources'], ds=ds['resources']))
    (outdir / 'SUMMARY.json').write_text(json.dumps(summary, indent=1, ensure_ascii=False))
    return summary


def build(outdir):
    """metal -c + metallib для object.metal и mesh.metal; возвращает {имя: (rc, сообщение)}."""
    outdir = Path(outdir)
    metal = subprocess.check_output(['/usr/bin/xcrun', '--find', 'metal'], text=True).strip()
    metallib = subprocess.check_output(['/usr/bin/xcrun', '--find', 'metallib'], text=True).strip()
    out = {}
    for name in ('object', 'mesh'):
        r = subprocess.run([metal, '-c', '-std=metal3.1', str(outdir / (name + '.metal')), '-o', str(outdir / (name + '.air'))],
                           capture_output=True, text=True)
        if r.returncode:
            out[name] = (r.returncode, r.stderr.strip()[:1200]); continue
        r2 = subprocess.run([metallib, str(outdir / (name + '.air')), '-o', str(outdir / (name + '.metallib'))],
                            capture_output=True, text=True)
        out[name] = (r2.returncode, r2.stderr.strip()[:400])
    return out


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('vs'); ap.add_argument('hs'); ap.add_argument('ds'); ap.add_argument('out')
    ap.add_argument('--rts0', help='файл части RTS0 (для контейнеров без корневой подписи)')
    a = ap.parse_args()
    rts0 = Path(a.rts0).read_bytes() if a.rts0 else None
    try:
        s = compose(a.vs, a.hs, a.ds, a.out, rts0)
    except (ComposeError, tess_body.BodyError) as e:
        print(json.dumps(dict(error=type(e).__name__, message=str(e)[:600]), ensure_ascii=False)); sys.exit(1)
    b = build(a.out)
    print(json.dumps(dict(decl=s['decl'], object={k: s['object'][k] for k in ('threads_per_patch', 'patch_per_group', 'factor_int', 'max_workloads', 'hs_regs', 'pc_scalars', 'payload_bytes', 'vertex_inputs')},
                          mesh={k: s['mesh'][k] for k in ('ds_regs', 'mesh_vertices', 'mesh_primitives', 'topology', 'clip')},
                          admission={k: s['admission'][k] for k in ('payload', 'mesh_bytes', 'split', 'mesh_groups', 'ok', 'why')},
                          build={k: v[0] for k, v in b.items()}, errors={k: v[1] for k, v in b.items() if v[0]}), ensure_ascii=False))
    sys.exit(1 if any(v[0] for v in b.values()) else 0)
