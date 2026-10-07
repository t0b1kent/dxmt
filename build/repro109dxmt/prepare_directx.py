#!/usr/bin/env python3
"""Complete a prepared DXMT source tree with its exact DirectX gitlink.

Fetch is cloud-only. Local preparation reads an existing checkout and never
executes a header, compiler, build script, hook, or downloaded executable.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
from wine_layout import recipe_root
REPO = recipe_root()
from verify_sources import snapshot, verify

PIN = dict(
    repository='https://github.com/misyltoad/mingw-directx-headers.git',
    revision='9df86f2341616ef1888ae59919feaa6d4fad693d',
    tree='66fcb250507a2c45bd9a22f44795d0e4c7b613f5',
    files=66, bytes=4140678,
    sha256='cbe93899e4bc277c4aeb90d35641f4f8c3e32a15a21b89357e8d7278eef2738d',
    license_path='COPYING.MinGW-w64.txt',
    license_sha256='f38e6194bd3bfa1b654f118e5acefe0aead437bbe669eee43957ccc65a7127f1',
)
MAX_RAW = 1024 * 1024


def relative(path):
    path = Path(path)
    if path.is_absolute() or '..' in path.parts or path == Path('.'):
        raise ValueError('A relative path inside the checkout is required')
    for ancestor in [path, *path.parents]:
        if ancestor.is_symlink():
            raise ValueError('Path contains a symlink')
    return path


def outside_headers(source):
    rows = []
    for path in sorted(source.rglob('*')):
        name = path.relative_to(source).as_posix()
        if name.startswith('include/native/directx/'):
            continue
        if path.is_file():
            data = path.read_bytes()
            rows.append((name, hashlib.sha256(data).hexdigest(), len(data)))
    payload = ''.join(f'{name}\0{digest}\0{size}\n' for name, digest, size in rows)
    return dict(files=len(rows), bytes=sum(row[2] for row in rows),
                sha256=hashlib.sha256(payload.encode()).hexdigest())


def git(checkout, *args):
    env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null',
               GIT_CONFIG_SYSTEM='/dev/null', GIT_TERMINAL_PROMPT='0', GIT_OPTIONAL_LOCKS='0')
    command = ['git', '-c', 'core.hooksPath=/dev/null', '-c', 'core.fsmonitor=false',
               '-c', 'core.autocrlf=false', *args]
    process = subprocess.run(command, cwd=checkout, env=env,
                             stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    if process.returncode or len(process.stdout) > MAX_RAW or len(process.stderr) > MAX_RAW:
        raise ValueError('Git input verification failed or exceeded the output cap')
    return process.stdout


def checked_headers(checkout):
    checkout = relative(checkout)
    if not checkout.is_dir():
        raise ValueError('Header checkout missing')
    identities = git(checkout, 'rev-parse', 'HEAD^{commit}', 'HEAD^{tree}').decode().splitlines()
    if identities != [PIN['revision'], PIN['tree']]:
        raise ValueError('DirectX commit/tree differs from the accepted gitlink')
    # git status alone cannot prove bytes (assume-unchanged / skip-worktree).
    if git(checkout, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Dirty DirectX checkout refused')
    tree = git(checkout, 'ls-tree', '-rz', 'HEAD')
    rows = []
    for entry in tree.split(b'\0'):
        if not entry:
            continue
        meta, raw_name = entry.split(b'\t', 1)
        mode, kind, oid = meta.split()
        name = raw_name.decode('utf-8')
        # The pinned tree is flat, regular, non-executable and has no submodules.
        if mode != b'100644' or kind != b'blob' or Path(name).name != name or name in ('.', '..'):
            raise ValueError('Foreign DirectX tree member')
        path = checkout / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 8 * 1024**2:
            raise ValueError('DirectX member is missing, oversized or a symlink')
        data = path.read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if blob != oid.decode('ascii'):
            raise ValueError('DirectX worktree bytes differ from their Git blob')
        rows.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    rows.sort(key=lambda row: row['path'])
    digest = hashlib.sha256(''.join(
        row['path'] + '\0' + row['sha256'] + '\0' + str(row['bytes']) + '\n'
        for row in rows).encode()).hexdigest()
    measured = dict(files=len(rows), bytes=sum(row['bytes'] for row in rows), sha256=digest)
    if measured != {key: PIN[key] for key in ('files', 'bytes', 'sha256')}:
        raise ValueError('DirectX file census differs from the accepted tree')
    if not any(row['path'] == 'd3d11.h' for row in rows):
        raise ValueError('Required DirectX d3d11.h is absent')
    license_row = next((row for row in rows if row['path'] == PIN['license_path']), None)
    if not license_row or license_row['sha256'] != PIN['license_sha256']:
        raise ValueError('DirectX license bytes differ')
    return dict(commit=identities[0], tree=identities[1], census=measured, files=rows)


def cloud_fetch(checkout, profile):
    # The existing guard runs before mkdir, Git fetch, or any payload execution.
    sys.path.insert(0, str(REPO / 'repro109wine'))
    from build_full import cloud_guard
    cloud_guard(profile)
    checkout = relative(checkout)
    if checkout.exists():
        raise ValueError('Fresh header download path required; no overwrite/retry')
    checkout.mkdir(parents=True)
    reports = checkout.parent / (checkout.name + '-fetch-reports')
    if reports.exists() or reports.is_symlink():
        raise ValueError('Header download report collision')
    reports.mkdir()
    env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_SYSTEM='/dev/null',
               GIT_TERMINAL_PROMPT='0')
    commands = [
        ['init', '--quiet'],
        ['fetch', '--quiet', '--depth=1', '--no-tags', PIN['repository'], PIN['revision']],
        ['checkout', '--quiet', '--detach', 'FETCH_HEAD'],
    ]
    for index, args in enumerate(commands):
        argv = ['git', '-c', 'core.hooksPath=/dev/null', '-c', 'core.fsmonitor=false',
                '-c', 'core.autocrlf=false', *args]
        stdout, stderr = reports / f'{index}.stdout', reports / f'{index}.stderr'
        state, code = 'PRESENT', None
        try:
            with stdout.open('xb') as out, stderr.open('xb') as err:
                result = subprocess.run(argv, cwd=checkout, env=env,
                                        stdin=subprocess.DEVNULL, stdout=out, stderr=err, timeout=120)
                code = result.returncode
        except subprocess.TimeoutExpired:
            state = 'TIMEOUT'
        receipt = dict(argv=argv, rc=code, state=state,
                       status='PASS' if code == 0 else 'FAILED',
                       stdout_state='PRESENT' if stdout.stat().st_size else 'EMPTY',
                       stderr_state='PRESENT' if stderr.stat().st_size else 'EMPTY',
                       stdout_bytes=stdout.stat().st_size, stderr_bytes=stderr.stat().st_size)
        (reports / f'{index}.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if code != 0 or stdout.stat().st_size > MAX_RAW or stderr.stat().st_size > MAX_RAW:
            raise ValueError('Official header fetch failed; raw evidence retained, no retry')
    return checked_headers(checkout)


def prepare(prepared, checkout, kind):
    verify()
    if kind not in ('native', 'pe'):
        raise ValueError('DXMT source kind must be native or pe')
    prepared = relative(prepared)
    checkout = relative(checkout)
    receipt = prepared / 'DIRECTX.json'
    if receipt.exists() or receipt.is_symlink():
        raise ValueError('DirectX preparation already exists; no overwrite')
    link = prepared / 'LLVM-LINK.json'
    if link.is_symlink():
        raise ValueError('LLVM source preparation receipt symlink refused')
    record = json.loads(link.read_bytes())
    source = prepared / 'source'
    before = snapshot(source)
    if (type(record) is not dict or record.get('status') != 'LLVM_LINK_PREPARED_NOT_CONFIGURED'
            or record.get('source_kind') != kind or record.get('source_after') != before):
        raise ValueError('DXMT source kind/digest differs; prepare LLVM link first')
    measured = checked_headers(checkout)
    destination = source / 'include/native/directx'
    if destination.is_symlink() or (destination.exists() and (not destination.is_dir() or any(destination.iterdir()))):
        raise ValueError('DirectX destination is not empty')
    destination.mkdir(parents=True, exist_ok=True)
    # Copy only the 66 verified files, never .git or the enclosing checkout.
    argv = ['/bin/cp', '-c', *[str(checkout / row['path']) for row in measured['files']], str(destination)]
    result = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, timeout=45)
    raw = dict(argv=argv, rc=result.returncode,
               stdout_b64=base64.b64encode(result.stdout).decode(),
               stderr_b64=base64.b64encode(result.stderr).decode())
    (prepared / 'directx-clone.raw.json').write_text(json.dumps(raw, indent=2) + '\n')
    if result.returncode:
        raise ValueError('APFS header clone failed; evidence retained, no copy fallback')
    if snapshot(destination) != measured['census'] or checked_headers(checkout) != measured:
        raise ValueError('DirectX copy or source changed during preparation')
    after = snapshot(source)
    if after['files'] != before['files'] + PIN['files'] or after['bytes'] != before['bytes'] + PIN['bytes']:
        raise ValueError('Unexpected change outside the DirectX destination')
    if outside_headers(source) != before:
        raise ValueError('Existing DXMT source bytes changed during DirectX preparation')
    report = dict(schema=1, status='DIRECTX_SOURCE_PREPARED_NOT_CONFIGURED', source_kind=kind,
                  source_before=before, source_after=after, headers=measured,
                  official_repository=PIN['repository'], clone='cp -c', git_metadata_copied=False,
                  license=dict(path='include/native/directx/' + PIN['license_path'], sha256=PIN['license_sha256']),
                  compilation='NOT_ENABLED', install='skipped')
    receipt.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--kind', choices=('native', 'pe'), required=True)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--profile', choices=('xcode-cloud', 'github', 'github-macos15-arm64', 'github-xcode27-arm64'),
                        default='github-macos15-arm64')
    args = parser.parse_args()
    try:
        if args.fetch:
            cloud_fetch(args.checkout, args.profile)
        result = prepare(args.prepared, args.checkout, args.kind)
        # Keep file census/bytes in the persisted receipt, not in the console.
        print(json.dumps(dict(status=result['status'], source_kind=args.kind,
                              source_after=result['source_after'], headers=result['headers']['census'],
                              compilation='NOT_ENABLED', install='skipped'), sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        print(json.dumps(dict(status='FAILED', error=str(error)[:1200]), sort_keys=True))
        return 1


if __name__ == '__main__':
    sys.exit(main())
