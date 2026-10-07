#!/usr/bin/env python3
"""Cloud-only source build of ARM64 LLVM15, static zstd and libunwind for AIR."""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sys
import tarfile
import time

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
from wine_layout import recipe_root
REPO = recipe_root()
sys.path.insert(0, str(REPO / 'repro109'))
import archive_safety
import public_archive
sys.path.insert(0, str(HERE))
from verify_sources import verify, sha
from live_results import LiveResults

LLVM_URL = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-15.0.7/llvm-project-15.0.7.src.tar.xz'
LLVM_SHA = '8b5fcb24b4128cf04df1b0b9410ce8b1a729cb3c544e6da885d234280dedeac6'
MINUTES = 95  # Existing Xcode dispatcher is killed after 120 minutes.
PROFILE_MINUTES = {'xcode-cloud': MINUTES, 'github': 330, 'github-macos15-arm64': 330}


def load(name, path):
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def llvm_source_pin():
    verify()
    lock = json.loads((HERE / 'sources.lock.json').read_bytes())
    rows = [row for row in lock['external_source_pins'] if row['name'] == 'llvm15']
    if len(rows) != 1 or rows[0]['version'] != '15.0.7' or rows[0]['url'] != LLVM_URL or rows[0]['sha256'] != LLVM_SHA:
        raise ValueError('LLVM publisher/version/source pin differs')
    return dict(name='llvm15', url=LLVM_URL, sha256=LLVM_SHA, maximum_size=256 * 1024**2)


def unpack_source(archive, destination, expected_sha):
    if archive.is_symlink() or sha(archive) != expected_sha:
        raise ValueError('LLVM source archive hash/symlink differs')
    if destination.exists() or destination.is_symlink():
        raise ValueError('LLVM source output already exists')
    with tarfile.open(archive) as stream:
        members = stream.getmembers()
        graph = archive_safety.validate_tar(members)
        roots = {Path(member.name).parts[0] for member in members if Path(member.name).parts}
        if roots != {'llvm-project-15.0.7.src'}:
            raise ValueError('LLVM source archive root differs')
        names = {member.name for member in members if member.isfile()}
        required = {'llvm-project-15.0.7.src/' + name for name in
                    ['llvm/CMakeLists.txt', 'runtimes/CMakeLists.txt', 'libunwind/CMakeLists.txt']}
        if not required <= names:
            raise ValueError('LLVM/runtimes/libunwind source inputs missing')
        destination.mkdir()
        if callable(getattr(tarfile, 'data_filter', None)):
            stream.extractall(destination, members=members, filter='data')
        else:
            stream.extractall(destination, members=members)
    return destination / 'llvm-project-15.0.7.src', graph


def llvm_rows():
    # EH/RTTI/threads/libc++ match recorded LLVM15 configuration. Static archives
    # are selected explicitly. Fresh functions/sections still require comparison.
    return [dict(name='llvm15', kind='cmake', source_subdir='llvm', build_dir='_repro_llvm', args=[
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DCMAKE_C_COMPILER={cc}', '-DCMAKE_CXX_COMPILER={cxx}',
        '-DBUILD_SHARED_LIBS=OFF', '-DLLVM_BUILD_LLVM_DYLIB=OFF', '-DLLVM_LINK_LLVM_DYLIB=OFF',
        '-DLLVM_ENABLE_PROJECTS=', '-DLLVM_ENABLE_RUNTIMES=', '-DLLVM_TARGETS_TO_BUILD=',
        '-DLLVM_ENABLE_WERROR=OFF',
        # TODO: workaround for LLVM15/new-libc++ missing standard includes;
        # replace with precise upstream header fixes after the cloud error names them.
        # This is a declared hypothesis, not a recovered b32 compiler diagnosis.
        '-DCMAKE_CXX_FLAGS=-include cstdint -include cstdlib',
        '-DLLVM_ENABLE_EH=ON', '-DLLVM_ENABLE_RTTI=ON', '-DLLVM_ENABLE_THREADS=ON',
        '-DLLVM_ENABLE_LIBCXX=ON', '-DLLVM_ENABLE_ZLIB=ON', '-DLLVM_ENABLE_ZSTD=ON',
        '-DLLVM_ENABLE_TERMINFO=ON', '-DLLVM_ENABLE_LIBXML2=ON', '-DLLVM_ENABLE_FFI=OFF',
        '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF',
        '-DLLVM_INCLUDE_DOCS=OFF', '-DLLVM_ENABLE_ASSERTIONS=OFF',
        '-DLLVM_BUILD_TOOLS=ON', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    ], build_targets=['llvm-config', 'LLVMPasses', 'LLVMBitWriter'],
       install_components=['llvm-headers', 'llvm-config']), dict(name='libunwind15', kind='cmake', source_subdir='runtimes', build_dir='_repro_unwind', args=[
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DCMAKE_C_COMPILER={cc}', '-DCMAKE_CXX_COMPILER={cxx}',
        '-DLLVM_ENABLE_RUNTIMES=libunwind', '-DLIBUNWIND_ENABLE_SHARED=OFF',
        '-DLIBUNWIND_ENABLE_STATIC=ON', '-DLIBUNWIND_INCLUDE_TESTS=OFF',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    ])]


