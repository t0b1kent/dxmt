#!/usr/bin/env python3
"""Clone the sealed ARM64 native source basis; no network or compilation."""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from verify_sources import snapshot, verify
from release_sources import native_rows


def prepare(out):
    verified = verify()
    out = Path(out)
    if out.is_absolute() or '..' in out.parts or out == Path('.'):
        raise ValueError('output must be a new relative path under the current checkout')
    out = Path.cwd() / out
    for ancestor in [out, *out.parents]:
        if ancestor.is_symlink():
            raise ValueError('output path contains a symlink')
        if ancestor == Path.cwd():
            break
    if out.exists():
        raise ValueError('output already exists; preserved without overwrite')
    lock = json.loads((HERE / 'sources.lock.json').read_text())
    expected = {key: lock['native_snapshot'][key] for key in ['files', 'bytes', 'sha256']}
    out.mkdir(parents=True)
    source = out / 'source'
    source.mkdir()
    modes = {row['path']: 0o755 if row['mode'] == '100755' else 0o644
             for row in json.loads((HERE / 'git-basis.lock.json').read_bytes())['files']}
    for name, data in native_rows():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(modes[name])
    actual = snapshot(out / 'source')
    if actual != expected:
        raise ValueError('reconstructed native source differs; failed evidence preserved')
    report = dict(status='NATIVE_SOURCE_PREPARED_NOT_BUILT', source_kind='native',
                  source=actual, inputs_verified=verified['inputs'],
                  source_origin='pinned public Git plus exact text postimages',
                  downloads=0, builds=0, install='skipped')
    (out / 'SOURCE.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
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
