"""Actual Bash dispatch and existing producer calls, with own inert payloads."""
import json
import os
from pathlib import Path
from types import SimpleNamespace
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_cloud_input as cloud


class SourceMatrix(unittest.TestCase):
    def setUp(self):
        root = Path(os.environ['REPRO109_TEST_TMP'])
        self.root = Path(tempfile.mkdtemp(prefix='source-matrix-', dir=root))
        self.old = Path.cwd()
        self.addCleanup(os.chdir, self.old)
        os.chdir(self.root)
        self.stub = self.root / 'python-stub.sh'
        self.stub.write_text('#!/bin/sh\nprintf "%s\\n" "$*"\nexit "${REPRO109_STUB_RC:-0}"\n')
        self.stub.chmod(0o755)
        self.full = SimpleNamespace(cloud_guard=Mock(), apply_profile=Mock(return_value={'toolchain': {'own': True}}))
        self.dep = SimpleNamespace(read_lock=Mock(return_value={}),
                                   toolchain_preflight=Mock(return_value=('own-sdk', 'own-cc', 'own-cxx')))

    def dispatch(self, axis, **extra):
        env = dict(os.environ, REPRO109_RECIPE=str(HERE), REPRO109_DRIVER_PYTHON=str(self.stub), **extra)
        return subprocess.run(['/bin/bash', str(HERE / 'run_source_axis.sh'), axis], env=env,
                              stdin=subprocess.DEVNULL, capture_output=True, timeout=10)

    def test_all_five_actual_bash_axes(self):
        expected = {'native-source': ['check_pe_preparation.py --kind native --out native-control'],
                    'pe-source': ['check_pe_preparation.py --kind pe --out pe-control'],
                    'directx-headers': ['prepare_native.py --out native',
                         'freeze_version.py --prepared native --kind native',
                         'llvm_link.py --prepared native --kind native',
                         'prepare_directx.py --prepared native --kind native --checkout headers --fetch --profile github-macos15-arm64'],
                    'llvm-source': ['check_cloud_input.py llvm-source'],
                    'toolchain': ['check_cloud_input.py toolchain']}
        for axis, commands in expected.items():
            with self.subTest(axis=axis):
                result = self.dispatch(axis)
                self.assertEqual(result.returncode, 0, result.stderr)
                lines = result.stdout.decode().splitlines()
                self.assertEqual(len(lines), len(commands))
                for line, command in zip(lines, commands):
                    self.assertEqual(line, '-I -B ' + str(HERE) + '/' + command)

    def test_missing_driver_refuses(self):
        with patch.dict(os.environ, REPRO109_DRIVER_PYTHON=str(self.root / 'missing')):
            env = dict(os.environ, REPRO109_RECIPE=str(HERE))
            result = subprocess.run(['/bin/bash', str(HERE / 'run_source_axis.sh'), 'llvm-source'],
                                    env=env, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(self.root / 'missing').encode(), result.stderr)
        self.assertIn(b'No such file or directory', result.stderr)
        self.assertEqual(result.stdout, b'')

    def test_failed_first_command_stops_headers_axis(self):
        result = self.dispatch('directx-headers', REPRO109_STUB_RC='37')
        self.assertEqual(result.returncode, 37)
        self.assertEqual(len(result.stdout.splitlines()), 1)

    def test_unknown_axis_refuses(self):
        result = self.dispatch('unknown')
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b'')

    def load(self, name, path):
        return self.dep if name == 'dxmt_axis_deps' else self.full

    def test_llvm_uses_exact_existing_public_pin_and_unpacker(self):
        pin = cloud.producer.llvm_source_pin()
        with patch.object(cloud.producer, 'load', side_effect=self.load), \
             patch.object(cloud.producer.public_archive, 'download', autospec=True) as download, \
             patch.object(cloud.producer, 'unpack_source', autospec=True, return_value=(Path('llvm15-source'), {'own': True})) as unpack, \
             patch.object(cloud.producer, 'sha', return_value=pin['sha256']):
            result = cloud.check('llvm-source')
        self.assertEqual(result['status'], 'VERIFIED_NOT_COMPILED')
        download.assert_called_once_with(pin, Path('llvm15.tar.xz'), Path('reports'))
        unpack.assert_called_once_with(Path('llvm15.tar.xz'), Path('llvm15-source'), pin['sha256'])
        self.full.cloud_guard.assert_called_once_with('github-macos15-arm64')

    def test_missing_downloader_keeps_failed_receipt(self):
        with patch.object(cloud.producer, 'load', side_effect=self.load), \
             patch.object(cloud.producer.public_archive, 'download', side_effect=FileNotFoundError('own missing tool')):
            with self.assertRaises(FileNotFoundError):
                cloud.check('llvm-source')
        result = json.loads(Path('reports/RESULT.json').read_bytes())
        self.assertEqual(result['status'], 'FAILED')
        self.assertEqual(result['first_failure']['type'], 'FileNotFoundError')
        self.assertEqual(result['compilation'], 'NOT_ENABLED')

    def fake_path(self, value):
        return SimpleNamespace(glob=lambda pattern: [self.root / 'Xcode.app']) if value == '/Applications' else Path(value)

    def test_sdk_uses_existing_preflight(self):
        command = SimpleNamespace(returncode=0, stdout=b'Xcode 26.3\nBuild version 17C529\n', stderr=b'')
        with patch.object(cloud.producer, 'load', side_effect=self.load), \
             patch.object(cloud, 'Path', side_effect=self.fake_path), \
             patch.object(cloud.subprocess, 'run', return_value=command):
            result = cloud.check('toolchain')
        self.assertEqual(result['status'], 'VERIFIED_NOT_COMPILED')
        self.dep.toolchain_preflight.assert_called_once_with({'own': True}, Path('reports'))

    def test_sdk_failure_is_separate_failed_receipt(self):
        command = SimpleNamespace(returncode=0, stdout=b'Xcode 26.3\nBuild version 17C529\n', stderr=b'')
        self.dep.toolchain_preflight.side_effect = ValueError('own empty SDK')
        with patch.object(cloud.producer, 'load', side_effect=self.load), \
             patch.object(cloud, 'Path', side_effect=self.fake_path), \
             patch.object(cloud.subprocess, 'run', return_value=command):
            with self.assertRaisesRegex(ValueError, 'empty SDK'):
                cloud.check('toolchain')
        result = json.loads(Path('reports/RESULT.json').read_bytes())
        self.assertEqual(result['status'], 'FAILED')
        self.assertEqual(result['axis'], 'toolchain')


if __name__ == '__main__':
    unittest.main(verbosity=2)