def install_air_archives(source, prefix, out, command):
    """Select the fresh build-tree closure before installing its static archives."""
    log = out / 'llvm-air-libnames.log'
    # The installed tool checks prefix/lib before returning --libnames. At this
    # point only headers and llvm-config are installed; the archives are still
    # in the fresh build tree. Query that tool, then validate the installed
    # closure through read_config after all components have been installed.
    command([str(source / '_repro_llvm/bin/llvm-config'), '--link-static', '--libnames', 'bitwriter', 'passes'], log)
    names = shlex.split(log.read_text())
    if not names or len(names) > 150 or len(set(names)) != len(names) or any(not re.fullmatch(r'libLLVM[A-Za-z0-9_-]+\.a', name) for name in names):
        raise ValueError('Fresh AIR static archive names differ')
    root = source / '_repro_llvm/lib'
    entries = []
    for name in names:
        src, dest = root / name, prefix / 'lib' / name
        if src.is_symlink() or not src.is_file() or not src.resolve().is_relative_to(root.resolve()) or dest.exists() or dest.is_symlink():
            raise ValueError('Missing/foreign/conflicting AIR archive: ' + name)
        raw_sha = sha(src)
        shutil.copy2(src, dest)
        if sha(dest) != raw_sha: raise ValueError('Installed AIR archive bytes differ')
        entries.append(dict(path='lib/' + name, bytes=dest.stat().st_size, sha256=raw_sha))
    (out / 'llvm-air-installed-archives.json').write_text(json.dumps(entries, sort_keys=True, indent=2) + '\n')
    return entries


def read_config(config, env, out, command):
    values = {}
    for key, flags in [('version', ['--version']), ('host_target', ['--host-target']),
                       ('libfiles', ['--link-static', '--libfiles', 'bitwriter', 'passes']),
                       ('system_libs', ['--link-static', '--system-libs', 'bitwriter', 'passes'])]:
        log = out / ('llvm-config-' + key + '.log')
        command([str(config), *flags], log, env=env)
        value = log.read_text().strip()
        if key == 'version' and value != '15.0.7':
            raise ValueError('Built llvm-config version differs')
        if key == 'libfiles' and not value:
            raise ValueError('Built LLVM static AIR closure is empty')
        values[key] = value
    return validate_config(values, config.parent.parent)


def validate_config(values, prefix):
    if values['version'] != '15.0.7' or not re.fullmatch(r'(arm64|aarch64)-apple-darwin[0-9.]+', values['host_target']):
        raise ValueError('Fresh LLVM version/host architecture differs')
    prefix = prefix.resolve()
    selected = []
    def archive(value):
        path = Path(value)
        if (not path.is_absolute() or path.suffix != '.a' or not path.is_file() or
                not path.resolve().is_relative_to(prefix)):
            raise ValueError('LLVM static library missing or outside the fresh prefix')
        selected.append(dict(path=str(path.relative_to(prefix)), bytes=path.stat().st_size, sha256=sha(path)))
    libfiles = shlex.split(values['libfiles'])
    if not libfiles:
        raise ValueError('Built LLVM static AIR closure is empty')
    for value in libfiles:
        archive(value)
    # Other system dependency names must be explained by the selected SDK;
    # preserve unknown flags as a first failure instead of guessing a substitute.
    sdk_libs = {'-lm', '-lz', '-lcurses', '-lncurses', '-lxml2', '-lffi', '-lpthread', '-ldl', '-lc'}
    for value in shlex.split(values['system_libs']):
        if value == '-lzstd':
            archive(str(prefix / 'lib/libzstd.a'))
        elif value not in sdk_libs:
            archive(value)
    for relative in ['lib/libzstd.a', 'lib/libunwind.a']:
        archive(str(prefix / relative))
    return dict(values, selected_archives=selected, architecture='arm64',
                runtime_or_ABI_acceptance='NOT_ENABLED')


