#!/usr/bin/env python3
"""Build the sealed D3D11 native/PE trees from fresh Wine/LLVM cloud outputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import sys
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from wine_layout import recipe_root
REPO = recipe_root()
sys.path.insert(0, str(REPO / 'repro109wine'))
import build_full as full
sys.path.insert(0, str(HERE))
import verify_sources
import validate_llvm_prefix

PE_TARGETS = ['src/winemetal/winemetal.dll.postproc', 'src/dxgi/dxgi.dll.postproc',
              'src/d3d11/d3d11.dll', 'src/d3d10/d3d10core.dll.postproc']
PE_OUTPUTS = ['src/winemetal/winemetal.dll', 'src/dxgi/dxgi.dll',
              'src/d3d11/d3d11.dll', 'src/d3d10/d3d10core.dll']
SCENARIOS = [('native-r2', 'native', 'aarch64', 'debug', False),
             ('native-pe', 'pe', 'aarch64', 'release', True),
             ('pe', 'pe', 'x86_64', 'release', True)]


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def sealed_json(path, expected):
    path = Path(path)
    require(re.fullmatch(r'[a-f0-9]{64}', expected) is not None, 'Explicit input SHA256 required')
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= 64 * 1024**2,
            'Input receipt missing/symlink/over cap')
    require(sha(path) == expected, 'Pinned input receipt differs')
    return json.loads(path.read_bytes(), object_pairs_hook=validate_llvm_prefix.unique_object)


def archive_machine(path, expected, *, hybrid_arm64=False, details=False):
    """Check COFF members and the ARM64/ARM64EC hybrid symbol table.

    /<ECSYMBOLS>/ is DWORD count, WORD object indexes, and NUL names,
    as read by Wine tools/winedump/lib.c. It has no COFF Machine field.
    """
    require(not hybrid_arm64 or expected == 0xaa64, 'Hybrid Wine archive must target ARM64')
    allowed = [expected] + ([0xa641] if hybrid_arm64 else [])
    count = 0
    machines = []
    ec_table = None
    with Path(path).open('rb') as stream:
        require(stream.read(8) == b'!<arch>\n', 'Wine import must be a regular archive')
        while True:
            offset = stream.tell()
            header = stream.read(60)
            if not header:
                break
            require(len(header) == 60 and header[58:60] == b'`\n', 'Malformed COFF archive member')
            name = header[:16].decode('ascii').strip()
            size = int(header[48:58].decode('ascii').strip())
            require(0 <= size <= 64 * 1024**2, 'COFF archive member over cap')
            data = stream.read(size)
            require(len(data) == size, 'Truncated COFF archive member')
            if size % 2:
                require(stream.read(1) == b'\n', 'COFF archive alignment differs')
            if name.startswith('#1/'):
                length = int(name[3:])
                require(0 < length <= len(data), 'BSD COFF archive name differs')
                name, data = data[:length].rstrip(b'\0').decode('ascii'), data[length:]
            if name in ('/', '//', '/SYM64/') or name.startswith('__.SYMDEF'):
                continue
            if name == '/<ECSYMBOLS>/':
                require(hybrid_arm64, 'Wine EC symbol table requires the ARM64 hybrid profile')
                require(ec_table is None, 'Duplicate Wine EC symbol table')
                require(len(data) >= 4, 'Truncated Wine EC symbol count')
                symbols = struct.unpack_from('<I', data)[0]
                end = 4 + 2 * symbols
                require(end <= len(data), 'Truncated Wine EC symbol indexes')
                names = data[end:].split(b'\0')
                require(len(names) == symbols + 1 and not names[-1] and all(names[:-1]),
                        'Wine EC symbol name count/termination differs')
                indexes = struct.unpack_from('<' + str(symbols) + 'H', data, 4)
                ec_table = dict(symbols=symbols, indexes=indexes, member_offset=offset,
                                bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                continue
            require(len(data) >= 20, 'COFF object/import header missing')
            machine = struct.unpack_from('<H', data, 6 if data[:4] == b'\0\0\xff\xff' else 0)[0]
            require(machine in allowed, 'Wine import architecture differs: ' + Path(path).name
                    + ' member=' + name + ' offset=' + str(offset) + ' Machine=0x' + format(machine, '04x')
                    + ' expected=' + '/'.join('0x' + format(x, '04x') for x in allowed))
            machines.append(machine)
            count += 1
    require(count > 0, 'Wine import archive has no object members')
    require(expected in machines, 'Wine import lacks the required native architecture')
    require(0xa641 not in machines or ec_table is not None, 'ARM64EC objects lack the EC symbol table')
    if ec_table is not None:
        require(all(1 <= index <= count for index in ec_table['indexes']), 'Wine EC symbol index out of range')
        target_counts = {}
        for index in ec_table.pop('indexes'):
            key = '0x' + format(machines[index - 1], '04x')
            target_counts[key] = target_counts.get(key, 0) + 1
        ec_table['target_machine_counts'] = target_counts
    report = dict(expected_machine=expected, allowed_machines=allowed, objects=count,
                  machine_counts={'0x' + format(value, '04x'): machines.count(value) for value in sorted(set(machines))},
                  ec_symbols=ec_table, ec_metadata_status='PRESENT' if ec_table is not None else 'NOT_PRESENT',
                  hybrid_arm64=0xa641 in machines)
    return report if details else count


def validate_wine(args, dep, wine):
    result = sealed_json(args.wine_result, args.wine_result_sha256)
    files = sealed_json(args.wine_files, args.wine_files_sha256)
    require(type(result) is dict and result.get('status') == 'FULL_WINE_BUILT_NOT_ACCEPTED'
            and result.get('source_revision') == wine.inputs()['revision']
            and result.get('install_skipped') == 0, 'Wine is not the pinned complete source build')
    require(all(type(result.get(key)) is int and result[key] > 0
                for key in ('ec', 'pe64', 'hexpthk', 'a64xrm')), 'Wine EC census missing')
    require(type(files) is list and files == dep.inventory(args.wine_prefix), 'Whole Wine inventory differs')
    manifest = wine.dependencies(args.deps_prefix, args.deps_manifest, args.deps_manifest_sha256)
    require(manifest['files'] == dep.inventory(args.deps_prefix), 'Dependency inventory differs')
    expected_tools = {'meson': '1.11.0', 'ninja': '1.13.2'}
    for name, version in expected_tools.items():
        require(manifest['components'].get(name, {}).get('version') == version,
                'Source-built DXMT tool version differs: ' + name)
    imports = []
    for arch, machine in [('aarch64', 0xaa64), ('x86_64', 0x8664)]:
        for name in ('winecrt0', 'ntdll', 'dbghelp'):
            path = args.wine_prefix / 'lib/wine' / (arch + '-windows') / ('lib' + name + '.a')
            require(path.is_file() and path.resolve().is_relative_to(args.wine_prefix.resolve()),
                    'Fresh Wine import missing or foreign: ' + arch + '/' + name)
            imports.append(dict(path=str(path.relative_to(args.wine_prefix)), sha256=sha(path),
                                **archive_machine(path, machine, hybrid_arm64=arch == 'aarch64', details=True)))
    for name in ('winemac.so', 'ntdll.so'):
        path = args.wine_prefix / 'lib/wine/aarch64-unix' / name
        require(path.is_file(), 'Fresh ARM64 Wine native module missing')
        with path.open('rb') as stream:
            header = stream.read(16)
        require(len(header) == 16 and struct.unpack_from('<II', header) == (0xfeedfacf, 0x100000c),
                'Wine native module is not ARM64 Mach-O')
    require((args.wine_prefix / 'bin/winebuild').is_file(), 'Fresh native Wine tool missing')
    return dict(result_sha256=args.wine_result_sha256, inventory_sha256=args.wine_files_sha256,
                inventory_files=len(files), ec={k: result[k] for k in ('ec', 'pe64', 'hexpthk', 'a64xrm')},
                imports=imports, dependency_manifest_sha256=args.deps_manifest_sha256)


def meson_string(value):
    # Machine files use Meson single-quoted strings, not JSON string tokens.
    # JSON's interior escapes are compatible after unescaping double quotes.
    escaped = json.dumps(str(value), ensure_ascii=False)[1:-1]
    return "'" + escaped.replace('\\"', '"').replace("'", "\\'") + "'"


def native_machine_file(clang, clangxx):
    return '[binaries]\nc = ' + meson_string(clang) + '\ncpp = ' + meson_string(clangxx) + '\n'


def machine_file(compiler, arch):
    values = ["[binaries]"]
    for key, suffix in [('c', 'gcc'), ('cpp', 'g++'), ('windres', 'windres')]:
        values.append(key + ' = ' + meson_string(compiler / 'bin' / (arch + '-w64-mingw32-' + suffix)))
    values += ['ar = ' + meson_string(compiler / 'bin/llvm-ar'),
               'strip = ' + meson_string(compiler / 'bin/llvm-strip'),
               "[host_machine]", "system = 'windows'", 'cpu_family = ' + meson_string(arch),
               'cpu = ' + meson_string(arch), "endian = 'little'", '[properties]', 'needs_exe_wrapper = true']
    return '\n'.join(values) + '\n'


def configure_argv(meson, build, source, cross, native, args, scenario):
    name, kind, arch, buildtype, builtin = scenario
    return [str(meson), 'setup', str(build), str(source), '--cross-file', str(cross),
            '--native-file', str(native), '--buildtype=' + buildtype, '-Db_lto=false', '-Dstrip=false',
            '-Dwine_builtin_dll=' + str(builtin).lower(), '-Dbuild_airconv_for_windows=false',
            '-Denable_nvapi=false', '-Denable_nvngx=false', '-Denable_tests=false',
            '-Dnative_llvm_path=' + str(args.llvm_prefix), '-Dwine_install_path=' + str(args.wine_prefix)]


def ownership(source, build, allowed_tools):
    paths = json.loads((build / 'meson-info/intro-buildsystem_files.json').read_bytes())
    require(type(paths) is list and paths and all(type(x) is str for x in paths), 'Meson source inventory missing')
    foreign = [x for x in paths if not Path(x).resolve().is_relative_to(source.resolve())
               and str(Path(x).resolve()) not in allowed_tools]
    require(not foreign, 'Meson configured foreign source/tool inputs')
    commands = json.loads((build / 'compile_commands.json').read_bytes())
    require(type(commands) is list and commands, 'Meson compiler inventory missing')
    native_c = []
    for row in commands:
        require(type(row) is dict and type(row.get('file')) is str and type(row.get('directory')) is str,
                'Compiler inventory row differs')
        directory = Path(row['directory']).resolve()
        file = (directory / row['file']).resolve()
        require(directory.is_relative_to(build.resolve()) and
                (file.is_relative_to(source.resolve()) or file.is_relative_to(build.resolve())),
                'Compiler selected foreign source/build directory')
        if file.name in ('winemetal_unix.c', 'cache.c'):
            argv = row.get('arguments') or shlex.split(row['command'])
            require(any(argv[i:i+2] == ['-arch', 'arm64'] for i in range(len(argv)-1)),
                    'Native C compiler architecture differs')
            native_c.append(str(file.relative_to(source.resolve())))
    return dict(configured_files=len(paths), compile_records=len(commands), foreign_files=0,
                native_c=native_c)


def output_machine(path, pe):
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if pe:
            require(len(header) == 64 and header[:2] == b'MZ', 'DXMT PE header missing')
            offset = struct.unpack_from('<I', header, 60)[0]
            require(offset <= 1024 * 1024, 'DXMT PE offset over cap')
            stream.seek(offset)
            value = stream.read(6)
            require(value == b'PE\0\0\x64\x86', 'DXMT PE is not x86_64')
            return 'x86_64_PE_EC_NOT_APPLICABLE'
        require(len(header) >= 16 and struct.unpack_from('<II', header) == (0xfeedfacf, 0x100000c),
                'DXMT native output is not ARM64 Mach-O')
        return 'arm64_MachO'


def check_inputs():
    source = verify_sources.verify()
    lock = full.check_inputs()
    validate_llvm_prefix.expected_inputs()
    return source, lock


def build(args):
    full.cloud_guard(args.profile)  # Before work creation, downloaded tools or network.
    source, lock = check_inputs()
    require(type(args.jobs) is int and 1 <= args.jobs <= 4, 'Use 1..4 compiler workers')
    root = args.work.resolve()
    require(not root.exists() and not root.is_symlink() and not any(c.isspace() for c in str(root)),
            'Fresh owned work path without whitespace required')
    root.mkdir(parents=True)
    out = root / 'reports'; out.mkdir()
    dep = full.load_driver('repro109_dxmt_deps', REPO / 'repro109deps/build_deps.py')
    wine = full.load_driver('repro109_dxmt_wine', REPO / 'repro109wine/build_wine.py')
    deadline = time.monotonic() + 95 * 60
    result = dict(schema=1, status='STARTED', first_failure=None, source_inputs=source,
                  comparison='NOT_ENABLED', stands='NOT_ENABLED', selected_package_mapping='NOT_ACCEPTED',
                  games='NOT_ENABLED', signing='NOT_ENABLED', notarization='NOT_ENABLED', outputs=[])
    phase = 'INPUTS'
    def run(name, argv, cwd=root):
        dep.command(argv, cwd, env, out / (name + '.log'), out, name, deadline, timeout=1800)
    try:
        tool = full.apply_profile(dep.read_lock(), args.profile)['toolchain']
        sdk, clang, clangxx = dep.toolchain_preflight(tool, out)
        env = dep.environment(args.deps_prefix, tool, clang, clangxx, sdk)
        env.pop('CFLAGS', None); env.pop('CXXFLAGS', None)
        env['PYTHONDONTWRITEBYTECODE'] = '1'
        env['MACRUNNER_WINE_RECIPE_ROOT'] = str(REPO)
        for key in ('GITHUB_ACTIONS', 'CI_BUILD_NUMBER', 'CI_WORKSPACE_PATH', 'CI_PRIMARY_REPOSITORY_PATH'):
            if key in os.environ:
                env[key] = os.environ[key]
        result['wine_inputs'] = validate_wine(args, dep, wine)
        result['llvm_inputs'] = validate_llvm_prefix.validate(args.llvm_prefix, args.llvm_result,
                                                             args.llvm_result_sha256)
        compiler_archive = root / 'llvm-mingw.tar.xz'
        wine.download_compiler(lock['compiler'], compiler_archive, out)
        compiler = wine.prepare_compiler(compiler_archive, root / 'compiler', lock['compiler']['sha256'], out)
        native = root / 'native.ini'
        native.write_text(native_machine_file(clang, clangxx))
        meson, ninja = args.deps_prefix / 'bin/meson', args.deps_prefix / 'bin/ninja'
        for name, version in [(meson, '1.11.0'), (ninja, '1.13.2')]:
            run(name.name + '-version', [str(name), '--version'])
            require((out / (name.name + '-version.log')).read_text().strip() == version, 'Tool version differs')
        result['toolchain'] = tool
        for scenario in SCENARIOS:
            name, kind, arch, buildtype, builtin = scenario
            phase = name + ':PREPARE'
            prepared = root / name
            builddir = prepared / 'build'
            script = 'prepare_native.py' if kind == 'native' else 'prepare_pe.py'
            run(name + '-prepare', [sys.executable, '-B', '-I', str(HERE / script), '--out', name])
            for script, options in [('freeze_version.py', []), ('llvm_link.py', []),
                                    ('prepare_directx.py', ['--checkout', 'headers', '--profile', args.profile])]:
                if script == 'prepare_directx.py' and name == SCENARIOS[0][0]:
                    options += ['--fetch']
                run(name + '-' + script[:-3], [sys.executable, '-B', '-I', str(HERE / script),
                                              '--kind', kind, '--prepared', name, *options])
            cross = root / (arch + '.ini')
            if not cross.exists():
                cross.write_text(machine_file(compiler, arch))
            selected = prepared / 'source'
            before = verify_sources.snapshot(selected)
            phase = name + ':CONFIGURE'
            run(name + '-configure', configure_argv(meson, builddir, selected, cross, native, args, scenario))
            run(name + '-version-header', [str(ninja), '-C', str(builddir), 'version.h'])
            require(sha(builddir / 'version.h') == json.loads((HERE / 'sources.lock.json').read_bytes())[
                    'historical_version']['version_h_sha256'], 'Configured DXMT version differs')
            if arch == 'aarch64':
                tools = [native, cross, Path('/usr/bin/printf'), Path('/usr/bin/xcrun'),
                         Path('/usr/bin/xxd'), args.wine_prefix / 'bin/winebuild',
                         compiler / 'bin' / (arch + '-w64-mingw32-windres')]
                measured = ownership(selected, builddir, {str(x.resolve()) for x in tools})
                require(len(measured['native_c']) == 2, 'Native winemetal/cache compilation inputs missing')
            else:
                # PE configure declares x64 native targets, but selected PE targets must exclude them.
                tools = [native, cross, Path('/usr/bin/printf'), Path('/usr/bin/xcrun'), Path('/usr/bin/xxd'),
                         args.wine_prefix / 'bin/winebuild', compiler / 'bin/x86_64-w64-mingw32-windres']
                measured = ownership_pe(selected, builddir, {str(x.resolve()) for x in tools})
            (out / (name + '-ownership.json')).write_text(json.dumps(measured, indent=2) + '\n')
            for relative in ('compile_commands.json', 'meson-info/intro-buildsystem_files.json',
                             'meson-info/intro-machines.json', 'meson-info/intro-buildoptions.json', 'build.ninja'):
                path = builddir / relative
                require(path.stat().st_size <= 64 * 1024**2, 'Configure evidence over cap; original retained')
                shutil.copyfile(path, out / (name + '-' + relative.replace('/', '-')))
            targets = PE_TARGETS if arch == 'x86_64' else ['src/winemetal/unix/winemetal.so']
            run(name + '-commands', [str(ninja), '-C', str(builddir), '-t', 'commands', *targets])
            if arch == 'x86_64':
                commands = (out / (name + '-commands.log')).read_text()
                require(not any(x in commands for x in ('airconv.dir/', 'DXBCParserNative', 'winemetal.so')),
                        'PE subset unexpectedly compiles Darwin AIR/native objects')
            phase = name + ':COMPILE'
            run(name + '-build', [str(ninja), '-C', str(builddir), '-j' + str(args.jobs), '-v', *targets])
            require(verify_sources.snapshot(selected) == before,
                    'Source tree changed during compilation')
            destination = root / 'outputs' / name; destination.mkdir(parents=True)
            for relative in PE_OUTPUTS if arch == 'x86_64' else ['src/winemetal/unix/winemetal.so']:
                path = builddir / relative
                architecture = output_machine(path, arch == 'x86_64')
                target = destination / path.name
                shutil.copyfile(path, target)
                result['outputs'].append(dict(path=str(target.relative_to(root)), bytes=target.stat().st_size,
                                               sha256=sha(target), architecture=architecture, source_kind=kind,
                                               configuration=name))
        result['wine_inputs_after'] = validate_wine(args, dep, wine)
        result['llvm_inputs_after'] = validate_llvm_prefix.validate(args.llvm_prefix, args.llvm_result,
                                                                  args.llvm_result_sha256)
        require(result['wine_inputs_after'] == result['wine_inputs'] and
                result['llvm_inputs_after'] == result['llvm_inputs'], 'Source-built inputs drifted')
        result['status'] = 'D3D11_SOURCE_BUILT_NOT_FUNCTION_SECTION_ACCEPTED'
    except Exception as error:
        result.update(status='FAILED', first_failure=dict(phase=phase, error=str(error)[:1600]))
        raise
    finally:
        result['install_skipped'] = sum('install skipped' in line.lower()
                                       for p in out.glob('*.log') for line in p.read_text(errors='replace').splitlines())
        result['install'] = 'EXPLICIT_OUTPUT_COPY_NO_MESON_INSTALL'
        result['logs'] = [dict(path=p.name, bytes=p.stat().st_size, sha256=sha(p),
                               coverage='PRESENT' if p.stat().st_size else 'EMPTY') for p in sorted(out.glob('*.log'))]
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({key: result[key] for key in ('status', 'first_failure', 'install_skipped')}))


def ownership_pe(source, build, allowed_tools):
    # Use the same source ownership check, without admitting ARM64 native C from an x64 configure.
    paths = json.loads((build / 'meson-info/intro-buildsystem_files.json').read_bytes())
    commands = json.loads((build / 'compile_commands.json').read_bytes())
    require(type(paths) is list and paths and all(type(x) is str for x in paths), 'PE Meson inventory missing')
    require(type(commands) is list and commands, 'PE compiler inventory missing')
    require(all(Path(x).resolve().is_relative_to(source.resolve()) or str(Path(x).resolve()) in allowed_tools
                for x in paths), 'PE Meson configured foreign source/tool inputs')
    for row in commands:
        directory = Path(row['directory']).resolve()
        file = (directory / row['file']).resolve()
        require(directory.is_relative_to(build.resolve()) and
                (file.is_relative_to(source.resolve()) or file.is_relative_to(build.resolve())),
                'PE compiler selected foreign source/build directory')
    return dict(configured_files=len(paths), compile_records=len(commands), foreign_files=0,
                selected_native_compilation='NOT_ENABLED_PE_SUBSET')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-inputs', action='store_true')
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--profile', choices=('xcode-cloud', 'github', 'github-macos15-arm64', 'github-xcode27-arm64'),
                        default='github-macos15-arm64')
    parser.add_argument('--work', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    for name in ('wine-prefix', 'wine-result', 'wine-files', 'deps-prefix', 'deps-manifest', 'llvm-prefix', 'llvm-result'):
        parser.add_argument('--' + name, type=Path)
    for name in ('wine-result-sha256', 'wine-files-sha256', 'deps-manifest-sha256', 'llvm-result-sha256'):
        parser.add_argument('--' + name)
    args = parser.parse_args()
    if args.check_inputs and not args.build:
        source, lock = check_inputs()
        print(json.dumps(dict(status='D3D11_SOURCE_RECIPE_CHECKED_NOT_COMPILED', source=source,
                              scenarios=len(SCENARIOS), pe_targets=PE_TARGETS, install='skipped')))
        return 0
    require(args.build and not args.check_inputs, 'Select --check-inputs or --build')
    require(all(value is not None for key, value in vars(args).items() if key not in ('check_inputs', 'build')),
            'All source-built input paths and their explicit SHA256 pins are required')
    for key, value in vars(args).items():
        if isinstance(value, Path) and key != 'work':
            setattr(args, key, value.resolve())
    build(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
