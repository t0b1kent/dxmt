"""Bind the separate public Wine recipe by its already delivered file pins."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent


def recipe_root():
    value = os.environ.get('MACRUNNER_WINE_RECIPE_ROOT')
    if not value:
        raise ValueError('Set MACRUNNER_WINE_RECIPE_ROOT to the sibling Wine checkout build directory')
    root = Path(value).absolute()
    if root.is_symlink() or not root.is_dir() or root.resolve() != root:
        raise ValueError('Wine recipe root must be an existing directory without symlink components')
    lock = json.loads((HERE / 'wine-recipe.lock.json').read_bytes())
    if lock.get('schema') != 1 or lock.get('changed_count') != 96:
        raise ValueError('Unexpected delivered Wine recipe lock')
    rows = lock.get('files')
    if type(rows) is not list or len(rows) != 96:
        raise ValueError('Wine recipe pin coverage differs')
    names = set()
    for row in rows:
        if type(row) is not dict or type(row.get('path')) is not str:
            raise ValueError('Invalid Wine recipe pin row')
        relative = PurePosixPath(row['path'])
        if (relative.is_absolute() or '..' in relative.parts or '.' in relative.parts
                or row['path'] != relative.as_posix() or row['path'] in names
                or not relative.parts or relative.parts[0] not in ('build', '.github')):
            raise ValueError('Invalid or duplicate Wine recipe pin path')
        names.add(row['path'])
        target = root.parent.joinpath(*relative.parts)
        if (not target.is_file() or target.is_symlink()
                or not target.resolve().is_relative_to(root.parent)):
            raise ValueError('Missing or foreign Wine recipe input: ' + row['path'])
        raw = target.read_bytes()
        if len(raw) != row['bytes'] or hashlib.sha256(raw).hexdigest() != row['sha256']:
            raise ValueError('Wine recipe input drift: ' + row['path'])
    return root
