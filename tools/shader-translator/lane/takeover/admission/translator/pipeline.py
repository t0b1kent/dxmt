#!/usr/bin/env python3
"""Единый прогон своего конвейера DXIL -> metallib (takeover 14.10.2026) + режим --abi root (веха ресурсов).

Веха допуска (admission): поля описателя +8 и +16 (своя копия spirv-cross читает их сама), счётчик UAV
(ресурс разбора с view=1 — счётчик того же описателя), варианты пиксельной функции item['variant']:
  sm     маска выборок конвейера — константа функции 128 (выход покрытия есть всегда)
  ds     двухисточниковое смешивание (spv_variant.dual_source)
--abi root: MSL следует соглашению прежнего пути слоя D3D12 (resources/ABI.md):
  разбор   dxil-spirv-logical --mr-logical-bindings (свой разбор лейна: у каждого ресурса свой номер и
           запись «класс, пространство, регистр»), ключи --ssbo-uav --ssbo-srv
  корень   часть RTS0 контейнера (или --rootsig файл) -> abi.map_resources -> описание для spirv-cross
  проходы  bindings.remap не нужен (номера уже разные); pack_tess_io.pack — как раньше
  Cross    своя копия resources/spirv-cross: --msl-root-abi <описание> --msl-texture-buffer-native
  Metal и библиотека — как раньше.
Без --abi конвейер работает как в исходном файле.

Сам конвейер НЕ меняется, только собран в одну команду:
  разбор   dxil-spirv (двоичный соседа, ключи --ssbo-uav --ssbo-srv, как в measure.py/collect_oracle.py)
  проходы  bindings.remap; затем либо isoline (lower_isoline.lower -> spirv-val x2 -> msl-writer),
           либо pack_tess_io.pack (+ spirv-val x2, если что-то упаковано) -> spirv-cross --msl 30100
  Metal    metal -c -std=metal3.1
  библ.    metallib
Проходы лейна импортируются с места, байт-код не пишется, файлы лейна не трогаются.
Игра, Wine, видеочип не запускаются. Данные шейдеров никуда не уходят.

Принят = собралась библиотека (metallib). Отдельно хранится «Metal принял» (счёт лейна был по нему).
"""
import sys
sys.dont_write_bytecode = True
import argparse, collections, concurrent.futures, datetime, hashlib, json, os, re
import struct, subprocess, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent            # .../takeover-20261014/resources/translator
RES = HERE.parent
TAKEOVER = RES.parent
LANE = TAKEOVER.parent
ROOT = LANE.parents[1]
OPEN = ROOT / 'artifacts/shaders-open-20261007'
CAPTURE = Path(os.environ.get('TRANSLATOR_CAPTURE_DIR', '/nonexistent'))     # corpus mode only; not part of this repository
sys.path.insert(0, str(LANE))
sys.path.insert(0, str(HERE))
import abi as rootabi                 # noqa: E402
import spv_variant                    # noqa: E402
from bindings import remap            # noqa: E402
from pack_tess_io import pack         # noqa: E402
from lower_isoline import lower       # noqa: E402

SIX = ('VS', 'PS', 'CS', 'HS', 'DS', 'GS')
CROSS_STAGE = dict(VS='vert', PS='frag', CS='comp', HS='tesc', DS='tese', GS='geom')
KIND_STAGE = {0: 'PS', 1: 'VS', 2: 'GS', 3: 'HS', 4: 'DS', 5: 'CS', 6: 'LIB'}
STEP_TIMEOUT = 120          # у лейна 30 с; строки дольше 30 с помечаются slow30
LANE_TIMEOUT_NS = 30 * 10**9


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def find_tool(name):
    return Path(subprocess.check_output(['/usr/bin/xcrun', '--find', name], text=True).strip())


ABI = None                            # 'root' в режиме --abi root
FRONTEND = None                       # свой разбор DXIL (--frontend); иначе двоичный лейна build-native/dxil-spirv-logical
ROOTSIG = None                        # байты RTS0 из --rootsig (иначе берётся часть RTS0 контейнера)


