"""Use the existing guarded producers for independent LLVM source and SDK checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import traceback

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_llvm15 as producer


def check(axis):
    full = producer.load('dxmt_axis_full', producer.REPO / 'repro109wine/build_full.py')
    full.cloud_guard('github-macos15-arm64')
    out = Path('reports')
    out.mkdir(exist_ok=True)
    result = dict(axis=axis, status='STARTED', first_failure=None, install='skipped',
                  compilation='NOT_ENABLED', source_execution='NOT_ENABLED')
    try:
        if axis == 'llvm-source':
            pin = producer.llvm_source_pin()
            archive = Path('llvm15.tar.xz')
            producer.public_archive.download(pin, archive, out)
            source, graph = producer.unpack_source(archive, Path('llvm15-source'), pin['sha256'])
            result.update(source_pin=pin, graph=graph,
                          archive_sha256=producer.sha(archive))
        elif axis == 'toolchain':
            chosen = None
            for app in sorted(Path('/Applications').glob('Xcode*.app')):
                developer = app / 'Contents/Developer'
                env = dict(os.environ, DEVELOPER_DIR=str(developer))
                command = subprocess.run(['xcodebuild', '-version'], env=env,
                    stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
                if command.returncode == 0 and command.stdout.strip() == b'Xcode 26.3\nBuild version 17C529':
                    chosen = developer
                    break
            if chosen is None:
                raise ValueError('Pinned Xcode 26.3/17C529 missing')
            os.environ['DEVELOPER_DIR'] = str(chosen)
            dep = producer.load('dxmt_axis_deps', producer.REPO / 'repro109deps/build_deps.py')
            lock = full.apply_profile(dep.read_lock(), 'github-macos15-arm64')
            sdk, cc, cxx = dep.toolchain_preflight(lock['toolchain'], out)
            result.update(toolchain=lock['toolchain'], sdk_verified=True)
        else:
            raise ValueError('Unknown cloud prerequisite')
        result['status'] = 'VERIFIED_NOT_COMPILED'
    except Exception as error:
        result.update(status='FAILED', first_failure=dict(type=type(error).__name__, error=str(error)[:1600]))
        raise
    finally:
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('axis', choices=('llvm-source', 'toolchain'))
    args = parser.parse_args()
    try:
        print(json.dumps(check(args.axis), sort_keys=True))
    except Exception:
        traceback.print_exc()
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
