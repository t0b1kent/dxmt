#!/usr/bin/env python3
"""Freeze DXMT's own VCS tag in a verified, newly prepared native or PE tree."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from verify_sources import snapshot, verify


def freeze(out, source_kind='pe'):
    verify()
    if source_kind not in ['pe', 'native']:
        raise ValueError('source kind must be pe or native')
    out = Path(out)
    if out.is_absolute() or '..' in out.parts or out == Path('.'):
        raise ValueError('prepared directory must be a relative path')
    for path in [out, *out.parents]:
        if path.is_symlink():
            raise ValueError('prepared directory contains a symlink')
    lock = json.loads((HERE / 'sources.lock.json').read_text())
    receipt = out / 'VERSION.json'
    backup = out / 'meson.build.before-version'
    if receipt.exists() or backup.exists():
        raise ValueError('version output already exists; preserved without overwrite')
    prepared = json.loads((out / 'SOURCE.json').read_text())
    expected = {key: lock[source_kind + '_snapshot'][key]
                for key in ['files', 'bytes', 'sha256']}
    source = out / 'source'
    status = source_kind.upper() + '_SOURCE_PREPARED_NOT_BUILT'
    if (prepared['status'] != status
            or prepared['source'] != expected or snapshot(source) != expected):
        raise ValueError('prepared source kind/digest differs from the sealed release closure')
    target = source / 'meson.build'
    before = target.read_bytes()
    old = b"command: ['git', 'describe', '--always'],"
    tag = lock['historical_version']['tag']
    if tag != '1226f44' or before.count(old) != 1:
        raise ValueError('DXMT version command/tag differs')
    new = b"command: ['/usr/bin/printf', '%s', '1226f44'],"
    header = (source / 'version.h.in').read_bytes().replace(b'@VCS_TAG@', tag.encode('ascii'))
    header_sha = hashlib.sha256(header).hexdigest()
    if header_sha != lock['historical_version']['version_h_sha256']:
        raise ValueError('DXMT generated version header differs from historical builds')
    after = before.replace(old, new)
    backup.write_bytes(before)
    target.write_bytes(after)
    report = dict(status='VERSION_COMMAND_FROZEN_NOT_CONFIGURED', tag=tag,
                  source_kind=source_kind,
                  source_before=expected, source_after=snapshot(source),
                  meson_before_sha256=hashlib.sha256(before).hexdigest(),
                  meson_after_sha256=hashlib.sha256(after).hexdigest(),
                  expected_version_h_sha256=header_sha,
                  changed_files=['meson.build'], downloads=0, builds=0, install='skipped')
    receipt.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--kind', choices=['pe', 'native'], default='pe')
    args = parser.parse_args()
    try:
        result = freeze(args.prepared, args.kind)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps(dict(status='FAILED', error=str(error)[:1200]), sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
