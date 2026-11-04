#!/usr/bin/env python3
"""Recover bounded PSO descriptions; reject gaps, duplicates, missing end records and hash mismatches."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

def rootsig(blob):
    def words(data, offset, count):
        assert 0 <= offset <= len(data) and count * 4 <= len(data) - offset, 'root bounds'
        return struct.unpack_from('<' + 'I' * count, data, offset)
    assert blob[:4] == b'DXBC' and len(blob) >= 32, 'root container'
    _, length, count = words(blob, 20, 3)
    assert length == len(blob) and count <= 64
    payloads = []
    for offset in words(blob, 32, count):
        size, = words(blob, offset + 4, 1)
        assert offset + 8 + size <= len(blob)
        if blob[offset:offset + 4] == b'RTS0': payloads.append(blob[offset + 8:offset + 8 + size])
    assert len(payloads) == 1, 'single RTS0 required'
    raw = payloads[0]
    version, nparams, params_at, nsamplers, samplers_at, flags = words(raw, 0, 6)
    assert version in (1, 2) and nparams <= 64 and nsamplers <= 2048
    result = dict(Version=version, Flags=flags, Parameters=[], StaticSamplers=[])
    for i in range(nparams):
        kind, visibility, at = words(raw, params_at + i * 12, 3)
        parameter = dict(ParameterType=kind, ShaderVisibility=visibility)
        if kind == 0:
            nranges, ranges_at = words(raw, at, 2)
            assert nranges <= 4096
            names = ['RangeType', 'NumDescriptors', 'BaseShaderRegister', 'RegisterSpace']
            if version == 2: names += ['Flags']
            names += ['OffsetInDescriptorsFromTableStart']
            parameter['DescriptorTable'] = [dict(zip(names, words(raw, ranges_at + j * len(names) * 4, len(names)))) for j in range(nranges)]
        elif kind == 1:
            parameter['Constants'] = dict(zip(['ShaderRegister', 'RegisterSpace', 'Num32BitValues'], words(raw, at, 3)))
        elif kind in (2, 3, 4):
            names = ['ShaderRegister', 'RegisterSpace'] + (['Flags'] if version == 2 else [])
            parameter['Descriptor'] = dict(zip(names, words(raw, at, len(names))))
        else: raise AssertionError('unknown root parameter')
        result['Parameters'].append(parameter)
    names = ['Filter', 'AddressU', 'AddressV', 'AddressW', 'MipLODBias_bits', 'MaxAnisotropy', 'ComparisonFunc',
             'BorderColor', 'MinLOD_bits', 'MaxLOD_bits', 'ShaderRegister', 'RegisterSpace', 'ShaderVisibility']
    for i in range(nsamplers): result['StaticSamplers'].append(dict(zip(names, words(raw, samplers_at + i * 52, 13))))
    return result

def decode(raw):
    active, completed, failures = {}, [], []
    graphics = set()
    pattern = re.compile(rb'^dx12_pso_capture attempt=(\d+) owner=(\S+) op=(\S+) (.*)$')
    for line_no, line in enumerate(raw.splitlines(), 1):
        if b'dx12_pso_capture' not in line: continue
        match = pattern.fullmatch(line)
        assert match is not None, ('malformed trace', line_no)
        attempt, owner, op, fields = match.groups()
        key = (int(attempt), owner.decode('ascii'))
        if op == b'begin':
            assert key not in graphics, 'duplicate graphics begin'
            graphics.add(key)
        if op == b'limit':
            failures.append({'attempt': key[0], 'owner': key[1], 'line': line_no,
                             'status': 'DROPPED', 'reason': 'graphics_attempt_limit'})
        if not op.startswith(b'state.'): continue
        data = dict(token.split('=', 1) for token in fields.decode('ascii').split())
        if op == b'state.failed':
            failures.append({'attempt': key[0], 'owner': key[1], 'line': line_no, **data})
        elif op == b'state.begin':
            assert key not in active and all((r['attempt'], r['owner']) != key for r in completed), 'duplicate begin'
            assert data['status'] == 'PRESENT' and data['schema'] == '1'
            assert 0 < int(data['bytes']) <= 256 * 1024
            active[key] = {'attempt': key[0], 'owner': key[1], 'begin_line': line_no, 'begin': data, 'payload': bytearray()}
        elif op == b'state.chunk':
            row = active[key]
            assert int(data['offset']) == len(row['payload']), 'chunk offset/gap/duplicate'
            chunk = bytes.fromhex(data['hex'])
            assert 0 < len(chunk) <= 128 and len(row['payload']) + len(chunk) <= int(row['begin']['bytes'])
            row['payload'] += chunk
        elif op == b'state.end':
            row = active.pop(key)
            payload = bytes(row.pop('payload'))
            digest = hashlib.sha1(payload).hexdigest()
            assert len(payload) == int(data['bytes']) == int(row['begin']['bytes'])
            assert digest == data['sha1'] == row['begin']['sha1'], 'state SHA1'
            description = json.loads(payload)
            assert description['schema'] == 'dx12-pso-state-v1'
            assert description['shader_identity_domain'] == 'application_bytecode_before_translation'
            root = description['RootSignature']
            assert row['begin']['root'] == root['status'], 'root status mismatch'
            decoded_root = None
            if root['status'] == 'PRESENT':
                blob = bytes.fromhex(root['blob_hex'])
                assert len(blob) == root['length'] and hashlib.sha1(blob).hexdigest() == root['sha1'], 'root SHA1'
                decoded_root = rootsig(blob)
            assert len(description['InputLayout']) == int(row['begin']['input_elements'])
            row.update(end_line=line_no, sha1=digest, description=description, decoded_root=decoded_root)
            completed.append(row)
        else: raise AssertionError(('unknown state operation', op))
    assert not active, 'missing end'
    described = {(row['attempt'], row['owner']) for row in completed}
    failed = {(row['attempt'], row['owner']) for row in failures}
    missing = sorted(graphics - described - failed)
    return {'complete': bool(completed) and not failures and not missing,
            'coverage': 'PRESENT' if completed else 'NOT_ENABLED_OR_MISSING',
            'graphics_begins': len(graphics), 'missing_state_attempts': missing,
            'rows': completed, 'failures': failures}

def controls(report):
    assert report['complete'] and len(report['rows']) == 6
    rows = sorted(report['rows'], key=lambda r: r['attempt'])
    digests = [r['sha1'] for r in rows]
    assert digests[0] == digests[1], 'address-dependent digest'
    assert len(set(digests)) == 5, 'state/shader changes lost'
    base = rows[0]['description']
    assert base['InputLayout'][0]['SemanticName_hex'] == b'POSITION'.hex()
    assert rows[2]['description']['InputLayout'][0]['AlignedByteOffset'] == 4
    assert rows[3]['description']['BlendState']['RenderTarget'][0]['RenderTargetWriteMask'] == 1
    root = rows[0]['decoded_root']
    assert root['Version'] == 2 and root['Flags'] == 1 and len(root['Parameters']) == 3
    assert root['Parameters'][0]['Constants'] == dict(ShaderRegister=10, RegisterSpace=0, Num32BitValues=4)
    assert rows[4]['decoded_root']['Parameters'][0]['Constants']['Num32BitValues'] == 8
    ranges = root['Parameters'][1]['DescriptorTable']
    assert len(ranges) == 2 and ranges[0]['OffsetInDescriptorsFromTableStart'] == 0xffffffff
    assert ranges[1]['Flags'] == 2 and ranges[1]['OffsetInDescriptorsFromTableStart'] == 5
    assert root['Parameters'][2]['Descriptor'] == dict(ShaderRegister=3, RegisterSpace=2, Flags=8)
    assert len(root['StaticSamplers']) == 1 and root['StaticSamplers'][0]['ShaderRegister'] == 5
    stages = lambda r: {s['stage']: s.get('sha1') for s in r['description']['shaders']}
    assert stages(rows[0])['PS'] != stages(rows[5])['PS'] and stages(rows[0])['VS'] == stages(rows[5])['VS']
    return {'same_content_different_addresses': 'PASS', 'input_offset': 'PASS', 'blend_state': 'PASS',
            'root_constants_and_v11_ranges_static_sampler': 'PASS', 'original_shader_identity': 'PASS'}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('log', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    raw = args.log.read_bytes()
    report = decode(raw)
    report['raw'] = {'path': str(args.log.resolve()), 'sha256': hashlib.sha256(raw).hexdigest()}
    args.out.mkdir(exist_ok=False)
    for row in report['rows']:
        name = f"{row['attempt']:04d}-{row['sha1']}"
        root = row['description']['RootSignature']
        if root['status'] == 'PRESENT': (args.out / (name + '.rootsig.bin')).write_bytes(bytes.fromhex(root['blob_hex']))
        (args.out / (name + '.json')).write_text(json.dumps(row, indent=2) + '\n')
    (args.out / 'INDEX.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'complete': report['complete'], 'psos': len(report['rows']), 'failures': report['failures']}))
