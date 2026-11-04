"""Inert source-transport and absent-tool checks; no renderer or Wine is executed."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import build as producer
import prepare_source as source


class SourceInputs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ['REPRO109_TEST_TMP'])
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_entrypoint_loads_its_sibling_in_isolated_python_from_another_directory(self):
        child = subprocess.run([sys.executable, '-I', '-B', str(Path(producer.__file__).resolve()), '--help'],
                               cwd=self.root, capture_output=True, text=True, timeout=10)
        self.assertEqual(child.returncode, 0, child.stderr)
        self.assertIn('--variant', child.stdout)

    def test_public_git_blob_transport_checks_real_objects(self):
        subprocess.run(['git', 'init', '--quiet', self.root], check=True)
        data = b'Own source fixture. Never compiled.\n'
        name = subprocess.check_output(['git', '-C', self.root, 'hash-object', '-w', '--stdin'], input=data).decode().strip()
        self.assertEqual(source.git_blobs([name], self.root), {name: data})
        with self.assertRaisesRegex(ValueError, 'Missing public source blob'):
            source.git_blobs(['0' * 40], self.root)

    def test_source_transport_restores_a_missing_final_lf_and_refuses_corruption(self):
        path = self.root / 'input.cpp'; path.write_bytes(b'// fixture\n')
        row = dict(path='input.cpp', store='input.cpp', transport_lf=True,
                   stored=source.identity(b'// fixture\n'), **source.identity(b'// fixture'))
        self.assertEqual(source.file_bytes(row, {}, None, self.root), b'// fixture')
        path.write_bytes(b'// replaced\n')
        with self.assertRaisesRegex(ValueError, 'Stored source differs'):
            source.file_bytes(row, {}, None, self.root)

    def test_source_paths_and_external_symlinks_are_refused(self):
        for name in ['../outside', '/outside', 'a//b', 'a/../b']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                source.relative(name)
        (self.root / 'input.cpp').symlink_to('/etc/hosts')
        row = dict(path='input.cpp', store='input.cpp')
        with self.assertRaisesRegex(ValueError, 'Missing or foreign'):
            source.file_bytes(row, {}, None, self.root)

    def test_missing_tools_refuse_before_creating_build_output_for_all_variants(self):
        for variant in ['x64', 'ec', 'native']:
            with self.subTest(variant=variant):
                missing = self.root / 'absent'
                args = argparse.Namespace(variant=variant, stage='configure',
                    meson=missing, ninja=missing, llvm=missing, wine=missing,
                    compiler=missing, work=self.root / variant)
                with self.assertRaisesRegex(ValueError, 'Required source-built input or tool missing'):
                    producer.build(args)
                self.assertFalse(args.work.exists())


if __name__ == '__main__':
    unittest.main()