def tools():
    t = dict(frontend=Path(FRONTEND) if (ABI and FRONTEND) else
             (LANE / 'build-native/dxil-spirv-logical') if ABI else (OPEN / 'dxil-native/build/dxil-spirv'),
             cross=(RES / 'build-cross/spirv-cross') if ABI else (LANE / 'build-native/spirv-cross'),
             writer=LANE / 'build-native/msl-writer',
             spirv_val=Path('/opt/homebrew/bin/spirv-val'),
             metal=find_tool('metal'), metallib=find_tool('metallib'))
    for k, p in t.items():
        if not p.is_file():
            raise RuntimeError('tool missing: %s %s' % (k, p))
    return t


def pins(t):
    out = {k: dict(path=str(p), sha256=sha256(p.read_bytes())) for k, p in t.items()}
    for n in ('bindings.py', 'pack_tess_io.py', 'lower_isoline.py', 'compile_sample.py', 'check_isoline.py'):
        if (LANE / n).is_file():        # compile_sample.py / check_isoline.py belong to the corpus tools, not shipped
            out['lane/' + n] = dict(sha256=sha256((LANE / n).read_bytes()))
    out['runner'] = dict(sha256=sha256(Path(__file__).read_bytes()))
    if ABI:
        for n in ('abi.py', 'abi_prelude.metal', 'abi_vertex_pull.metal', 'spv_variant.py'):
            out['translator/' + n] = dict(sha256=sha256((HERE / n).read_bytes()))
        out['abi'] = ABI
    def ver(argv):
        p = subprocess.run(argv, capture_output=True, text=True)
        return (p.stdout + p.stderr).strip().splitlines()[:3]
    out['metal_version'] = ver(['/usr/bin/xcrun', 'metal', '--version'])
    out['xcodebuild_version'] = ver(['/usr/bin/xcodebuild', '-version'])
    out['macos'] = ver(['/usr/bin/sw_vers'])
    return out


# ---------- входы ----------
def dxbc_chunks(b):
    if b[:4] != b'DXBC':
        raise ValueError('not a DXBC container')
    count = struct.unpack_from('<I', b, 28)[0]
    out = {}
    for off in struct.unpack_from('<%dI' % count, b, 32):
        size = struct.unpack_from('<I', b, off + 4)[0]
        out[b[off:off + 4].decode('latin1')] = b[off + 8:off + 8 + size]
    return out


def load_corpus(sample):
    rows = json.loads((OPEN / 'inputs.json').read_text())
    want = set(json.loads((OPEN / 'sample-sha1.json').read_text())) if sample else None
    out = []
    for r in rows:
        if r['stage'] not in SIX or r['source_status'] != 'PRESENT_VERIFIED':
            continue
        if want is not None and r['sha1'] not in want:
            continue
        out.append(dict(sha1=r['sha1'], stage=r['stage'], source=r['source'], check=('sha256', r['sha256'])))
    out.sort(key=lambda r: r['sha1'])
    return out, []


def load_capture():
    ledger = next(CAPTURE.glob('capture-*.jsonl'))
    seen = {}
    with ledger.open() as f:
        for line in f:
            if '"shader.end"' not in line:
                continue
            x = json.loads(line)
            if x.get('event') == 'shader.end' and x.get('sha1') and x['file'] not in seen:
                seen[x['file']] = (x['sha1'], x['stage'])
    dxil, other = [], []
    for name, (h, stage) in sorted(seen.items()):
        b = (CAPTURE / name).read_bytes()
        if hashlib.sha1(b).hexdigest() != h:
            raise RuntimeError('capture drift ' + name)
        c = dxbc_chunks(b)
        if 'DXIL' in c:
            token = struct.unpack_from('<I', c['DXIL'])[0]
            assert KIND_STAGE[token >> 16] == stage
            dxil.append(dict(sha1=h, stage=stage, source=str(CAPTURE / name), check=('sha1', h),
                             model='%d.%d' % ((token >> 4) & 15, token & 15)))
        else:
            code = c.get('SHEX', c.get('SHDR'))
            token = struct.unpack_from('<I', code)[0]
            other.append(dict(sha1=h, stage=stage, file=name, bytes=len(b), chunks=sorted(c),
                              code_chunk='SHEX' if 'SHEX' in c else 'SHDR',
                              model='%d.%d' % ((token >> 4) & 15, token & 15)))
    dxil.sort(key=lambda r: r['sha1'])
    return dxil, other


