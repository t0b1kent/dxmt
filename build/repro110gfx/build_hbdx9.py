#!/usr/bin/env python3
"""Build the c33r i386 DX9 carrier/shim or ARM64 native core without running Wine."""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build as graphics
import prepare_source

LLVM_COMPONENTS = '''Passes Target ObjCARCOpts Coroutines ipo Instrumentation Vectorize
Linker IRReader AsmParser FrontendOpenMP ScalarOpts InstCombine AggressiveInstCombine
TransformUtils BitWriter Analysis ProfileData Symbolize DebugInfoPDB DebugInfoMSF
DebugInfoDWARF Object TextAPI MCParser MC DebugInfoCodeView BitReader Core Remarks
BitstreamReader BinaryFormat Support Demangle'''.split()


def pe_commands(args, source, output):
    tools = args.compiler / 'bin'
    src = source / 'src/hbdx9'
    carrier = output / 'hbdx9.dll'
    archive = output / 'libhbdx9.a'
    wine_libs = args.wine / 'lib/wine/i386-windows'
    return [
        ('carrier', [tools / 'i686-w64-mingw32-clang', '-O2', '-Wall', '-shared', '-o', carrier,
            src / 'hbdx9_carrier.c', src / 'hbdx9.def', '-Wl,--out-implib,' + str(archive),
            wine_libs / 'libwinecrt0.a', wine_libs / 'libntdll.a',
            '-Wl,--enable-stdcall-fixup', '-Wl,--kill-at', '-Wl,--file-alignment=4096']),
        ('builtin', [args.wine / 'bin/winebuild', '--builtin', carrier]),
        ('shim', [tools / 'i686-w64-mingw32-clang++', '-O2', '-std=c++20', '-Wall',
            '-Wno-unused-parameter', '-fno-exceptions', '-shared', '-msse2', '-mfpmath=sse',
            '-I' + str(src), '-o', output / 'd3d9.dll.new', src / 'd3d9_shim.cpp', src / 'd3d9.def',
            archive, '-luser32', '-lgdi32', '-luuid', '-static', '-Wl,--enable-stdcall-fixup',
            '-Wl,--kill-at', '-Wl,--file-alignment=4096', '-Wl,--no-insert-timestamp']),
    ]


def native_compile_commands(args, source, builddir, output, wrapper):
    objects = output / 'obj'
    src = source / 'src'
    includes = ['-I' + str(source / name) for name in ['src/airconv', 'src/airconv/dxso',
        'src/hbdx9', 'include/native', 'include/native/windows', 'include/native/directx']]
    compile_prefix = [sys.executable, wrapper, args.native_clangxx, '-arch', 'arm64',
        '-mmacosx-version-min=26.5', '-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti',
        '-Wall', '-Wno-unused-function', *includes]
    commands = []
    dxso = []
    for name in ['dxso_decoder', 'dxso_tables', 'dxso_code', 'dxso_dxbc']:
        obj = objects / (name + '.o')
        dxso.append(obj)
        commands.append((name, [*compile_prefix, '-c', src / 'airconv/dxso' / (name + '.cpp'), '-o', obj]))
    commands.append(('dxso-archive', [args.native_ar, 'rcs', objects / 'libdxso.a', *dxso]))
    for name, suffix in [('hbdx9_ffp', '.cpp'), ('hbdx9_unix', '.mm')]:
        commands.append((name, [*compile_prefix, *(['-fno-objc-arc'] if suffix == '.mm' else []),
            '-c', src / 'hbdx9/unix' / (name + suffix), '-o', objects / (name + '.o')]))
    commands.append(('native-link', [args.native_clangxx, '-arch', 'arm64', '-mmacosx-version-min=26.5',
        '-dynamiclib', '-o', output / 'hbdx9.so', objects / 'hbdx9_unix.o', objects / 'hbdx9_ffp.o',
        objects / 'libdxso.a', '-L' + str(args.llvm / 'lib'),
        builddir / 'src/airconv/darwin/libairconv.a', builddir / 'libs/DXBCParser/libDXBCParserNative.a',
        '-lm', '-lz', '-lcurses', '-lxml2', args.llvm / 'lib/libzstd.a', args.llvm / 'lib/libunwind.a',
        *['-lLLVM' + name for name in LLVM_COMPONENTS], '-Wl,-install_name,@rpath/hbdx9.so',
        '-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'CoreGraphics',
        '-framework', 'Foundation', '-lsqlite3']))
    return commands


