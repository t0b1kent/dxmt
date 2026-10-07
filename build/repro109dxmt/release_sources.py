"""Reconstruct retained DXMT source bytes from a pinned public Git revision.

Only Git object reads and pinned text inputs are used. Source code is not executed.
The two PNG fixtures come from unchanged public Git blobs. One text input has an
explicit transport LF; its retained postimage lacks that byte, as pinned below.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess

HERE = Path(__file__).resolve().parent
MAX_BYTES = 16 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def source_root():
    root = Path(os.environ.get('MACRUNNER_DXMT_SOURCE_REPOSITORY', str(HERE.parents[1]))).absolute()
    require(root.is_dir() and not root.is_symlink() and root.resolve() == root,
            'Public DXMT Git root must be an existing directory without symlinks')
    return root


def git(root, args, data=None):
    result = subprocess.run(['git', '-c', 'core.hooksPath=/dev/null',
                             '-c', 'core.fsmonitor=false', '-C', str(root), *args],
                            input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    require(result.returncode == 0 and len(result.stdout) <= MAX_BYTES,
            'Pinned public Git object read failed or exceeded the source cap')
    return result.stdout


def relative(name):
    require(type(name) is str, 'Source path must be a string')
    path = PurePosixPath(name)
    require(path.parts and not path.is_absolute() and '..' not in path.parts
            and '\\' not in name and path.as_posix() == name,
            'Unsafe or noncanonical source path')
    return path


def inventory(rows):
    records = [(name, hashlib.sha256(data).hexdigest(), len(data)) for name, data in sorted(rows)]
    payload = ''.join(f'{name}\0{digest}\0{size}\n' for name, digest, size in records)
    return dict(files=len(records), bytes=sum(row[2] for row in records),
                sha256=hashlib.sha256(payload.encode()).hexdigest())


def base_rows(root=None):
    root = source_root() if root is None else Path(root)
    lock = json.loads((HERE / 'git-basis.lock.json').read_bytes())
    require(type(lock) is dict and lock.get('schema') == 1, 'Git basis lock schema differs')
    revision, tree = lock.get('revision'), lock.get('tree')
    require(type(revision) is str and re.fullmatch('[0-9a-f]{40}', revision)
            and type(tree) is str and re.fullmatch('[0-9a-f]{40}', tree), 'Immutable Git pins required')
    require(Path(git(root, ['rev-parse', '--show-toplevel']).decode().strip()).resolve() == root.resolve(),
            'Selected source repository is a different Git worktree')
    require(git(root, ['rev-parse', revision + '^{tree}']).decode().strip() == tree,
            'Pinned public source tree differs')
    public = {}
    for raw in git(root, ['ls-tree', '-rz', '--full-tree', revision]).split(b'\0'):
        if raw:
            meta, name = raw.split(b'\t', 1)
            mode, kind, object_id = meta.decode().split()
            if kind == 'blob':
                public[name.decode()] = object_id
    members = lock.get('files')
    require(type(members) is list and len(members) == 336, 'Retained Git basis member count differs')
    names, objects = set(), []
    for row in members:
        require(type(row) is dict, 'Invalid basis member')
        name = relative(row.get('path')).as_posix()
        require(name not in names and row.get('mode') in ('100644', '100755'), 'Duplicate or special source member')
        names.add(name)
        require(type(row.get('bytes')) is int and 0 <= row['bytes'] <= MAX_BYTES,
                'Invalid source byte count')
        require(type(row.get('sha256')) is str and re.fullmatch('[0-9a-f]{64}', row['sha256']),
                'Invalid source digest')
        require(('git_blob' in row) != ('input' in row), 'Exactly one source origin required')
        if 'git_blob' in row:
            require(public.get(name) == row['git_blob'], 'Selected blob is outside the pinned public tree')
            objects.append(row['git_blob'])
    blobs = {}
    if objects:
        raw = git(root, ['cat-file', '--batch'], ''.join(obj + '\n' for obj in objects).encode())
        position = 0
        for expected in objects:
            end = raw.find(b'\n', position)
            require(end >= position, 'Truncated Git batch header')
            object_id, kind, size = raw[position:end].decode().split()
            require(object_id == expected and kind == 'blob' and size.isdecimal(), 'Unexpected Git batch object')
            size = int(size); position = end + 1
            require(size <= MAX_BYTES and position + size < len(raw)
                    and raw[position + size:position + size + 1] == b'\n', 'Truncated Git batch payload')
            blobs[expected] = raw[position:position + size]; position += size + 1
        require(position == len(raw), 'Extra Git batch bytes')
    rows = []
    for row in members:
        if 'git_blob' in row:
            data = blobs[row['git_blob']]
        else:
            path = HERE / 'inputs' / relative(row['input'])
            require(path.is_file() and not path.is_symlink()
                    and path.resolve().is_relative_to((HERE / 'inputs').resolve()), 'Missing or foreign text postimage')
            data = path.read_bytes()
            if row.get('input_trim_final_lf') is True:
                require(data.endswith(b'\n'), 'Explicit text transport LF missing')
                data = data[:-1]
        require((len(data), hashlib.sha256(data).hexdigest()) == (row['bytes'], row['sha256']),
                'Retained source postimage differs: ' + row['path'])
        rows.append((row['path'], data))
    require(inventory(rows) == lock.get('snapshot'), 'Retained source basis inventory differs')
    return rows


def native_rows(root=None):
    rows = dict(base_rows(root))
    lock = json.loads((HERE / 'sources.lock.json').read_bytes())
    for overlay in lock['native_overlays']:
        name = relative(overlay['path']).as_posix()
        require(name in rows, 'Native overlay selects an unknown source path')
        path = HERE / relative(overlay['source'])
        require(path.is_file() and not path.is_symlink(), 'Missing native text overlay')
        data = path.read_bytes()
        require(hashlib.sha256(data).hexdigest() == overlay['sha256'], 'Native overlay digest differs')
        rows[name] = data
    return sorted(rows.items())


def pe_overlay_rows():
    root = HERE / 'inputs/pe-overlay'
    require(root.is_dir() and not root.is_symlink(), 'PE text overlay root missing')
    rows = []
    for path in sorted(root.rglob('*')):
        require(not path.is_symlink(), 'PE overlay symlink rejected')
        if path.is_file():
            rows.append((relative(path.relative_to(root).as_posix()).as_posix(), path.read_bytes()))
    require(len(rows) == 7, 'PE text overlay count differs')
    return rows
