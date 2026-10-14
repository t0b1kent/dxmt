#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
"""Builds the native tools of the DXIL -> Metal shader translator from pinned upstream sources.

  fetch     dxil-spirv (and its submodules) at the pinned revisions into <work>/src
  dxil      dxil-spirv libraries (CMake, Release, arm64)
  frontend  command line front end: upstream dxil_spirv.cpp + patches/dxil-spirv/*.patch, built against the libraries above
  cross     three SPIRV-Cross variants (base; + root ABI and descriptor fields; + tess/gs field): patches/spirv-cross/*.patch
  writer    msl-writer (lane/msl_writer.cpp) against the base variant
  all       fetch, dxil, frontend, cross, writer
  verify    digests of the patched SPIRV-Cross files and of the front end against inputs.lock.json

Outputs land where the Python translator looks for them (see inputs.lock.json: tool_locations).
Reuse of existing trees (local checks only): TRANSLATOR_DXIL_BUILD=<build dir>, TRANSLATOR_DXIL_SOURCE=<source dir>,
TRANSLATOR_SPIRV_CROSS_SOURCE=<git clone of SPIRV-Cross>.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOCK = json.loads((HERE / 'inputs.lock.json').read_text())
LANE = HERE / 'lane'


def sha256(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def run(cmd, cwd=None, check=True, env=None):
    print('+', ' '.join(map(str, cmd))[:220], flush=True)
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env)
    if check and r.returncode:
        raise SystemExit('failed rc=%d: %s' % (r.returncode, ' '.join(map(str, cmd))[:200]))
    return r.returncode


def out(cmd, cwd=None):
    return subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True, check=True).stdout.strip()


class W:
    def __init__(self, work):
        self.work = Path(work).resolve()
        self.src = self.work / 'src'
        self.dxil_src = Path(os.environ.get('TRANSLATOR_DXIL_SOURCE', self.src / 'dxil-spirv')).resolve()
        self.dxil_build = Path(os.environ.get('TRANSLATOR_DXIL_BUILD', self.work / 'build-dxil')).resolve()
        self.cross_src = Path(os.environ.get('TRANSLATOR_SPIRV_CROSS_SOURCE', self.src / 'SPIRV-Cross')).resolve()
        self.jobs = os.environ.get('TRANSLATOR_JOBS') or str(os.cpu_count() or 4)


def fetch(w):
    d = LOCK['dxil_spirv']
    w.src.mkdir(parents=True, exist_ok=True)
    if not (w.src / 'dxil-spirv').exists():
        run(['git', 'clone', '-q', d['repository'], w.src / 'dxil-spirv'])
    run(['git', '-C', w.src / 'dxil-spirv', 'fetch', '-q', 'origin', d['commit']], check=False)
    run(['git', '-C', w.src / 'dxil-spirv', 'checkout', '-q', '--detach', d['commit']])
    head = out(['git', '-C', w.src / 'dxil-spirv', 'rev-parse', 'HEAD'])
    if head != d['commit']:
        raise SystemExit('dxil-spirv revision %s != pinned %s' % (head, d['commit']))
    run(['git', '-C', w.src / 'dxil-spirv', 'submodule', 'update', '--init', '--', 'third_party/SPIRV-Cross',
         'third_party/SPIRV-Tools', 'third_party/spirv-headers', 'subprojects/dxbc-spirv'])
    for path, sha in d['submodules'].items():
        got = out(['git', '-C', w.src / 'dxil-spirv' / path, 'rev-parse', 'HEAD'])
        if got != sha:
            raise SystemExit('submodule %s is %s, pinned %s' % (path, got, sha))
    # the pristine SPIRV-Cross for the patched variants is the same revision as dxil-spirv's submodule
    c = LOCK['spirv_cross']
    if not (w.src / 'SPIRV-Cross').exists():
        run(['git', 'clone', '-q', c['repository'], w.src / 'SPIRV-Cross'])
    run(['git', '-C', w.src / 'SPIRV-Cross', 'fetch', '-q', 'origin', c['commit']], check=False)
    run(['git', '-C', w.src / 'SPIRV-Cross', 'checkout', '-q', '--detach', c['commit']])
    if out(['git', '-C', w.src / 'SPIRV-Cross', 'rev-parse', 'HEAD']) != c['commit']:
        raise SystemExit('SPIRV-Cross revision differs from the pin')
    print('fetch OK: dxil-spirv %s, %d submodules verified, SPIRV-Cross %s' % (head[:12], len(d['submodules']), c['commit'][:12]))


def dxil(w):
    if os.environ.get('TRANSLATOR_DXIL_BUILD'):
        print('reusing dxil-spirv build', w.dxil_build)
        return
    b = w.dxil_build
    flags = '-O2 -arch arm64'
    run(['cmake', '-S', w.dxil_src, '-B', b, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=arm64',
         '-DCMAKE_CXX_FLAGS=' + flags, '-DCMAKE_C_FLAGS=' + flags, '-DDXIL_SPIRV_CLI=ON', '-DDXIL_SPIRV_NATIVE_LLVM=OFF',
         '-DSPIRV_BUILD_FUZZER=OFF', '-DSPIRV_SKIP_TESTS=ON', '-DSKIP_SPIRV_TOOLS_INSTALL=ON'])
    run(['ninja', '-C', b, '-j', w.jobs])


def apply_patches(tree, names, patch_dir):
    for n in names:
        run(['git', 'apply', '--check', patch_dir / n], cwd=tree)
        run(['git', 'apply', patch_dir / n], cwd=tree)


def frontend(w):
    fe = w.work / 'frontend'
    if fe.exists():
        shutil.rmtree(fe)
    fe.mkdir(parents=True)
    shutil.copy(w.dxil_src / 'dxil_spirv.cpp', fe / 'dxil_spirv.cpp')
    run(['git', 'init', '-q', fe]); run(['git', '-C', fe, 'add', '.'])
    run(['git', '-C', fe, '-c', 'user.name=t', '-c', 'user.email=translator-build', 'commit', '-q', '-m', 'upstream'])
    first, second = LOCK['frontend']['patches']
    apply_patches(fe, [first], HERE / 'patches/dxil-spirv')               # onto upstream dxil_spirv.cpp
    shutil.copy(fe / 'dxil_spirv.cpp', fe / 'dxil_spirv_frontend.cpp')    # the lane's CLI is a copy of it, named so
    apply_patches(fe, [second], HERE / 'patches/dxil-spirv')
    final = fe / 'dxil_spirv_frontend.cpp'
    if sha256(final) != LOCK['frontend']['source_sha256']:
        raise SystemExit('front end source differs from the recorded one: %s' % sha256(final))
    src, b = w.dxil_src, w.dxil_build
    sdk = out(['xcrun', '--show-sdk-path'])
    dst = LANE / 'takeover/admission/build-frontend'
    dst.mkdir(parents=True, exist_ok=True)
    target = dst / 'dxil-spirv-logical'
    dylib = next(b.glob('libdxil-spirv-c-shared.*.*.*.dylib'))
    cross_libs = [b / 'third_party/SPIRV-Cross' / ('libspirv-cross-%s.a' % n)
                  for n in ('c', 'hlsl', 'msl', 'cpp', 'glsl', 'core', 'reflect')]
    run([out(['xcrun', '--find', 'clang++']), '-O2', '-arch', 'arm64', '-isysroot', sdk, '-std=gnu++14', '-fno-exceptions',
         '-fno-rtti', '-I', src, '-I', src / 'third_party/cli_parser', '-I', src / 'debug',
         '-I', src / 'third_party/SPIRV-Tools/include', '-I', src / 'third_party/SPIRV-Cross', final, '-o', target,
         '-Wl,-rpath,' + str(b), dylib, b / 'libcli-parser.a', b / 'third_party/SPIRV-Tools/source/libSPIRV-Tools.a',
         cross_libs[0], b / 'libdxil-debug.a'] + cross_libs[1:])
    print('frontend OK', sha256(target)[:16])


def cross_source(w, name, n_patches):
    """Pinned SPIRV-Cross tree + the first n patches (git archive of the dxil-spirv submodule revision)."""
    tree = w.work / ('spirv-cross-' + name)
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    arc = subprocess.run(['git', '-C', w.cross_src, 'archive', LOCK['spirv_cross']['commit']],
                         capture_output=True, check=True).stdout
    subprocess.run(['tar', '-x', '-C', str(tree)], input=arc, check=True)
    run(['git', 'init', '-q', tree]); run(['git', '-C', tree, 'add', '.'])
    run(['git', '-C', tree, '-c', 'user.name=t', '-c', 'user.email=translator-build', 'commit', '-q', '-m', 'pristine'])
    apply_patches(tree, LOCK['spirv_cross']['patches'][:n_patches], HERE / 'patches/spirv-cross')
    return tree


def cross(w):
    for name, n, dest in (('base', 1, LANE / 'build-native'),
                          ('admission', 3, LANE / 'takeover/admission/build-cross'),
                          ('tess-gs', 4, LANE / 'takeover/tess-gs/build-cross')):
        tree = cross_source(w, name, n)
        b = w.work / ('build-cross-' + name)
        run(['cmake', '-S', tree, '-B', b, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DSPIRV_CROSS_ENABLE_TESTS=OFF',
             '-DSPIRV_CROSS_SHARED=OFF', '-DSPIRV_CROSS_STATIC=ON', '-DSPIRV_CROSS_CLI=ON', '-DCMAKE_OSX_ARCHITECTURES=arm64'])
        run(['ninja', '-C', b, '-j', w.jobs, 'spirv-cross'])
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copy(b / 'spirv-cross', dest / 'spirv-cross')
        print('cross %s OK %s' % (name, sha256(dest / 'spirv-cross')[:16]))


def writer(w):
    tree = w.work / 'spirv-cross-base'
    b = w.work / 'build-cross-base'
    sdk = out(['xcrun', '--show-sdk-path'])
    target = LANE / 'build-native/msl-writer'
    run([out(['xcrun', '--find', 'clang++']), '-O2', '-arch', 'arm64', '-isysroot', sdk, '-std=c++17', '-I', tree,
         LANE / 'msl_writer.cpp', '-o', target, b / 'libspirv-cross-msl.a', b / 'libspirv-cross-glsl.a',
         b / 'libspirv-cross-core.a'])
    print('writer OK', sha256(target)[:16])


def verify(w):
    bad = 0
    for name, n in (('base', 1), ('admission', 3), ('tess-gs', 4)):
        tree = cross_source(w, name, n)
        for f, want in LOCK['spirv_cross']['patched_files'][name].items():
            got = sha256(tree / f)
            ok = got == want
            bad += 0 if ok else 1
            print('%-10s %-14s %s' % (name, f, 'OK' if ok else 'DIFFERS ' + got))
    for rel, want in LOCK['files'].items():
        got = sha256(HERE / rel)
        if got != want:
            bad += 1
            print('SOURCE DIFFERS', rel)
    print('verify: %d problems' % bad)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('step', choices=['fetch', 'dxil', 'frontend', 'cross', 'writer', 'all', 'verify'])
    ap.add_argument('--work', default=str(HERE / '_build'))
    a = ap.parse_args()
    w = W(a.work)
    steps = {'fetch': fetch, 'dxil': dxil, 'frontend': frontend, 'cross': cross, 'writer': writer, 'verify': verify}
    if a.step == 'all':
        if not os.environ.get('TRANSLATOR_DXIL_SOURCE'):
            fetch(w)
        dxil(w); frontend(w); cross(w); writer(w)
        return verify(w)
    r = steps[a.step](w)
    return r if isinstance(r, int) else 0


if __name__ == '__main__':
    sys.exit(main())
