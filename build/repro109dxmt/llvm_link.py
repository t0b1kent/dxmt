#!/usr/bin/env python3
"""Select fresh LLVM's static AIR link closure in a verified DXMT build copy."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from verify_sources import snapshot, verify

TARGET = 'src/airconv/darwin/meson.build'
BEFORE_SHA = {
    'native': '297cb172b02a9fdf38f47a804224b63f7b1f6459ea081f1fed4ac77509ba22c8',
    'pe': '7bdee39f72c085bd736d7b256fb5a8673d354b303bb267f66742fe0f1ff6ac1d',
}
OLD = """llvm_ld_flags_darwin = [
  '-L'+join_paths(native_llvm_path, 'lib'),
  '-lm', '-lz', '-lcurses', '-lxml2'
]

if native_llvm_path.startswith('/usr/local/opt')
  llvm_ld_flags_darwin = [
    llvm_ld_flags_darwin,
    '/usr/local/opt/zstd/lib/libzstd.a', '/usr/local/opt/llvm@15/lib/libunwind.a'
  ]
endif"""
OLD_PE = OLD.replace('endif', """elif native_llvm_path.startswith('/opt/homebrew/opt')
  llvm_ld_flags_darwin = [
    llvm_ld_flags_darwin,
    '/opt/homebrew/opt/zstd/lib/libzstd.a', '/opt/homebrew/opt/llvm@15/lib/libunwind.a'
  ]
endif""")
NEW = """# The builder verifies this source-built prefix before Meson starts.
# Native/PE configurations each query their selected architecture's LLVM.
native_llvm_config = find_program(join_paths(native_llvm_path, 'bin', 'llvm-config'), native: true)
native_llvm_version = run_command(native_llvm_config, '--version', check: true).stdout().strip()
if native_llvm_version != '15.0.7'
  error('Pinned LLVM 15.0.7 is required')
endif
llvm_air_libfiles = run_command(native_llvm_config, '--link-static', '--libfiles', 'bitwriter', 'passes', check: true).stdout().strip().split()
llvm_air_system_libs = run_command(native_llvm_config, '--link-static', '--system-libs', 'bitwriter', 'passes', check: true).stdout().strip().split()
llvm_air_system_flags = []
foreach llvm_air_system_lib : llvm_air_system_libs
  if llvm_air_system_lib == '-lzstd'
    llvm_air_zstd = join_paths(native_llvm_path, 'lib', 'libzstd.a')
    if not fs.is_file(llvm_air_zstd)
      error('Source-built static zstd is required')
    endif
    llvm_air_system_flags += [llvm_air_zstd]
  else
    llvm_air_system_flags += [llvm_air_system_lib]
  endif
endforeach
if llvm_air_libfiles.length() == 0
  error('LLVM static AIR library closure is empty')
endif
# libunwind is an explicit AIR/native C++ input, not an llvm-config component.
llvm_air_unwind = join_paths(native_llvm_path, 'lib', 'libunwind.a')
if not fs.is_file(llvm_air_unwind)
  error('Source-built static libunwind is required')
endif
llvm_ld_flags_darwin = llvm_air_libfiles + llvm_air_system_flags + [llvm_air_unwind]"""


def transform(before, kind='native'):
    if kind not in BEFORE_SHA or hashlib.sha256(before).hexdigest() != BEFORE_SHA[kind]:
        raise ValueError('Pinned Darwin AIR Meson source differs')
    text = before.decode('utf-8')
    link = '[ llvm_ld_flags_darwin, llvm_deps ]'
    old = OLD if kind == 'native' else OLD_PE
    if text.count(old) != 1 or text.count(link) != 2:
        raise ValueError('Darwin AIR link sites differ')
    return text.replace(old, NEW).replace(link, 'llvm_ld_flags_darwin').encode('utf-8')


def apply(prepared, kind):
    verify()
    if kind not in ('native', 'pe'):
        raise ValueError('Source kind must be native or pe')
    prepared = Path(prepared)
    if prepared.is_absolute() or '..' in prepared.parts or prepared == Path('.'):
        raise ValueError('Prepared path must be relative to the checkout')
    for ancestor in [prepared, *prepared.parents]:
        if ancestor.is_symlink():
            raise ValueError('Prepared path contains a symlink')
    receipt = prepared / 'LLVM-LINK.json'
    backup = prepared / 'airconv-darwin.meson.before-llvm-link'
    if receipt.exists() or receipt.is_symlink() or backup.exists() or backup.is_symlink():
        raise ValueError('LLVM link preparation already exists; no overwrite')
    version_path = prepared / 'VERSION.json'
    if version_path.is_symlink():
        raise ValueError('Version receipt symlink refused')
    version = json.loads(version_path.read_bytes())
    source = prepared / 'source'
    before_tree = snapshot(source)
    if (not isinstance(version, dict) or
            version.get('status') != 'VERSION_COMMAND_FROZEN_NOT_CONFIGURED' or
            version.get('source_kind') != kind or version.get('tag') != '1226f44' or
            version.get('source_after') != before_tree):
        raise ValueError('Frozen source kind/digest differs')
    target = source / TARGET
    before = target.read_bytes()
    after = transform(before, kind)
    backup.write_bytes(before)
    target.write_bytes(after)
    result = dict(schema=1, status='LLVM_LINK_PREPARED_NOT_CONFIGURED', source_kind=kind,
                  source_before=before_tree, source_after=snapshot(source),
                  changed_files=[TARGET], before_sha256=BEFORE_SHA[kind],
                  after_sha256=hashlib.sha256(after).hexdigest(),
                  llvm_version='15.0.7', components=['bitwriter', 'passes'],
                  library_selection='actual llvm-config --link-static --libfiles / --system-libs',
                  additional_static_input='lib/libunwind.a',
                  llvm_prefix_validation='REQUIRED_IN_FUTURE_BUILDER_NOT_IMPLEMENTED_HERE',
                  downloads=0, source_execution=0, builds=0, install='skipped')
    receipt.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=['native', 'pe'])
    args = parser.parse_args()
    try:
        result = apply(args.prepared, args.kind)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps(dict(status='FAILED', error=str(error)[:1200]), sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
