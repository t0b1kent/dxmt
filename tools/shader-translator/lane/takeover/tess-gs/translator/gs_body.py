"""tess-gs 14.10.2026: тело геометрической стадии как inner-функция MSL (способ 3 сдачи 0083, mesh).

Как tess_body, но понижение — lower_geometry (выдачи записываются в массивы), без буферов записей
(host_stub не нужен: обёртка-ядро однoблочная). Возвращает dict тела + 'gs' (метаданные стадии и роли новых
глобалов: record/count/cut) + 'params' (отражение main0 через tess_reflect с ролями из lower_geometry).
"""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tess_body                      # noqa: E402
import tess_reflect                   # noqa: E402
import lower_geometry                 # noqa: E402
from tess_body import BodyError, run, add_root_buffer_params, split_call, TOOLS, ADM, UNSAFE_ARRAY_RE  # noqa: E402
rootabi = tess_body.rootabi


def body(container, workdir, rts0=None, require_root=True):
    workdir = Path(workdir); workdir.mkdir(parents=True, exist_ok=True)
    data = Path(container).read_bytes()
    spv, lmap = workdir / 'dxil.spv', workdir / 'logical.json'
    parts = tess_body.rootabi.dxbc_chunks(data)
    if 'MRSV' in parts and 'DXIL' not in parts:
        # синтетический контейнер проб: готовый SPIR-V геометрической стадии в части MRSV, ресурсов нет
        spv.write_bytes(parts['MRSV']); resources = []
    else:
        run([TOOLS['frontend'], container, '--ssbo-uav', '--ssbo-srv', '--mr-logical-bindings', lmap, '--output', spv], 'frontend')
        resources = json.loads(lmap.read_text())['bindings']
    rts0 = rts0 if rts0 is not None else rootabi.dxbc_chunks(data).get('RTS0')
    rs = mapping = None
    if rts0 is not None:
        rs = rootabi.root_layout(rootabi.parse_rts0(rts0))
        mapping, missing = rootabi.map_resources(rs, 'GS', resources)
        if missing:
            raise BodyError('resource is not in the root signature: ' + ', '.join(
                '%s space %d register %d' % (rootabi.CLASS_NAME[m['class']], m['space'], m['register']) for m in missing[:4]))
        for r in resources:
            if r.get('view'):
                mapping[r['binding']] = dict(mapping[r['binding']], counter=1)
    elif resources and require_root:
        raise BodyError('shader reads resources but has no root signature')
    try:
        lowered, meta = lower_geometry.lower(spv.read_bytes())
    except lower_geometry.Unsupported as e:
        raise BodyError('lower_geometry: ' + str(e))
    (workdir / 'lowered.spv').write_bytes(lowered)
    (workdir / 'gs-meta.json').write_text(json.dumps({k: v for k, v in meta.items() if k != 'records'}))
    run([TOOLS['spirv_val'], workdir / 'lowered.spv'], 'spirv-val')
    msl = workdir / 'stub.metal'
    cmd = [TOOLS['cross'], workdir / 'lowered.spv', '--msl', '--msl-version', '30100', '--entry', 'mr_isoline_compute',
           '--stage', 'comp', '--msl-texture-buffer-native', '--output', msl]
    if rs is not None:
        abifile = workdir / 'root-abi.txt'
        rootabi.write_abi_file(abifile, rs, mapping, False, ADM / 'translator/abi_prelude.metal', ADM / 'translator/abi_vertex_pull.metal')
        cmd[-3:-3] = ['--msl-root-abi', str(abifile)]
    run(cmd, 'spirv-cross')
    raw_text = msl.read_text()
    text, buffer_users = add_root_buffer_params(raw_text)
    (workdir / 'stub-fixed.metal').write_text(text)
    k = text.find('kernel void mr_isoline_compute(')
    if k < 0:
        raise BodyError('kernel entry not found in MSL')
    kernel, head = text[k:], text[:k]
    ua = UNSAFE_ARRAY_RE.search(head)
    unsafe_array = ua.group(0) if ua else ''
    if ua:
        head = head[:ua.start()] + head[ua.end():]
    m = re.search(r'\n(struct \w+\n\{|static inline __attribute__)', head)
    prologue, body_text = (head[:m.start() + 1], head[m.start() + 1:]) if m else (head, '')
    dsb = {}
    for n in (0, 1):
        mm = re.search(r'struct spvDescriptorSetBuffer%d\n\{(.*?)\n\};\n\n?' % n, body_text, re.S)
        if mm:
            dsb[n] = mm.group(0); body_text = body_text.replace(mm.group(0), '')
    ksig = re.search(r'kernel void mr_isoline_compute\((.*?)\)\s*\n\{', kernel, re.S).group(1)   # до «)\n{» — внутри бывают [[buffer(0)]]
    kernel_params = [p.strip() for p in split_call(re.sub(r'\s+', ' ', ksig))] if ksig.strip() else []
    call = re.search(r'\n\s+(main0|\w+)\((.*)\);\n', kernel)
    if not call:
        raise BodyError('orchestrator call not found in kernel')
    call_fn, call_args = call.group(1), split_call(call.group(2))
    locals_ = re.findall(r'\n\s+(threadgroup )?(spvUnsafeArray<[^;=]+>|[\w<>]+) (\w+)(?: = [^;]+)?;', kernel)
    abi = dict(records=meta['records'])
    kinds = {100: 'record', 101: 'count', 102: 'cut'}
    rec_of = {r['variable']: r.get('record_of') for r in meta['records'] if r['storage'] == 100}
    per = {}
    for fn in ('main0', 'mr_gs_emit', 'mr_gs_cut'):
        try:
            per[fn] = tess_reflect.describe(lowered, raw_text, fn, abi)
        except ValueError as e:
            if fn == 'main0':
                raise BodyError(str(e))
            continue   # функции разреза нет у GS без EndPrimitive
        for p in per[fn]:
            if p.get('role') in ('100', '101', '102'):
                p['kind'] = kinds[int(p['role'])]
                if p['kind'] == 'record':
                    p['record_of'] = rec_of[p['id']]
    params = per['main0']
    return dict(stage='GS', workdir=str(workdir), prologue=prologue, unsafe_array=unsafe_array, body=body_text,
                descriptor_set_structs=dsb, kernel_params=kernel_params, call_fn=call_fn, call_args=call_args,
                kernel_locals=locals_, params=per, abi=abi, resources=len(resources),
                root_qwords=rs['qwords'] if rs else 0, root_key=rootabi.root_key(rts0) if (rts0 is not None and resources) else None,
                buffer_users=buffer_users, gs={k: v for k, v in meta.items() if k != 'records'})


if __name__ == '__main__':
    b = body(sys.argv[1], sys.argv[2], require_root=False)
    print(json.dumps(dict(gs=b['gs'], resources=b['resources'], root_key=b['root_key'], call_fn=b['call_fn'],
                          params=[(p['name'], p.get('role'), p.get('kind'), p.get('builtin'), p.get('location'), p['type']) for p in b['params']['main0']]), ensure_ascii=False))