def cmake_query(source, row, argv):
    if row['kind'] != 'cmake' or '-S' not in argv or '-B' not in argv:
        return None
    directory = argv[argv.index('-B') + 1]
    if Path(directory).is_absolute() or '..' in Path(directory).parts:
        raise ValueError('CMake build directory must belong to this source')
    query = source / directory / '.cmake/api/v1/query'
    query.mkdir(parents=True)
    for name in ['cmakeFiles-v1', 'codemodel-v2']:
        (query / name).touch()
    return directory


def cmake_evidence(dep, source, row, prefix, sdk, out, directory):
    # Preserve the exact compilation database before the inherited ownership
    # check can fail. The existing dependency guard preserves cache/API replies.
    commands = source / directory / 'compile_commands.json'
    ledger = dict(state='NOT_ENABLED', path=None, bytes=0, sha256=None)
    if '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON' in row['args']:
        if commands.is_symlink() or not commands.is_file():
            raise ValueError('LLVM compilation database missing or symlink')
        if commands.stat().st_size > 64 * 1024**2:
            raise ValueError('LLVM compilation database exceeds 64 MiB cap; source raw retained')
        target = out / (row['name'] + '-compile_commands.json')
        if target.exists() or target.is_symlink():
            raise ValueError('LLVM compilation evidence collision; no overwrite')
        shutil.copyfile(commands, target)
        ledger = dict(state='PRESENT', path=target.name, bytes=target.stat().st_size, sha256=sha(target))
    ownership = dep.cmake_source_ownership(source, source / row.get('source_subdir', '.'),
                                          prefix, sdk, out, row['name'], directory)
    return dict(ownership=ownership, compile_commands=ledger)


