#!/usr/bin/env python3
"""Read-only verification of the separately sealed DXMT source inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import release_sources


def sha(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def snapshot(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError('native source root must be a regular directory')
    rows = []
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('native source symlink rejected: ' + path.relative_to(root).as_posix())
        if path.is_file():
            rows.append((path.relative_to(root).as_posix(), sha(path), path.stat().st_size))
        elif not path.is_dir():
            raise ValueError('native source special file rejected')
    payload = ''.join(f'{name}\0{digest}\0{size}\n' for name, digest, size in rows)
    return dict(files=len(rows), bytes=sum(row[2] for row in rows),
                sha256=hashlib.sha256(payload.encode('utf-8')).hexdigest())


def verify(root=None):
    lock = json.loads((HERE / 'sources.lock.json').read_text())
    if not isinstance(lock, dict) or not isinstance(lock.get('inputs'), dict):
        raise ValueError('source lock containers differ')
    native = release_sources.native_rows() if root is None else None
    actual = release_sources.inventory(native) if root is None else snapshot(root)
    expected = {key: lock['native_snapshot'][key] for key in ['files', 'bytes', 'sha256']}
    if actual != expected:
        raise ValueError(f'native source differs; expected={expected}; actual={actual}')
    inputs = HERE / 'inputs'
    if inputs.is_symlink() or not inputs.is_dir():
        raise ValueError('source input root differs')
    actual_names = sorted(path.relative_to(inputs).as_posix() for path in inputs.rglob('*') if path.is_file())
    if any(path.is_symlink() for path in inputs.rglob('*')):
        raise ValueError('source input symlink rejected')
    if actual_names != sorted(lock['inputs']):
        raise ValueError('source input names differ')
    for name, row in lock['inputs'].items():
        path = inputs / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('source input must be a regular file: ' + name)
        if dict(bytes=path.stat().st_size, sha256=sha(path)) != row:
            raise ValueError('source input digest/size differs: ' + name)
    for row in lock['native_overlays']:
        digest = hashlib.sha256(dict(native)[row['path']]).hexdigest() if root is None else sha(root / row['path'])
        if digest != row['sha256']:
            raise ValueError('native overlay differs: ' + row['path'])
    if sha(HERE / 'git-basis.lock.json') != lock['public_source']['basis_lock_sha256']:
        raise ValueError('public Git basis lock differs')
    return dict(status='SOURCE_INPUTS_VERIFIED_NOT_BUILT', snapshot=actual,
                inputs=len(lock['inputs']), overlays=len(lock['native_overlays']),
                downloads=0, builds=0, install='skipped')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-source', type=Path)
    args = parser.parse_args()
    try:
        result = verify(args.native_source)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps(dict(status='FAILED', error=str(error)[:1600]), sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