# ---------- классы отказов (без исходных имён и строк шейдера) ----------
def norm(msg):
    msg = re.sub(r'(?<![\w<])/[^\s:\'"]+', '<path>', msg)
    msg = re.sub(r'"[^"]*"|\x27[^\x27]*\x27', '<id>', msg)
    msg = re.sub(r'\b0x[0-9a-fA-F]+\b|\b\d+(?:\.\d+)?\b', '<n>', msg)
    return re.sub(r'\s+', ' ', msg).strip()[:170]


def classes(text, rc, status):
    if status == 'TIMEOUT':
        return ['TIMEOUT']
    out = []
    for line in text.splitlines():
        m = re.search(r'\b(?:fatal )?error:\s*(.*)', line, re.I)
        if m:
            c = 'error: ' + norm(m.group(1))
            if c not in out:
                out.append(c)
    if not out:
        for line in text.splitlines():
            if line.strip():
                out.append(norm(line))
                break
    if not out:
        if rc is not None and rc < 0:
            out.append('SIGNAL_%d' % -rc)
        elif rc:
            out.append('RC_%d_NO_TEXT' % rc)
        else:
            out.append('NO_OUTPUT_FILE')
    return out[:8]


# ---------- SPIR-V мелочи ----------
def spirv_local_size(data):
    """Размер группы потоков вычислительной стадии (OpExecutionMode LocalSize) или None."""
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    i = 5
    while i < len(w):
        n = w[i] >> 16
        op = w[i] & 65535
        if op == 16 and n >= 6 and w[i + 2] == 17:
            return list(w[i + 3:i + 6])
        if op == 54 or not n:
            break
        i += n
    return None


def spirv_entry_and_isoline(data):
    """Имя точки входа (как compile_sample.entry) и признак isoline HS/DS —
    то же условие, по которому lower_isoline.lower берёт шейдер без allow_tessellation."""
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    i = 5
    name = None
    entries = []
    modes = []
    while i < len(w):
        n = w[i] >> 16
        op = w[i] & 65535
        if not n or i + n > len(w):
            raise ValueError('SPIRV bounds')
        if op == 15:
            entries.append((w[i + 1], w[i + 2]))
            if name is None:
                name = struct.pack('<%dI' % (n - 3), *w[i + 3:i + n]).split(b'\0', 1)[0].decode()
        elif op == 16:
            modes.append((w[i + 1], w[i + 2]))
        elif op == 54:
            break
        i += n
    if name is None:
        raise ValueError('entry missing')
    iso = len(entries) == 1 and entries[0][0] in (1, 2) and (entries[0][1], 25) in modes
    return name, iso


