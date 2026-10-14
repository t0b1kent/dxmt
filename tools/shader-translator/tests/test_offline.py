# SPDX-License-Identifier: LGPL-2.1-or-later
# Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
"""Offline tests of the DXIL -> Metal translator: no tools, no network, no game data.

Run: python3 -I tests/test_offline.py     (from tools/shader-translator)
"""
import hashlib, json, py_compile, struct, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANE = ROOT / 'lane'
sys.dont_write_bytecode = True
sys.path.insert(0, str(LANE / 'takeover/tess-gs/translator'))
import abi                                  # noqa: E402


def container(chunks):
    """A DXBC container holding the given {fourcc: bytes}."""
    head = b'DXBC' + bytes(16) + struct.pack('<I', 1)
    body_off = 32 + 4 * len(chunks)
    offsets, blobs, at = [], b'', body_off
    for name, data in chunks.items():
        offsets.append(at)
        blob = name.encode() + struct.pack('<I', len(data)) + data
        blobs += blob
        at += len(blob)
    total = at
    return head + struct.pack('<I', total) + struct.pack('<I', len(chunks)) + struct.pack('<%dI' % len(chunks), *offsets) + blobs


def rts0_v10(params, statics=()):
    """Root signature 1.0: params = [('table', visibility, [(type, num, base, space, offset)]),
    ('const', visibility, register, space, count), ('cbv', visibility, register, space)]."""
    header = 24
    table = header + 12 * len(params)
    pdata, bodies, extra = b'', [], b''
    cur = table + 0
    # first pass: sizes of the per-parameter bodies
    body_off = []
    sizes = []
    for p in params:
        if p[0] == 'table':
            sizes.append(8 + 20 * len(p[2]))
        elif p[0] == 'const':
            sizes.append(12)
        else:
            sizes.append(8)
    pos = table
    body = b''
    entries = b''
    for p, size in zip(params, sizes):
        off = pos
        kind = {'table': abi.TABLE, 'const': abi.CONSTANTS, 'cbv': abi.ROOT_CBV}[p[0]]
        entries += struct.pack('<3I', kind, p[1], off)
        if p[0] == 'table':
            body += struct.pack('<2I', len(p[2]), off + 8)
            for r in p[2]:
                body += struct.pack('<5I', *r)
        elif p[0] == 'const':
            body += struct.pack('<3I', p[2], p[3], p[4])
        else:
            body += struct.pack('<2I', p[2], p[3])
        pos += size
    return struct.pack('<6I', 1, len(params), header, 0, pos, 0) + entries + body


class AbiTests(unittest.TestCase):
    def test_container_chunks_and_rts0(self):
        r = rts0_v10([('table', 0, [(abi.SRV, 2, 0, 0, abi.UNBOUNDED)])])
        c = container({'RTS0': r, 'DXIL': b'\x00' * 8})
        self.assertEqual(set(abi.dxbc_chunks(c)), {'RTS0', 'DXIL'})
        self.assertEqual(abi.rts0_of(c), r)
        self.assertEqual(abi.rts0_of(r), r)
        self.assertEqual(abi.root_key(r), hashlib.sha1(r).hexdigest())
        with self.assertRaises(abi.AbiError):
            abi.dxbc_chunks(b'NOPE' + bytes(40))

    def test_layout_and_mapping(self):
        r = rts0_v10([('table', 0, [(abi.SRV, 4, 0, 0, abi.UNBOUNDED), (abi.UAV, 2, 0, 0, abi.UNBOUNDED)]),
                      ('const', 0, 0, 0, 5),
                      ('cbv', 1, 1, 0)])
        rs = abi.root_layout(abi.parse_rts0(r))
        self.assertEqual([p['qword'] for p in rs['parameters']], [0, 1, 4])        # table 1, constants (5+1)//2 = 3, cbv 1
        self.assertEqual(rs['qwords'], 5)
        res = [dict(**{'class': abi.SRV}, space=0, register=2, count=1, binding=10),
               dict(**{'class': abi.UAV}, space=0, register=1, count=1, binding=11),
               dict(**{'class': abi.CBV}, space=0, register=0, count=1, binding=12),
               dict(**{'class': abi.CBV}, space=0, register=1, count=1, binding=13),
               dict(**{'class': abi.SRV}, space=7, register=0, count=1, binding=14)]
        found, missing = abi.map_resources(rs, 'VS', res)
        self.assertEqual(found[10], dict(where=abi.WHERE_TABLE, qword=0, index=2))
        self.assertEqual(found[11], dict(where=abi.WHERE_TABLE, qword=0, index=4 + 1))   # second range starts after the 4 SRVs
        self.assertEqual(found[12]['where'], abi.WHERE_CONSTANTS)
        self.assertEqual(found[13]['where'], abi.WHERE_ROOT)                             # visible to VS
        self.assertEqual([m['binding'] for m in missing], [14])
        _, missing_ps = abi.map_resources(rs, 'PS', res)
        self.assertIn(13, [m['binding'] for m in missing_ps])                            # the CBV is VS-only

    def test_short_rts0_rejected(self):
        with self.assertRaises(abi.AbiError):
            abi.parse_rts0(b'\x00' * 8)


class SourcesTests(unittest.TestCase):
    def test_every_python_source_compiles(self):
        files = sorted(p for p in LANE.rglob('*.py'))
        self.assertGreaterEqual(len(files), 17)
        with tempfile.TemporaryDirectory() as d:
            for p in files:
                py_compile.compile(str(p), cfile=str(Path(d) / (p.name + 'c')), doraise=True)

    def test_lock_digests(self):
        lock = json.loads((ROOT / 'inputs.lock.json').read_text())
        for rel, want in lock['files'].items():
            self.assertEqual(hashlib.sha256((ROOT / rel).read_bytes()).hexdigest(), want, rel)

    def test_no_game_identifiers(self):
        import re
        sha1 = re.compile(r'\b[0-9a-f]{40}\b')
        for p in list(LANE.rglob('*.py')) + list(LANE.rglob('*.metal')) + list(LANE.rglob('*.cpp')):
            text = p.read_text(encoding='utf-8', errors='replace')
            self.assertNotRegex(text, '/' + 'Volumes/|/' + 'Users/', str(p))          # no machine-local paths
            self.assertIsNone(sha1.search(text), 'a 40-hex identifier in %s' % p)

    def test_patch_series_present(self):
        lock = json.loads((ROOT / 'inputs.lock.json').read_text())
        for n in lock['spirv_cross']['patches']:
            self.assertTrue((ROOT / 'patches/spirv-cross' / n).is_file(), n)
        for n in lock['frontend']['patches']:
            self.assertTrue((ROOT / 'patches/dxil-spirv' / n).is_file(), n)


if __name__ == '__main__':
    unittest.main(verbosity=2)
