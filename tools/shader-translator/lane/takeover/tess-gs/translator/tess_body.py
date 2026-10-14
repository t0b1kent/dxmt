"""tess-gs 14.10.2026: тело одной стадии (VS/HS/DS) как набор inner-функций MSL под root-ABI слоя.

Шаги: свой разбор DXIL (dxil-spirv-logical, --mr-logical-bindings) -> lower_isoline (allow_vertex/allow_tessellation,
pack_records) -> host_stub.strip_records -> spirv-val -> abi.map_resources -> spirv-cross (tess-gs) --msl-root-abi
-> текст MSL -> правка: callee, трогающий spvDescriptorSetN, получает его параметром -> отражение (tess_reflect + ABI).
Результат: dict с текстом тела (без пролога и ядра), параметрами функций, материализованными аргументами ресурсов
(как их пишет ядро), объявлениями локалей ядра, корневой раскладкой. Ничего не собирает в библиотеку.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESS = HERE.parent
LANE = TESS.parents[1]            # artifacts/shaders-own-20261008
ADM = TESS.parent / 'admission'
sys.path.insert(0, str(LANE))
sys.path.insert(0, str(ADM / 'translator'))
sys.path.insert(0, str(HERE))
import abi as rootabi            # noqa: E402
from lower_isoline import lower  # noqa: E402
import host_stub                 # noqa: E402
import tess_reflect              # noqa: E402

TOOLS = dict(frontend=ADM / 'build-frontend/dxil-spirv-logical', cross=TESS / 'build-cross/spirv-cross',
             spirv_val=Path('/opt/homebrew/bin/spirv-val'))
PRELUDE = (ADM / 'translator/abi_prelude.metal').read_text()
UNSAFE_ARRAY_RE = re.compile(r'template<typename T, size_t Num>\nstruct spvUnsafeArray\n\{.*?\n\};\n', re.S)
FUNC_RE = re.compile(r'static inline __attribute__\(\(always_inline\)\)\s*\n?\s*void (\w+)\(([^)]*)\)\s*\n\{(.*?)\n\}', re.S)


class BodyError(Exception):
    pass


def run(argv, what):
    r = subprocess.run([str(x) for x in argv], capture_output=True, text=True)
    if r.returncode:
        raise BodyError('%s: rc=%d %s' % (what, r.returncode, (r.stderr or r.stdout).strip()[:400]))
    return r


def add_root_buffer_params(text):
    """Callee, читающий spvDescriptorSetN (или зовущий такой), получает `constant spvDescriptorSetBufferN&` параметром."""
    funcs = {m.group(1): dict(params=m.group(2), body=m.group(3)) for m in FUNC_RE.finditer(text)}
    need = {}
    for n in (0, 1):
        users = set(fn for fn, f in funcs.items() if 'spvDescriptorSet%d' % n in f['body'])
        changed = True
        while changed:
            changed = False
            for fn, f in funcs.items():
                if fn not in users and any(re.search(r'\b' + u + r'\(', f['body']) for u in users):
                    users.add(fn)
                    changed = True
        need[n] = users
    out = text
    for fn in funcs:
        extra = ['constant spvDescriptorSetBuffer%d& spvDescriptorSet%d' % (n, n) for n in (0, 1) if fn in need[n]]
        if not extra:
            continue
        params = funcs[fn]['params'].strip()
        newparams = ', '.join(extra + ([params] if params else []))
        args = ', '.join('spvDescriptorSet%d' % n for n in (0, 1) if fn in need[n])
        # вызовы (не определение: перед именем нет 'void ')
        out = re.sub(r'(?<!void )(?<![\w])' + fn + r'\(', lambda m: fn + '(' + args + ', ', out)
        out = out.replace(fn + '(' + args + ', )', fn + '(' + args + ')')
        # определение
        out = re.sub(r'(void ' + fn + r')\(([^)]*)\)', lambda m: m.group(1) + '(' + newparams + ')', out, count=1)
    return out, {n: sorted(u) for n, u in need.items()}


def split_call(expr):
    """Аргументы вызова по запятым верхнего уровня."""
    out, depth, cur = [], 0, ''
    for ch in expr:
        if ch in '(<[': depth += 1
        if ch in ')>]': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def body(container, stage, workdir, rts0=None, entry_kind=None):
    """container: путь к контейнеру DXIL; stage: 'VS'|'HS'|'DS'; workdir: каталог для промежуточных файлов."""
    workdir = Path(workdir); workdir.mkdir(parents=True, exist_ok=True)
    data = Path(container).read_bytes()
    spv, lmap = workdir / 'dxil.spv', workdir / 'logical.json'
    run([TOOLS['frontend'], container, '--ssbo-uav', '--ssbo-srv', '--mr-logical-bindings', lmap, '--output', spv], 'frontend')
    resources = json.loads(lmap.read_text())['bindings']
    rts0 = rts0 if rts0 is not None else rootabi.dxbc_chunks(data).get('RTS0')
    if rts0 is None and resources:
        raise BodyError('shader reads resources but has no root signature')
    rs = rootabi.root_layout(rootabi.parse_rts0(rts0)) if rts0 is not None else None
    mapping, missing = rootabi.map_resources(rs, stage, resources) if rs else ({}, [])
    if missing:
        raise BodyError('resource is not in the root signature: ' + ', '.join(
            '%s space %d register %d' % (rootabi.CLASS_NAME[m['class']], m['space'], m['register']) for m in missing[:4]))
    for r in resources:
        if r.get('view'):
            mapping[r['binding']] = dict(mapping[r['binding']], counter=1)
    abifile = workdir / 'root-abi.txt'
    rootabi.write_abi_file(abifile, rs, mapping, False, ADM / 'translator/abi_prelude.metal',
                           ADM / 'translator/abi_vertex_pull.metal')
    try:
        lowered, abi = lower(spv.read_bytes(), allow_vertex=(stage == 'VS'), allow_tessellation=(stage in ('HS', 'DS')),
                             pack_records=True)
    except Exception as e:
        if stage == 'VS' and 'no stage interface' in str(e) and not resources:
            # VS без входов и выходов (например, control_point_phase_vs из тестов vkd3d): телу нечего делать —
            # object-функция обходится без него.
            return dict(stage=stage, workdir=str(workdir), prologue='', unsafe_array='', body='', descriptor_set_structs={},
                        kernel_params=[], call_fn=None, call_args=[], kernel_locals=[], params={'main0': []}, abi=None,
                        resources=0, root_qwords=rs['qwords'] if rs else 0, root_key=None, buffer_users={0: [], 1: []},
                        records_stripped=None, empty=True)
        raise BodyError('lower(): %s: %s' % (type(e).__name__, str(e)[:200]))
    if abi is None:
        raise BodyError('lower() declined the shader (stage %s)' % stage)
    (workdir / 'lowered.spv').write_bytes(lowered)
    (workdir / 'ABI.json').write_text(json.dumps(abi))
    stub, info = host_stub.strip_records(lowered)
    (workdir / 'stub.spv').write_bytes(stub)
    run([TOOLS['spirv_val'], workdir / 'stub.spv'], 'spirv-val')
    msl = workdir / 'stub.metal'
    run([TOOLS['cross'], workdir / 'stub.spv', '--msl', '--msl-version', '30100', '--entry', 'mr_isoline_compute',
         '--stage', 'comp', '--msl-root-abi', abifile, '--msl-texture-buffer-native', '--output', msl], 'spirv-cross')
    raw_text = msl.read_text()
    text, buffer_users = add_root_buffer_params(raw_text)
    (workdir / 'stub-fixed.metal').write_text(text)
    # разрезка: пролог (до первого struct/static inline после прелюдии), тело (типы + функции), ядро
    k = text.find('kernel void mr_isoline_compute(')
    if k < 0:
        raise BodyError('kernel entry not found in MSL')
    kernel = text[k:]
    head = text[:k]
    # шаблон spvUnsafeArray стоит на границе пролога и тела — вырезаем отдельно, чтобы не разрезать его пополам
    ua = UNSAFE_ARRAY_RE.search(head)
    unsafe_array = ua.group(0) if ua else ''
    if ua:
        head = head[:ua.start()] + head[ua.end():]
    # пролог: #pragma/#include/using namespace/прелюдия/spvUnsafeArray — всё до первого 'struct ' или 'static inline' тела
    m = re.search(r'\n(struct \w+\n\{|static inline __attribute__)', head)
    prologue, body_text = (head[:m.start() + 1], head[m.start() + 1:]) if m else (head, '')
    # spvDescriptorSetBufferN — объявлен в теле (если стадия читает корень); вырезаем отдельно
    dsb = {}
    for n in (0, 1):
        mm = re.search(r'struct spvDescriptorSetBuffer%d\n\{(.*?)\n\};\n\n?' % n, body_text, re.S)
        if mm:
            dsb[n] = mm.group(0)
            body_text = body_text.replace(mm.group(0), '')
    # сигнатура ядра и вызов оркестратора в ядре
    ksig = re.search(r'kernel void mr_isoline_compute\(([^)]*)\)', kernel, re.S).group(1)
    kernel_params = [p.strip() for p in split_call(re.sub(r'\s+', ' ', ksig))] if ksig.strip() else []
    call = re.search(r'\n\s+(main0|\w+)\((.*)\);\n', kernel)
    if not call:
        raise BodyError('orchestrator call not found in kernel')
    call_fn, call_args = call.group(1), split_call(call.group(2))
    locals_ = re.findall(r'\n\s+(threadgroup )?(spvUnsafeArray<[^;=]+>|[\w<>]+) (\w+);', kernel)
    # отражение
    per = {}
    for fn in ('main0', 'hull_main', 'patch_main'):
        try:
            per[fn] = tess_reflect.describe(stub, raw_text, fn, abi)   # по тексту ДО вставки параметра буфера корня
        except ValueError as e:
            if fn == 'main0':
                raise BodyError(str(e))
    return dict(stage=stage, workdir=str(workdir), prologue=prologue, unsafe_array=unsafe_array, body=body_text, descriptor_set_structs=dsb,
                kernel_params=kernel_params, call_fn=call_fn, call_args=call_args, kernel_locals=locals_,
                params=per, abi=abi, resources=len(resources), root_qwords=rs['qwords'] if rs else 0,
                root_key=rootabi.root_key(rts0) if (rts0 is not None and resources) else None,
                buffer_users=buffer_users, records_stripped=info)


if __name__ == '__main__':
    container, stage, workdir = sys.argv[1:4]
    b = body(container, stage, workdir)
    print(json.dumps(dict(stage=b['stage'], resources=b['resources'], root_qwords=b['root_qwords'], root_key=b['root_key'],
                          kernel_params=b['kernel_params'], call_fn=b['call_fn'], call_args_n=len(b['call_args']),
                          call_args_head=[a[:70] for a in b['call_args'][:4]], buffer_users=b['buffer_users'],
                          dsb=sorted(b['descriptor_set_structs']), locals=len(b['kernel_locals']),
                          funcs={k: len(v) for k, v in b['params'].items()}, body_lines=b['body'].count('\n'),
                          prologue_lines=b['prologue'].count('\n')), ensure_ascii=False))
