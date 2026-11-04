#!/usr/bin/env python3
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('extract_test', HERE / 'extract.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
raw_root = struct.pack('<6I', 2, 0, 24, 0, 24, 1)
blob = b'DXBC' + bytes(16) + struct.pack('<4I', 1, 44 + len(raw_root), 1, 36) + b'RTS0' + struct.pack('<I', len(raw_root)) + raw_root
description = dict(schema='dx12-pso-state-v1', shader_identity_domain='application_bytecode_before_translation',
                   RootSignature=dict(status='PRESENT', length=len(blob), sha1=hashlib.sha1(blob).hexdigest(), blob_hex=blob.hex()), InputLayout=[])
payload = json.dumps(description, separators=(',', ':')).encode()
digest = hashlib.sha1(payload).hexdigest()
prefix = 'dx12_pso_capture attempt=1 owner=00000100 op='
lines = [f'{prefix}state.begin schema=1 status=PRESENT bytes={len(payload)} sha1={digest} input_elements=0 root=PRESENT']
for offset in range(0, len(payload), 128): lines.append(f'{prefix}state.chunk offset={offset} hex={payload[offset:offset+128].hex()}')
lines.append(f'{prefix}state.end bytes={len(payload)} sha1={digest}')
raw = ('\n'.join(lines) + '\n').encode()
parsed = m.decode(raw)
assert parsed['complete'] and parsed['rows'][0]['decoded_root'] == dict(Version=2, Flags=1, Parameters=[], StaticSamplers=[])
empty = m.decode(b'ordinary unrelated output\xff\n')
assert not empty['complete'] and empty['coverage'] == 'NOT_ENABLED_OR_MISSING' and not empty['rows']
assert not m.decode((prefix + 'state.failed status=FAILED reason=total_description_limit\n').encode())['complete']
assert not m.decode(raw + (prefix.replace('attempt=1', 'attempt=2') + 'begin root=1\n').encode())['complete']
assert not m.decode(raw + (prefix.replace('attempt=1', 'attempt=2049') + 'limit truncated=1 attempt_limit=2048\n').encode())['complete']
negative = [b'\n'.join(raw.splitlines()[:-1]), raw.replace(b'offset=128', b'offset=129'),
            raw.replace((digest + '\n').encode(), ('0' * 40 + '\n').encode()),
            raw.replace(lines[1].encode(), (lines[1] + '\n' + lines[1]).encode()),
            raw.replace(lines[0].encode(), (lines[0] + '\n' + lines[0]).encode()),
            raw.replace(b'op=state.begin', b'op=state.unknown'), raw.replace(b'root=PRESENT', b'root=PRESENT\x00')]
for index, bad in enumerate(negative):
    try: m.decode(bad)
    except (AssertionError, KeyError, ValueError): pass
    else: raise AssertionError(('accepted damaged trace', index))
bad_blob = bytearray(blob)
struct.pack_into('<I', bad_blob, 32, 0xffffffff)
try: m.rootsig(bad_blob)
except (AssertionError, struct.error): pass
else: raise AssertionError('out-of-bounds root accepted')
print('PASS controls=13; no Wine/GPU; missing/chopped/corrupted/limited traces rejected')