def selected_inputs(args):
    if args.stage == 'source':
        return
    target = ('i686' if args.kind == 'pe' else graphics.TRIPLES['native']) + '-w64-mingw32-'
    required = [args.compiler / 'bin' / (target + name) for name in ['clang', 'clang++']]
    if args.kind == 'pe':
        required += [args.wine / 'bin/winebuild', *[args.wine / 'lib/wine/i386-windows' / name
                     for name in ['libwinecrt0.a', 'libntdll.a']]]
    else:
        required += [args.meson, args.ninja, args.native_clang, args.native_clangxx, args.native_ar,
                     args.wine / 'include/wine/windows/windef.h',
                     args.llvm / 'include/llvm/Config/llvm-config.h']
    for path in required:
        if not path.is_file():
            raise ValueError('Required source-built input or tool missing: ' + str(path))
    banner = subprocess.check_output([str(required[0]), '--version'], text=True, timeout=15)
    if not re.search(r'^clang version 23\.1\.1(?:\s|$)', banner, re.M):
        raise ValueError('Pinned LLVM23.1.1 compiler required')
    if args.kind == 'native':
        graphics.native_link_options('native', args.llvm)
    prepare_source.build_tool('ldpfilter23')


def build(args):
    selected_inputs(args)
    root = args.work.resolve()
    if root.exists() or root.is_symlink():
        raise ValueError('Fresh owned HBDX9 work path required')
    root.mkdir(parents=True)
    reports = root / 'reports'; reports.mkdir()
    result = dict(schema=1, status='STARTED', kind=args.kind, stage=args.stage,
        first_failure=None, commands=[], outputs=[], games='NOT_ENABLED', signing='NOT_ENABLED', install='skipped')
    env = {key: value for key, value in os.environ.items()
           if key in ['HOME', 'TMPDIR', 'DEVELOPER_DIR', 'LANG', 'LC_ALL', 'PATH', 'SDKROOT']}
    env.update(MACOSX_DEPLOYMENT_TARGET='26.5', PYTHONDONTWRITEBYTECODE='1', TSO64_LDPFILTER='1')
    # Only the six direct native core objects use the filter. The native
    # airconv/DXBCParser static libraries use the unwrapped Apple compiler.
    env.update(CC=str(args.native_clang), CXX=str(args.native_clangxx))
    env['PATH'] = str(args.ninja.parent) + os.pathsep + env.get('PATH', '')
    phase = 'SOURCE'
    def run(name, argv):
        path = reports / (name + '.log')
        started = time.monotonic()
        with path.open('xb') as stream:
            child = subprocess.run([str(x) for x in argv], cwd=root, env=env, stdin=subprocess.DEVNULL,
                stdout=stream, stderr=subprocess.STDOUT, timeout=600)
        result['commands'].append(dict(name=name, argv=[str(x) for x in argv], rc=child.returncode,
            seconds=round(time.monotonic() - started, 3), bytes=path.stat().st_size, sha256=graphics.sha(path)))
        if child.returncode or path.stat().st_size > 32 * 1024 * 1024:
            raise ValueError(name + ' failed or exceeded log budget; complete log retained')
    try:
        source = root / 'source'
        result['source'] = prepare_source.prepare('hbdx9', args.headers, source, zlib=args.zlib)
        if args.stage == 'source':
            result['status'] = 'EXACT_PRODUCT_SOURCE_PREPARED'
            return 0
        output = root / 'outputs'; output.mkdir()
        if args.kind == 'pe':
            commands = pe_commands(args, source, output)
        else:
            phase = 'CONFIGURE'
            cross = root / 'cross.ini'
            # Match the working ARM64EC context: the project uses its host CPU
            # family to select -arch arm64 for the native AIR/DXBCParser libraries.
            # No PE target is built here; these libraries use unwrapped Apple clang.
            tools = {key: str(args.compiler / 'bin' / (graphics.TRIPLES['native'] + '-w64-mingw32-' + value))
                for key, value in [('c', 'clang'), ('cpp', 'clang++'), ('ar', 'ar'), ('strip', 'strip'),
                                   ('windres', 'windres'), ('widl', 'widl')]}
            cross.write_text('[binaries]\n' + ''.join(k + ' = ' + repr(v) + '\n' for k, v in tools.items()) +
                "\n[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'windows'\n"
                "cpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n")
            builddir = root / 'build'
            run('meson', [args.meson, 'setup', builddir, source, '--cross-file', cross, '-Dbuildtype=release',
                '-Dnative_llvm_path=' + str(args.llvm), '-Dwine_install_path=' + str(args.wine),
                '-Dwine_builtin_dll=true', '-Denable_d3d12=false', '-Denable_tests=false', '--wrap-mode=nodownload'])
            static_targets = ['src/airconv/darwin/libairconv.a', 'libs/DXBCParser/libDXBCParserNative.a']
            run('ninja-targets', [args.ninja, '-C', builddir, '-n', *static_targets])
            commands = [('native-static-libraries', [args.ninja, '-C', builddir, '-j', '2', *static_targets])]
            (output / 'obj').mkdir()
            commands += native_compile_commands(args, source, builddir, output, prepare_source.build_tool('ldpfilter23'))
        if args.stage == 'configure':
            result.update(status='HBDX9_COMMANDS_PREPARED_NOT_COMPILED', planned_commands=[
                dict(name=name, argv=[str(x) for x in argv]) for name, argv in commands])
            return 0
        phase = 'COMPILE'
        for name, argv in commands:
            run(name, argv)
        if args.kind == 'pe':
            (output / 'd3d9.dll.new').rename(output / 'd3d9.dll')
        for name in (['hbdx9.dll', 'd3d9.dll'] if args.kind == 'pe' else ['hbdx9.so']):
            path = output / name
            if args.kind == 'pe':
                architecture = graphics.pe_identity(path, 'i386')
            else:
                if struct.unpack_from('<II', path.read_bytes()) != (0xfeedfacf, 0x0100000c):
                    raise ValueError('Native output must be ARM64 Mach-O')
                architecture = dict(machine='arm64')
            result['outputs'].append(dict(path=name, bytes=path.stat().st_size,
                sha256=graphics.sha(path), architecture=architecture))
        result['status'] = 'HBDX9_SOURCE_BUILT_NOT_RELEASE_ACCEPTED'
        return 0
    except Exception as error:
        result.update(status='FAILED', first_failure=dict(phase=phase, error=str(error)))
        return 1
    finally:
        (reports / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({k: result[k] for k in ['status', 'first_failure']}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=['pe', 'native'], required=True)
    parser.add_argument('--stage', choices=['source', 'configure', 'compile'], required=True)
    for name in ['headers', 'zlib', 'work']:
        parser.add_argument('--' + name, type=Path, required=True)
    for name in ['compiler', 'wine', 'llvm', 'meson', 'ninja']:
        parser.add_argument('--' + name, type=Path, default=Path('/MISSING_' + name.upper()))
    parser.add_argument('--native-clang', type=Path, default=Path('/usr/bin/clang'))
    parser.add_argument('--native-clangxx', type=Path, default=Path('/usr/bin/clang++'))
    parser.add_argument('--native-ar', type=Path, default=Path('/usr/bin/ar'))
    raise SystemExit(build(parser.parse_args()))


if __name__ == '__main__':
    main()
