"""Source reconstruction controls; Git reads are read-only, no source execution."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import release_sources as source
import verify_sources
import prepare_native
import prepare_pe


class SourceInputs(unittest.TestCase):
    def test_historical_basis_matches_exact_pin(self):
        lock = json.loads((HERE / 'git-basis.lock.json').read_bytes())
        rows = source.base_rows()
        self.assertEqual(source.inventory(rows), lock['snapshot'])
        self.assertEqual(len(rows), 336)

    def test_native_snapshot_matches_exact_pin(self):
        result = verify_sources.verify()
        self.assertEqual(result['snapshot']['sha256'],
                         '307f7b21cf82ef506bac546f58d4a004fb9148de3a40a35dcea34cfba830f7b9')
        self.assertEqual(result['snapshot']['files'], 336)
        self.assertEqual(result['inputs'], 112)

    def test_both_png_fixtures_retained_from_public_git(self):
        rows = dict(source.base_rows())
        expected = {'tests/dx11/test.png': '16f7b855e9f83ffafe3e1136935d9f4b25e82069401aec56f1bed463c47ab4f6',
                    'tests/dx11/testTexture.png': '9dfe608f7542aa26ab9ddda6097e40584410c65392e973e369db1490bdea206f'}
        lock = json.loads((HERE / 'git-basis.lock.json').read_bytes())
        pins = {row['path']: row for row in lock['files']}
        for name, digest in expected.items():
            self.assertIn('git_blob', pins[name])
            self.assertEqual(hashlib.sha256(rows[name]).hexdigest(), digest)

    def test_transport_lf_reconstructs_original_no_lf(self):
        name = 'src/winemetal/winemetal.h'
        row = next(row for row in json.loads((HERE / 'git-basis.lock.json').read_bytes())['files']
                   if row['path'] == name)
        stored = (HERE / 'inputs' / row['input']).read_bytes()
        rebuilt = dict(source.base_rows())[name]
        self.assertTrue(row['input_trim_final_lf'])
        self.assertTrue(stored.endswith(b'\n'))
        self.assertFalse(rebuilt.endswith(b'\n'))
        self.assertEqual(stored[:-1], rebuilt)

    def test_unknown_tree_refuses_before_blob_reads(self):
        actual = source.git
        calls = []
        def wrong(root, args, data=None):
            calls.append(args)
            if args == ['rev-parse', json.loads((HERE / 'git-basis.lock.json').read_bytes())['revision'] + '^{tree}']:
                return b'0000000000000000000000000000000000000000\n'
            return actual(root, args, data)
        with patch.object(source, 'git', side_effect=wrong):
            with self.assertRaisesRegex(ValueError, 'tree differs'):
                source.base_rows()
        self.assertFalse(any(args[0] == 'cat-file' for args in calls))

    def test_missing_public_blob_refuses_before_batch(self):
        actual = source.git
        def missing(root, args, data=None):
            raw = actual(root, args, data)
            if args[0] == 'ls-tree':
                return b'\0'.join(part for part in raw.split(b'\0') if b'tests/dx11/test.png' not in part)
            return raw
        with patch.object(source, 'git', side_effect=missing):
            with self.assertRaisesRegex(ValueError, 'outside the pinned public tree'):
                source.base_rows()

    def test_truncated_batch_refuses(self):
        actual = source.git
        def truncated(root, args, data=None):
            raw = actual(root, args, data)
            return raw[:-1] if args == ['cat-file', '--batch'] else raw
        with patch.object(source, 'git', side_effect=truncated):
            with self.assertRaisesRegex(ValueError, 'Truncated Git batch payload'):
                source.base_rows()

    def test_changed_batch_bytes_refuse_digest(self):
        actual = source.git
        def changed(root, args, data=None):
            raw = actual(root, args, data)
            if args == ['cat-file', '--batch']:
                at = raw.index(b'\n') + 1
                return raw[:at] + bytes([raw[at] ^ 1]) + raw[at + 1:]
            return raw
        with patch.object(source, 'git', side_effect=changed):
            with self.assertRaisesRegex(ValueError, 'postimage differs'):
                source.base_rows()

    def test_paths_refuse_traversal_and_absolute(self):
        for name in ('../escape', '/escape', 'a\\escape', 'a/./b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                source.relative(name)

    def test_text_overlays_are_seven_disjoint_files(self):
        base = dict(source.base_rows())
        overlays = dict(source.pe_overlay_rows())
        self.assertEqual(len(overlays), 7)
        self.assertFalse(set(base) & set(overlays))
        for data in overlays.values():
            self.assertNotIn(b'\0', data)
            data.decode('utf-8')

    def test_native_and_pe_keep_the_git_executable_flag(self):
        root = Path(tempfile.mkdtemp(prefix='source-modes-', dir=os.environ['REPRO109_TEST_TMP']))
        before = Path.cwd()
        self.addCleanup(os.chdir, before)
        os.chdir(root)
        lock = json.loads((HERE / 'git-basis.lock.json').read_bytes())
        expected = {row['path'] for row in lock['files'] if row['mode'] == '100755'}
        self.assertEqual(expected, {'src/winemetal/unix/install.sh'})
        for name, prepare in [('native', prepare_native.prepare), ('pe', prepare_pe.prepare)]:
            prepare(Path(name))
            actual = {path.relative_to(Path(name) / 'source').as_posix()
                      for path in (Path(name) / 'source').rglob('*')
                      if path.is_file() and path.stat().st_mode & 0o111}
            self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
