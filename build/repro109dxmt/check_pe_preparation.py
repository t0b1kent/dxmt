#!/usr/bin/env python3
"""Real source preparation + no-overwrite controls; retain every result on disk."""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from verify_sources import snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--kind', choices=['pe', 'native'], default='pe')
    args = parser.parse_args()
    out = args.out
    if out.is_absolute() or '..' in out.parts or out == Path('.') or out.exists():
        parser.error('--out must be a new relative evidence directory')
    out.mkdir(parents=True)
    lock = json.loads((HERE / 'sources.lock.json').read_text())
    expected = {key: lock[args.kind + '_snapshot'][key]
                for key in ['files', 'bytes', 'sha256']}
    controls = []
    frozen_inventories = []
    for number, (python, optimized) in enumerate([
            (sys.executable, False), (sys.executable, True),
            ('/usr/bin/python3', False), ('/usr/bin/python3', True)]):
        source_out = out / ('case-' + str(number))
        command = [python, '-B', '-I'] + (['-O'] if optimized else [])
        command += [str(HERE / ('prepare_' + args.kind + '.py')), '--out', str(source_out)]
        for attempt in ['prepare', 'collision']:
            result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=45)
            raw = dict(argv=command, rc=result.returncode,
                       stdout_b64=base64.b64encode(result.stdout).decode('ascii'),
                       stderr_b64=base64.b64encode(result.stderr).decode('ascii'))
            (out / f'{number}-{attempt}-raw.json').write_text(
                json.dumps(raw, indent=2, sort_keys=True) + '\n')
            actual = snapshot(source_out / 'source')
            try:
                answer = json.loads(result.stdout)
            except (ValueError, UnicodeError):
                answer = {}
            passed = actual == expected and (
                (attempt == 'prepare' and result.returncode == 0
                  and answer.get('status') == args.kind.upper() + '_SOURCE_PREPARED_NOT_BUILT') or
                (attempt == 'collision' and result.returncode == 1
                 and 'already exists' in answer.get('error', '')))
            controls.append(dict(python=python, optimized=optimized, case=attempt,
                                 passed=passed, actual=actual,
                                 raw=f'{number}-{attempt}-raw.json'))
        command = [python, '-B', '-I'] + (['-O'] if optimized else [])
        command += [str(HERE / 'freeze_version.py'), '--prepared', str(source_out)]
        wrong = command + ['--kind', 'native' if args.kind == 'pe' else 'pe']
        result = subprocess.run(wrong, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
        raw = dict(argv=wrong, rc=result.returncode,
                   stdout_b64=base64.b64encode(result.stdout).decode('ascii'),
                   stderr_b64=base64.b64encode(result.stderr).decode('ascii'))
        raw_name = f'{number}-wrong-kind-raw.json'
        (out / raw_name).write_text(json.dumps(raw, indent=2, sort_keys=True) + '\n')
        answer = json.loads(result.stdout)
        actual = snapshot(source_out / 'source')
        passed = (result.returncode == 1 and actual == expected
                  and 'source kind/digest differs' in answer.get('error', '')
                  and not (source_out / 'VERSION.json').exists()
                  and not (source_out / 'meson.build.before-version').exists())
        controls.append(dict(python=python, optimized=optimized, case='wrong-kind',
                             passed=passed, actual=actual, raw=raw_name))
        command += ['--kind', args.kind]
        for attempt in ['freeze', 'freeze-collision']:
            result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=45)
            raw = dict(argv=command, rc=result.returncode,
                       stdout_b64=base64.b64encode(result.stdout).decode('ascii'),
                       stderr_b64=base64.b64encode(result.stderr).decode('ascii'))
            (out / f'{number}-{attempt}-raw.json').write_text(
                json.dumps(raw, indent=2, sort_keys=True) + '\n')
            answer = json.loads(result.stdout)
            actual = snapshot(source_out / 'source')
            version = json.loads((source_out / 'VERSION.json').read_text())
            before = (source_out / 'meson.build.before-version').read_bytes()
            after = (source_out / 'source/meson.build').read_bytes()
            expected_after = before.replace(b"command: ['git', 'describe', '--always'],",
                b"command: ['/usr/bin/printf', '%s', '1226f44'],")
            passed = (actual == version['source_after'] and after == expected_after
                       and version['source_before'] == expected
                       and version['source_kind'] == args.kind
                      and version['expected_version_h_sha256'] ==
                      lock['historical_version']['version_h_sha256'] and (
                (attempt == 'freeze' and result.returncode == 0
                 and answer.get('status') == 'VERSION_COMMAND_FROZEN_NOT_CONFIGURED') or
                (attempt == 'freeze-collision' and result.returncode == 1
                 and 'already exists' in answer.get('error', ''))))
            if attempt == 'freeze':
                frozen_inventories.append(actual)
            controls.append(dict(python=python, optimized=optimized, case=attempt,
                                 passed=passed, actual=actual,
                                 raw=f'{number}-{attempt}-raw.json'))
    identical = len(frozen_inventories) == 4 and all(
        row == frozen_inventories[0] for row in frozen_inventories)
    report = dict(controls=controls, passed=sum(x['passed'] for x in controls),
                  total=len(controls), frozen_trees_identical=identical, source_kind=args.kind,
                  source_builds=0, downloads=0, install='skipped')
    (out / 'CONTROLS.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps(dict(passed=report['passed'], total=report['total'],
                         receipt=str(out / 'CONTROLS.json'), install='skipped'), sort_keys=True))
    return 0 if report['passed'] == report['total'] and identical else 1


if __name__ == '__main__':
    sys.exit(main())
