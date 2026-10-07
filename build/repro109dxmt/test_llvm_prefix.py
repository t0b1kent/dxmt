"""Own inert source-result/Mach-O/ar fixtures, kept under the pinned Flash scratch."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import unittest
import uuid
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import validate_llvm_prefix as check


def macho(cpu=check.ARM64, filetype=1):
    return b'\xcf\xfa\xed\xfe' + struct.pack('<III', cpu, 0, filetype) + b'own inert header fixture'


def member(name, data):
    raw = (name.ljust(16) + '0'.ljust(12) + '0'.ljust(6) + '0'.ljust(6)
           + '100644'.ljust(8) + str(len(data)).ljust(10) + '`\n').encode('ascii')
    return raw + data + (b'\n' if len(data) % 2 else b'')


def archive(objects=None):
    objects = objects if objects is not None else [macho()]
    return b'!<arch>\n' + b''.join(member('own%d.o/' % i, data) for i, data in enumerate(objects))


class PrefixControls(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ['REPRO109_TEST_TMP']).resolve()
        self.assertTrue(base.is_dir())
        self.root = base / ('prefix-' + uuid.uuid4().hex)
        self.prefix = self.root / 'prefix'
        for name, raw in {'bin/llvm-config': macho(filetype=2),
                          'include/llvm/Config/llvm-config.h': b'own LLVM header',
                          'lib/libLLVMBitWriter.a': archive(), 'lib/libLLVMPasses.a': archive(),
                          'lib/libzstd.a': archive(), 'lib/libunwind.a': archive()}.items():
            path = self.prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        self.dep, full, lock, pins, llvm_pin = check.expected_inputs()
        self.record = dict(schema=1, status=check.STATUS, source_built=True, first_failure=None,
                           install_skipped=0,
                           profile='xcode-cloud', toolchain=full.apply_profile(lock, 'xcode-cloud')['toolchain'],
                           sources=[dict(name=name, url=row['url'], archive_sha256=row['sha256'],
                                         **({'args': row['args']} if name != 'llvm15' else {}))
                                    for name, row in {**pins, 'llvm15': llvm_pin}.items()],
                           components={name: dict(state='BUILT', version=row['version'], source_built=True)
                                       for name, row in pins.items()})
        self.measured_cloud_toolchain()
        for row in check.llvm.llvm_rows():
            self.record['components'][row['name']] = dict(state='BUILT', version='15.0.7',
                                                         source_built=True, args=row['args'])
        self.refresh_inventory()
        self.result = self.root / 'RESULT.json'

    def refresh_inventory(self):
        self.record['files'] = self.dep.inventory(self.prefix)
        values = dict(version='15.0.7', host_target='arm64-apple-darwin26.0.0',
                      libfiles=' '.join(str(self.prefix / name) for name in
                                       ('lib/libLLVMBitWriter.a', 'lib/libLLVMPasses.a')),
                      system_libs='-lm -lz -lcurses -lxml2 -lzstd')
        self.record['air_link'] = check.llvm.validate_config(values, self.prefix)
        index = {row['path']: row for row in self.record['files']}
        self.record['air_installed_archives'] = [{key: index[name][key] for key in ('path', 'bytes', 'sha256')}
                                               for name in ('lib/libLLVMBitWriter.a', 'lib/libLLVMPasses.a')]

    def save(self):
        self.result.write_text(json.dumps(self.record, sort_keys=True) + '\n')
        return hashlib.sha256(self.result.read_bytes()).hexdigest()

    def accept(self):
        return check.validate(self.prefix, self.result, self.save())

    def reject(self, text):
        with self.assertRaisesRegex(ValueError, text):
            self.accept()

    def test_complete_input_and_every_archive_object(self):
        result = self.accept()
        self.assertEqual((result['source_pins'], result['selected_archives'], result['archive_objects']), (4, 4, 4))
        self.assertEqual(result['source_execution'], 0)

    def measured_cloud_toolchain(self):
        self.record['toolchain'].update(
            python='3.9.6', python_path='/own/xcode/bin/python3',
            xcode='27.0', xcode_build='27A266a',
            apple_clang='Apple clang version 21.0.0 (own fixture)',
            apple_ld='PROJECT:ld-' + self.record['toolchain']['ld'],
            macos_build='26A428', exact_pins_state='MEASURED_BEFORE_SOURCES')

    def test_staged_producer_measured_toolchain_without_root_profile(self):
        # Actual b37 plan.accept writes profile only in the measured toolchain.
        self.record.pop('profile')
        self.measured_cloud_toolchain()
        original = copy.deepcopy(self.record)
        result = self.accept()
        self.assertEqual(result['profile'], 'xcode-cloud')
        self.assertEqual(result['toolchain'], original['toolchain'])
        self.assertEqual(self.record, original)

    def test_direct_github_producer_exact_toolchain(self):
        _, full, lock, _, _ = check.expected_inputs()
        self.record['profile'] = 'github'
        self.record['toolchain'] = full.apply_profile(lock, 'github')['toolchain']
        self.assertEqual(self.accept()['profile'], 'github')

    def test_github_macos15_producer_exact_toolchain(self):
        _, full, lock, _, _ = check.expected_inputs()
        self.record['profile'] = 'github-macos15-arm64'
        self.record['toolchain'] = full.apply_profile(lock, 'github-macos15-arm64')['toolchain']
        self.assertEqual(self.accept()['profile'], 'github-macos15-arm64')
        self.record['toolchain']['sdk'] = 'own-wrong-sdk'
        self.reject('toolchain differs')

    def test_root_and_nested_profile_conflict_is_rejected(self):
        self.record['profile'] = 'github'
        self.reject('profile conflict')

    def test_staged_measurements_are_all_required(self):
        self.record.pop('profile')
        saved = copy.deepcopy(self.record)
        for name in ('python', 'python_path', 'xcode', 'xcode_build', 'apple_clang',
                     'apple_ld', 'macos_build', 'exact_pins_state'):
            with self.subTest(field=name):
                self.record = copy.deepcopy(saved)
                self.record['toolchain'].pop(name)
                self.reject('toolchain')

    def test_measured_cloud_toolchain_policy_is_checked(self):
        saved = copy.deepcopy(self.record)
        cases = {'python': '3.8.20', 'python_path': 'relative/bin/python3',
                 'xcode': '26.0', 'xcode_build': '', 'apple_clang': 'Apple clang version 20.0.0',
                 'apple_ld': 'PROJECT:ld-1.0', 'macos_build': 'unmeasured',
                 'exact_pins_state': 'NOT_ENABLED', 'sdk': '28.0'}
        for name, value in cases.items():
            with self.subTest(field=name):
                self.record = copy.deepcopy(saved)
                self.record['toolchain'][name] = value
                self.reject('toolchain|cloud policy|macOS build')
        self.record = copy.deepcopy(saved)
        self.record['toolchain']['unexpected_field'] = 'unknown'
        self.reject('toolchain')

    def test_missing_and_invalid_install_skipped_are_rejected(self):
        for value in (None, False, '0', 1):
            with self.subTest(value=value):
                self.record['install_skipped'] = value
                self.reject('install skipped')
        self.record.pop('install_skipped')
        self.reject('install skipped')

    def test_relocation_uses_relative_bytes_not_historical_absolute_path(self):
        old = str(self.prefix)
        self.record['air_link']['libfiles'] = self.record['air_link']['libfiles'].replace(old, '/own/old/prefix')
        self.assertEqual(self.accept()['relocation'], 'RELATIVE_ARCHIVE_DIGESTS_VERIFIED')

    def test_failed_paused_started_and_missing_source_build_are_rejected(self):
        for status in ('FAILED', 'PAUSED_NOT_ACCEPTED', 'STARTED'):
            with self.subTest(status=status):
                self.record['status'] = status
                self.reject('complete source build')
        self.record['status'] = check.STATUS
        self.record['source_built'] = 1
        self.reject('complete source build')

    def test_first_failure_is_not_ignored(self):
        self.record['first_failure'] = {'phase': 'llvm15:finish'}
        self.reject('complete source build')

    def test_wrong_pinned_manifest(self):
        self.save()
        with self.assertRaisesRegex(ValueError, 'RESULT bytes differ'):
            check.validate(self.prefix, self.result, '0' * 64)

    def test_json_root_type_and_duplicate_fields(self):
        for raw in (b'[]', b'{"schema":1,"schema":1}'):
            with self.subTest(raw=raw):
                self.result.write_bytes(raw)
                with self.assertRaises(ValueError):
                    check.validate(self.prefix, self.result, hashlib.sha256(raw).hexdigest())

    def test_source_sha_and_official_publisher(self):
        for field, value in (('url', 'https://invalid.example/own'), ('archive_sha256', '0' * 64)):
            saved = copy.deepcopy(self.record)
            self.record['sources'][0][field] = value
            self.reject('publisher/archive pin')
            self.record = saved

    def test_duplicate_missing_and_extra_source(self):
        saved = copy.deepcopy(self.record)
        for rows in ([*saved['sources'], saved['sources'][0]], saved['sources'][:-1],
                     [*saved['sources'], dict(name='unexpected')]):
            self.record['sources'] = rows
            self.reject('source set')

    def test_component_versions_flags_and_toolchain(self):
        saved = copy.deepcopy(self.record)
        for key, value in (('version', '16.0.0'), ('args', []), ('source_built', False)):
            self.record = copy.deepcopy(saved)
            self.record['components']['llvm15'][key] = value
            self.reject('component|CMake flags')
        self.record = saved
        self.record['toolchain'] = {}
        self.reject('toolchain')

    def test_zstd_shared_flags_are_rejected(self):
        row = next(row for row in self.record['sources'] if row['name'] == 'zstd')
        row['args'] = []
        self.reject('configure flags')

    def test_unrecorded_extra_file_and_byte_drift(self):
        path = self.prefix / 'own-extra'
        path.write_bytes(b'own')
        self.reject('Whole LLVM prefix')
        self.record['files'] = self.dep.inventory(self.prefix)
        path.write_bytes(b'changed')
        self.reject('Whole LLVM prefix')

    def test_missing_archive(self):
        row = self.record['air_link']['selected_archives'][0]
        row['path'] = 'lib/own-missing.a'
        self.reject('selection/digests')

    def test_external_symlink_is_rejected(self):
        target = self.root / 'own-outside'
        target.write_bytes(b'own')
        (self.prefix / 'own-link').symlink_to(target)
        self.reject('inventory invalid')

    def test_required_internal_symlink_is_rejected(self):
        path = self.prefix / 'bin/own-config'
        path.write_bytes(macho(filetype=2))
        current = self.prefix / 'bin/llvm-config'
        # Preserve the own existing fixture before replacing its path.
        current.rename(self.prefix / 'bin/own-original-config')
        current.symlink_to('own-config')
        self.record['files'] = self.dep.inventory(self.prefix)
        self.reject('Required LLVM input')

    def test_selected_archive_symlink_is_rejected(self):
        current = self.prefix / 'lib/libLLVMPasses.a'
        current.rename(self.prefix / 'lib/own-original-passes.a')
        current.symlink_to('own-original-passes.a')
        self.record['files'] = self.dep.inventory(self.prefix)
        self.reject('AIR archive missing or symlink')

    def test_wrong_architecture_even_with_matching_inventory_and_hashes(self):
        (self.prefix / 'lib/libLLVMPasses.a').write_bytes(archive([macho(0x01000007)]))
        self.refresh_inventory()
        self.reject('architecture or file type')

    def test_wrong_llvm_config_architecture_even_with_matching_inventory(self):
        (self.prefix / 'bin/llvm-config').write_bytes(macho(0x01000007, 2))
        self.refresh_inventory()
        self.reject('architecture or file type')

    def test_unknown_sdk_flag_and_outside_recorded_archive(self):
        for value in ('-lunknown', '/own/outside/lib.a'):
            self.record['air_link']['system_libs'] = value
            self.reject('path leaves recorded prefix')

    def test_duplicate_install_archive(self):
        self.record['air_installed_archives'].append(copy.deepcopy(self.record['air_installed_archives'][0]))
        self.reject('Duplicate LLVM AIR')

    def test_install_and_selected_sets_must_match(self):
        self.record['air_installed_archives'].pop()
        self.reject('installed/selected AIR')

    def test_apple_extended_names_symbol_table_and_all_objects(self):
        name = b'own-long-object-name.o\0'
        path = self.prefix / 'lib/libLLVMPasses.a'
        path.write_bytes(b'!<arch>\n' + member('__.SYMDEF/', b'own index')
                         + member('#1/' + str(len(name)), name + macho()) + member('other.o/', macho()))
        # The fixed-width BSD index name convention also permits a trailing slash.
        self.refresh_inventory()
        self.assertEqual(self.accept()['archive_objects'], 5)

    def test_gnu_names_and_64bit_symbol_index(self):
        path = self.prefix / 'lib/libLLVMPasses.a'
        path.write_bytes(b'!<arch>\n' + member('/SYM64/', b'own index')
                         + member('//', b'own-long-object.o/\n') + member('/0', macho()))
        self.refresh_inventory()
        self.assertEqual(self.accept()['archive_objects'], 4)

    def test_empty_truncated_and_mixed_archives(self):
        path = self.prefix / 'lib/libLLVMPasses.a'
        for raw in (b'!<arch>\n', archive()[:-4], archive([macho(), macho(0x01000007)])):
            with self.subTest(size=len(raw)):
                path.write_bytes(raw)
                self.refresh_inventory()
                self.reject('no ARM64 objects|Truncated ar|architecture or file type')

    def test_cli_acceptance_does_not_execute_llvm_config_and_preserves_output(self):
        pin = self.save()
        output = self.root / 'ACCEPTANCE.json'
        argv = [sys.executable, '-B', '-I', str(Path(check.__file__)), '--prefix', str(self.prefix),
                '--result', str(self.result), '--result-sha256', pin, '--output', str(output)]
        run = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, timeout=15)
        (self.root / 'cli.stdout.raw').write_bytes(run.stdout)
        (self.root / 'cli.stderr.raw').write_bytes(run.stderr)
        self.assertEqual(run.returncode, 0, 'own CLI failed; raw retained')
        before = output.read_bytes()
        run = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, timeout=15)
        (self.root / 'collision.stdout.raw').write_bytes(run.stdout)
        (self.root / 'collision.stderr.raw').write_bytes(run.stderr)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(output.read_bytes(), before)

    def test_prefix_mutation_between_hash_and_archive_inspection_is_rejected(self):
        original = check.archive_architecture
        def inspect(path):
            count = original(path)
            (self.prefix / 'own-new-after-hash').write_bytes(b'own drift')
            return count
        with mock.patch.object(check, 'archive_architecture', side_effect=inspect):
            self.reject('prefix drifted during')

    def test_result_mutation_during_archive_inspection_is_rejected(self):
        original = check.archive_architecture
        def inspect(path):
            count = original(path)
            with self.result.open('ab') as stream:
                stream.write(b' ')
            return count
        with mock.patch.object(check, 'archive_architecture', side_effect=inspect):
            self.reject('RESULT drifted during')


if __name__ == '__main__':
    unittest.main()
