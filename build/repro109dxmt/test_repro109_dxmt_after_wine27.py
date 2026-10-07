"""Owned inert fixtures and mocks only; never downloads or executes vendor bytes."""
import argparse
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tarfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch, Mock

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('source_dxmt_job', Path(__file__).with_name('repro109_dxmt_after_wine27.py'))
job = importlib.util.module_from_spec(spec)
spec.loader.exec_module(job)


class SourceDXMTTests(unittest.TestCase):
    def setUp(self):
        directory = os.environ.get('REPRO109_TEST_TMP')
        if not directory or not Path(directory).is_dir():
            raise RuntimeError('REPRO109_TEST_TMP must be an existing owned Flash/cloud scratch directory')
        self.root = Path(directory) / self._testMethodName
        self.root.mkdir()

    def test_local_refusal_before_paths_tools_and_writes(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(job, 'load_builder') as loader, \
                patch.object(job, 'pins_from_environment') as pins:
            with self.assertRaisesRegex(ValueError, 'local execution refused'):
                job.execute(argparse.Namespace(phase='full'))
            loader.assert_not_called()
            pins.assert_not_called()

    def test_cloud_identity_is_dxmt_arm64_at_full_branch_revision(self):
        env = {'GITHUB_ACTIONS': 'true', 'GITHUB_REPOSITORY': job.SOURCE_REPO,
               'GITHUB_REF': 'refs/heads/macrunner-d3d12', 'GITHUB_SHA': job.DXMT_REVISION}
        with patch.dict(os.environ, env, clear=True), patch.object(job.platform, 'system', return_value='Darwin'), \
                patch.object(job.platform, 'machine', return_value='arm64'):
            job.cloud_guard()
            for key, value in [('GITHUB_ACTIONS', 'false'), ('GITHUB_REPOSITORY', 'unrelated/public'),
                               ('GITHUB_REF', 'refs/heads/main'), ('GITHUB_SHA', ''),
                               ('GITHUB_SHA', '398b841'), ('GITHUB_SHA', 'A' * 40)]:
                with patch.dict(os.environ, {key: value}):
                    with self.assertRaises(ValueError): job.cloud_guard()
            with patch.object(job.platform, 'machine', return_value='x86_64'):
                with self.assertRaises(ValueError): job.cloud_guard()

    def test_four_sha_pins_missing_and_invalid_refused(self):
        env = {name.upper() + '_SHA256': 'a' * 64 for name in job.PIN_NAMES}
        with patch.dict(os.environ, env, clear=True):
            self.assertEqual(len(job.pins_from_environment()), 4)
            for value in ['', 'A' * 64, 'a' * 63, 'a' * 65]:
                with patch.dict(os.environ, {'WINE_DIST_SHA256': value}):
                    with self.assertRaises(ValueError): job.pins_from_environment()

    def test_run_metadata_completed_success_at_exact_revision(self):
        record = dict(status='completed', conclusion='success', headSha=job.WINE_REVISION)
        job.run_metadata(record, job.WINE_REVISION)
        for key, value in [('status', 'in_progress'), ('conclusion', 'failure'), ('headSha', '0' * 40)]:
            with self.assertRaises(ValueError): job.run_metadata(dict(record, **{key: value}), job.WINE_REVISION)
        with self.assertRaises(ValueError): job.run_metadata([], job.WINE_REVISION)

    def test_wine_source_run_requires_main_not_next_version(self):
        record = dict(status='completed', conclusion='success', headSha=job.WINE_REVISION, headBranch='main')
        job.run_metadata(record, job.WINE_REVISION, 'main')
        for value in ['next-1.1.0', None]:
            with self.assertRaises(ValueError):
                job.run_metadata(dict(record, headBranch=value), job.WINE_REVISION, 'main')

    def test_source_run_pins_match_curator_main_and_existing_llvm(self):
        self.assertEqual(job.WINE_REVISION, 'cb9d08f9a429385338aaa61ba8a3127009d635d4')
        self.assertEqual(job.WINE_RUN, '37616083395')
        self.assertEqual(job.LLVM_RUN, '37567821557')
        self.assertEqual(job.LLVM_ARTIFACT_ID, 11460272198)

    def test_named_artifact_identity_expiry_duplicates_and_shape(self):
        row = dict(name='artifact', id=job.LLVM_ARTIFACT_ID, expired=False, size_in_bytes=10)
        self.assertEqual(job.artifact_metadata({'artifacts': [row]}, 'artifact', job.LLVM_ARTIFACT_ID)['id'], row['id'])
        for record in [[], {'artifacts': {}}, {'artifacts': []}, {'artifacts': [row, row]},
                       {'artifacts': [dict(row, expired=True)]}, {'artifacts': [dict(row, id=True)]}]:
            with self.assertRaises(ValueError): job.artifact_metadata(record, 'artifact')
        with self.assertRaises(ValueError): job.artifact_metadata({'artifacts': [row]}, 'artifact', row['id'] + 1)

    def test_seal_hash_and_symlink_refused(self):
        file = self.root / 'receipt.json'; file.write_text('{"inert": true}\n')
        digest = job.sha(file)
        self.assertEqual(job.sealed(file, digest, True), {'inert': True})
        with self.assertRaises(ValueError): job.sealed(file, '0' * 64)
        link = self.root / 'link'; link.symlink_to(file)
        with self.assertRaises(ValueError): job.sealed(link, digest)

    def archive(self, name='owned.txt', link=None):
        path = self.root / 'inert.tar.gz'
        with tarfile.open(path, 'w:gz') as stream:
            member = tarfile.TarInfo(name)
            if link is not None:
                member.type = tarfile.SYMTYPE; member.linkname = link
                stream.addfile(member)
            else:
                content = b'owned inert fixture\n'; member.size = len(content)
                stream.addfile(member, io.BytesIO(content))
        return path

    def test_unpack_owned_bytes_collision_and_wrong_hash(self):
        archive = self.archive(); destination = self.root / 'prefix'
        with self.assertRaises(ValueError): job.unpack(archive, '0' * 64, destination)
        self.assertFalse(destination.exists())
        job.unpack(archive, job.sha(archive), destination)
        self.assertEqual((destination / 'owned.txt').read_bytes(), b'owned inert fixture\n')
        with self.assertRaises(ValueError): job.unpack(archive, job.sha(archive), destination)

    def test_unpack_path_escape_refused(self):
        archive = self.archive('../escaped.txt')
        with self.assertRaises(tarfile.FilterError): job.unpack(archive, job.sha(archive), self.root / 'prefix')
        self.assertFalse((self.root / 'escaped.txt').exists())

    def test_unpack_external_symlink_refused(self):
        archive = self.archive('link', '../../external')
        with self.assertRaises(tarfile.FilterError): job.unpack(archive, job.sha(archive), self.root / 'prefix')

    def test_missing_tool_refused_without_a_bootstrap(self):
        with self.assertRaises(FileNotFoundError):
            job.command([str(self.root / 'missing-tool'), '--version'], self.root, 'missing')
        self.assertTrue((self.root / 'missing.log').is_file())

    def test_adapter_contract_keeps_existing_builder_arguments(self):
        args = argparse.Namespace(**{name: self.root / name for name in
                    ('wine_prefix', 'wine_result', 'deps_prefix', 'deps_manifest', 'llvm_prefix', 'llvm_result')})
        pins = {name: 'a' * 64 for name in job.PIN_NAMES}
        args.wine_files = self.root / 'preflight-wine-files.json'
        args.wine_files_sha256 = 'b' * 64
        argv = job.adapter_argv(self.root / 'dxmt', self.root / 'wine', args, pins, self.root)
        self.assertEqual(argv[1:3], ['-I', '-B'])
        self.assertTrue(argv[3].endswith('build_dxmt.py'))
        self.assertEqual(argv.count('--wine-files'), 1)
        self.assertEqual(argv[argv.index('--wine-files-sha256') + 1], args.wine_files_sha256)
        self.assertEqual(argv[argv.index('--work') + 1], str(self.root / 'dxmt-work'))
        self.assertEqual(argv[argv.index('--wine-result-sha256') + 1], pins['wine_result'])
        self.assertIn('--build', argv)
        self.assertEqual(argv[argv.index('--profile') + 1], job.PROFILE)

    def test_download_checks_metadata_before_fetch(self):
        with patch.object(job, 'command', return_value=json.dumps(dict(status='in_progress', conclusion='', headSha=job.WINE_REVISION))) as command:
            with self.assertRaises(ValueError):
                job.download('public/wine', job.WINE_RUN, job.WINE_REVISION, 'wine', self.root, self.root / 'artifact')
            self.assertEqual(command.call_count, 1)

    def test_download_refuses_next_wine_branch_before_artifact_access(self):
        record = dict(status='completed', conclusion='success', headSha=job.WINE_REVISION,
                      headBranch='next-1.1.0')
        with patch.object(job, 'command', return_value=json.dumps(record)) as command:
            with self.assertRaises(ValueError):
                job.download('t0b1kent/macrunner-wine', job.WINE_RUN, job.WINE_REVISION,
                             'wine', self.root, self.root / 'artifact')
            self.assertEqual(command.call_count, 1)
            self.assertIn('headBranch', command.call_args.args[0][-1])

    def test_recipe_revision_missing_refused_before_loading_or_tools(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(job, 'command') as command, \
                patch.object(job.importlib.util, 'spec_from_file_location') as loader:
            with self.assertRaisesRegex(ValueError, 'Immutable DXMT recipe'):
                job.load_builder(self.root / 'dxmt', self.root / 'wine', self.root)
            command.assert_not_called()
            loader.assert_not_called()

    def test_workflow_uses_pinned_wine_and_dispatch_revision_at_xcode27(self):
        path = Path(__file__).resolve().parents[2] / '.github/workflows/repro109-dxmt-after-wine27-xcode27-arm64.yml'
        text = path.read_text()
        self.assertIn('runs-on: xcode-27', text)
        self.assertIn('DEVELOPER_DIR: /Applications/Xcode_27.app/Contents/Developer', text)
        self.assertIn('ref: ${{ github.sha }}', text)
        self.assertIn('ref: ' + job.WINE_REVISION, text)
        self.assertIn("github.ref == 'refs/heads/macrunner-d3d12'", text)
        self.assertIn("python-version: '3.13.7'", text)
        self.assertIn('contents: read', text)
        for forbidden in ['gh release', 'contents: write', '37579087015']:
            self.assertNotIn(forbidden, text)
        self.assertEqual(text.count('Use the same preparation for inputs and full'), 1)
        for name in job.PIN_NAMES:
            self.assertIn(name.upper() + '_SHA256: ${{ inputs.' + name + '_sha256 }}', text)
        self.assertIn('if: always()', text)
        self.assertIn('path: ${{ runner.temp }}/repro109-dxmt-after-wine27-results/', text)

    def test_source_load_uses_exact_producer_toolchain_preflight(self):
        tool = {'xcode': 'inert pinned SDK fixture'}
        dep = SimpleNamespace(read_lock=Mock(return_value={'schema': 1}), toolchain_preflight=Mock())
        builder = SimpleNamespace(REPO=self.root / 'wine/build', check_inputs=Mock(),
            full=SimpleNamespace(cloud_guard=Mock(), load_driver=Mock(return_value=dep),
                                 apply_profile=Mock(return_value={'toolchain': tool})))
        loader = SimpleNamespace(exec_module=Mock())
        spec = SimpleNamespace(loader=loader)
        with patch.dict(os.environ, GITHUB_SHA='f' * 40), \
                patch.object(job, 'command', side_effect=['f' * 40, '', job.WINE_REVISION, '']), \
                patch.object(job.importlib.util, 'spec_from_file_location', return_value=spec), \
                patch.object(job.importlib.util, 'module_from_spec', return_value=builder):
            actual = job.load_builder(self.root / 'dxmt', self.root / 'wine', self.root)
        self.assertIs(actual, builder)
        builder.full.cloud_guard.assert_called_once_with(job.PROFILE)
        builder.full.apply_profile.assert_called_once_with({'schema': 1}, job.PROFILE)
        dep.toolchain_preflight.assert_called_once_with(tool, self.root)

    def route_fixture(self, phase, consumer_failure=False):
        args = argparse.Namespace(phase=phase, dxmt_checkout=self.root / 'dxmt',
                                  wine_checkout=self.root / 'wine')
        child = {'status': 'FULL_WINE_BUILT_NOT_ACCEPTED', 'install_skipped': 0}
        pins = {name: 'a' * 64 for name in job.PIN_NAMES}
        def put(path, value):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(value) + '\n')
        def download(repo, run_id, revision, kind, out, dest, artifact_id=None):
            if kind == 'wine':
                base = dest / 'repro109-wine-c9-results'
                file = base / 'wine/reports/RESULT.json'; put(file, child); pins['wine_result'] = job.sha(file)
                file = base / 'reports/composition/dependency-manifest.json'; put(file, {}); pins['deps_manifest'] = job.sha(file)
                put(base / 'reports/RESULT.json', dict(status='FULL_WINE_SOURCE_BUILT_NOT_ACCEPTED',
                    first_failure=None, wine=child, install_skipped=0))
            else:
                base = dest / 'repro109-llvm15-results'; put(base / 'reports/RESULT.json', {})
            return {'run': run_id, 'revision': revision}
        def unpack(archive, digest, destination):
            destination.mkdir(parents=True)
            if destination.name == 'prefix' and destination.parent.name == 'deps':
                (destination / 'bin').mkdir()
                for name in ('meson', 'ninja'): (destination / 'bin' / name).write_text('inert fixture\n')
        def compile(argv, cwd, env, log, reports, name, deadline, timeout):
            log.write_text('inert mocked producer\n')
            put(cwd / 'dxmt-work/reports/RESULT.json',
                dict(status='D3D11_SOURCE_BUILT_NOT_FUNCTION_SECTION_ACCEPTED', first_failure=None))
        dep = SimpleNamespace(inventory=lambda prefix: [], command=Mock(side_effect=compile))
        builder = SimpleNamespace(REPO=self.root, full=SimpleNamespace(load_driver=lambda *a: dep),
            validate_wine=Mock(side_effect=ValueError('inert consumer refusal') if consumer_failure else None,
                               return_value={'ec': 'inert'}),
            validate_llvm_prefix=SimpleNamespace(validate=Mock(return_value={'files': 0})))
        env = dict(RUNNER_TEMP=str(self.root), GITHUB_SHA=job.DXMT_REVISION)
        # All compiled/vendor/tool execution and network paths are replaced with inert mocks.
        with patch.dict(os.environ, env), patch.object(job, 'cloud_guard'), \
                patch.object(job, 'pins_from_environment', return_value=pins), \
                patch.object(job, 'load_builder', return_value=builder), \
                patch.object(job, 'download', side_effect=download), \
                patch.object(job, 'unpack', side_effect=unpack), \
                patch.object(job, 'LLVM_RESULT_SHA', hashlib.sha256(b'{}\n').hexdigest()), \
                patch.object(job, 'command', side_effect=lambda argv, *a, **k: '1.11.0' if argv[0].endswith('meson') else '1.13.2'):
            if consumer_failure:
                with self.assertRaisesRegex(ValueError, 'consumer refusal'): job.execute(args)
            else:
                job.execute(args)
        record = json.loads((self.root / 'repro109-dxmt-after-wine27/reports/RESULT.json').read_text())
        self.assertEqual(args.deps_prefix, self.root / 'repro109-wine-c9-work/deps/prefix')
        self.assertEqual(args.wine_prefix, self.root / 'repro109-wine-c9-work/wine/install')
        return dep, record

    def test_inputs_preserves_original_prefix_without_compilation(self):
        dep, record = self.route_fixture('inputs')
        self.assertEqual(record['status'], 'DXMT_INPUTS_VERIFIED_NOT_COMPILED')
        dep.command.assert_not_called()

    def test_full_uses_same_preparation_then_existing_adapter(self):
        dep, record = self.route_fixture('full')
        self.assertEqual(record['status'], 'D3D11_SOURCE_BUILT_NOT_FUNCTION_SECTION_ACCEPTED')
        dep.command.assert_called_once()
        argv = dep.command.call_args.args[0]
        self.assertTrue(argv[3].endswith('build_dxmt.py'))

    def test_consumer_failure_keeps_first_failure_and_never_compiles(self):
        dep, record = self.route_fixture('full', consumer_failure=True)
        self.assertEqual(record['first_failure']['phase'], 'CONSUMER_INPUTS')
        dep.command.assert_not_called()


spec_inputs = importlib.util.spec_from_file_location('source_dxmt_prefix', Path(__file__).with_name('validate_llvm_prefix.py'))
check = importlib.util.module_from_spec(spec_inputs)
spec_inputs.loader.exec_module(check)


class PublishedLLVMInputsTests(unittest.TestCase):
    def setUp(self):
        _, _, _, self.pins, self.llvm = check.expected_inputs()
        self.frozen = json.loads(check.PUBLISHED_INPUTS.read_bytes())

    def frozen_path(self, value):
        return SimpleNamespace(read_bytes=lambda: json.dumps(value).encode())

    def test_exact_published_receipt_keeps_its_independent_zstd_archive(self):
        pins, llvm = check.producer_source_pins(self.pins, self.llvm, check.PUBLISHED_RESULT_SHA256)
        self.assertEqual(pins['zstd']['sha256'], '37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3')
        self.assertEqual(self.pins['zstd']['sha256'], 'eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3')
        self.assertEqual(pins['zstd']['version'], self.pins['zstd']['version'])
        self.assertEqual(pins['zstd']['args'], self.pins['zstd']['args'])
        self.assertEqual(llvm, self.llvm)
        self.assertEqual(set(pins), {'cmake', 'ninja', 'zstd'})

    def test_other_or_missing_receipt_sha_keeps_current_recipe(self):
        with patch.object(check, 'PUBLISHED_INPUTS', self.frozen_path({'invalid': True})):
            for digest in [None, '0' * 64, 'ede9dc8']:
                pins, llvm = check.producer_source_pins(self.pins, self.llvm, digest)
                self.assertIs(pins, self.pins)
                self.assertIs(llvm, self.llvm)

    def test_wrong_producer_run_revision_or_receipt_refused(self):
        for key, value in [('schema', 2), ('producer_run', 'other'),
                           ('producer_revision', '0' * 40), ('result_sha256', '0' * 64)]:
            frozen = copy.deepcopy(self.frozen); frozen[key] = value
            with patch.object(check, 'PUBLISHED_INPUTS', self.frozen_path(frozen)):
                with self.assertRaisesRegex(ValueError, 'provenance'):
                    check.producer_source_pins(self.pins, self.llvm, check.PUBLISHED_RESULT_SHA256)

    def test_missing_extra_or_wrong_source_coverage_refused(self):
        for value in [{}, [], dict(self.frozen['sources'], extra={})]:
            frozen = copy.deepcopy(self.frozen); frozen['sources'] = value
            with patch.object(check, 'PUBLISHED_INPUTS', self.frozen_path(frozen)):
                with self.assertRaisesRegex(ValueError, 'coverage'):
                    check.producer_source_pins(self.pins, self.llvm, check.PUBLISHED_RESULT_SHA256)

    def test_invalid_sha_version_and_flag_shape_refused(self):
        for key, value in [('sha256', 'bad'), ('version', '1.5.6'), ('args', None), ('args', [3])]:
            frozen = copy.deepcopy(self.frozen); frozen['sources']['zstd'][key] = value
            with patch.object(check, 'PUBLISHED_INPUTS', self.frozen_path(frozen)):
                with self.assertRaises(ValueError):
                    check.producer_source_pins(self.pins, self.llvm, check.PUBLISHED_RESULT_SHA256)

    def test_different_llvm_compiler_source_refused(self):
        for key in ['url', 'sha256']:
            frozen = copy.deepcopy(self.frozen)
            frozen['sources']['llvm15'][key] = '0' * 64 if key == 'sha256' else 'https://example.invalid/source'
            with patch.object(check, 'PUBLISHED_INPUTS', self.frozen_path(frozen)):
                with self.assertRaisesRegex(ValueError, 'compiler source'):
                    check.producer_source_pins(self.pins, self.llvm, check.PUBLISHED_RESULT_SHA256)

    def test_duplicate_frozen_json_fields_refused(self):
        source = SimpleNamespace(read_bytes=lambda: b'{"schema":1,"schema":1}')
        with patch.object(check, 'PUBLISHED_INPUTS', source):
            with self.assertRaisesRegex(ValueError, 'Duplicate JSON'):
                check.producer_source_pins(self.pins, self.llvm, check.PUBLISHED_RESULT_SHA256)



if __name__ == '__main__':
    unittest.main(verbosity=2)