class Runner:
    def __init__(self, out, t, keep_cap, keep_per_class):
        self.out = out
        self.t = t
        self.local = threading.local()
        self.lock = threading.Lock()
        self.wcount = 0
        self.keep_cap = keep_cap
        self.keep_per_class = keep_per_class
        self.kept_bytes = 0
        self.kept_class = collections.Counter()
        fdir = out / 'failures'
        if fdir.is_dir():
            self.kept_bytes = sum(p.stat().st_size for p in fdir.rglob('*') if p.is_file())

    def wdir(self):
        d = getattr(self.local, 'dir', None)
        if d is None:
            with self.lock:
                self.wcount += 1
                d = self.out / 'work' / ('w%d' % self.wcount)
            d.mkdir(parents=True, exist_ok=True)
            self.local.dir = d
        return d

    def step(self, row, name, argv, output, fresh=True):
        if fresh:
            try:
                output.unlink()
            except FileNotFoundError:
                pass
        t0 = time.perf_counter_ns()
        try:
            p = subprocess.run([str(x) for x in argv], stdin=subprocess.DEVNULL, capture_output=True,
                               timeout=STEP_TIMEOUT, cwd=str(self.wdir()))
            rc, so, se = p.returncode, p.stdout, p.stderr
            status = 'OK' if rc == 0 and output.is_file() else 'FAILED'
        except subprocess.TimeoutExpired as e:
            rc, so, se, status = None, e.stdout or b'', e.stderr or b'', 'TIMEOUT'
        ns = time.perf_counter_ns() - t0
        row['t'][name] = row['t'].get(name, 0) + ns
        if ns > LANE_TIMEOUT_NS:
            row['slow30'] = True
        text = (se + so).decode('utf-8', 'replace')
        if name == 'metal' and status == 'OK':
            row['warnings'] = text.count('warning:')
        if status != 'OK':
            row['fail_step'] = name
            cl = classes(text, rc, status)
            row['fail_class'] = cl[0]
            if len(cl) > 1:
                row['fail_classes'] = cl
            row['_stderr'] = se + (b'\n--stdout--\n' + so if so else b'')
            return False
        return True

    def translate(self, item):
        try:
            return self._translate(item)
        except Exception as e:  # сбой обвязки виден в сводке отдельным классом, прогон не падает
            return dict(sha1=item['sha1'], stage=item['stage'], accepted=False, metal_ok=False, route='?', t={},
                        fail_step='runner', fail_class='RUNNER_EXCEPTION ' + type(e).__name__ + ': ' + norm(str(e)))

    def _translate(self, item):
        d = self.wdir()
        row = dict(sha1=item['sha1'], stage=item['stage'], accepted=False, metal_ok=False, route='cross', t={})
        variant = [x for x in (item.get('variant') or '').split('.') if x]
        if variant and (item['stage'] != 'PS' or set(variant) - {'sm', 'ds'}):
            row.update(fail_step='input', fail_class='UNKNOWN_VARIANT')
            return row
        row['variant'] = '.'.join(variant)
        src = Path(item['source'])
        data = src.read_bytes()
        kind, want = item['check']
        got = hashlib.sha1(data).hexdigest() if kind == 'sha1' else sha256(data)
        if got != want:
            row.update(fail_step='input', fail_class='SOURCE_DRIFT')
            return row
        row['dxil_bytes'] = len(data)
        spv = d / 'dxil.spv'
        passed = d / 'passes.spv'
        msl = d / 'shader.metal'
        air = d / 'shader.air'
        lib = d / 'shader.metallib'
        for p in (passed, msl, air, lib):
            try:
                p.unlink()
            except FileNotFoundError:
                pass
        lmap = d / 'logical.json'
        abifile = d / 'root-abi.txt'
        if ABI:
            try:
                lmap.unlink()
            except FileNotFoundError:
                pass
            ok = self.step(row, 'frontend', [self.t['frontend'], src, '--ssbo-uav', '--ssbo-srv',
                                             '--mr-logical-bindings', lmap, '--output', spv], spv)
        else:
            ok = self.step(row, 'frontend', [self.t['frontend'], src, '--ssbo-uav', '--ssbo-srv', '--output', spv], spv)
        if ok and ABI:
            # Раскладка привязок по корневой подписи: правила airconv, см. abi.map_resources.
            t0 = time.perf_counter_ns()
            try:
                resources = json.loads(lmap.read_text())['bindings']
                rts0 = ROOTSIG if ROOTSIG is not None else rootabi.dxbc_chunks(data).get('RTS0')
                if rts0 is None and resources:
                    raise rootabi.AbiError('shader reads resources but no root signature was given')
                rs = rootabi.root_layout(rootabi.parse_rts0(rts0)) if rts0 is not None else None
                mapping_abi, missing = rootabi.map_resources(rs, item['stage'], resources) if rs else ({}, [])
                if missing:
                    raise rootabi.AbiError('resource is not in the root signature: ' + ', '.join(
                        '%s space %d register %d' % (rootabi.CLASS_NAME[m['class']], m['space'], m['register'])
                        for m in missing[:4]))
                # Счётчик UAV — отдельный ресурс разбора (view=1) с тем же регистром: тот же описатель, поле +16.
                for r in resources:
                    if r.get('view'):
                        if mapping_abi[r['binding']]['where'] != rootabi.WHERE_TABLE:
                            raise rootabi.AbiError('UAV counter outside a descriptor table')
                        mapping_abi[r['binding']] = dict(mapping_abi[r['binding']], counter=1)
                rootabi.write_abi_file(abifile, rs, mapping_abi, True, HERE / 'abi_prelude.metal',
                                       HERE / 'abi_vertex_pull.metal', sample_mask_constant='sm' in variant)
                if item['stage'] == 'PS':
                    row['coverage_output'], row['colors'] = rootabi.pixel_outputs(data)
                row['resources'] = len(resources)
                row['root_qwords'] = rs['qwords'] if rs else 0
                row['root_key'] = rootabi.root_key(rts0) if (rts0 is not None and resources) else None
                row['where'] = collections.Counter(str(m['where']) for m in mapping_abi.values())
            except Exception as e:
                row['fail_step'] = 'rootsig'
                row['fail_class'] = type(e).__name__ + ': ' + norm(str(e))
                row['_stderr'] = repr(e).encode()
                ok = False
            row['t']['rootsig'] = time.perf_counter_ns() - t0
        if ok:
            raw = spv.read_bytes()
            row['spv_sha256'] = sha256(raw)
            row['spv_bytes'] = len(raw)
            row['local_size'] = spirv_local_size(raw)
            t0 = time.perf_counter_ns()
            try:
                entry, iso = spirv_entry_and_isoline(raw)
                if ABI:
                    norm_spv, mapping = raw, []
                    if iso:
                        raise rootabi.AbiError('isoline route is not served in --abi root')
                else:
                    norm_spv, mapping = remap(raw)
                row['bindings_changed'] = sum(x['logical_binding'] != x['physical_binding'] for x in mapping)
                if iso:
                    row['route'] = 'isoline'
                    out_spv, abi = lower(norm_spv)
                    if not abi:
                        raise AssertionError('isoline detected but lower returned no ABI')
                    twice, again = lower(out_spv)
                    if twice != out_spv or again is not None:
                        raise AssertionError('isoline lowering not idempotent')
                    packed_n = 0
                else:
                    out_spv, interface = pack(norm_spv)
                    packed_n = len(interface)
                    if 'ds' in variant:
                        out_spv, row['dual_source_outputs'] = spv_variant.dual_source(out_spv)
                row['packed'] = packed_n
            except Exception as e:  # отказ своего прохода — это отказ шейдера, а не прогона
                row['t']['passes'] = time.perf_counter_ns() - t0
                row['fail_step'] = 'passes'
                row['fail_class'] = type(e).__name__ + ': ' + norm(str(e))
                row['_stderr'] = repr(e).encode()
                ok = False
            else:
                row['t']['passes'] = time.perf_counter_ns() - t0
                passed.write_bytes(out_spv)
                row['passes_sha256'] = sha256(out_spv)
        if ok and (row['route'] == 'isoline' or row['packed']):
            # как у лейна: проверка исходного и преобразованного SPIR-V; у isoline обе обязательны
            v1 = self.step(row, 'validate', [self.t['spirv_val'], spv], spv, fresh=False)
            if not v1 and row['route'] != 'isoline':
                row['original_spirv_invalid'] = row.pop('fail_class')
                row.pop('fail_step', None); row.pop('fail_classes', None); row.pop('_stderr', None)
                v1 = True
            ok = v1 and self.step(row, 'validate', [self.t['spirv_val'], passed], passed, fresh=False)
        if ok:
            if row['route'] == 'isoline':
                ok = self.step(row, 'cross', [self.t['writer'], passed, msl, d / 'slots.json'], msl)
            else:
                ok = self.step(row, 'cross', [self.t['cross'], passed, '--msl', '--msl-version', '30100',
                                              '--entry', entry, '--stage', CROSS_STAGE[item['stage']]] +
                               (['--msl-root-abi', abifile, '--msl-texture-buffer-native'] if ABI else []) +
                               ['--output', msl], msl)
        if ok:
            m = msl.read_bytes()
            row['msl_sha256'] = sha256(m)
            row['msl_bytes'] = len(m)
            if ABI:
                text = m.decode('utf-8', 'replace')
                row['vertex_inputs'] = sorted(int(x) for x in re.findall(r'constant uint spvVin(\d+)_entry ', text))
                row['metal_slots'] = sorted(set(re.findall(r'\[\[((?:buffer|texture|sampler)\(\d+\))\]\]', text)))
            ok = self.step(row, 'metal', [self.t['metal'], '-c', '-std=metal3.1', msl, '-o', air], air)
        if ok:
            row['metal_ok'] = True
            ok = self.step(row, 'link', [self.t['metallib'], air, '-o', lib], lib)
        if ok:
            row['accepted'] = True
            row['lib_bytes'] = lib.stat().st_size
        else:
            self.keep(row, d)
        row.pop('_stderr', None)
        return row

    def keep(self, row, d):
        """По отказам: текст ошибки всем (до 16 КБ); SPIR-V после проходов и MSL — первым N каждого класса.
        Общий потолок keep_cap байт на прогон."""
        key = (row.get('fail_step'), row.get('fail_class'))
        err = (row.get('_stderr') or b'')[:16384]
        with self.lock:
            full = self.kept_class[key] < self.keep_per_class
            self.kept_class[key] += 1
            files = [('stderr.txt', err)]
            if full:
                for n in ('passes.spv', 'shader.metal'):
                    p = d / n
                    if p.is_file() and p.stat().st_size <= 4 * 1024 * 1024:
                        files.append((n, p.read_bytes()))
            size = sum(len(b) for _, b in files)
            if self.kept_bytes + size > self.keep_cap:
                row['kept'] = 'none_cap'
                return
            self.kept_bytes += size
        fd = self.out / 'failures' / row['sha1']
        fd.mkdir(parents=True, exist_ok=True)
        for n, b in files:
            (fd / n).write_bytes(b)
        row['kept'] = 'full' if full else 'stderr'


