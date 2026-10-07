"""Offline compiler-route controls: owned disk fixtures, no vendor execution/network."""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_dxmt as builder


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value) + '\n')
    return builder.sha(path)


def member(data, name='object.o/'):
    header = (name.ljust(16) + '0'.ljust(12) + '0'.ljust(6) + '0'.ljust(6)
              + '100644'.ljust(8) + str(len(data)).ljust(10) + '`\n').encode()
    return header + data + (b'\n' if len(data) % 2 else b'')


def archive(machine, short=False):
    data = bytearray(20)
    if short:
        data[:4] = b'\0\0\xff\xff'
    struct.pack_into('<H', data, 6 if short else 0, machine)
    return b'!<arch>\n' + member(data)


def macho():
    return struct.pack('<IIII', 0xfeedfacf, 0x100000c, 0, 6) + b'\0' * 48


def pe():
    value = bytearray(70)
    value[:2] = b'MZ'
    struct.pack_into('<I', value, 60, 64)
    value[64:70] = b'PE\0\0\x64\x86'
    return value


class CompilerRouteTests(unittest.TestCase):
    def setUp(self):
        root = os.environ.get('REPRO109_TEST_TMP')
        if not root or not Path(root).is_dir():
            raise RuntimeError('REPRO109_TEST_TMP must name an owned existing Flash/cloud fixture root')
        self.temp = tempfile.TemporaryDirectory(prefix='dxmt-source-', dir=root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.args = SimpleNamespace(profile='xcode-cloud', jobs=4, work=self.root / 'work',
                                    wine_prefix=self.root / 'wine', deps_prefix=self.root / 'deps',
                                    llvm_prefix=self.root / 'llvm')

    def test_cloud_refusal_before_input_reads_or_work_creation(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(builder, 'check_inputs') as check:
            with self.assertRaises(ValueError):
                builder.build(self.args)
            check.assert_not_called()
        self.assertFalse(self.args.work.exists())

    def test_configuration_has_owned_prefixes_and_architecture(self):
        text = builder.machine_file(self.root / 'compiler', 'aarch64')
        self.assertIn('aarch64-w64-mingw32-gcc', text)
        self.assertIn('needs_exe_wrapper = true', text)
        for scenario in builder.SCENARIOS:
            argv = builder.configure_argv(self.root / 'meson', self.root / 'build', self.root / 'source',
                                          self.root / 'cross', self.root / 'native', self.args, scenario)
            self.assertIn('-Dnative_llvm_path=' + str(self.args.llvm_prefix), argv)
            self.assertIn('-Dwine_install_path=' + str(self.args.wine_prefix), argv)
            self.assertIn('-Dstrip=false', argv)
        self.assertNotIn('src/d3d11/d3d11.dll.postproc', builder.PE_TARGETS)
        self.assertEqual(len(builder.PE_TARGETS), 4)

    def test_coff_objects_and_short_imports_both_architectures(self):
        path = self.root / 'import.a'
        for machine in (0xaa64, 0x8664):
            for short in (False, True):
                path.write_bytes(archive(machine, short))
                self.assertEqual(builder.archive_machine(path, machine), 1)
                with self.assertRaisesRegex(ValueError, 'architecture'):
                    builder.archive_machine(path, 0x8664 if machine == 0xaa64 else 0xaa64)

    def test_coff_bsd_long_name_and_symbols(self):
        path = self.root / 'import.a'
        long_name = b'very-long-object-name.o'
        obj = archive(0xaa64)[68:88]
        path.write_bytes(b'!<arch>\n' + member(b'symbols', '/') +
                         member(long_name + obj, '#1/' + str(len(long_name))))
        self.assertEqual(builder.archive_machine(path, 0xaa64), 1)

    def test_coff_empty_thin_mixed_and_truncated_refused(self):
        path = self.root / 'import.a'
        values = [b'!<arch>\n', b'!<thin>\n', archive(0xaa64)[:-1],
                  archive(0xaa64) + archive(0x8664)[8:]]
        for value in values:
            path.write_bytes(value)
            with self.assertRaises(ValueError):
                builder.archive_machine(path, 0xaa64)

    def test_output_native_pe_and_corrupt_pe_offset(self):
        path = self.root / 'output'
        path.write_bytes(macho())
        self.assertEqual(builder.output_machine(path, False), 'arm64_MachO')
        path.write_bytes(pe())
        self.assertEqual(builder.output_machine(path, True), 'x86_64_PE_EC_NOT_APPLICABLE')
        value = pe(); struct.pack_into('<I', value, 60, 2 * 1024**2); path.write_bytes(value)
        with self.assertRaises(ValueError):
            builder.output_machine(path, True)

    def test_receipt_hash_duplicate_keys_and_shape(self):
        path = self.root / 'receipt.json'
        path.write_bytes(b'{"status":1,"status":2}\n')
        with self.assertRaises(ValueError):
            builder.sealed_json(path, builder.sha(path))
        with self.assertRaises(ValueError):
            builder.sealed_json(path, '0' * 64)

    def wine_fixture(self):
        dep = builder.full.load_driver('dxmt_fixture_deps', builder.REPO / 'repro109deps/build_deps.py')
        for arch, machine in [('aarch64', 0xaa64), ('x86_64', 0x8664)]:
            for name in ('winecrt0', 'ntdll', 'dbghelp'):
                path = self.args.wine_prefix / 'lib/wine' / (arch + '-windows') / ('lib' + name + '.a')
                path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(archive(machine, True))
        for name in ('winemac.so', 'ntdll.so'):
            path = self.args.wine_prefix / 'lib/wine/aarch64-unix' / name
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(macho())
        path = self.args.wine_prefix / 'bin/winebuild'; path.parent.mkdir(); path.write_bytes(macho())
        self.args.wine_result = self.root / 'wine-result.json'
        record = dict(status='FULL_WINE_BUILT_NOT_ACCEPTED', source_revision='pinned-fixture',
                      install_skipped=0, ec=2, pe64=3, hexpthk=2, a64xrm=2)
        self.args.wine_result_sha256 = write_json(self.args.wine_result, record)
        self.args.wine_files = self.root / 'wine-files.json'
        self.args.wine_files_sha256 = write_json(self.args.wine_files, dep.inventory(self.args.wine_prefix))
        path = self.args.deps_prefix / 'owned-file'; path.parent.mkdir(); path.write_text('owned inert fixture\n')
        manifest = dict(components={'meson': {'version': '1.11.0'}, 'ninja': {'version': '1.13.2'}},
                        files=dep.inventory(self.args.deps_prefix))
        self.args.deps_manifest = self.root / 'deps.json'
        self.args.deps_manifest_sha256 = write_json(self.args.deps_manifest, manifest)
        wine = SimpleNamespace(inputs=lambda: {'revision': 'pinned-fixture'}, dependencies=lambda *a: manifest)
        return dep, wine, record

    def test_wine_complete_disk_inventory_and_import_architectures(self):
        dep, wine, record = self.wine_fixture()
        value = builder.validate_wine(self.args, dep, wine)
        self.assertEqual(len(value['imports']), 6)
        self.assertEqual(value['ec']['ec'], 2)
        (self.args.wine_prefix / 'extra').write_text('unrecorded\n')
        with self.assertRaisesRegex(ValueError, 'inventory'):
            builder.validate_wine(self.args, dep, wine)

    def test_wine_incomplete_install_and_zero_ec_refused(self):
        dep, wine, record = self.wine_fixture()
        for change in ({'install_skipped': 1}, {'ec': 0}, {'source_revision': 'other'}):
            self.args.wine_result_sha256 = write_json(self.args.wine_result, dict(record, **change))
            with self.assertRaises(ValueError):
                builder.validate_wine(self.args, dep, wine)

    def ownership_fixture(self, source, build, foreign=False, architecture='arm64'):
        source.mkdir(parents=True, exist_ok=True)
        for name in ('winemetal_unix.c', 'cache.c'):
            if not (source / name).exists():
                (source / name).write_text('/* owned inert fixture */\n')
        build.mkdir(parents=True, exist_ok=True)
        write_json(build / 'meson-info/intro-buildsystem_files.json',
                   [str(source / 'cache.c')] + ([str(self.root / 'foreign.c')] if foreign else []))
        rows = [dict(directory=str(build), file=str(source / name),
                     arguments=['owned-clang', '-arch', architecture, '-c', str(source / name)])
                for name in ('winemetal_unix.c', 'cache.c')]
        write_json(build / 'compile_commands.json', rows)

    def test_source_ownership_native_arch_and_foreign_build_inputs(self):
        source, build = self.root / 'source', self.root / 'build'
        self.ownership_fixture(source, build)
        self.assertEqual(len(builder.ownership(source, build, set())['native_c']), 2)
        self.ownership_fixture(source, build, architecture='x86_64')
        with self.assertRaisesRegex(ValueError, 'architecture'):
            builder.ownership(source, build, set())
        self.ownership_fixture(source, build, foreign=True)
        with self.assertRaisesRegex(ValueError, 'foreign'):
            builder.ownership_pe(source, build, set())

    def fake_pipeline(self, inject=None):
        self.args.llvm_result = self.root / 'llvm-result.json'
        self.args.llvm_result_sha256 = 'fixture'
        for name in ('wine_result', 'wine_files', 'deps_manifest'):
            setattr(self.args, name, self.root / (name + '.json'))
            setattr(self.args, name + '_sha256', 'fixture')
        self.calls = []
        compiler = self.root / 'compiler'
        def command(argv, cwd, env, log, out, component, deadline, timeout):
            self.calls.append((component, argv))
            self.assertEqual(env.get('PYTHONDONTWRITEBYTECODE'), '1')
            self.assertEqual(env.get('MACRUNNER_WINE_RECIPE_ROOT'), str(builder.REPO))
            self.assertEqual(env.get('CI_BUILD_NUMBER'), 'owned-fixture-build')
            log.write_text('owned inert fixture\n')
            if inject == component:
                raise ValueError('injected compiler refusal')
            if component in ('meson-version', 'ninja-version'):
                log.write_text('1.11.0\n' if component == 'meson-version' else '1.13.2\n')
            elif component.endswith('-prepare'):
                name = argv[-1]
                source = cwd / name / 'source'
                source.mkdir(parents=True)
                for file in ('winemetal_unix.c', 'cache.c'):
                    (source / file).write_text('/* owned fixture */\n')
            elif component.endswith('-configure'):
                build, source = Path(argv[2]), Path(argv[3])
                self.ownership_fixture(source, build, foreign=inject == 'foreign-configure',
                                       architecture='x86_64' if component == 'pe-configure' else 'arm64')
                for file in ('intro-machines.json', 'intro-buildoptions.json'):
                    write_json(build / 'meson-info' / file, {})
                (build / 'build.ninja').write_text('# owned fixture\n')
            elif component.endswith('-version-header'):
                (Path(argv[2]) / 'version.h').write_text('#pragma once\n\n#define DXMT_VERSION "1226f44"\n')
            elif component.endswith('-commands'):
                log.write_text('compile-owned-PE\n' if component == 'pe-commands' else 'compile-owned-native\n')
                if inject == 'pe-native-air' and component == 'pe-commands':
                    log.write_text('compile airconv.dir/foreign.o\n')
            elif component.endswith('-build'):
                build = Path(argv[2])
                for relative in builder.PE_OUTPUTS if component == 'pe-build' else ['src/winemetal/unix/winemetal.so']:
                    path = build / relative; path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(pe() if component == 'pe-build' else macho())
                if inject == 'source-drift':
                    (build.parent / 'source/cache.c').write_text('changed source\n')
        dep = SimpleNamespace(command=command, read_lock=lambda: {'toolchain': {'deployment_target': '14.0'}},
                              toolchain_preflight=lambda *a: (self.root / 'sdk', 'owned-clang', 'owned-clang++'),
                              environment=lambda *a: {})
        wine = SimpleNamespace(download_compiler=lambda *a: None, prepare_compiler=lambda *a: compiler)
        with patch.dict(os.environ, {'CI_BUILD_NUMBER': 'owned-fixture-build'}), \
             patch.object(builder.full, 'cloud_guard'), patch.object(builder, 'check_inputs',
                return_value=({'files': 1}, {'compiler': {'sha256': 'fixture'}})), \
             patch.object(builder.full, 'apply_profile', side_effect=lambda value, profile: value), \
             patch.object(builder.full, 'load_driver', side_effect=[dep, wine]), \
             patch.object(builder, 'validate_wine', return_value={'owned': True}), \
             patch.object(builder.validate_llvm_prefix, 'validate', return_value={'owned': True}):
            builder.build(self.args)

    def test_full_route_preserves_source_trees_and_selects_only_four_pe_targets(self):
        self.fake_pipeline()
        result = json.loads((self.args.work / 'reports/RESULT.json').read_bytes())
        self.assertEqual(result['status'], 'D3D11_SOURCE_BUILT_NOT_FUNCTION_SECTION_ACCEPTED')
        self.assertEqual(len(result['outputs']), 6)
        builds = [argv for component, argv in self.calls if component == 'pe-build']
        self.assertEqual(builds[0][-4:], builder.PE_TARGETS)
        self.assertEqual(result['install_skipped'], 0)
        self.assertEqual(result['selected_package_mapping'], 'NOT_ACCEPTED')

    def test_full_route_first_failure_keeps_partial_outputs_and_logs(self):
        with self.assertRaisesRegex(ValueError, 'injected'):
            self.fake_pipeline('pe-build')
        result = json.loads((self.args.work / 'reports/RESULT.json').read_bytes())
        self.assertEqual(result['first_failure']['phase'], 'pe:COMPILE')
        self.assertEqual(len(result['outputs']), 2)
        self.assertTrue(result['logs'])
        self.assertTrue((self.args.work / 'outputs/native-r2/winemetal.so').exists())

    def test_pe_subset_native_air_refused_before_pe_compilation(self):
        with self.assertRaisesRegex(ValueError, 'Darwin AIR'):
            self.fake_pipeline('pe-native-air')
        self.assertFalse(any(component == 'pe-build' for component, argv in self.calls))

    def test_foreign_configure_refused_before_compile(self):
        with self.assertRaisesRegex(ValueError, 'foreign'):
            self.fake_pipeline('foreign-configure')
        self.assertFalse(any(component.endswith('-build') for component, argv in self.calls))

    def test_source_drift_during_build_refused_and_recorded(self):
        with self.assertRaisesRegex(ValueError, 'Source tree changed'):
            self.fake_pipeline('source-drift')
        result = json.loads((self.args.work / 'reports/RESULT.json').read_bytes())
        self.assertEqual(result['status'], 'FAILED')
        self.assertEqual(result['first_failure']['phase'], 'native-r2:COMPILE')


if __name__ == '__main__':
    unittest.main(verbosity=2)
