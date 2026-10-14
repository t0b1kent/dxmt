# SPDX-License-Identifier: LGPL-2.1-or-later
# Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
"""Smoke test of the built translator on two OPEN test shaders (vkd3d-proton, pinned in inputs.lock.json).

Needs the tools built by build.py and the Metal compiler of Xcode (xcrun metal / metallib); downloads two small header
files from the pinned vkd3d-proton revision and checks their git blob ids. No game data is involved.
Run: python3 -I tests/test_smoke.py     (from tools/shader-translator, after build.py all)
"""
import hashlib, json, re, struct, subprocess, sys, tempfile, unittest, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = json.loads((ROOT / 'inputs.lock.json').read_text())
FRONTEND = ROOT / 'lane/takeover/admission/build-frontend/dxil-spirv-logical'


def fetch_array(rel, info):
    t = LOCK['tests']['vkd3d_proton']
    url = 'https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/%s/%s' % (t['commit'], rel)
    data = urllib.request.urlopen(url, timeout=60).read()
    blob = hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()
    assert blob == info['git_blob'], 'git blob of %s is %s, pinned %s' % (rel, blob, info['git_blob'])
    m = re.search(r'static const (BYTE|DWORD) %s\[\]\s*=\s*\{(.*?)\};' % re.escape(info['array']), data.decode(), re.S)
    assert m, 'array %s not found' % info['array']
    values = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(2))]
    if m.group(1) == 'BYTE':
        return bytes(values)
    return struct.pack('<%dI' % len(values), *values)


class Smoke(unittest.TestCase):
    def test_open_triangle_pair(self):
        self.assertTrue(FRONTEND.is_file(), 'build the tools first: python3 build.py all')
        files = LOCK['tests']['vkd3d_proton']['files']
        vs = fetch_array('tests/shaders/shaders/headers/vs_derivative_hoisting.h', files['tests/shaders/shaders/headers/vs_derivative_hoisting.h'])
        ps = fetch_array('tests/shaders/render_target/headers/ps_render_a8.h', files['tests/shaders/render_target/headers/ps_render_a8.h'])
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            req, out, work = d / 'req', d / 'out', d / 'work'
            req.mkdir()
            lines = []
            for stage, blob in (('vs', vs), ('ps', ps)):
                sha1 = hashlib.sha1(blob).hexdigest()
                (req / ('%s.%s.dxbc' % (sha1, stage))).write_bytes(blob)
                lines.append('stage=%s shader=%s root=none' % (stage, sha1))
            (req / 'requests.txt').write_text('\n'.join(lines) + '\n')
            r = subprocess.run([sys.executable, '-I', str(ROOT / 'lane/takeover/admission/translator/translate_requests.py'),
                                str(req), str(out), '--work', str(work), '--frontend', str(FRONTEND)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout[-1500:] + r.stderr[-1500:])
            index = json.loads((out / 'INDEX.json').read_text())
            self.assertEqual([e['accepted'] for e in index['translations']], [True, True])
            for e in index['translations']:
                lib = (out / (e['name'] + '.metallib')).read_bytes()
                self.assertEqual(lib[:4], b'MTLB')
                desc = (out / (e['name'] + '.desc')).read_text().splitlines()
                self.assertIn('format=3', desc)
                self.assertIn('entry=main0', desc)
                self.assertIn('resources=0', desc)
                msl = (out / 'msl' / (e['name'] + '.metal')).read_text()
                self.assertIn('vertex' if e['stage'] == 'vs' else 'fragment', msl)


if __name__ == '__main__':
    unittest.main(verbosity=2)
