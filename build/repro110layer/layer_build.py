#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
"""Облачная (и локальная) сборка слоя Direct3D 12 (d3d12.dll, x86_64 PE) из дерева репозитория.

Шаги идут по одному, каждый пишет отчёт в <work>/reports/<шаг>-<режим>.json и возвращает 0 только при
выполнении своего критерия, поэтому матрица заданий показывает ВСЕ отказавшие шаги за один запуск.

  fetch      llvm-mingw и заголовки DirectX по закреплению из inputs.lock.json (проверка digest/ревизии)
  tree       подготовка дерева сборки.  faithful: копия с СНЯТЫМИ строками заметок об авторстве, каждый
             файл описи ER-V6-SOURCE.json сверяется по sha256 с деревом T0, из которого собран рабочий
             файл; git describe фиксируется как 266e503.  plain: дерево как есть.
  configure  meson setup (заглушки Wine/LLVM, см. inputs.lock.json: stand_ins)
  units      18 объектов слоя собираются отдельно; sha256 каждого против ожидаемых (faithful: все обязаны
             совпасть; plain: отчёт о расхождениях)
  build      цель src/d3d12/d3d12.dll целиком; sha256 после установки меток времени рабочего файла
  all        fetch, tree, configure, units, build

Режим — только faithful или plain. Источник (--source) — дерево репозитория, не рабочая копия разработчика.
Пути и версии инструментов берутся из inputs.lock.json; ничего не качается, кроме того, что там перечислено.
Переменные LAYER_LLVM_MINGW_DIR и LAYER_DIRECTX_DIR подставляют готовые каталоги вместо скачивания
(локальные проверки; в облаке не задаются).
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, tarfile, time, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOCK = json.loads((HERE / 'inputs.lock.json').read_text())
PAT = re.compile(rb'^(// SPDX-License-Identifier: LGPL-2\.1-or-later|// Added 2026 by the MacRunner project.*| \* Modified 2026 by the MacRunner project.*)$')
UNITS = ['d3d12', 'd3d12_buffer', 'd3d12_command_allocator', 'd3d12_command_list', 'd3d12_command_queue',
         'd3d12_command_signature', 'd3d12_descriptor_heap', 'd3d12_device', 'd3d12_fence', 'd3d12_heap',
         'd3d12_pipeline_compute', 'd3d12_pipeline_graphics', 'd3d12_query_heap', 'd3d12_resource_helper',
         'd3d12_root_signature', 'd3d12_sampler', 'd3d12_swapchain', 'd3d12_texture']
EXTRA_UNITS = {'d3d10_blob': '.._d3d10_d3d10_blob'}     # shared source ../d3d10/d3d10_blob.cpp: object file name in the target directory


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()


def run(cmd, cwd=None, env=None, log=None, check=True):
    t = time.time()
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if log is not None:
        log.append({'cmd': ' '.join(map(str, cmd))[:300], 'rc': r.returncode, 'seconds': round(time.time() - t, 1),
                    'stderr_tail': r.stderr[-600:] if r.returncode else ''})
    if check and r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-3000:])
        raise SystemExit('command failed rc=%d: %s' % (r.returncode, ' '.join(map(str, cmd))[:200]))
    return r


class Ctx:
    def __init__(self, a):
        self.work = Path(a.work).resolve()
        self.source = Path(a.source).resolve()
        self.mode = a.mode
        self.reports = self.work / 'reports'
        self.reports.mkdir(parents=True, exist_ok=True)
        self.env_file = self.work / 'env.json'
        self.tree = self.work / ('tree-' + self.mode)
        self.build = self.work / ('build-' + self.mode)
        self.jobs = str(a.jobs)

    def env(self):
        return json.loads(self.env_file.read_text())

    def report(self, step, data):
        data = dict(data, step=step, mode=self.mode)
        (self.reports / ('%s-%s.json' % (step, self.mode))).write_text(json.dumps(data, indent=1, sort_keys=True))
        print(json.dumps({k: data[k] for k in data if k in ('step', 'mode', 'status', 'summary')}))


# ---------------------------------------------------------------------------------------------- fetch
def step_fetch(c):
    log = []
    tc = c.work / 'toolchain'
    lm = LOCK['llvm_mingw']
    if os.environ.get('LAYER_LLVM_MINGW_DIR'):
        llvm = Path(os.environ['LAYER_LLVM_MINGW_DIR']).resolve()
    else:
        dl = c.work / 'download'
        dl.mkdir(parents=True, exist_ok=True)
        arc = dl / 'llvm-mingw.tar.xz'
        if not arc.exists():
            urllib.request.urlretrieve(lm['url'], arc)
        got = sha256_file(arc)
        if got != lm['sha256'] or arc.stat().st_size != lm['size']:
            c.report('fetch', {'status': 'FAILED', 'summary': 'llvm-mingw digest/size differ', 'got': got})
            return 1
        tc.mkdir(parents=True, exist_ok=True)
        if not (tc / lm['top_directory']).exists():
            with tarfile.open(arc, 'r:xz') as t:
                t.extractall(tc, filter='tar') if hasattr(tarfile, 'tar_filter') else t.extractall(tc)
        llvm = tc / lm['top_directory']
    r = run([str(llvm / 'bin/x86_64-w64-mingw32-clang++'), '--version'], log=log)
    first = r.stdout.splitlines()[0]
    if first.strip() != lm['clang_version_line']:
        c.report('fetch', {'status': 'FAILED', 'summary': 'compiler version line differs', 'got': first})
        return 1
    dx = LOCK['directx_headers']
    if os.environ.get('LAYER_DIRECTX_DIR'):
        dxdir = Path(os.environ['LAYER_DIRECTX_DIR']).resolve()
    else:
        dxdir = c.work / 'directx-headers'
        if not dxdir.exists():
            run(['git', 'clone', '-q', dx['repository'], str(dxdir)], log=log)
        run(['git', '-C', str(dxdir), 'checkout', '-q', dx['commit']], log=log)
        head = run(['git', '-C', str(dxdir), 'rev-parse', 'HEAD'], log=log).stdout.strip()
        if head != dx['commit']:
            c.report('fetch', {'status': 'FAILED', 'summary': 'directx headers revision differs', 'got': head})
            return 1
    c.env_file.write_text(json.dumps({'llvm': str(llvm), 'directx': str(dxdir)}, indent=1))
    c.report('fetch', {'status': 'OK', 'summary': 'toolchain %s, directx headers %s' % (lm['top_directory'], dx['commit'][:12]),
                       'compiler': first, 'log': log})
    return 0


# ---------------------------------------------------------------------------------------------- tree
def copy_tree(src, dst):
    if sys.platform == 'darwin' and subprocess.run(['cp', '-cR', str(src), str(dst)]).returncode == 0:
        return                                   # APFS clone: no extra space
    if Path(dst).exists():
        shutil.rmtree(dst)                       # clone not possible here: plain copy
    shutil.copytree(src, dst, symlinks=True)


def step_tree(c):
    env = c.env()
    if c.tree.exists():
        shutil.rmtree(c.tree)
    log = []
    man = json.loads((c.source / 'build/repro110layer/ER-V6-SOURCE.json').read_text())
    copy_tree(c.source, c.tree)
    # .git бывает каталогом (обычный клон) или файлом (worktree): убрать любой, иначе git init ниже
    # переиспользовал бы репозиторий-источник и записал бы в него коммит и метку
    gp = c.tree / '.git'
    if gp.is_symlink() or gp.is_file():
        gp.unlink()
    elif gp.exists():
        shutil.rmtree(gp)
    dxdst = c.tree / 'include/native/directx'
    if dxdst.exists():
        shutil.rmtree(dxdst)
    shutil.copytree(env['directx'], dxdst, ignore=shutil.ignore_patterns('.git'))
    bad = []
    if c.mode == 'faithful':
        for p, e in sorted(man['entries'].items()):
            fp = c.tree / p
            if not fp.is_file():
                bad.append(['missing', p]); continue
            lines = fp.read_bytes().split(b'\n')
            ok = True
            for i, text in sorted(e['inserted'], reverse=True):
                if i >= len(lines) or lines[i].decode('latin-1') != text:
                    bad.append(['insert-not-found', p, i]); ok = False; break
                del lines[i]
            if not ok:
                continue
            data = b'\n'.join(lines)
            if hashlib.sha256(data).hexdigest() != e['sha256_t0']:
                bad.append(['sha-differs', p]); continue
            fp.write_bytes(data)
    # git describe --always даёт имя аннотированной метки. faithful: закреплённое значение рабочего файла.
    # plain: то, что даёт сам репозиторий-источник (проверка идёт в нём, а не в копии).
    if c.mode == 'faithful':
        frozen = LOCK['working_file']['frozen_git_describe']
    else:
        frozen = run(['git', '-C', str(c.source), 'describe', '--always'], log=log).stdout.strip()
    g = ['git', '-c', 'user.name=layer', '-c', 'user.email=layer-build', '-c', 'core.hooksPath=/dev/null']
    run(['git', 'init', '-q', str(c.tree)], log=log)
    run(['git', '-C', str(c.tree), 'add', '-A'], log=log)
    run(g + ['-C', str(c.tree), 'commit', '-q', '-m', 'build tree'], log=log)
    run(g + ['-C', str(c.tree), 'tag', '-a', frozen, '-m', 'frozen'], log=log)
    desc = run(['git', '-C', str(c.tree), 'describe', '--always'], log=log, check=False).stdout.strip()
    status = 'OK' if not bad else 'FAILED'
    c.report('tree', {'status': status, 'summary': '%d manifest files checked, %d bad; git describe=%s' % (len(man['entries']), len(bad), desc),
                      'bad': bad[:50], 'git_describe': desc, 'log': log})
    return 0 if not bad else 1


# ---------------------------------------------------------------------------------------------- configure
def tool_env(c):
    env = c.env()
    e = dict(os.environ)
    for k in ('CC', 'CXX', 'CPP', 'LD', 'AR'):
        e.pop(k, None)
    e['PATH'] = os.pathsep.join([str(Path(env['llvm']) / 'bin'), e['PATH']])
    return e


def step_configure(c):
    env = c.env()
    llvm = Path(env['llvm'])
    log = []
    stub = c.work / 'stand-ins'
    wine = stub / 'wine'
    (wine / 'lib/wine/x86_64-windows').mkdir(parents=True, exist_ok=True)
    (wine / 'bin').mkdir(parents=True, exist_ok=True)
    (stub / 'llvm/include').mkdir(parents=True, exist_ok=True)
    (stub / 'llvm/lib').mkdir(parents=True, exist_ok=True)
    clang = str(llvm / 'bin/x86_64-w64-mingw32-clang')
    run([clang, '-c', str(HERE / 'winecrt0_stub.c'), '-o', str(stub / 'unix_lib.o')], log=log)
    a = wine / 'lib/wine/x86_64-windows/libwinecrt0.a'
    if a.exists():
        a.unlink()
    run([str(llvm / 'bin/llvm-ar'), 'rcs', str(a), str(stub / 'unix_lib.o')], log=log)
    (wine / 'bin/winebuild').write_text('#!/bin/sh\nexit 0\n')
    os.chmod(wine / 'bin/winebuild', 0o755)
    cross = c.work / 'x86_64-windows-llvm-mingw.meson'
    b = llvm / 'bin'
    cross.write_text("[binaries]\nc = '%s/x86_64-w64-mingw32-clang'\ncpp = '%s/x86_64-w64-mingw32-clang++'\n"
                     "ar = '%s/x86_64-w64-mingw32-ar'\nstrip = '%s/x86_64-w64-mingw32-strip'\n"
                     "windres = '%s/x86_64-w64-mingw32-windres'\nwidl = '%s/x86_64-w64-mingw32-widl'\n\n"
                     "[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'windows'\ncpu_family = 'x86_64'\n"
                     "cpu = 'x86_64'\nendian = 'little'\n" % ((b,) * 6))
    if c.build.exists():
        shutil.rmtree(c.build)
    meson = [sys.executable, '-m', 'mesonbuild.mesonmain']
    cmd = meson + ['setup', str(c.build), str(c.tree), '--cross-file', str(cross)] + LOCK['meson_options'] + [
        '-Dnative_llvm_path=%s' % (stub / 'llvm'), '-Dwine_install_path=%s' % wine]
    r = run(cmd, env=tool_env(c), log=log, check=False)
    ok = r.returncode == 0
    c.report('configure', {'status': 'OK' if ok else 'FAILED', 'summary': 'meson setup rc=%d' % r.returncode,
                           'stdout_tail': r.stdout[-1200:], 'log': log})
    return 0 if ok else 1


# ---------------------------------------------------------------------------------------------- units
def step_units(c):
    log = []
    man = json.loads((c.source / 'build/repro110layer/ER-V6-SOURCE.json').read_text())
    exp = dict(man['expected_objects'], **LOCK['extra_objects'])
    names = {u: u for u in UNITS}
    names.update(EXTRA_UNITS)
    targets = ['src/d3d12/d3d12.dll.p/%s.cpp.obj' % n for n in names.values()]
    r = run(['ninja', '-C', str(c.build), '-j', c.jobs] + targets, env=tool_env(c), log=log, check=False)
    rows, bad = [], 0
    for u, n in names.items():
        p = c.build / ('src/d3d12/d3d12.dll.p/%s.cpp.obj' % n)
        h = sha256_file(p) if p.exists() else None
        match = bool(h and h.startswith(exp[u]))
        rows.append({'unit': u, 'sha256': h, 'expected_prefix': exp[u], 'match': match})
        bad += 0 if match else 1
    matched = len(names) - bad
    if c.mode == 'faithful':
        status = 'OK' if (r.returncode == 0 and bad == 0) else 'FAILED'
    else:
        status = 'REPORT' if r.returncode == 0 else 'FAILED'      # plain: расхождения ожидаемы (__LINE__), это отчёт
    c.report('units', {'status': status, 'summary': '%d of %d objects equal the working file objects' % (matched, len(names)),
                       'units': rows, 'ninja_rc': r.returncode, 'stdout_tail': r.stdout[-600:], 'log': log})
    return 0 if status in ('OK', 'REPORT') else 1


# ---------------------------------------------------------------------------------------------- build
def pe_timestamp_offsets(b):
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    nsec, = struct.unpack_from('<H', b, pe + 6)
    optsz, = struct.unpack_from('<H', b, pe + 20)
    magic, = struct.unpack_from('<H', b, pe + 24)
    opt = pe + 24
    dd = opt + (112 if magic == 0x20b else 96)
    secs = []
    for i in range(nsec):
        n, vs, va, rs, rp = struct.unpack_from('<8sIIII', b, opt + optsz + 40 * i)
        secs.append((n.rstrip(b'\0').decode('latin-1'), va, vs, rp, rs))
    offs = [pe + 8]
    rva, size = struct.unpack_from('<II', b, dd + 6 * 8)
    for n, va, vs, rp, rs in secs:
        if rva and va <= rva < va + max(vs, rs):
            o = rp + (rva - va)
            offs += [o + 28 * k + 4 for k in range(size // 28)]
    return offs, secs


def step_build(c):
    log = []
    r = run(['ninja', '-C', str(c.build), '-j', c.jobs, 'src/d3d12/d3d12.dll'], env=tool_env(c), log=log, check=False)
    dll = c.build / 'src/d3d12/d3d12.dll'
    wf = LOCK['working_file']
    out = {'ninja_rc': r.returncode, 'log': log, 'stdout_tail': r.stdout[-500:]}
    if r.returncode != 0 or not dll.exists():
        c.report('build', dict(out, status='FAILED', summary='ninja failed'))
        return 1
    b = bytearray(dll.read_bytes())
    out.update(size=len(b), sha256_raw=hashlib.sha256(b).hexdigest())
    offs, secs = pe_timestamp_offsets(bytes(b))
    for o in offs:
        struct.pack_into('<I', b, o, wf['link_timestamp'])
    out['sha256_with_working_file_timestamps'] = hashlib.sha256(b).hexdigest()
    ml = c.build / LOCK['embedded_metallib']['path_in_build']
    if ml.exists():
        m = ml.read_bytes()
        out['embedded_metallib'] = {'size': len(m), 'sha256': hashlib.sha256(m).hexdigest(),
                                    'equals_working_file_metallib': hashlib.sha256(m).hexdigest() == LOCK['embedded_metallib']['sha256_in_working_file'],
                                    'embedded_in_dll': bytes(b).find(m) >= 0}
    out['sections'] = {n: hashlib.sha256(bytes(b)[rp:rp + rs]).hexdigest() for n, va, vs, rp, rs in secs}
    out['working_file_sha256'] = wf['sha256']
    equal = out['sha256_with_working_file_timestamps'] == wf['sha256']
    out['equals_working_file'] = equal
    if c.mode == 'faithful':
        status = 'OK' if equal else 'DIFFERS'
        summary = 'd3d12.dll equals the working file 0bec3f8d (link timestamps aside)' if equal else \
            'd3d12.dll differs from the working file; see embedded_metallib and sections'
    else:
        status = 'REPORT'
        summary = 'plain build; sha256=%s' % out['sha256_with_working_file_timestamps'][:16]
    c.report('build', dict(out, status=status, summary=summary))
    return 0 if status in ('OK', 'REPORT') else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('step', choices=['fetch', 'tree', 'configure', 'units', 'build', 'all'])
    ap.add_argument('--source', required=True, help='дерево репозитория dxmt (чистая выгрузка)')
    ap.add_argument('--work', required=True)
    ap.add_argument('--mode', choices=['faithful', 'plain'], default='faithful')
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--chain', action='store_true', help='сначала все предшествующие шаги (для заданий матрицы)')
    a = ap.parse_args()
    c = Ctx(a)
    steps = {'fetch': step_fetch, 'tree': step_tree, 'configure': step_configure, 'units': step_units, 'build': step_build}
    full = ['fetch', 'tree', 'configure', 'units', 'build']
    if a.step == 'all':
        order = full
    elif a.chain:
        order = full[:full.index(a.step) + 1]
    else:
        order = [a.step]
    worst = 0
    for s in order:
        if s != 'fetch' and not c.env_file.exists():
            raise SystemExit('run fetch first (no %s)' % c.env_file)
        rc = steps[s](c)
        worst = worst or rc
        if rc and len(order) > 1 and s in ('fetch', 'tree', 'configure'):
            break
    return worst


if __name__ == '__main__':
    sys.exit(main())
