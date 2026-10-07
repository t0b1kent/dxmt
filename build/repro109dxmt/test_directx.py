"""Own inert Git fixtures; no external network, compiler, or third-party code."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import prepare_directx as module
from verify_sources import snapshot


class DirectXTests(unittest.TestCase):
    def setUp(self):
        root = os.environ.get('REPRO109_TEST_TMP')
        if not root or not Path(root).is_absolute() or not Path(root).is_dir():
            raise RuntimeError('Set REPRO109_TEST_TMP to the existing Flash scratch directory')
        self.temp = tempfile.mkdtemp(prefix='directx-', dir=root)
        self.old = Path.cwd()
        os.chdir(self.temp)
        self.headers = Path('headers'); self.headers.mkdir()
        for name, text in [('d3d11.h', 'own inert header\n'), ('COPYING.MinGW-w64.txt', 'own inert license\n')]:
            (self.headers / name).write_text(text)
        env = dict(os.environ, GIT_AUTHOR_NAME='MacRunner Build', GIT_AUTHOR_EMAIL='build@users.noreply.github.com',
                   GIT_COMMITTER_NAME='MacRunner Build', GIT_COMMITTER_EMAIL='build@users.noreply.github.com',
                   GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_SYSTEM='/dev/null')
        for argv in [['git', 'init', '-q'], ['git', 'add', 'd3d11.h', 'COPYING.MinGW-w64.txt'],
                     ['git', '-c', 'core.hooksPath=/dev/null', 'commit', '-q', '-m', 'Own inert fixture']]:
            result = subprocess.run(argv, cwd=self.headers, env=env, capture_output=True)
            if result.returncode:
                raise RuntimeError('Own Git fixture preparation failed')
        identities = module.git(self.headers, 'rev-parse', 'HEAD^{commit}', 'HEAD^{tree}').decode().splitlines()
        pin = dict(module.PIN)
        rows = []
        for name in sorted(['d3d11.h', 'COPYING.MinGW-w64.txt']):
            data = (self.headers / name).read_bytes()
            rows.append((name, hashlib.sha256(data).hexdigest(), len(data)))
        pin.update(revision=identities[0], tree=identities[1], files=len(rows), bytes=sum(r[2] for r in rows),
                   sha256=hashlib.sha256(''.join(f'{n}\0{h}\0{s}\n' for n,h,s in rows).encode()).hexdigest(),
                   license_sha256=hashlib.sha256((self.headers / pin['license_path']).read_bytes()).hexdigest())
        self.pin_patch = patch.object(module, 'PIN', pin); self.pin_patch.start()
        self.verify_patch = patch.object(module, 'verify', return_value={}); self.verify_patch.start()
        self.prepared = Path('prepared'); (self.prepared / 'source').mkdir(parents=True)
        (self.prepared / 'source/meson.build').write_text('own inert Meson content\n')
        self.link('native')

    def link(self, kind):
        (self.prepared / 'LLVM-LINK.json').write_text(json.dumps(dict(
            status='LLVM_LINK_PREPARED_NOT_CONFIGURED', source_kind=kind,
            source_after=snapshot(self.prepared / 'source'))))

    def tearDown(self):
        self.verify_patch.stop(); self.pin_patch.stop()
        os.chdir(self.old)  # Retain own fixtures with this iteration's evidence.

    def test_native_and_license_copied_without_git(self):
        result = module.prepare(self.prepared, self.headers, 'native')
        destination = self.prepared / 'source/include/native/directx'
        self.assertEqual(result['source_after']['files'], 3)
        self.assertEqual(sorted(p.name for p in destination.iterdir()), ['COPYING.MinGW-w64.txt', 'd3d11.h'])
        self.assertFalse((destination / '.git').exists())
        self.assertEqual(result['headers']['census']['files'], 2)
        self.assertEqual(result['compilation'], 'NOT_ENABLED')

    def test_pe_uses_own_kind(self):
        self.link('pe')
        self.assertEqual(module.prepare(self.prepared, self.headers, 'pe')['source_kind'], 'pe')

    def test_wrong_kind_refuses_before_copy(self):
        with self.assertRaisesRegex(ValueError, 'kind/digest'):
            module.prepare(self.prepared, self.headers, 'pe')
        self.assertFalse((self.prepared / 'source/include').exists())

    def test_source_drift_refuses_before_copy(self):
        (self.prepared / 'source/meson.build').write_text('other same-length data\n')
        with self.assertRaisesRegex(ValueError, 'kind/digest'):
            module.prepare(self.prepared, self.headers, 'native')

    def test_repeat_preserves_receipt_and_source(self):
        module.prepare(self.prepared, self.headers, 'native')
        before = snapshot(self.prepared / 'source')
        receipt = (self.prepared / 'DIRECTX.json').read_bytes()
        with self.assertRaisesRegex(ValueError, 'already exists'):
            module.prepare(self.prepared, self.headers, 'native')
        self.assertEqual(snapshot(self.prepared / 'source'), before)
        self.assertEqual((self.prepared / 'DIRECTX.json').read_bytes(), receipt)

    def test_assume_unchanged_does_not_hide_bad_blob(self):
        module.git(self.headers, 'update-index', '--assume-unchanged', 'd3d11.h')
        (self.headers / 'd3d11.h').write_text('wrong inert header\n')
        with self.assertRaisesRegex(ValueError, 'Git blob'):
            module.checked_headers(self.headers)

    def test_wrong_tree_refused(self):
        with patch.dict(module.PIN, tree='0' * 40):
            with self.assertRaisesRegex(ValueError, 'commit/tree'):
                module.checked_headers(self.headers)

    def test_symlink_refused(self):
        (self.headers / 'd3d11.h').unlink()
        (self.headers / 'd3d11.h').symlink_to('COPYING.MinGW-w64.txt')
        with self.assertRaises(ValueError):
            module.checked_headers(self.headers)

    def test_dirty_extra_refused(self):
        (self.headers / 'unexpected.h').write_text('own extra\n')
        with self.assertRaisesRegex(ValueError, 'Dirty'):
            module.checked_headers(self.headers)

    def test_license_digest_refused(self):
        with patch.dict(module.PIN, license_sha256='0' * 64):
            with self.assertRaisesRegex(ValueError, 'license'):
                module.checked_headers(self.headers)

    def test_relative_path_and_ancestor_symlink_refused(self):
        for name in ['../headers', str(self.headers.resolve()), '.']:
            with self.assertRaises(ValueError):
                module.relative(name)
        Path('linked').symlink_to(self.headers)
        with self.assertRaises(ValueError):
            module.relative('linked/child')

    def test_nonempty_destination_preserved(self):
        destination = self.prepared / 'source/include/native/directx'
        destination.mkdir(parents=True)
        (destination / 'existing.h').write_text('own existing\n')
        self.link('native')
        with self.assertRaisesRegex(ValueError, 'not empty'):
            module.prepare(self.prepared, self.headers, 'native')
        self.assertEqual((destination / 'existing.h').read_text(), 'own existing\n')

    def test_copy_failure_keeps_raw(self):
        real = subprocess.run
        def runner(argv, **kwargs):
            if argv[0] == '/bin/cp':
                return subprocess.CompletedProcess(argv, 9, b'own stdout\n', b'own cp error\n')
            return real(argv, **kwargs)
        with patch.object(module.subprocess, 'run', side_effect=runner):
            with self.assertRaisesRegex(ValueError, 'clone failed'):
                module.prepare(self.prepared, self.headers, 'native')
        raw = json.loads((self.prepared / 'directx-clone.raw.json').read_bytes())
        self.assertEqual(raw['rc'], 9)
        self.assertTrue(raw['stderr_b64'])
        self.assertFalse((self.prepared / 'DIRECTX.json').exists())

    def test_same_length_other_source_mutation_refused(self):
        real = subprocess.run
        def runner(argv, **kwargs):
            result = real(argv, **kwargs)
            if argv[0] == '/bin/cp':
                path = self.prepared / 'source/meson.build'; data = path.read_bytes()
                path.write_bytes(b'x' + data[1:])
            return result
        with patch.object(module.subprocess, 'run', side_effect=runner):
            with self.assertRaisesRegex(ValueError, 'Existing DXMT source bytes'):
                module.prepare(self.prepared, self.headers, 'native')

    def test_local_fetch_guard_precedes_mkdir_and_git(self):
        env = dict(os.environ)
        for key in ['GITHUB_ACTIONS', 'CI_WORKSPACE_PATH', 'CI_PRIMARY_REPOSITORY_PATH', 'CI_BUILD_NUMBER']:
            env.pop(key, None)
        with patch.dict(os.environ, env, clear=True), patch.object(module.subprocess, 'run') as run:
            with self.assertRaises((ValueError, AssertionError)):
                module.cloud_fetch(Path('download'), 'xcode-cloud')
        run.assert_not_called()
        self.assertFalse(Path('download').exists())

    def test_cloud_fetch_failure_keeps_raw_without_retry(self):
        calls = []
        def runner(argv, **kwargs):
            calls.append(argv)
            if 'fetch' in argv:
                kwargs['stderr'].write(b'own inert transfer failure\n')
                return subprocess.CompletedProcess(argv, 7)
            return subprocess.CompletedProcess(argv, 0)
        fake = types.SimpleNamespace(cloud_guard=lambda profile: None)
        with patch.dict(sys.modules, {'build_full': fake}), patch.object(module.subprocess, 'run', side_effect=runner):
            with self.assertRaisesRegex(ValueError, 'fetch failed'):
                module.cloud_fetch(Path('download'), 'xcode-cloud')
        receipt = json.loads(Path('download-fetch-reports/1.json').read_bytes())
        self.assertEqual(receipt['rc'], 7)
        self.assertEqual(receipt['stdout_state'], 'EMPTY')
        self.assertEqual(receipt['stderr_state'], 'PRESENT')
        self.assertEqual(Path('download-fetch-reports/1.stderr').read_bytes(), b'own inert transfer failure\n')
        self.assertEqual(sum('fetch' in argv for argv in calls), 1)
        self.assertFalse(Path('download-fetch-reports/2.json').exists())

    def test_cloud_fetch_timeout_is_not_empty_or_success(self):
        def runner(argv, **kwargs):
            if 'fetch' in argv:
                kwargs['stdout'].write(b'own partial transfer\n')
                raise subprocess.TimeoutExpired(argv, 120)
            return subprocess.CompletedProcess(argv, 0)
        fake = types.SimpleNamespace(cloud_guard=lambda profile: None)
        with patch.dict(sys.modules, {'build_full': fake}), patch.object(module.subprocess, 'run', side_effect=runner):
            with self.assertRaisesRegex(ValueError, 'fetch failed'):
                module.cloud_fetch(Path('download'), 'xcode-cloud')
        receipt = json.loads(Path('download-fetch-reports/1.json').read_bytes())
        self.assertEqual(receipt['state'], 'TIMEOUT')
        self.assertEqual(receipt['status'], 'FAILED')
        self.assertIsNone(receipt['rc'])
        self.assertEqual(receipt['stdout_state'], 'PRESENT')
        self.assertEqual(receipt['stderr_state'], 'EMPTY')
        self.assertEqual(Path('download-fetch-reports/1.stdout').read_bytes(), b'own partial transfer\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
