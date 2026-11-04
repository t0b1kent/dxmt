#!/usr/bin/env python3
"""Build the selected c32 graphics modules from the exact retained source lines."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import prepare_source

TARGETS = {
    'x64': ['src/d3d11/d3d11.dll', 'src/d3d10/d3d10core.dll.postproc',
            'src/dxgi/dxgi.dll.postproc', 'src/winemetal/winemetal.dll.postproc'],
    'ec': ['src/d3d12/d3d12.dll', 'src/dxgi/dxgi.dll.postproc', 'src/winemetal/winemetal.dll.postproc'],
    'native': ['src/winemetal/unix/winemetal.so', 'src/airconv/darwin/airconv'],
}
STACK_PROBE = bytes.fromhex('f0ed7cd3f1030091310640d1100640f13f0240f9acffff54c0035fd6')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def text_section(data):
    machine, count, _, _, _, optional, _ = struct.unpack_from('<HHIIIHH', data)
    if machine != 0xa641:
        raise ValueError('Stack probe object must be ARM64EC')
    for i in range(count):
        start = 20 + optional + 40 * i
        if data[start:start + 8].rstrip(b'\0') == b'.text':
            size, offset = struct.unpack_from('<II', data, start + 16)
            return data[offset:offset + size]
    raise ValueError('Stack probe object has no text section')


def selected_inputs(args):
    if args.variant not in TARGETS:
        raise ValueError('Unknown graphics build variant')
    if args.stage == 'source':
        return
    required = [args.meson, args.ninja, args.llvm / 'include/llvm/Config/llvm-config.h',
                args.wine / 'include/wine/windows/windef.h']
    triple = 'x86_64' if args.variant == 'x64' else 'arm64ec'
    required += [args.compiler / 'bin' / (triple + '-w64-mingw32-' + tool)
                 for tool in ['clang', 'clang++', 'ar', 'strip', 'windres', 'widl']]
    for path in required:
        if not path.is_file():
            raise ValueError('Required source-built input or tool missing: ' + str(path))


def build(args):
    selected_inputs(args)
    root = args.work.resolve()
    if root.exists() or root.is_symlink():
        raise ValueError('Fresh owned graphics work path required')
    root.mkdir(parents=True)
    reports = root / 'reports'; reports.mkdir()
    result = dict(schema=1, status='STARTED', variant=args.variant, stage=args.stage,
                  first_failure=None, outputs=[], commands=[], games='NOT_ENABLED',
                  signing='NOT_ENABLED', stands='NOT_ENABLED', install='skipped')
    phase = 'SOURCE'
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET='14.0', PYTHONDONTWRITEBYTECODE='1')
    env['PATH'] = str(args.ninja.parent) + os.pathsep + env.get('PATH', '')
    def run(name, argv, cwd=root):
        started = time.monotonic()
        path = reports / (name + '.log')
        with path.open('xb') as stream:
            child = subprocess.run([str(x) for x in argv], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                   stdout=stream, stderr=subprocess.STDOUT, timeout=1800)
        result['commands'].append(dict(name=name, argv=[str(x) for x in argv], rc=child.returncode,
                                       seconds=round(time.monotonic() - started, 3),
                                       bytes=path.stat().st_size, sha256=sha(path)))
        if path.stat().st_size > 32 * 1024 * 1024 or child.returncode:
            raise ValueError(name + ' failed or exceeded log budget; complete log retained')
        return path
    try:
        source = root / 'source'
        result['source'] = prepare_source.prepare('native' if args.variant == 'native' else 'c17', args.headers, source, zlib=args.zlib)
        if args.stage == 'source':
            result['status'] = 'EXACT_PRODUCT_SOURCE_PREPARED'
            return 0
        phase = 'VERSION'
        run('git-init', ['git', 'init', '--quiet', source])
        run('git-add', ['git', '-c', 'core.hooksPath=/dev/null', 'add', '--', 'src', 'include', 'libs',
                        'meson.build', 'meson.options', 'version.h.in', 'LICENSE', 'COPYING.LIB', 'LICENSE.OLD'], source)
        identity = ['git', '-c', 'core.hooksPath=/dev/null', '-c', 'user.name=MacRunner Build',
                    '-c', 'user.email=noreply@users.noreply.github.com']
        run('git-commit', identity + ['commit', '--quiet', '-m', 'Exact graphics compilation inputs'], source)
        run('git-tag', identity + ['tag', '-a', result['source']['git_describe'], '-m', 'Retained build version'], source)
        phase = 'CONFIGURE'
        triple = 'x86_64' if args.variant == 'x64' else 'arm64ec'
        tools = {key: str(args.compiler / 'bin' / (triple + '-w64-mingw32-' + val)) for key, val in
                 [('c', 'clang'), ('cpp', 'clang++'), ('ar', 'ar'), ('strip', 'strip'), ('windres', 'windres'), ('widl', 'widl')]}
        cross = root / 'cross.ini'
        cross.write_text('[binaries]\n' + ''.join(k + ' = ' + repr(v) + '\n' for k, v in tools.items()) +
                         "\n[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'windows'\n" +
                         "cpu_family = '" + ('x86_64' if args.variant == 'x64' else 'aarch64') + "'\n" +
                         "cpu = '" + ('x86_64' if args.variant == 'x64' else 'aarch64') + "'\nendian = 'little'\n")
        options = ['-Dbuildtype=release', '-Dwine_builtin_dll=false', '-Denable_d3d12=true',
                   '-Denable_tests=false', '-Denable_nvapi=false', '-Denable_nvngx=false', '--wrap-mode=nodownload',
                   '-Dnative_llvm_path=' + str(args.llvm), '-Dwine_install_path=' + str(args.wine)]
        if args.variant != 'x64':
            stub = root / 'chkstk.obj'
            run('stack-probe', [tools['c'], '-c', HERE / 'chkstk_arm64ec.S', '-o', stub])
            if text_section(stub.read_bytes()) != STACK_PROBE:
                raise ValueError('Rebuilt stack probe differs from the accepted c17 text')
            result['stack_probe_text'] = hashlib.sha256(STACK_PROBE).hexdigest()
            options.append('-Dc_link_args=' + str(stub))
        builddir = root / 'build'
        run('meson', [args.meson, 'setup', builddir, source, '--cross-file', cross, *options])
        if args.stage == 'configure':
            result['status'] = 'GRAPHICS_CONFIGURED_NOT_COMPILED'
            return 0
        phase = 'COMPILE'
        run('ninja', [args.ninja, '-C', builddir, '-j', '2', *TARGETS[args.variant]])
        outputs = root / 'outputs'; outputs.mkdir()
        for name in TARGETS[args.variant]:
            path = builddir / name.removesuffix('.postproc')
            if not path.is_file():
                raise ValueError('Graphics output missing: ' + name)
            data = path.read_bytes()
            if args.variant != 'native':
                pe = struct.unpack_from('<I', data, 0x3c)[0]
                machine = struct.unpack_from('<H', data, pe + 4)[0]
                if data[pe:pe + 4] != b'PE\0\0' or machine != (0x8664 if args.variant == 'x64' else 0xa641):
                    raise ValueError('Wrong output PE architecture: ' + name)
            elif struct.unpack_from('<II', data) != (0xfeedfacf, 0x0100000c):
                raise ValueError('Native output must be ARM64 Mach-O: ' + name)
            target = outputs / path.name
            target.write_bytes(data)
            result['outputs'].append(dict(path=target.name, bytes=len(data), sha256=sha(target)))
        result['status'] = 'GRAPHICS_SOURCE_BUILT_NOT_RELEASE_ACCEPTED'
        return 0
    except Exception as error:
        result.update(status='FAILED', first_failure=dict(phase=phase, error=str(error)))
        return 1
    finally:
        result['install_skipped'] = sum('install skipped' in line.lower() for p in reports.glob('*.log')
                                        for line in p.read_text(errors='replace').splitlines())
        (reports / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({k: result[k] for k in ['status', 'first_failure', 'install_skipped']}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=sorted(TARGETS), required=True)
    parser.add_argument('--stage', choices=['source', 'configure', 'compile'], required=True)
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--zlib', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, default=Path('/MISSING_COMPILER'))
    parser.add_argument('--wine', type=Path, default=Path('/MISSING_WINE'))
    parser.add_argument('--llvm', type=Path, default=Path('/MISSING_LLVM'))
    parser.add_argument('--meson', type=Path, default=Path('/MISSING_MESON'))
    parser.add_argument('--ninja', type=Path, default=Path('/MISSING_NINJA'))
    parser.add_argument('--work', type=Path, required=True)
    raise SystemExit(build(parser.parse_args()))


if __name__ == '__main__':
    main()
