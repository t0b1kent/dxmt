#!/usr/bin/env python3
"""Own buffer protocol example, with an in-memory check and explicit export mode."""
import argparse
import json
from pathlib import Path
import plistlib


def records():
    document = json.loads(Path(__file__).with_name('synthetic-events.json').read_text())
    output = []
    for row in document['records']:
        row = dict(row, fields=dict(row['fields']))
        fields = row['fields']
        if 'bytes_hex' in fields:
            fields['bytes'] = bytes.fromhex(fields.pop('bytes_hex'))
        if 'snapshots' in fields:
            fields['snapshots'] = [dict(s, bytes=bytes.fromhex(s['bytes_hex'])) for s in fields['snapshots']]
            for s in fields['snapshots']:
                del s['bytes_hex']
        output.append(row)
    assert [r['sequence'] for r in output] == list(range(len(output)))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument('--check', action='store_true')
    modes.add_argument('--destination', type=Path)
    args = parser.parse_args()
    rows = records()
    for row in rows:
        encoded = plistlib.dumps(row, fmt=plistlib.FMT_BINARY)
        assert plistlib.loads(encoded) == row
    if args.destination:
        args.destination.mkdir(exist_ok=False)
        for row in rows:
            (args.destination / f'event-{row["sequence"]:08d}.plist').write_bytes(plistlib.dumps(row, fmt=plistlib.FMT_BINARY))
    print('SYNTHETIC_FORMAT_ONLY PASS: 7 own buffer events; draw/frame/native replay NOT_ENABLED')


if __name__ == '__main__':
    main()
