#!/usr/bin/env python3
"""Read-only acceptance of a pinned source-built ARM64 LLVM input for DXMT."""
import argparse
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_llvm15 as llvm

STATUS = 'LLVM15_ARM64_SOURCE_BUILT_NOT_DXMT_ACCEPTED'
ARM64 = 0x0100000C
SDK_LIBS = {'-lm', '-lz', '-lcurses', '-lncurses', '-lxml2', '-lffi', '-lpthread', '-ldl', '-lc'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON field')
        result[key] = value
    return result


def relative(value):
    require(type(value) is str and bool(value), 'Missing relative input path')
    path = PurePosixPath(value)
    require(not path.is_absolute() and '..' not in path.parts and str(path) == value
            and value != '.', 'Unsafe relative input path')
    return path


def arm64_header(data, filetype):
    require(len(data) >= 16 and data[:4] == b'\xcf\xfa\xed\xfe', 'Expected thin Mach-O 64 input')
    cpu, _, actual_type = struct.unpack('<III', data[4:16])
    require(cpu == ARM64 and actual_type == filetype, 'Mach-O architecture or file type differs')


def archive_architecture(path):
    """Inspect every ar object header; never load or execute an archive member."""
    objects = 0
    with path.open('rb') as stream:
        size = path.stat().st_size
        require(stream.read(8) == b'!<arch>\n', 'Expected a regular static ar archive')
        names = b''
        while stream.tell() < size:
            header = stream.read(60)
            require(len(header) == 60 and header[58:] == b'`\n', 'Truncated ar member header')
            try:
                length = int(header[48:58].strip())
                name = header[:16].decode('ascii').strip()
            except (ValueError, UnicodeError):
                raise ValueError('Invalid ar member metadata') from None
            start = stream.tell()
            require(length >= 0 and start + length <= size, 'Truncated ar member payload')
            payload = length
            if name.startswith('#1/'):
                try:
                    name_size = int(name[3:])
                except ValueError:
                    raise ValueError('Invalid BSD ar name') from None
                require(0 <= name_size <= min(length, 65536), 'BSD ar name exceeds cap')
                try:
                    name = stream.read(name_size).rstrip(b'\0').decode('utf-8')
                except UnicodeError:
                    raise ValueError('Invalid BSD ar name bytes') from None
                payload -= name_size
            elif name == '//':
                require(length <= 4 * 1024**2, 'GNU ar name table exceeds cap')
                names = stream.read(length)
            elif name.startswith('/') and name[1:].isdigit():
                offset = int(name[1:])
                end = names.find(b'/\n', offset)
                require(offset < len(names) and end >= offset, 'Invalid GNU ar name offset')
                try:
                    name = names[offset:end].decode('utf-8')
                except UnicodeError:
                    raise ValueError('Invalid GNU ar name bytes') from None
            if name.endswith('/') and not name.startswith('/'):
                name = name[:-1]
            if name not in ('/', '/SYM64/', '//', '__.SYMDEF', '__.SYMDEF SORTED',
                            '__.SYMDEF_64', '__.SYMDEF_64 SORTED'):
                require(payload >= 16, 'Empty or truncated ar object')
                arm64_header(stream.read(16), 1)
                objects += 1
            stream.seek(start + length)
            if length % 2:
                require(stream.read(1) == b'\n', 'Missing ar member padding')
        require(stream.tell() == size and objects > 0, 'Static archive contains no ARM64 objects')
    return objects


def expected_inputs():
    dep = llvm.load('dxmt_prefix_deps', llvm.REPO / 'repro109deps/build_deps.py')
    full = llvm.load('dxmt_prefix_full', llvm.REPO / 'repro109wine/build_full.py')
    lock = dep.read_lock()
    rows = {row['name']: copy.deepcopy(row) for row in lock['components']
            if row['name'] in ('cmake', 'ninja', 'zstd')}
    require(set(rows) == {'cmake', 'ninja', 'zstd'}, 'Producer dependency source pins incomplete')
    rows['zstd']['args'] = [arg.replace('ZSTD_BUILD_SHARED=ON', 'ZSTD_BUILD_SHARED=OFF').replace(
        'ZSTD_BUILD_STATIC=OFF', 'ZSTD_BUILD_STATIC=ON') for arg in rows['zstd']['args']]
    return dep, full, lock, rows, llvm.llvm_source_pin()


def validate_source_record(record):
    """Check both existing producers without executing their compiled outputs."""
    require(type(record) is dict and type(record.get('schema')) is int and record['schema'] == 1,
            'Unexpected LLVM RESULT schema')
    require(record.get('status') == STATUS and record.get('source_built') is True
            and record.get('first_failure', 'MISSING') is None, 'LLVM input is not a complete source build')
    require(type(record.get('install_skipped')) is int and record['install_skipped'] == 0,
            'LLVM source build contains missing/invalid install skipped count')
    dep, full, lock, pins, llvm_pin = expected_inputs()
    tool = record.get('toolchain')
    require(type(tool) is dict, 'LLVM toolchain must be an object')
    # build_llvm15 writes a root profile; staged plan.accept writes it only in
    # toolchain. An explicitly conflicting/missing root value is not a fallback.
    profile = record.get('profile', tool.get('profile'))
    require(profile in ('github', 'github-macos15-arm64', 'xcode-cloud'), 'LLVM source build profile missing')
    require(tool.get('profile', profile) == profile, 'LLVM toolchain/root profile conflict')
    expected = full.apply_profile(lock, profile)['toolchain']
    if profile in ('github', 'github-macos15-arm64'):
        require(tool == expected, 'LLVM toolchain differs from selected profile')
    else:
        # toolchain_preflight enriches this selected policy in place. The eight
        # measured fields are pinned by the caller's byte-exact RESULT SHA;
        # verify the policy and every measured field rather than discard them.
        measured = {'python', 'python_path', 'xcode', 'xcode_build', 'apple_clang',
                    'apple_ld', 'macos_build', 'exact_pins_state'}
        require(set(tool) == set(expected) | measured and
                all(tool[key] == value for key, value in expected.items()),
                'LLVM toolchain policy/measured fields differ from selected profile')
        require(all(type(tool[key]) is str and tool[key] for key in measured),
                'LLVM measured toolchain fields must be nonempty strings')
        require(tool['exact_pins_state'] == 'MEASURED_BEFORE_SOURCES',
                'LLVM toolchain was not measured before source execution')
        minimum = expected.get('python_minimum')
        require(type(minimum) is list and len(minimum) == 2 and
                all(type(value) is int and value >= 0 for value in minimum),
                'LLVM cloud Python minimum policy is invalid')
        require(re.fullmatch(r'3\.\d+\.\d+', tool['python']) is not None and
                tuple(map(int, tool['python'].split('.'))) >=
                tuple(minimum) and
                Path(tool['python_path']).is_absolute(), 'LLVM measured Python differs from cloud policy')
        require(re.fullmatch(re.escape(str(expected['xcode_major'])) + r'(?:\.\d+)*',
                             tool['xcode']) is not None and
                re.fullmatch(r'[A-Za-z0-9]+', tool['xcode_build']) is not None,
                'LLVM measured Xcode differs from cloud policy')
        require(re.search(r'^Apple clang version 21\.\d+\.\d+(?![\d.])',
                          tool['apple_clang'], re.M) is not None,
                'LLVM measured Apple clang differs from cloud policy')
        require(re.search(r'PROJECT:ld-' + re.escape(expected['ld']) + r'(?![\d.])',
                          tool['apple_ld']) is not None, 'LLVM measured Apple ld differs from cloud policy')
        require(re.fullmatch(r'\d+[A-Z]\d+[a-z]?', tool['macos_build']) is not None,
                'LLVM measured macOS build is invalid')
    sources = record.get('sources')
    require(type(sources) is list and all(type(row) is dict and type(row.get('name')) is str
            for row in sources), 'LLVM sources must be named records')
    source_rows = {row.get('name'): row for row in sources}
    require(len(source_rows) == len(sources) and set(source_rows) == set(pins) | {'llvm15'},
            'LLVM source set missing, duplicated or extra')
    for name, pin in {**pins, 'llvm15': llvm_pin}.items():
        row = source_rows[name]
        require(row.get('url') == pin['url'] and row.get('archive_sha256') == pin['sha256'],
                'LLVM dependency publisher/archive pin differs')
        if name != 'llvm15':
            require(row.get('args') == pin['args'], 'LLVM dependency configure flags differ')
    components = record.get('components')
    require(type(components) is dict and set(components) == set(pins) | {'llvm15', 'libunwind15'},
            'LLVM source component set differs')
    for name, pin in {**pins, **{row['name']: dict(row, version='15.0.7') for row in llvm.llvm_rows()}}.items():
        row = components[name]
        require(type(row) is dict and row.get('state') == 'BUILT' and row.get('source_built') is True
                and row.get('version') == pin['version'], 'LLVM source component not built at selected version')
        if name in ('llvm15', 'libunwind15'):
            require(row.get('args') == pin['args'], 'LLVM/libunwind CMake flags differ')
    return dep, profile, source_rows


def validate(prefix, result_path, expected_sha256):
    prefix, result_path = Path(prefix), Path(result_path)
    require(re.fullmatch(r'[a-f0-9]{64}', expected_sha256) is not None, 'Expected RESULT SHA256 required')
    require(not prefix.is_symlink() and prefix.is_dir() and not result_path.is_symlink()
            and result_path.is_file(), 'LLVM prefix/result missing or symlink')
    require(result_path.stat().st_size <= 64 * 1024**2, 'LLVM RESULT exceeds 64 MiB cap')
    raw = result_path.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == expected_sha256, 'Pinned LLVM RESULT bytes differ')
    record = json.loads(raw, object_pairs_hook=unique_object)
    dep, profile, source_rows = validate_source_record(record)
    try:
        files = dep.inventory(prefix)
    except (AssertionError, OSError, RuntimeError):
        raise ValueError('LLVM prefix inventory invalid') from None
    require(type(record.get('files')) is list and record['files'] == files, 'Whole LLVM prefix inventory differs')
    index = {row['path']: row for row in files}
    for name in ('bin/llvm-config', 'include/llvm/Config/llvm-config.h', 'lib/libunwind.a', 'lib/libzstd.a'):
        require(name in index and index[name]['symlink'] is None, 'Required LLVM input missing or symlink')
    with (prefix / 'bin/llvm-config').open('rb') as stream:
        arm64_header(stream.read(16), 2)
    air = record.get('air_link')
    require(type(air) is dict and air.get('architecture') == 'arm64', 'LLVM AIR architecture record differs')
    require(all(type(air.get(key)) is str for key in ('version', 'host_target', 'libfiles', 'system_libs')),
            'LLVM AIR config fields missing')
    libfiles = shlex.split(air['libfiles'])
    require(bool(libfiles) and all(Path(value).is_absolute() and Path(value).parent.name == 'lib'
            for value in libfiles), 'LLVM AIR archive paths differ')
    old_prefix = Path(libfiles[0]).parent.parent
    def relocate(value):
        if value in SDK_LIBS or value == '-lzstd':
            return value
        require(Path(value).is_absolute() and Path(value).is_relative_to(old_prefix),
                'LLVM AIR path leaves recorded prefix')
        part = relative(Path(value).relative_to(old_prefix).as_posix())
        require(str(part) in index and index[str(part)]['symlink'] is None, 'LLVM AIR archive missing or symlink')
        return str(prefix.resolve() / str(part))
    values = {key: air[key] for key in ('version', 'host_target')}
    values['libfiles'] = shlex.join([relocate(value) for value in libfiles])
    values['system_libs'] = shlex.join([relocate(value) for value in shlex.split(air['system_libs'])])
    actual = llvm.validate_config(values, prefix)
    require(air.get('selected_archives') == actual['selected_archives'], 'LLVM static AIR selection/digests differ')
    installed = record.get('air_installed_archives')
    require(type(installed) is list and bool(installed), 'LLVM AIR install receipt missing')
    # Producer records each copied archive as the same path/bytes/SHA tuple.
    for row in installed:
        require(type(row) is dict and type(row.get('path')) is str, 'LLVM AIR install row invalid')
        part = str(relative(row['path']))
        require(part in index and row == {key: index[part][key] for key in ('path', 'bytes', 'sha256')},
                'LLVM AIR copied archive differs')
    require(len({row['path'] for row in installed}) == len(installed), 'Duplicate LLVM AIR installed archive')
    names = sorted({row['path'] for row in actual['selected_archives']})
    require({row['path'] for row in installed} == {str(Path(value).relative_to(prefix.resolve()))
            for value in shlex.split(values['libfiles'])}, 'LLVM installed/selected AIR archive set differs')
    objects = sum(archive_architecture(prefix / name) for name in names)
    require(dep.inventory(prefix) == files, 'LLVM prefix drifted during archive inspection')
    require(not result_path.is_symlink() and llvm.sha(result_path) == expected_sha256,
            'LLVM RESULT drifted during input inspection')
    return dict(schema=1, status='LLVM_PREFIX_VERIFIED_NOT_DXMT_BUILT', result_sha256=expected_sha256,
                profile=profile, toolchain=copy.deepcopy(record['toolchain']),
                files=len(files), bytes=sum(row['bytes'] for row in files), architecture='arm64',
                source_pins=len(source_rows), selected_archives=len(names), archive_objects=objects,
                relocation='RELATIVE_ARCHIVE_DIGESTS_VERIFIED', source_execution=0, downloads=0,
                comparison='NOT_ENABLED', stands='NOT_ENABLED', install='skipped')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', required=True, type=Path)
    parser.add_argument('--result', required=True, type=Path)
    parser.add_argument('--result-sha256', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists() and not args.output.is_symlink(), 'LLVM acceptance output collision')
    result = validate(args.prefix, args.result, args.result_sha256)
    with args.output.open('x') as stream:
        stream.write(json.dumps(result, sort_keys=True, indent=2) + '\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
