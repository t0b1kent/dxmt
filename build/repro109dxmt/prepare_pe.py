#!/usr/bin/env python3
"""Reconstruct the sealed D3D11 PE source tree without downloads or compilation."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from verify_sources import snapshot, verify
from release_sources import base_rows, pe_overlay_rows


def members(archive):
    """Only the already pinned, small regular-file release archives are accepted."""
    rows = []
    names = set()
    total = 0
    with tarfile.open(archive, 'r:xz') as stream:
        for item in stream.getmembers():
            path = PurePosixPath(item.name)
            if (path.is_absolute() or '..' in path.parts or '\\' in item.name
                    or not path.parts or path.parts[0] != 'dxmt'
                    or not (item.isfile() or item.isdir())):
                raise ValueError('unsupported source archive member: ' + item.name)
            relative = PurePosixPath(*path.parts[1:]).as_posix()
            if relative == '.':
                if not item.isdir():
                    raise ValueError('source archive root is not a directory')
                continue
            if relative in names:
                raise ValueError('duplicate source archive member: ' + relative)
            names.add(relative)
            if item.isfile():
                total += item.size
                if len(rows) >= 500 or total > 16 * 1024 * 1024:
                    raise ValueError('source archive exceeds bounded source inventory')
                data = stream.extractfile(item).read()
                if len(data) != item.size:
                    raise ValueError('source archive member is incomplete: ' + relative)
                rows.append((relative, data))
    return rows


def patch_paths(data):
    paths = []
    for line in data.decode('utf-8').splitlines():
        if line.startswith('diff --git '):
            match = re.fullmatch(r'diff --git a/([^ ]+) b/([^ ]+)', line)
            if not match or match[1] != match[2]:
                raise ValueError('unsupported release patch path/header')
            path = PurePosixPath(match[1])
            if path.is_absolute() or '..' in path.parts or '\\' in match[1]:
                raise ValueError('unsafe release patch path')
            paths.append(path.as_posix())
    if len(paths) != 25 or len(set(paths)) != len(paths):
        raise ValueError('release patch file coverage differs')
    return paths


def prepare(out):
    verified = verify()
    out = Path(out)
    if out.is_absolute() or '..' in out.parts or not out.parts or out == Path('.'):
        raise ValueError('output must be a new relative path under the current checkout')
    out = Path.cwd() / out
    for ancestor in [out, *out.parents]:
        if ancestor.is_symlink():
            raise ValueError('output path contains a symlink')
        if ancestor == Path.cwd():
            break
    if out.exists():
        raise ValueError('output already exists; preserved without overwrite')
    inputs = HERE / 'inputs'
    base = base_rows()
    overlay = pe_overlay_rows()
    patch = (inputs / 'dxmt-worktree-changes.patch').read_bytes()
    modified = patch_paths(patch)
    base_names = {name for name, _ in base}
    if len(base) != 336 or len(overlay) != 7:
        raise ValueError('release source file coverage differs')
    if base_names.intersection(name for name, _ in overlay):
        raise ValueError('untracked source overlay overwrites tracked source')
    if not set(modified).issubset(base_names):
        raise ValueError('release patch selects missing source files')
    out.mkdir(parents=True)
    source = out / 'source'
    source.mkdir()
    modes = {row['path']: 0o755 if row['mode'] == '100755' else 0o644
             for row in json.loads((HERE / 'git-basis.lock.json').read_bytes())['files']}
    for name, data in base:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(modes[name])
    args = ['/usr/bin/patch', '-t', '-F', '0', '-p', '1', '-i',
            str(inputs / 'dxmt-worktree-changes.patch')]
    result = subprocess.run(args, cwd=source, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=30)
    (out / 'patch.stdout').write_bytes(result.stdout)
    (out / 'patch.stderr').write_bytes(result.stderr)
    (out / 'patch-command.json').write_text(json.dumps(dict(
        argv=['/usr/bin/patch', '-t', '-F', '0', '-p', '1', '-i',
              'repro109dxmt/inputs/dxmt-worktree-changes.patch'],
        cwd='source', rc=result.returncode), sort_keys=True) + '\n')
    if result.returncode or re.search(rb'(?i)fuzz|offset|failed|reversed|skipping',
                                      result.stdout + result.stderr):
        raise ValueError('release patch did not apply exactly; raw output retained')
    # BSD patch retains preimages beside each modified file. Keep those raw bytes
    # outside the source inventory, and reject any unexpected additional files.
    before_overlay = {p.relative_to(source).as_posix() for p in source.rglob('*')
                      if p.is_file()}
    backups = before_overlay - base_names
    allowed = {name + '.orig' for name in modified}
    if not backups.issubset(allowed):
        raise ValueError('unexpected patch output files: ' + repr(sorted(backups)))
    for name in sorted(backups):
        dest = out / 'patch-preimages' / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        (source / name).rename(dest)
    for name, data in overlay:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o644)
    inventory = snapshot(source)
    lock = json.loads((HERE / 'sources.lock.json').read_text())
    expected = {key: lock['pe_snapshot'][key] for key in ['files', 'bytes', 'sha256']}
    if inventory != expected:
        raise ValueError('final PE source count/size/digest differs: ' + repr(inventory))
    report = dict(status='PE_SOURCE_PREPARED_NOT_BUILT', source=inventory,
                  verified_inputs=verified['inputs'], base_files=len(base),
                  patched_files=len(modified), added_files=len(overlay),
                  patch_preimages=len(backups),
                  patch_stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                  patch_stderr_sha256=hashlib.sha256(result.stderr).hexdigest(),
                  version='NOT_CHANGED: configure must explicitly pin 1226f44',
                  submodules='NOT_DOWNLOADED', downloads=0, builds=0,
                  install='skipped')
    (out / 'SOURCE.json').write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.out)
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        print(json.dumps(dict(status='FAILED', error=str(error)[:1200]), sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
