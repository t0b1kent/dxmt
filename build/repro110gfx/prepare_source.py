#!/usr/bin/env python3
"""Reconstruct the c32 graphics compilation inputs from public Git and sealed text postimages."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import subprocess

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]


def identity(data):
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def relative(name):
    path = PurePosixPath(name)
    if not path.parts or path.is_absolute() or any(p in ['.', '..'] for p in path.parts) or path.as_posix() != name:
        raise ValueError('Unsafe source path')
    return path


def git_blobs(names, repository=REPOSITORY):
    names = sorted(set(names))
    if not names:
        return {}
    result = subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', '-C', str(repository), 'cat-file', '--batch'],
                            input=('\n'.join(names) + '\n').encode(), capture_output=True, timeout=60, check=True)
    if len(result.stdout) > 32 * 1024 * 1024:
        raise ValueError('Source object budget exceeded')
    stream = io.BytesIO(result.stdout)
    values = {}
    for name in names:
        header = stream.readline().decode().split()
        if len(header) != 3 or header[:2] != [name, 'blob']:
            raise ValueError('Missing public source blob: ' + name)
        size = int(header[2])
        if not 0 <= size <= 16 * 1024 * 1024:
            raise ValueError('Source object size exceeded')
        data = stream.read(size)
        if len(data) != size or stream.read(1) != b'\n':
            raise ValueError('Truncated public source blob')
        actual = hashlib.sha1(b'blob ' + str(size).encode() + b'\0' + data).hexdigest()
        if actual != name:
            raise ValueError('Public source blob identity differs')
        values[name] = data
    if stream.read():
        raise ValueError('Unexpected public source object data')
    return values


def file_bytes(row, blobs, headers, here=HERE, zlib=None):
    if 'git_blob' in row:
        data = blobs[row['git_blob']]
    elif 'store' in row:
        name = relative(row['store'])
        path = here.joinpath(*name.parts)
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(here.resolve()):
            raise ValueError('Missing or foreign source postimage: ' + row['path'])
        data = path.read_bytes()
        if identity(data) != row['stored']:
            raise ValueError('Stored source differs: ' + row['path'])
        if row['transport_lf']:
            if not data.endswith(b'\n'):
                raise ValueError('Source transport LF missing')
            data = data[:-1]
    elif 'zlib' in row:
        if zlib is None:
            raise ValueError('Pinned upstream zlib source checkout required')
        name = relative(row['zlib'])
        path = zlib.joinpath(*name.parts)
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(zlib.resolve()):
            raise ValueError('Missing or foreign upstream zlib source')
        data = path.read_bytes()
    elif 'directx' in row:
        if headers is None:
            raise ValueError('Pinned DirectX source checkout required')
        name = relative(row['directx'])
        path = headers.joinpath(*name.parts)
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(headers.resolve()):
            raise ValueError('Missing or foreign DirectX header')
        data = path.read_bytes()
    else:
        raise ValueError('Unknown source input')
    if identity(data) != {k: row[k] for k in ['bytes', 'sha256']}:
        raise ValueError('Product source differs: ' + row['path'])
    return data


def prepare(variant, headers, output=None, *, zlib=None, here=HERE, repository=REPOSITORY):
    lock = json.loads((here / 'source.lock.json').read_bytes())
    if lock['schema'] != 1 or variant not in lock['variants']:
        raise ValueError('Unknown graphics source variant')
    rows = lock['variants'][variant]['files']
    if len({r['path'] for r in rows}) != len(rows):
        raise ValueError('Duplicate source path')
    for row in rows:
        relative(row['path'])
    blobs = git_blobs([r['git_blob'] for r in rows if 'git_blob' in r], repository)
    if headers is not None:
        headers = Path(headers).resolve()
        actual = subprocess.check_output(['git', '-C', str(headers), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != lock['directx']['revision']:
            raise ValueError('DirectX source revision differs')
    if zlib is not None:
        zlib = Path(zlib).resolve()
        actual = subprocess.check_output(['git', '-C', str(zlib), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != lock['zlib']['revision']:
            raise ValueError('Upstream zlib source revision differs')
    if output is not None:
        output = Path(output)
        if output.exists() or output.is_symlink():
            raise ValueError('Fresh source output required')
    contents = [(row, file_bytes(row, blobs, headers, here, zlib)) for row in rows]
    if output is not None:
        output.mkdir(parents=True)
        for row, data in contents:
            path = output / row['path']
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o755 if row['executable'] else 0o644)
    result = dict(status='EXACT_PRODUCT_SOURCE_PREPARED' if output is not None else 'EXACT_PRODUCT_SOURCE_VERIFIED',
                  variant=variant, files=len(rows), bytes=sum(len(data) for row, data in contents),
                  directx_revision=lock['directx']['revision'], public_base=lock['public_base'],
                  zlib_revision=lock['zlib']['revision'],
                  git_describe=lock['variants'][variant]['git_describe'], compilation='NOT_ENABLED', install='skipped')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=['c17', 'native'], required=True)
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--zlib', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.variant, args.headers, args.out, zlib=args.zlib), sort_keys=True))


if __name__ == '__main__':
    main()