def build(work, jobs, profile):
    full = load('dxmt_llvm_full', REPO / 'repro109wine/build_full.py')
    full.cloud_guard(profile)  # Before mkdir, any payload download or execution.
    pin = llvm_source_pin()
    dep = load('dxmt_llvm_deps', REPO / 'repro109deps/build_deps.py')
    lock = full.apply_profile(dep.read_lock(), profile)
    work = Path(work).resolve()
    if work.exists() or any(c.isspace() for c in str(work)):
        raise ValueError('Fresh cloud work without whitespace required')
    if type(jobs) is not int or not 1 <= jobs <= 8:
        raise ValueError('Jobs must be between 1 and 8')
    work.mkdir(parents=True)
    out = work / 'reports'; out.mkdir()
    prefix = work / 'prefix'; prefix.mkdir()
    minutes = PROFILE_MINUTES[profile]
    deadline = time.monotonic() + minutes * 60
    result = dict(schema=1, status='STARTED', source_built=False, first_failure=None,
                  comparison='NOT_ENABLED', stands='NOT_ENABLED', games='NOT_ENABLED',
                  signing='NOT_ENABLED', notarization='NOT_ENABLED', sources=[], components={},
                  minutes=minutes, jobs=jobs, profile=profile)
    phase = 'TOOLCHAIN'
    live = LiveResults(out, REPO, profile, publish_dir=None, initial_publish_required=False)
    def command(argv, log, *, cwd=work, env=None):
        live.record(phase, 'COMMAND_BEGIN', status=result['status'])
        try:
            remaining = int(deadline - time.monotonic())
            if remaining <= 0:
                raise TimeoutError('LLVM profile build budget exhausted before command')
            dep.command(argv, cwd, env, log, out, phase, deadline, timeout=remaining)
        except Exception:
            try:
                live.record(phase, 'COMMAND_FAILED', status='FAILED')
            except Exception:
                (out / 'failure-checkpoint-publish-failed.txt').write_text('FAILED\n')
            raise
        live.record(phase, 'COMMAND_END', status=result['status'])
    try:
        live.record(phase, 'STARTED')
        sdk, cc, cxx = dep.toolchain_preflight(lock['toolchain'], out)
        env = dep.environment(prefix, lock['toolchain'], cc, cxx, sdk)
        (out / 'effective-toolchain.json').write_text(json.dumps(lock['toolchain'], indent=2) + '\n')
        available = {row['name']: row for row in lock['components']}
        for name in ['cmake', 'ninja', 'zstd']:
            phase = name
            live.record(phase, 'PHASE_BEGIN')
            row = copy.deepcopy(available[name])
            if name == 'zstd':
                row['args'] = [arg.replace('ZSTD_BUILD_SHARED=ON', 'ZSTD_BUILD_SHARED=OFF').replace(
                    'ZSTD_BUILD_STATIC=OFF', 'ZSTD_BUILD_STATIC=ON') for arg in row['args']]
                row['outputs'] = ['include/zstd.h', 'lib/libzstd.a']
            archive = work / (name + '.archive')
            dep.download(row, archive, out)
            source = dep.unpack(archive, work / (name + '-source'))
            result['sources'].append(dict(name=name, url=row['url'], archive_sha256=sha(archive), args=row['args']))
            for number, argv in enumerate(dep.build_steps(row, source, prefix, sdk, jobs, (cc, cxx))):
                directory = cmake_query(source, row, argv)
                command(argv, out / (name + '-' + str(number) + '.log'), cwd=source, env=env)
                if directory is not None:
                    result.setdefault('cmake_evidence', {})[name] = cmake_evidence(
                        dep, source, row, prefix, sdk, out, directory)
            if name == 'ninja':
                (prefix / 'bin').mkdir(exist_ok=True)
                shutil.copy2(source / 'ninja', prefix / 'bin/ninja')
            dep.licenses(source, prefix, name)
            for relative in row['outputs']:
                if not (prefix / relative).is_file():
                    raise ValueError('Source-built output missing: ' + relative)
            result['components'][name] = dict(state='BUILT', version=row['version'], source_built=True)
            live.record(phase, 'PHASE_END')
        phase = 'LLVM_DOWNLOAD'
        live.record(phase, 'PHASE_BEGIN')
        archive = work / 'llvm15.tar.xz'
        public_archive.download(pin, archive, out)
        source, graph = unpack_source(archive, work / 'llvm15-source', pin['sha256'])
        result['sources'].append(dict(name='llvm15', url=pin['url'], archive_sha256=sha(archive), graph=graph))
        live.record(phase, 'PHASE_END')
        for row in llvm_rows():
            phase = row['name']
            live.record(phase, 'PHASE_BEGIN')
            for number, argv in enumerate(dep.build_steps(row, source, prefix, sdk, jobs, (cc, cxx))):
                directory = cmake_query(source, row, argv)
                command(argv, out / (phase + '-' + str(number) + '.log'), cwd=source, env=env)
                if directory is not None:
                    result.setdefault('cmake_evidence', {})[phase] = cmake_evidence(
                        dep, source, row, prefix, sdk, out, directory)
            if row['name'] == 'llvm15':
                result['air_installed_archives'] = install_air_archives(source, prefix, out, command)
            result['components'][phase] = dict(state='BUILT', version='15.0.7', source_built=True, args=row['args'])
            live.record(phase, 'PHASE_END')
        phase = 'LICENSES'
        dep.licenses(source / 'llvm', prefix, 'llvm15')
        dep.licenses(source / 'libunwind', prefix, 'libunwind15')
        phase = 'LLVM_CONFIG'
        result['air_link'] = read_config(prefix / 'bin/llvm-config', env, out, command)
        for relative in ['bin/llvm-config', 'include/llvm/Config/llvm-config.h', 'lib/libunwind.a', 'lib/libzstd.a']:
            if not (prefix / relative).is_file():
                raise ValueError('LLVM support output missing: ' + relative)
        result.update(status='LLVM15_ARM64_SOURCE_BUILT_NOT_DXMT_ACCEPTED', source_built=True,
                      files=dep.inventory(prefix), toolchain=lock['toolchain'])
    except Exception as error:
        result.update(status='FAILED', first_failure=dict(phase=phase, error=str(error)))
        raise
    finally:
        result['install_skipped'] = sum('install skipped' in line.lower() for path in out.glob('*.log')
                                        for line in path.read_text(errors='replace').splitlines())
        result['raw_logs'] = [dict(path=x.name, bytes=x.stat().st_size, sha256=sha(x)) for x in sorted(out.glob('*.log'))]
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
        try:
            live.record(phase, 'TERMINAL', status=result['status'], failure=result['first_failure'])
        except Exception:
            # Preserve the original build/publication failure, plus a local explicit marker.
            (out / 'terminal-checkpoint-publish-failed.txt').write_text('FAILED\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--work', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--profile', choices=['github', 'github-macos15-arm64', 'xcode-cloud'],
                        default='github-macos15-arm64')
    args = parser.parse_args()
    if not args.build:
        print(json.dumps(dict(status='LLVM15_SOURCE_RECIPE_NOT_BUILT', source=llvm_source_pin(),
                              recipes=llvm_rows(), downloads=0, builds=0, install='skipped'), sort_keys=True))
        return 0
    if args.work is None:
        parser.error('--work is required for cloud build')
    build(args.work, args.jobs, args.profile)
    return 0


if __name__ == '__main__':
    sys.exit(main())
