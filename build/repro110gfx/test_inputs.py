"""Inert source-transport and absent-tool checks; no renderer or Wine is executed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
import build as producer
import build_hbdx9 as hbdx9
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

    def test_native_link_closure_is_explicit_and_windows_variants_do_not_use_it(self):
        for name in ['libzstd.a', 'libunwind.a']:
            path = self.root / 'lib' / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b'own inert archive fixture')
        options = producer.native_link_options('native', self.root)
        self.assertEqual([value.split('=', 1)[0] for value in options],
                         ['-Dbuild.c_link_args', '-Dbuild.cpp_link_args'])
        expected = [str(self.root / 'lib' / name) for name in ['libzstd.a', 'libunwind.a']]
        for value in options:
            self.assertEqual(json.loads(value.split('=', 1)[1]), expected)
        for variant in ['x64', 'ec', 'i386']:
            self.assertEqual(producer.native_link_options(variant, self.root / 'missing'), [])

    def test_missing_static_native_dependency_refuses_before_build(self):
        with self.assertRaisesRegex(ValueError, 'native static library missing'):
            producer.native_link_options('native', self.root)

    def test_ec_image_uses_amd64_machine_with_pinned_chpe_checker(self):
        data = bytearray(128); data[:2] = b'MZ'
        struct.pack_into('<I', data, 0x3c, 64)
        data[64:68] = b'PE\0\0'; struct.pack_into('<H', data, 68, 0x8664)
        path = self.root / 'own.dll'; path.write_bytes(data)
        checker = self.root / 'own_checker.py'
        checker.write_text('def pe_priznaki(path): return (True, True, True)\n')
        self.assertEqual(producer.pe_identity(path, 'ec', checker)['machine'], '0x8664')
        checker.write_text('def pe_priznaki(path): return (False, False, False)\n')
        with self.assertRaisesRegex(ValueError, 'metadata incomplete'):
            producer.pe_identity(path, 'ec', checker)
        struct.pack_into('<H', data, 68, 0xa641); path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'PE image machine'):
            producer.pe_identity(path, 'ec', checker)

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
        for variant in ['x64', 'ec', 'native', 'i386']:
            with self.subTest(variant=variant):
                missing = self.root / 'absent'
                args = argparse.Namespace(variant=variant, stage='configure',
                    meson=missing, ninja=missing, llvm=missing, wine=missing,
                    compiler=missing, work=self.root / variant)
                with self.assertRaisesRegex(ValueError, 'Required source-built input or tool missing'):
                    producer.build(args)
                self.assertFalse(args.work.exists())

    def test_public_source_transport_checks_contents_and_refuses_symlinks(self):
        here = self.root / 'build/repro110gfx'
        here.mkdir(parents=True)
        path = self.root / 'src/hbdx9/own.cpp'
        path.parent.mkdir(parents=True)
        data = b'// own inert source fixture\n'
        path.write_bytes(data)
        row = dict(path='src/hbdx9/own.cpp', public_source='src/hbdx9/own.cpp', **source.identity(data))
        self.assertEqual(source.file_bytes(row, {}, None, here), data)
        path.write_bytes(data + b'// corruption\n')
        with self.assertRaisesRegex(ValueError, 'Product source differs'):
            source.file_bytes(row, {}, None, here)
        path.unlink(); path.symlink_to('/etc/hosts')
        with self.assertRaisesRegex(ValueError, 'Missing or foreign public source'):
            source.file_bytes(row, {}, None, here)

    def test_build_tool_pin_refuses_changed_compiler_filter(self):
        path = self.root / 'filter.py'
        data = b'# own inert tool fixture\n'
        path.write_bytes(data)
        lock = dict(build_tools=dict(ldpfilter23=dict(path=path.name, **source.identity(data))))
        (self.root / 'source.lock.json').write_text(json.dumps(lock))
        self.assertEqual(source.build_tool('ldpfilter23', self.root), path)
        path.write_bytes(data + b'# changed\n')
        with self.assertRaisesRegex(ValueError, 'build tool bytes differ'):
            source.build_tool('ldpfilter23', self.root)

    def test_hbdx9_missing_inputs_refuse_before_creating_output(self):
        for kind in ['pe', 'native']:
            absent = self.root / 'missing'
            args = argparse.Namespace(kind=kind, stage='compile', compiler=absent, wine=absent,
                meson=absent, ninja=absent, llvm=absent, native_clang=absent, native_clangxx=absent,
                native_ar=absent, work=self.root / kind)
            with self.assertRaisesRegex(ValueError, 'Required source-built input or tool missing'):
                hbdx9.build(args)
            self.assertFalse(args.work.exists())

    def test_hbdx9_commands_keep_filter_scope_and_sse_guest_contract(self):
        args = argparse.Namespace(compiler=self.root / 'compiler', wine=self.root / 'wine',
            native_clangxx=self.root / 'clang++', native_ar=self.root / 'ar', llvm=self.root / 'llvm')
        commands = hbdx9.pe_commands(args, self.root / 'source', self.root / 'output')
        shim = commands[-1][1]
        self.assertIn('-msse2', shim)
        self.assertIn('-mfpmath=sse', shim)
        self.assertIn('-Wl,--no-insert-timestamp', shim)
        self.assertIn('i686-w64-mingw32-clang++', str(shim[0]))
        wrapper = self.root / 'pinned-filter.py'
        commands = hbdx9.native_compile_commands(args, self.root / 'source', self.root / 'build',
                                                self.root / 'output', wrapper)
        compilations = [argv for name, argv in commands if '-c' in argv]
        self.assertEqual(len(compilations), 6)
        for argv in compilations:
            self.assertEqual(argv[:3], [sys.executable, wrapper, args.native_clangxx])
            self.assertIn('-mmacosx-version-min=26.5', argv)
        link = commands[-1][1]
        self.assertEqual(link[0], args.native_clangxx)
        self.assertNotIn(wrapper, link)
        self.assertNotIn('-lLLVMX86CodeGen', link)

    def test_i386_image_checker_rejects_amd64(self):
        data = bytearray(128); data[:2] = b'MZ'
        struct.pack_into('<I', data, 0x3c, 64)
        data[64:68] = b'PE\0\0'; struct.pack_into('<H', data, 68, 0x14c)
        path = self.root / 'own.dll'; path.write_bytes(data)
        self.assertEqual(producer.pe_identity(path, 'i386')['machine'], '0x14c')
        struct.pack_into('<H', data, 68, 0x8664); path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'PE image machine'):
            producer.pe_identity(path, 'i386')


if __name__ == '__main__':
    unittest.main()
