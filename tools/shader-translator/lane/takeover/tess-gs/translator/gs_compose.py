"""tess-gs 14.10.2026: собственный путь геометрической стадии — object (VS) + mesh (GS) по ABI прежнего пути.

Прежний путь (dxbc_converter_gs.cpp): object-группа = warp из W вершин (точки 32, линии 32, треугольники 30),
payload 16256 Б: int[0]=instance, int[1]=маска годных примитивов, int[2]=warp_id, с int[4] — вершины
(int4 vertices[W][vs_regs]); сетка mesh = (примитивов в warp, экземпляров GS, 1), mesh-группа — одна нить на
примитив. Выдачи GS записаны lower_geometry; здесь они проигрываются формулами прежнего конвертера
(полоса: при третьей и далее вершине треугольник (v-2, v-even, v-1+even), разрез: при >2 вершинах
primitive_count += n-2, vertex_offset += n). Атрибуты примитива (Layer/Viewport) — от ведущей вершины.
Только списковые входные примитивы (point/line/triangle), один поток, один экземпляр GS.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tess_body, gs_body, tess_dxil  # noqa: E402
from tess_compose import (ComposeError, parse_type, scalar_of, SWZ, key_of, link, qualify, struct_names, is_resource,  # noqa: E402
                          assign_position_registers, located, buffer_extra, uses_set1, set1_struct, sem_map,
                          PRELUDE, VERTEX_PULL, INDEX_KIND_CONSTANT, build)

WARP = {'point': (32, 32, 1), 'line': (32, 16, 2), 'triangle': (30, 10, 3)}
D3D_PRIMITIVE = {'point': 1, 'line': 2, 'triangle': 3, 'lineadj': 6, 'triangleadj': 7}
D3D_TOPOLOGY = {'point': 1, 'line': 3, 'triangle': 5}        # POINTLIST, LINESTRIP, TRIANGLESTRIP
PAYLOAD_BYTES = 16256


def store_register(dst, p, src):
    """Записать значение src (тип p['type']) в регистр payload dst (uint4) по компоненте p['component']."""
    elem, n, arr = parse_type(p['type'])
    s, w = scalar_of(elem)
    comp = p.get('component', 0)
    out = []
    for i in range(w):
        v = src if w == 1 else '%s.%s' % (src, SWZ[i])
        out.append('%s.%s = %s;' % (dst, SWZ[comp + i], v if s == 'uint' else 'as_type<uint>(%s)' % v))
    return out


def load_register(p, src):
    """Выражение значения типа p['type'] из регистра payload src (uint4) по компоненте p['component']."""
    elem, n, arr = parse_type(p['type'])
    s, w = scalar_of(elem)
    comp = p.get('component', 0)
    parts = []
    for i in range(w):
        c = '%s.%s' % (src, SWZ[comp + i])
        parts.append(c if s == 'uint' else 'as_type<%s>(%s)' % (s, c))
    return parts[0] if w == 1 else '%s(%s)' % (elem, ', '.join(parts))


# ---------------------------------------------------------------- прямая выдача (как у прежнего конвертера)
FUNC_RE = re.compile(r'static inline __attribute__\(\(always_inline\)\)\s*\n?\s*void (\w+)\(([^)]*)\)\s*\n\{(.*?)\n\}', re.S)
CULL_CONSTANT = 131


def propagate_params(text, targets, decl, args):
    """Функциям targets и всем, кто их зовёт (транзитивно), добавить параметры decl в начало списка; вызовы — args."""
    funcs = {m.group(1): m.group(3) for m in FUNC_RE.finditer(text)}
    need = set(t for t in targets if t in funcs)
    changed = True
    while changed:
        changed = False
        for fn, body in funcs.items():
            if fn not in need and any(re.search(r'\b' + u + r'\(', body) for u in need):
                need.add(fn); changed = True
    for fn in need:
        text = re.sub(r'(?<!void )(?<![\w])' + fn + r'\(', lambda m: fn + '(' + args + ', ', text)
        text = text.replace(fn + '(' + args + ', )', fn + '(' + args + ')')
        text = re.sub(r'(void ' + fn + r')\(([^)]*)\)', lambda m: m.group(1) + '(' + decl + (', ' + m.group(2) if m.group(2).strip() else '') + ')', text, count=1)
    return text, sorted(need)


def rewrite_emit(gs, mode, cfg):
    """Заменить тела mr_gs_emit / mr_gs_cut: mode 'mesh' — вершина и индексы пишутся в mesh на месте выдачи
    (формулы прежнего конвертера), атрибуты примитива и отсечение по SV_CullDistance — при завершении примитива
    из кольца трёх последних вершин; mode 'expand' — кольцо из трёх полных выходов, выбор угла c примитива t."""
    text = gs['body']
    topo, maxv = cfg['topo'], cfg['maxv']
    need = {'triangle': 2, 'line': 1, 'point': 0}[topo]
    cull = cfg.get('cull')
    L = []
    e = L.append
    e('    if (st.total >= %du) return;' % maxv)
    if mode == 'mesh':
        e('    const uint v = st.vertex_offset + st.next_write;')
        e('    MeshVertex mv;')
        e('    mv.position = %s;' % cfg['pos'])
        for member, pname, decl, w in cfg['members']:
            e('    mv.%s = %s;' % (member, pname))
        for i in range(cfg['clip'][1] if cfg.get('clip') else 0):
            e('    mv.clip[%d] = %s[%d];' % (i, cfg['clip'][0], i))
        for i in range(cull[1] if cull else 0):
            e('    mv.cull_%d = %s[%d];' % (i, cull[0], i))
        if topo == 'point':
            e('    mv.point_size = %s;' % (cfg['point_size'] or '1.0f'))
        e('    mesh.set_vertex(v, mv);')
        e('    st.r0 = st.r1; st.r1 = st.r2;')
        e('    { GsRing r = {};')
        for i in range(cull[1] if cull else 0):
            e('      r.cull[%d] = %s[%d];' % (i, cull[0], i))
        if cfg.get('layer'):
            e('      r.layer = %s;' % cfg['layer'])
        if cfg.get('viewport'):
            e('      r.viewport = %s;' % cfg['viewport'])
        e('      st.r2 = r; }')
        e('    if (st.next_write >= %du) {' % need)
        e('        const uint p = st.prim_count + st.next_write - %du;' % need)
        if topo == 'triangle':
            e('        const uint even = (st.next_write & 1u) == 0u ? 1u : 0u;')
            e('        mesh.set_index(3u * p, v - 2u);')
            e('        mesh.set_index(3u * p + 1u, v - even);')
            e('        mesh.set_index(3u * p + 2u, v - 1u + even);')
            lead = 'st.r0'; verts = ['st.r0', 'st.r1', 'st.r2']
        elif topo == 'line':
            e('        mesh.set_index(2u * p, v - 1u);')
            e('        mesh.set_index(2u * p + 1u, v);')
            lead = 'st.r1'; verts = ['st.r1', 'st.r2']
        else:
            e('        mesh.set_index(p, v);')
            lead = 'st.r2'; verts = ['st.r2']
        if cfg['prim_attrs'] or cull:
            e('        MeshPrimitive mp = {};')
            if cfg.get('layer'):
                e('        mp.layer = %s.layer;' % lead)
            if cfg.get('viewport'):
                e('        mp.viewport = %s.viewport;' % lead)
            if cull:
                e('        if (spvHonestCull) {')
                e('            bool culled = false;')
                for i in range(cull[1]):
                    e('            culled = culled || (%s);' % ' && '.join('%s.cull[%d] < 0.0f' % (vv, i) for vv in verts))
                e('            mp.culled = culled;')
                e('        }')
            e('        mesh.set_primitive(p, mp);')
        e('    }')
        e('    st.next_write += 1u; st.total += 1u;')
    else:
        e('    ExpandOut o = {};')
        e('    o.position = %s;' % cfg['pos'])
        for member, pname, decl, w in cfg['members']:
            if decl:
                e('    o.%s = %s(%s%s);' % (member, decl, pname, ', 0' * (4 - w)))
            else:
                e('    o.%s = %s;' % (member, pname))
        for i in range(cfg['clip'][1] if cfg.get('clip') else 0):
            e('    o.clip[%d] = %s[%d];' % (i, cfg['clip'][0], i))
        for i in range(cull[1] if cull else 0):
            e('    o.cull_%d = %s[%d];' % (i, cull[0], i))
        if cfg.get('layer'):
            e('    o.layer = %s;' % cfg['layer'])
        if cfg.get('viewport'):
            e('    o.viewport = %s;' % cfg['viewport'])
        if topo == 'point':
            e('    o.point_size = %s;' % (cfg['point_size'] or '1.0f'))
        e('    st.r0 = st.r1; st.r1 = st.r2; st.r2 = o;')
        e('    if (st.next_write >= %du && !st.found) {' % need)
        e('        const uint p = st.prim_count + st.next_write - %du;' % need)
        e('        if (p == st.t) {')
        if topo == 'triangle':
            e('            const bool even = (st.next_write & 1u) == 0u;')
            e('            ExpandOut pick = st.c == 0u ? st.r0 : (st.c == 1u ? (even ? st.r1 : st.r2) : (even ? st.r2 : st.r1));')
            verts = ['st.r0', 'st.r1', 'st.r2']
        elif topo == 'line':
            e('            ExpandOut pick = st.c == 0u ? st.r1 : st.r2;')
            verts = ['st.r1', 'st.r2']
        else:
            e('            ExpandOut pick = st.r2;')
            verts = ['st.r2']
        e('            bool culled = false;')
        if cull:
            e('            if (spvHonestCull) {')
            for i in range(cull[1]):
                e('                culled = culled || (%s);' % ' && '.join('%s.cull_%d < 0.0f' % (vv, i) for vv in verts))
            e('            }')
        e('            if (!culled) st.picked = pick;')
        e('            st.found = true;')
        e('        }')
        e('    }')
        e('    st.next_write += 1u; st.total += 1u;')
    emit_body = '\n'.join(L)
    cut_lines = []
    if topo != 'point':
        if mode == 'mesh':
            cut_lines.append('    if (st.next_write > %du) { st.prim_count += st.next_write - %du; st.vertex_offset += st.next_write; }' % (need, need))
        else:
            cut_lines.append('    if (st.next_write > %du) st.prim_count += st.next_write - %du;' % (need, need))
        cut_lines.append('    st.next_write = 0u;')
    cut_body = '\n'.join(cut_lines) if cut_lines else '    (void)st;'
    def replace_body(text, fn, body):
        m = re.search(r'(static inline __attribute__\(\(always_inline\)\)\s*\n?\s*void ' + fn + r'\([^)]*\)\s*\n\{)(.*?)(\n\})', text, re.S)
        if not m:
            return text, False
        return text[:m.start(2)] + '\n' + body + text[m.end(2):], True
    text, ok = replace_body(text, 'mr_gs_emit', emit_body)
    if not ok:
        raise ComposeError('mr_gs_emit not found in the GS body')
    text, _ = replace_body(text, 'mr_gs_cut', cut_body)
    decl = ('MeshT mesh, thread GsEmitState& st' if mode == 'mesh' else 'thread GsEmitState& st')
    args = ('mesh, st' if mode == 'mesh' else 'st')
    text, touched = propagate_params(text, ['mr_gs_emit', 'mr_gs_cut'], decl, args)
    return text, args


def emit_cfg(gs, gs_out, gs_out_bi, members, widen, pos_out):
    """Имена параметров функции выдачи по id записей отражения."""
    names = {p['id']: p['name'] for p in gs['params'].get('mr_gs_emit', [])}
    if pos_out['id'] not in names:
        raise ComposeError('emit function does not take SV_Position')
    cfg = dict(pos=names[pos_out['id']], members=[], prim_attrs=False)
    for p in sorted(gs_out, key=key_of):
        if p.get('pos_builtin'):
            continue
        decl, w = widen.get(p['id'], (None, 0))
        cfg['members'].append((members[p['id']], names[p['id']], decl, w))
    for b, key in (('ClipDistance', 'clip'), ('CullDistance', 'cull')):
        if b in gs_out_bi:
            elem, n, arr = parse_type(gs_out_bi[b]['type'])
            cfg[key] = (names[gs_out_bi[b]['id']], n)
    for b, key in (('Layer', 'layer'), ('ViewportIndex', 'viewport'), ('PointSize', 'point_size')):
        cfg[key] = names[gs_out_bi[b]['id']] if b in gs_out_bi else None
    cfg['prim_attrs'] = bool(cfg['layer'] or cfg['viewport'])
    return cfg


def compose(vs_path, gs_path, outdir, rts0=None, mesh_only=False):
    outdir = Path(outdir); outdir.mkdir(parents=True, exist_ok=True)
    gs = gs_body.body(gs_path, outdir / 'gs', rts0, require_root=not mesh_only)
    if mesh_only:
        # псевдо-VS: по одному выходу на каждый вход GS (регистр/компонента/тип), без тела
        pseudo = []
        for p in gs['params']['main0']:
            if p.get('role') == 'input' and ('location' in p or p.get('builtin') == 'Position'):
                elem, n, arr = parse_type(p['type'])
                q = dict(p, type=elem, role='output', id=10000 + p['id'], name='vs_' + p['name'], storage='Private', space='thread')
                pseudo.append(q)
        vs = dict(stage='VS', params={'main0': pseudo}, resources=0, root_qwords=0, root_key=None, buffer_users={0: [], 1: []},
                  unsafe_array='', body='', call_args=[], empty=True)
    else:
        vs = tess_body.body(vs_path, 'VS', outdir / 'vs', rts0)
    meta = gs['gs']
    in_prim, out_topo, maxv = meta['input_primitive'], meta['output_topology'], meta['max_vertices']
    if in_prim not in WARP:
        raise ComposeError('GS input primitive %s: only point/line/triangle lists are served' % in_prim)
    if meta['invocations'] != 1:
        raise ComposeError('GS instancing (%d) not served' % meta['invocations'])
    W, WPC, VPP = WARP[in_prim]
    sig = {}
    for name, path in (('vs', vs_path), ('gs', gs_path)):
        if name == 'vs' and mesh_only:
            d = Path(gs_path).read_bytes(); sig['vs'] = {'ISG1': [], 'OSG1': tess_dxil.signature(d, 'ISG1')}; continue
        data = Path(path).read_bytes()
        sig[name] = {tag: tess_dxil.signature(data, tag) for tag in ('ISG1', 'OSG1')}
    vs_main, gs_main = vs['params']['main0'], gs['params']['main0']
    assign_position_registers(vs_main, sig['vs']['OSG1'], 'output', 'VS')
    assign_position_registers(gs_main, sig['gs']['ISG1'], 'input', 'GS')
    assign_position_registers(gs_main, sig['gs']['OSG1'], 'output', 'GS out')
    vs_out = [p for p in vs_main if p.get('role') == 'output']
    vs_in = [p for p in vs_main if p.get('role') == 'input' and 'location' in p]
    gs_in = [p for p in gs_main if p.get('role') == 'input' and located(p)]
    gs_in_bi = [p for p in gs_main if p.get('role') == 'input' and 'builtin' in p and not p.get('pos_builtin')]
    gs_out = [p for p in gs_main if p.get('role') == 'output' and (located(p) or 'builtin' not in p)]
    gs_out_bi = {p['builtin']: p for p in gs_main if p.get('role') == 'output' and 'builtin' in p}
    records = {p['record_of']: p for p in gs_main if p.get('kind') == 'record'}
    count = next(p for p in gs_main if p.get('kind') == 'count')
    cut = next((p for p in gs_main if p.get('kind') == 'cut'), None)   # нет EndPrimitive — нет и массива разрезов
    vs_regs = max([p.get('location', -1) for p in vs_out] + [-1]) + 1
    gs_regs = max([p.get('location', -1) for p in gs_out] + [-1]) + 1
    if 16 + W * vs_regs * 16 > PAYLOAD_BYTES:
        raise ComposeError('payload %d > %d' % (16 + W * vs_regs * 16, PAYLOAD_BYTES))
    if not vs_regs:
        raise ComposeError('VS without outputs')
    vs_sem = sem_map(sig['vs']['OSG1']); gs_isem = sem_map(sig['gs']['ISG1'])
    gs_in_src = link(vs_out, gs_in, vs_sem, gs_isem, 'VS->GS')
    for p in gs_in_bi:
        if p['builtin'] not in ('PrimitiveId', 'InvocationId'):
            raise ComposeError('GS reads builtin %s: not served' % p['builtin'])
    pos_out = gs_out_bi.get('Position')
    if pos_out is None:
        raise ComposeError('GS without SV_Position output')
    for b in gs_out_bi:
        if b not in ('Position', 'Layer', 'ViewportIndex', 'ClipDistance', 'CullDistance', 'PointSize'):
            raise ComposeError('GS output builtin %s: not served' % b)
    qwords = max(vs['root_qwords'], gs['root_qwords'])
    set1 = uses_set1(vs, gs)
    # ------------------------------------------------------------------ общий пролог
    def prologue(L):
        emit = L.append
        emit('#pragma clang diagnostic ignored "-Wmissing-prototypes"')
        emit('#pragma clang diagnostic ignored "-Wmissing-braces"')
        emit('#pragma clang diagnostic ignored "-Wunused-variable"')
        emit('#include <metal_stdlib>')
        emit('#include <simd/simd.h>')
        emit('using namespace metal;')
        emit(PRELUDE)
        emit(vs['unsafe_array'] or gs['unsafe_array'])
        emit('struct spvDescriptorSetBuffer0\n{\n' + ''.join('    ulong r%d;\n' % i for i in range(max(qwords, 1))) + '};')
        if set1:
            emit(set1_struct(vs, gs))
        emit('struct Payload {')
        emit('    int header[4];                    // [0] instance, [1] valid primitive mask, [2] warp id')
        emit('    uint4 vertices[%d];               // [vertex * %d + register]' % (W * vs_regs, vs_regs))
        emit('};')
    # ------------------------------------------------------------------ object (VS)
    L = []; emit = L.append
    emit('// tess-gs: object-функция (VS, warp из %d вершин) собственного пути геометрии; раскладка payload прежнего пути.' % W)
    prologue(L)
    emit(VERTEX_PULL)
    emit('namespace vs_body {\n' + vs['body'] + '\n}')
    for p in sorted(vs_in, key=key_of):
        loc = p['location']
        for k, nm in enumerate(('entry', 'offset', 'format', 'step')):
            emit('constant uint spvVin%d_%s [[function_constant(%d)]];' % (loc, nm, 4 * loc + k))
    emit('constant uint spvIndexKind [[function_constant(%d)]];' % INDEX_KIND_CONSTANT)
    emit('[[object, max_total_threads_per_threadgroup(%d)]] void main0_object(object_data Payload& payload [[payload]], mesh_grid_properties mgp,' % W)
    emit('    uint tid [[thread_position_in_threadgroup]], uint2 tgid [[threadgroup_position_in_grid]],')
    emit('    constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]],')
    if set1:
        emit('    constant spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]],')
    emit('    constant spvVertexBufferEntry* spvVertexBuffers [[buffer(2)]],')
    emit('    device const uint* spvIndexBuffer [[buffer(3)]], constant uint* spvDrawArguments [[buffer(4)]])')
    emit('{')
    emit('    threadgroup atomic_uint valid_vertex_mask;')
    emit('    if (tid == 0u) atomic_store_explicit(&valid_vertex_mask, 0u, memory_order_relaxed);')
    emit('    threadgroup_barrier(mem_flags::mem_threadgroup);')
    emit('    const uint warp_id = tgid.x, instance_id = tgid.y;')
    emit('    const uint global_index = warp_id * %du + tid;' % W)
    emit('    const uint vertex_count = spvDrawArguments[0];')
    emit('    if (global_index < vertex_count) {')
    emit('        uint vid;')
    emit('        if (spvIndexKind == 1u) vid = uint(((device const ushort*)spvIndexBuffer)[spvDrawArguments[2] + global_index]);')
    emit('        else if (spvIndexKind == 2u) vid = spvIndexBuffer[spvDrawArguments[2] + global_index];')
    emit('        else vid = global_index;')
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
        for line in store_register('payload.vertices[tid * %du + %du]' % (vs_regs, p['location']), p, 'vs_%d' % p['id']):
            emit('        ' + line)
    emit('        atomic_fetch_or_explicit(&valid_vertex_mask, 1u << tid, memory_order_relaxed);')
    emit('    }')
    emit('    threadgroup_barrier(mem_flags::mem_threadgroup);')
    emit('    if (tid == 0u) {')
    emit('        const uint vmask = atomic_load_explicit(&valid_vertex_mask, memory_order_relaxed);')
    emit('        uint pmask = 0u;')
    emit('        for (uint p = 0u; p < %du; ++p) {' % WPC)
    emit('            const uint need = ((1u << %du) - 1u) << (p * %du);' % (VPP, VPP))
    emit('            if ((vmask & need) == need) pmask |= 1u << p;')
    emit('        }')
    emit('        payload.header[0] = int(instance_id); payload.header[1] = int(pmask); payload.header[2] = int(warp_id); payload.header[3] = 0;')
    emit('        mgp.set_threadgroups_per_grid(uint3(%du, 1u, 1u));' % WPC)
    emit('    }')
    emit('}')
    object_text = '\n'.join(L) + '\n'
    vin = sorted(set(p['location'] for p in vs_in))
    # ------------------------------------------------------------------ mesh (GS)
    L = []; emit = L.append
    emit('// tess-gs: mesh-функция (GS, одна нить на примитив) собственного пути геометрии; проигрывание выдач.')
    prologue(L)
    emit('struct MeshVertex {')
    emit('    float4 position [[position]];')
    members = {}
    for p in sorted(gs_out, key=key_of):
        if p.get('pos_builtin'):
            continue
        elem, n, arr = parse_type(p['type'])
        if arr:
            raise ComposeError('arrayed GS output not served: %s' % p['name'])
        comp = p.get('component', 0)
        members[p['id']] = 'm_%d' % p['id']
        emit('    %s m_%d [[user(locn%d%s)]];' % (elem, p['id'], p['location'], '_%d' % comp if comp else ''))
    clip_n = cull_n = 0
    if 'ClipDistance' in gs_out_bi:
        elem, clip_n, arr = parse_type(gs_out_bi['ClipDistance']['type'])
        emit('    float clip [[clip_distance]] [%d];' % clip_n)
    if 'CullDistance' in gs_out_bi:
        elem, cull_n, arr = parse_type(gs_out_bi['CullDistance']['type'])
        for i in range(cull_n):
            emit('    float cull_%d [[user(SV_CullDistance_%d)]];' % (i, i))
    if out_topo == 'point':
        emit('    float point_size [[point_size]];')
    emit('};')
    prim_attrs = [b for b in ('Layer', 'ViewportIndex') if b in gs_out_bi]
    has_prim = bool(prim_attrs) or cull_n > 0
    if has_prim:
        emit('struct MeshPrimitive {')
        if 'Layer' in prim_attrs:
            emit('    uint layer [[render_target_array_index]];')
        if 'ViewportIndex' in prim_attrs:
            emit('    uint viewport [[viewport_array_index]];')
        if cull_n:
            emit('    bool culled [[primitive_culled]];')
        emit('};')
    if cull_n:
        emit('constant bool spvHonestCull [[function_constant(%d)]];   // честный SV_CullDistance (семантика D3D), ключ слоя' % CULL_CONSTANT)
    emit('using MeshT = metal::mesh<MeshVertex, %s, %d, %d, topology::%s>;' % ('MeshPrimitive' if has_prim else 'void', maxv, maxv, out_topo))
    emit('struct GsRing { float cull[%d]; uint layer; uint viewport; };' % max(cull_n, 1))
    emit('struct GsEmitState { uint next_write, vertex_offset, prim_count, total; GsRing r0, r1, r2; };')
    cfg = emit_cfg(gs, gs_out, gs_out_bi, members, {}, pos_out)
    cfg.update(topo=out_topo, maxv=maxv)
    body_text, emit_args = rewrite_emit(gs, 'mesh', cfg)
    emit('namespace gs_body {\n' + body_text + '\n}')
    emit('[[mesh, max_total_threads_per_threadgroup(1)]] void main0_mesh(object_data const Payload& payload [[payload]], MeshT mesh,')
    extra_params = []
    if mesh_only and not gs['root_qwords']:
        # без корневой подписи ресурсы приходят параметрами ядра SPIRV-Cross (plain bindings) — переносим их как есть
        extra_params = [qualify(q, 'gs_body', struct_names(gs['body'])) for q in gs['kernel_params']
                        if '[[thread' not in q and '[[threadgroup' not in q and 'spvDescriptorSet' not in q]
    emit('    uint3 tgid [[threadgroup_position_in_grid]]' + (',' if (extra_params or gs['root_qwords'] or not mesh_only) else ')'))
    if not mesh_only or gs['root_qwords']:
        emit('    constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]' + (',' if (set1 or extra_params) else ')'))
        if set1:
            emit('    constant spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]]' + (',' if extra_params else ')'))
    if extra_params:
        emit('    ' + ',\n    '.join(extra_params) + ')')
    emit('{')
    emit('    const uint prim = tgid.x, gs_instance = tgid.y;')
    emit('    const uint pmask = uint(payload.header[1]), warp_id = uint(payload.header[2]);')
    emit('    if ((pmask & (1u << prim)) == 0u) { mesh.set_primitive_count(0u); return; }')
    emit('    const uint primitive_id = warp_id * %du + prim;' % WPC)
    emit('    const uint lead = prim * %du;' % VPP)
    gs_names = struct_names(gs['body'])
    args = []
    for p, arg in zip(gs_main, gs['call_args'][(1 if gs['call_args'] and gs['call_args'][0].startswith('spvDescriptorSet') else 0):]):
        if is_resource(p):
            args.append(qualify(arg, 'gs_body', gs_names)); continue
        nm = 'gs_%d' % p['id']
        elem, n, arr = parse_type(p['type'])
        if p['id'] in gs_in_src:
            src = gs_in_src[p['id']]
            if not arr or n != VPP:
                raise ComposeError('GS input %s is %s, expected array of %d' % (p['name'], p['type'], VPP))
            emit('    %s %s = {};' % (p['type'], nm))
            q = dict(p, type=elem, component=src.get('component', 0))
            for v in range(VPP):
                emit('    %s[%d] = %s;' % (nm, v, load_register(q, 'payload.vertices[(lead + %du) * %du + %du]' % (v, vs_regs, src['location']))))
        elif p.get('builtin') == 'PrimitiveId':
            emit('    uint %s = primitive_id;' % nm)
        elif p.get('builtin') == 'InvocationId':
            emit('    uint %s = gs_instance;' % nm)
        elif p.get('kind') == 'count':
            emit('    uint %s = 0u;' % nm)
        elif p.get('kind') in ('record', 'cut'):
            emit('    %s %s = {};' % (p['type'], nm))
        elif p.get('role') == 'output':
            emit('    %s %s = {};' % (p['type'], nm))
        else:
            raise ComposeError('GS param not handled: %s' % p)
        args.append(nm)
    need = {'triangle': 2, 'line': 1, 'point': 0}[out_topo]
    emit('    GsEmitState st = {};')
    emit('    gs_body::main0(%s);' % ', '.join([emit_args] + buffer_extra(gs, 'main0') + args))
    if out_topo != 'point':
        emit('    if (st.next_write > %du) { st.prim_count += st.next_write - %du; st.vertex_offset += st.next_write; }' % (need, need))
    emit('    mesh.set_primitive_count(%s);' % ('st.total' if out_topo == 'point' else 'st.prim_count'))
    emit('}')
    mesh_text = '\n'.join(L) + '\n'
    if not mesh_only:
        (outdir / 'object.metal').write_text(object_text)
    (outdir / 'mesh.metal').write_text(mesh_text)
    root_key = vs['root_key'] or gs['root_key']
    info = dict(primitive=in_prim, d3d_primitive=D3D_PRIMITIVE[in_prim], output=out_topo, d3d_topology=D3D_TOPOLOGY[out_topo],
                max_vertices=maxv, vertices_per_group=W, primitives_per_group=WPC, vertices_per_primitive=VPP,
                vs_out_regs=vs_regs, gs_out_regs=gs_regs, payload_bytes=16 + W * vs_regs * 16, instances=1,
                clip=clip_n, cull=cull_n, layer=int('Layer' in prim_attrs), viewport=int('ViewportIndex' in prim_attrs))
    vs_isg = {e['register']: e for e in sig['vs']['ISG1']}
    vin_lines = []
    for loc in vin:
        e = vs_isg.get(loc)
        if e is None:
            raise ComposeError('no VS input signature element for register %d' % loc)
        vin_lines.append('vin=%d %d %s' % (loc, e['index'], e['name']))
    for kind, entry, res, vl in (('vsgs', 'main0_object', vs['resources'] + gs['resources'], vin_lines), ('gsm', 'main0_mesh', gs['resources'], [])):
        lines = ['format=5', 'kind=%s' % kind, 'entry=%s' % entry, 'root=%s' % (root_key or 'none'), 'root_qwords=%d' % qwords,
                 'resources=%d' % res, 'variant=none',
                 'gs_primitive=%d' % info['d3d_primitive'], 'gs_output_topology=%d' % info['d3d_topology'],
                 'gs_max_vertices=%d' % maxv, 'gs_vertices_per_group=%d' % W, 'gs_primitives_per_group=%d' % WPC,
                 'gs_vs_out_regs=%d' % vs_regs, 'gs_out_regs=%d' % gs_regs, 'gs_payload_bytes=%d' % info['payload_bytes'],
                 'gs_instances=1', 'gs_cull_constant=%d' % int(info['cull'] > 0), 'vertex_inputs=%d' % len(vl)] + vl
        (outdir / ('object.desc' if kind == 'vsgs' else 'mesh.desc')).write_text('\n'.join(lines) + '\n')
    summary = dict(gs=info, root_key=root_key, qwords=qwords, resources=dict(vs=vs['resources'], gs=gs['resources']), vertex_inputs=vin)
    (outdir / 'SUMMARY.json').write_text(json.dumps(summary, indent=1, ensure_ascii=False))
    return summary


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('vs'); ap.add_argument('gs'); ap.add_argument('out'); ap.add_argument('--rts0')
    a = ap.parse_args()
    rts0 = Path(a.rts0).read_bytes() if a.rts0 else None
    s = compose(a.vs, a.gs, a.out, rts0)
    b = build(a.out)
    print(json.dumps(dict(summary=s, build={k: (v[0], v[1][:300]) for k, v in b.items()}), ensure_ascii=False))


# ---------------------------------------------------------------- уровень 4 (0083 C.4): расширение в VS
EXPANSION = {'triangle': lambda maxv: 3 * (maxv - 2), 'line': lambda maxv: 2 * (maxv - 1), 'point': lambda maxv: maxv}


def compose_expanded(vs_path, gs_path, outdir, rts0=None):
    """Вершинная функция, исполняющая VS+GS на каждую выходную вершину (способ 2 сдачи 0083, C.4) с подавлением
    отсутствующих примитивов вырожденными: один обычный draw списком из E вершин на входной примитив, без object/mesh,
    без payload и буферов. Аргументы draw (buffer 4, пишет список команд): count, instances, start, base_vertex,
    start_instance, index_kind. Порядок примитивов — порядок draw (гарантия API), чего у mesh-пути нет."""
    outdir = Path(outdir); outdir.mkdir(parents=True, exist_ok=True)
    gs = gs_body.body(gs_path, outdir / 'gs', rts0)
    vs = tess_body.body(vs_path, 'VS', outdir / 'vs', rts0)
    meta = gs['gs']
    in_prim, out_topo, maxv = meta['input_primitive'], meta['output_topology'], meta['max_vertices']
    if in_prim not in WARP:
        raise ComposeError('GS input primitive %s: only point/line/triangle lists are served' % in_prim)
    if meta['invocations'] != 1:
        raise ComposeError('GS instancing (%d) not served' % meta['invocations'])
    VPP = WARP[in_prim][2]
    E = EXPANSION[out_topo](maxv)
    if E <= 0:
        raise ComposeError('maxvertexcount %d too small for %s output' % (maxv, out_topo))
    sig = {}
    for name, path in (('vs', vs_path), ('gs', gs_path)):
        data = Path(path).read_bytes()
        sig[name] = {tag: tess_dxil.signature(data, tag) for tag in ('ISG1', 'OSG1')}
    vs_main, gs_main = vs['params']['main0'], gs['params']['main0']
    assign_position_registers(vs_main, sig['vs']['OSG1'], 'output', 'VS')
    assign_position_registers(gs_main, sig['gs']['ISG1'], 'input', 'GS')
    assign_position_registers(gs_main, sig['gs']['OSG1'], 'output', 'GS out')
    vs_out = [p for p in vs_main if p.get('role') == 'output']
    vs_in = [p for p in vs_main if p.get('role') == 'input' and 'location' in p]
    gs_in = [p for p in gs_main if p.get('role') == 'input' and located(p)]
    gs_in_bi = [p for p in gs_main if p.get('role') == 'input' and 'builtin' in p and not p.get('pos_builtin')]
    gs_out = [p for p in gs_main if p.get('role') == 'output' and (located(p) or 'builtin' not in p)]
    gs_out_bi = {p['builtin']: p for p in gs_main if p.get('role') == 'output' and 'builtin' in p}
    records = {p['record_of']: p for p in gs_main if p.get('kind') == 'record'}
    count = next(p for p in gs_main if p.get('kind') == 'count')
    cut = next((p for p in gs_main if p.get('kind') == 'cut'), None)
    vs_sem = sem_map(sig['vs']['OSG1']); gs_isem = sem_map(sig['gs']['ISG1'])
    gs_in_src = link(vs_out, gs_in, vs_sem, gs_isem, 'VS->GS')
    for p in gs_in_bi:
        if p['builtin'] not in ('PrimitiveId', 'InvocationId'):
            raise ComposeError('GS reads builtin %s: not served' % p['builtin'])
    pos_out = gs_out_bi.get('Position')
    if pos_out is None:
        raise ComposeError('GS without SV_Position output')
    for b in gs_out_bi:
        if b not in ('Position', 'ClipDistance', 'CullDistance', 'PointSize', 'Layer', 'ViewportIndex'):
            raise ComposeError('GS output builtin %s: not served' % b)
    qwords = max(vs['root_qwords'], gs['root_qwords'])
    set1 = uses_set1(vs, gs)
    L = []; emit = L.append
    emit('// tess-gs: уровень 4 — VS+GS в одной вершинной функции (0083 C.4), E=%d вершин на входной примитив.' % E)
    emit('#pragma clang diagnostic ignored "-Wmissing-prototypes"')
    emit('#pragma clang diagnostic ignored "-Wmissing-braces"')
    emit('#pragma clang diagnostic ignored "-Wunused-variable"')
    emit('#include <metal_stdlib>')
    emit('#include <simd/simd.h>')
    emit('using namespace metal;')
    emit(PRELUDE)
    emit(vs['unsafe_array'] or gs['unsafe_array'])
    emit(VERTEX_PULL)
    emit('struct spvDescriptorSetBuffer0\n{\n' + ''.join('    ulong r%d;\n' % i for i in range(max(qwords, 1))) + '};')
    if set1:
        emit(set1_struct(vs, gs))
    emit('namespace vs_body {\n' + vs['body'] + '\n}')
    emit('struct ExpandOut {')
    emit('    float4 position [[position]];')
    members = {}
    widen = {}   # id -> (тип в структуре, ширина исходника): dxil-spirv объявляет входы PS 4-компонентными, а связка
                 # vertex->fragment у Metal требует точного совпадения типов (mesh->fragment это терпит)
    for p in sorted(gs_out, key=key_of):
        if p.get('pos_builtin'):
            continue
        elem, n, arr = parse_type(p['type'])
        if arr:
            raise ComposeError('arrayed GS output not served: %s' % p['name'])
        comp = p.get('component', 0)
        members[p['id']] = 'm_%d' % p['id']
        s_, w_ = scalar_of(elem)
        decl = elem
        if comp == 0 and w_ < 4:
            decl = s_ + '4'; widen[p['id']] = (decl, w_)
        emit('    %s m_%d [[user(locn%d%s)]];' % (decl, p['id'], p['location'], '_%d' % comp if comp else ''))
    clip_n = cull_n = 0
    if 'ClipDistance' in gs_out_bi:
        elem, clip_n, arr = parse_type(gs_out_bi['ClipDistance']['type'])
        emit('    float clip [[clip_distance]] [%d];' % clip_n)
    if 'CullDistance' in gs_out_bi:
        elem, cull_n, arr = parse_type(gs_out_bi['CullDistance']['type'])
        for i in range(cull_n):
            emit('    float cull_%d [[user(SV_CullDistance_%d)]];' % (i, i))
    if 'Layer' in gs_out_bi:
        emit('    uint layer [[render_target_array_index]];')
    if 'ViewportIndex' in gs_out_bi:
        emit('    uint viewport [[viewport_array_index]];')
    if out_topo == 'point':
        emit('    float point_size [[point_size]];')
    emit('};')
    if cull_n:
        emit('constant bool spvHonestCull [[function_constant(%d)]];   // честный SV_CullDistance: вырожденный примитив' % CULL_CONSTANT)
    emit('struct GsEmitState { uint next_write, prim_count, total, t, c; bool found; ExpandOut r0, r1, r2, picked; };')
    cfg = emit_cfg(gs, gs_out, gs_out_bi, members, widen, pos_out)
    cfg.update(topo=out_topo, maxv=maxv)
    body_text, emit_args = rewrite_emit(gs, 'expand', cfg)
    emit('namespace gs_body {\n' + body_text + '\n}')
    for p in sorted(vs_in, key=key_of):
        loc = p['location']
        for k, nm in enumerate(('entry', 'offset', 'format', 'step')):
            emit('constant uint spvVin%d_%s [[function_constant(%d)]];' % (loc, nm, 4 * loc + k))
    emit('[[vertex]] ExpandOut main0_expand(uint vid [[vertex_id]], uint iid [[instance_id]],')
    emit('    constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]],')
    if set1:
        emit('    constant spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]],')
    emit('    constant spvVertexBufferEntry* spvVertexBuffers [[buffer(2)]],')
    emit('    device const uint* spvIndexBuffer [[buffer(3)]], constant uint* spvDrawArguments [[buffer(4)]])')
    emit('{')
    emit('    const uint prim = vid / %du, corner = vid %% %du;' % (E, E))
    emit('    const uint index_kind = spvDrawArguments[5];')
    emit('    const uint base_vertex = index_kind ? spvDrawArguments[3] : 0u;')
    emit('    const uint instance_index = iid + spvDrawArguments[4];')
    emit('    const uint base_instance = spvDrawArguments[4];')
    vs_names = struct_names(vs['body'])
    gs_locals = {}
    for p in gs_in:
        elem, n, arr = parse_type(p['type'])
        if not arr or n != VPP:
            raise ComposeError('GS input %s is %s, expected array of %d' % (p['name'], p['type'], VPP))
        gs_locals[p['id']] = 'gs_%d' % p['id']
        emit('    %s gs_%d = {};' % (p['type'], p['id']))
    emit('    for (uint v = 0u; v < %du; ++v) {' % VPP)
    emit('        const uint global_index = spvDrawArguments[2] + prim * %du + v;' % VPP)
    emit('        uint vidx;')
    emit('        if (index_kind == 1u) vidx = uint(((device const ushort*)spvIndexBuffer)[global_index]);')
    emit('        else if (index_kind == 2u) vidx = spvIndexBuffer[global_index];')
    emit('        else vidx = global_index;')
    emit('        const uint vertex_index = vidx + base_vertex;')
    args = []
    for p, arg in zip(vs_main, vs['call_args']):
        if is_resource(p):
            args.append(qualify(arg, 'vs_body', vs_names)); continue
        if arg.startswith('spvDescriptorSet'):
            args.append(arg); continue
        nm = 'vs_%d' % p['id']
        elem, n, arr = parse_type(p['type'])
        if p.get('role') == 'input' and 'location' in p:
            s_, w_ = scalar_of(elem)
            loc = p['location']
            pull = 'spvPull(spvVertexBuffers, spvVin%d_entry, spvVin%d_offset, spvVin%d_format, spvVin%d_step, vertex_index, instance_index, base_instance)' % (loc, loc, loc, loc)
            swz = '' if w_ == 4 else '.' + SWZ[:w_]
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
    for p in gs_in:
        src = gs_in_src[p['id']]
        elem, n, arr = parse_type(p['type'])
        s_, w_ = scalar_of(elem)
        se, sn, sarr = parse_type(src['type'])
        ss, sw = scalar_of(se)
        val = 'vs_%d' % src['id']
        if w_ == sw:
            expr = val if s_ == ss else 'as_type<%s>(%s)' % (elem, val)
        else:
            comps = ['%s%s' % (val, ('.' + SWZ[src.get('component', 0) + i]) if sw > 1 else '') for i in range(w_)]
            expr = '%s(%s)' % (elem, ', '.join(comps))
        emit('        gs_%d[v] = %s;' % (p['id'], expr))
    emit('    }')
    gs_names = struct_names(gs['body'])
    args = []
    for p, arg in zip(gs_main, gs['call_args'][(1 if gs['call_args'] and gs['call_args'][0].startswith('spvDescriptorSet') else 0):]):
        if is_resource(p):
            args.append(qualify(arg, 'gs_body', gs_names)); continue
        nm = 'gs_%d' % p['id']
        if p['id'] in gs_locals:
            pass
        elif p.get('builtin') == 'PrimitiveId':
            emit('    uint %s = prim;' % nm)
        elif p.get('builtin') == 'InvocationId':
            emit('    uint %s = 0u;' % nm)
        elif p.get('kind') == 'count':
            emit('    uint %s = 0u;' % nm)
        elif p.get('kind') in ('record', 'cut'):
            emit('    %s %s = {};' % (p['type'], nm))
        elif p.get('role') == 'output':
            emit('    %s %s = {};' % (p['type'], nm))
        else:
            raise ComposeError('GS param not handled: %s' % p)
        args.append(nm)
    per = {'triangle': 3, 'line': 2, 'point': 1}[out_topo]
    emit('    GsEmitState st = {};')
    emit('    st.t = corner / %du; st.c = corner %% %du;' % (per, per))
    emit('    st.picked.position = float4(0.0f, 0.0f, 0.0f, 1.0f);   // вырожденный примитив, если выдачи нет или она отсечена')
    emit('    gs_body::main0(%s);' % ', '.join([emit_args] + buffer_extra(gs, 'main0') + args))
    emit('    ExpandOut out = st.picked;')
    emit('    return out;')
    emit('}')
    (outdir / 'expanded.metal').write_text('\n'.join(L) + '\n')
    vin = sorted(set(p['location'] for p in vs_in))
    vs_isg = {e['register']: e for e in sig['vs']['ISG1']}
    vin_lines = ['vin=%d %d %s' % (loc, vs_isg[loc]['index'], vs_isg[loc]['name']) for loc in vin]
    root_key = vs['root_key'] or gs['root_key']
    lines = ['format=6', 'kind=vsx', 'entry=main0_expand', 'root=%s' % (root_key or 'none'), 'root_qwords=%d' % qwords,
             'resources=%d' % (vs['resources'] + gs['resources']), 'variant=none',
             'gs_primitive=%d' % D3D_PRIMITIVE[in_prim], 'gs_output_topology=%d' % D3D_TOPOLOGY[out_topo],
             'gs_max_vertices=%d' % maxv, 'gs_input_vertices=%d' % VPP, 'gs_expand=%d' % E, 'gs_instances=1',
             'gs_vertices_per_group=%d' % WARP[in_prim][0], 'gs_primitives_per_group=%d' % WARP[in_prim][1],
             'gs_vs_out_regs=%d' % (max([p.get('location', -1) for p in vs_out] + [-1]) + 1), 'gs_out_regs=%d' % (max([p.get('location', -1) for p in gs_out] + [-1]) + 1),
             'gs_payload_bytes=0', 'gs_cull_constant=%d' % int(cull_n > 0), 'vertex_inputs=%d' % len(vin_lines)] + vin_lines
    (outdir / 'expanded.desc').write_text('\n'.join(lines) + '\n')
    info = dict(primitive=in_prim, output=out_topo, max_vertices=maxv, expand=E, input_vertices=VPP, clip=clip_n, cull=cull_n,
                layer=int('Layer' in gs_out_bi), root_key=root_key, qwords=qwords, vertex_inputs=vin)
    (outdir / 'EXPANDED.json').write_text(json.dumps(info, indent=1))
    return info


def build_expanded(outdir):
    outdir = Path(outdir)
    metal = subprocess.check_output(['/usr/bin/xcrun', '--find', 'metal'], text=True).strip()
    metallib = subprocess.check_output(['/usr/bin/xcrun', '--find', 'metallib'], text=True).strip()
    r = subprocess.run([metal, '-c', '-std=metal3.1', str(outdir / 'expanded.metal'), '-o', str(outdir / 'expanded.air')], capture_output=True, text=True)
    if r.returncode:
        return (r.returncode, r.stderr.strip()[:1200])
    r2 = subprocess.run([metallib, str(outdir / 'expanded.air'), '-o', str(outdir / 'expanded.metallib')], capture_output=True, text=True)
    return (r2.returncode, r2.stderr.strip()[:400])