def gate(out, state):
    """Перед каждой порцией: builds-ok; код 1 — ждать 30 с и повторить. STOP проверяется и в ожидании."""
    waited = 0.0
    while True:
        if (out / 'STOP').exists():
            return None
        if not (ROOT / 'scripts/wine-slot.sh').is_file():     # standalone tool: no project build gate
            return waited
        r = subprocess.run(['bash', str(ROOT / 'scripts/wine-slot.sh'), 'builds-ok'], cwd=str(ROOT),
                           capture_output=True)
        if r.returncode == 0:
            return waited
        state('WAITING_GATE', r.stderr.decode('utf-8', 'replace').strip()[:200])
        time.sleep(30)
        waited += 30


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--set', required=True, choices=['sample533', 'capture093', 'full46000'])
    ap.add_argument('--out', required=True)
    ap.add_argument('--workers', type=int, default=1)
    ap.add_argument('--portion', type=int, default=64)
    ap.add_argument('--limit', type=int)
    ap.add_argument('--keep-cap-mb', type=int, default=1900)
    ap.add_argument('--keep-per-class', type=int, default=20)
    ap.add_argument('--abi', choices=['root'], help='MSL по соглашению прежнего пути слоя (resources/ABI.md)')
    ap.add_argument('--rootsig', help='файл с корневой подписью (RTS0 или контейнер); иначе часть RTS0 шейдера')
    ap.add_argument('--frontend', help='свой двоичный разбора DXIL вместо двоичного лейна (только с --abi root)')
    a = ap.parse_args()
    global ABI, ROOTSIG, FRONTEND
    ABI = a.abi
    FRONTEND = a.frontend
    if a.rootsig:
        ROOTSIG = rootabi.rts0_of(Path(a.rootsig).read_bytes())
    if not 1 <= a.workers <= 4:
        raise SystemExit('workers 1..4')
    cur = os.getpriority(os.PRIO_PROCESS, 0)
    if cur < 20:
        os.nice(20 - cur)
    out = (RES / a.out).resolve()
    if RES not in out.parents:
        raise SystemExit('out must be inside the resources dir')
    out.mkdir(parents=True, exist_ok=True)
    t = tools()
    if a.set == 'capture093':
        items, other = load_capture()
        (out / 'NON-DXIL.json').write_text(json.dumps(other, indent=1) + '\n')
    else:
        items, other = load_corpus(a.set == 'sample533')
    if a.limit:
        items = items[:a.limit]
    total = len(items)
    results = out / 'results.jsonl'
    done = {}
    if results.exists():
        with results.open() as f:
            for line in f:
                line = line.strip()
                if line:
                    r = json.loads(line)
                    done[r['sha1']] = r
    runner = Runner(out, t, a.keep_cap_mb * 1024 * 1024, a.keep_per_class)
    for r in done.values():
        if not r['accepted']:
            runner.kept_class[(r.get('fail_step'), r.get('fail_class'))] += 1
    todo = [x for x in items if x['sha1'] not in done]
    pin = pins(t)
    pin.update(set=a.set, inputs=total, workers=a.workers, portion=a.portion, nice=os.getpriority(os.PRIO_PROCESS, 0),
               started_local=datetime.datetime.now().isoformat(timespec='seconds'), resumed_done=len(done),
               load_at_start=os.getloadavg(), step_timeout_s=STEP_TIMEOUT, argv=sys.argv)
    with (out / 'PINS.jsonl').open('a') as f:
        f.write(json.dumps(pin, ensure_ascii=False) + '\n')
    acc = sum(r['accepted'] for r in done.values())
    stage_tot = collections.Counter(x['stage'] for x in items)
    stage_done = collections.Counter(r['stage'] for r in done.values())
    stage_acc = collections.Counter(r['stage'] for r in done.values() if r['accepted'])
    session = dict(done=0, active_s=0.0, gate_wait_s=0.0, begin=time.time())

    def state(status, note=''):
        n = len(done)
        rate = session['done'] / session['active_s'] if session['active_s'] > 0 else None
        left = total - n
        eta = None
        if rate and left:
            eta = (datetime.datetime.now() + datetime.timedelta(seconds=left / rate)).isoformat(timespec='seconds')
        p = dict(status=status, note=note, set=a.set, total=total, done=n, accepted=acc, failed=n - acc,
                 by_stage={s: dict(total=stage_tot[s], done=stage_done[s], accepted=stage_acc[s]) for s in SIX},
                 workers=a.workers, rate_per_s=rate, eta_local_if_no_waits=eta,
                 session_done=session['done'], session_active_s=round(session['active_s'], 1),
                 session_gate_wait_s=session['gate_wait_s'], failures_kept_bytes=runner.kept_bytes,
                 updated_local=datetime.datetime.now().isoformat(timespec='seconds'), pid=os.getpid(),
                 load=os.getloadavg())
        tmp = out / 'progress.json.tmp'
        tmp.write_text(json.dumps(p, ensure_ascii=False, indent=1) + '\n')
        tmp.replace(out / 'progress.json')

    state('STARTING')
    final = 'COMPLETE'
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as ex:
        for i in range(0, len(todo), a.portion):
            if (out / 'STOP').exists():
                final = 'STOPPED_BY_FILE'
                break
            w = gate(out, state)
            if w is None:
                final = 'STOPPED_BY_FILE'
                break
            session['gate_wait_s'] += w
            batch = todo[i:i + a.portion]
            t0 = time.monotonic()
            rows = list(ex.map(runner.translate, batch))
            session['active_s'] += time.monotonic() - t0
            with results.open('a') as f:
                for r in rows:
                    f.write(json.dumps(r, ensure_ascii=False) + '\n')
                f.flush()
                os.fsync(f.fileno())
            for r in rows:
                done[r['sha1']] = r
                stage_done[r['stage']] += 1
                if r['accepted']:
                    acc += 1
                    stage_acc[r['stage']] += 1
            session['done'] += len(rows)
            state('RUNNING')
    session['wall_s'] = time.time() - session['begin']
    session['load_at_end'] = os.getloadavg()
    state(final)
    with (out / 'SESSIONS.jsonl').open('a') as f:
        f.write(json.dumps(dict(final=final, workers=a.workers, **session), ensure_ascii=False) + '\n')
    print(json.dumps(dict(status=final, set=a.set, total=total, done=len(done), accepted=acc,
                          session_done=session['done'], active_s=round(session['active_s'], 2),
                          gate_wait_s=session['gate_wait_s'])))


if __name__ == '__main__':
    main()
