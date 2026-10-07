"""Own inert fixtures; no downloads, third-party tool execution or compilation."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_llvm15 as build
import llvm_link as link
from prepare_native import prepare as native
from prepare_pe import prepare as pe
from freeze_version import freeze
from verify_sources import snapshot, sha
from release_sources import native_rows, inventory


class LLVMInputs(unittest.TestCase):
    def setUp(self):
        self.root = (Path(os.environ['REPRO109_TEST_ROOT']) / self._testMethodName).resolve()
        self.root.mkdir(parents=True, exist_ok=False)
        self.checkpoint = HERE / 'inputs/basis-overlay' / link.TARGET
        previous = Path.cwd()
        self.addCleanup(os.chdir, previous)
        os.chdir(self.root)

    def prepared(self, kind='native'):
        out = Path('prepared')
        (native if kind == 'native' else pe)(out)
        freeze(out, kind)
        return out

    def fixture_archive(self, *, root='llvm-project-15.0.7.src', missing=False, traversal=False):
        path = self.root / 'source.tar'
        with tarfile.open(path, 'w') as stream:
            names = ['llvm/CMakeLists.txt', 'runtimes/CMakeLists.txt', 'libunwind/CMakeLists.txt']
            if missing:
                names.pop()
            if traversal:
                names.append('../outside')
            for name in names:
                data = b'# Own inert source fixture. Never compiled.\n'
                member = tarfile.TarInfo(root + '/' + name)
                member.size = len(data)
                stream.addfile(member, io.BytesIO(data))
        return path

    def config(self):
        prefix = (self.root / 'prefix').resolve()
        (prefix / 'lib').mkdir(parents=True)
        for name in ['LLVMPasses', 'LLVMBitWriter', 'zstd', 'unwind']:
            (prefix / ('lib/lib' + name + '.a')).write_bytes(b'own inert archive fixture\n')
        return prefix, dict(version='15.0.7', host_target='arm64-apple-darwin26.0.0',
                            libfiles=str(prefix / 'lib/libLLVMPasses.a') + ' ' + str(prefix / 'lib/libLLVMBitWriter.a'),
                            system_libs='-lm -lz -lzstd -lcurses -lxml2')

    def test_native_preparation_real(self):
        out = self.prepared()
        sealed = inventory(native_rows())
        result = link.apply(out, 'native')
        self.assertEqual(result['source_after'], snapshot(out / 'source'))
        self.assertEqual(result['source_after']['files'], 336)
        self.assertEqual(inventory(native_rows()), sealed)
        self.assertEqual(result['changed_files'], [link.TARGET])
        text = (out / 'source' / link.TARGET).read_text()
        self.assertNotIn('/usr/local/opt', text)
        self.assertNotIn('llvm_deps', text)
        self.assertEqual(text.count('link_args           : llvm_ld_flags_darwin'), 3)

    def test_pe_preparation_real(self):
        out = self.prepared('pe')
        result = link.apply(out, 'pe')
        self.assertEqual(result['source_after']['files'], 343)
        self.assertEqual(result['source_kind'], 'pe')
        self.assertEqual(result['before_sha256'], link.BEFORE_SHA['pe'])
        text = (out / 'source' / link.TARGET).read_text()
        self.assertNotIn('/opt/homebrew/opt', text)
        self.assertNotIn('llvm_deps', text)
        self.assertEqual(text.count('link_args           : llvm_ld_flags_darwin'), 3)

    def test_transform_source_kind_pins(self):
        before = self.checkpoint.read_bytes()
        with self.assertRaises(ValueError):
            link.transform(before, 'pe')
        out = self.prepared('pe')
        before_pe = (out / 'source' / link.TARGET).read_bytes()
        with self.assertRaises(ValueError):
            link.transform(before_pe, 'native')
        self.assertEqual(link.transform(before, 'native'), link.transform(before_pe, 'pe'))

    def test_static_zstd_selection(self):
        text = link.transform(self.checkpoint.read_bytes()).decode()
        self.assertIn("llvm_air_system_lib == '-lzstd'", text)
        self.assertIn("join_paths(native_llvm_path, 'lib', 'libzstd.a')", text)
        self.assertIn('llvm_air_libfiles + llvm_air_system_flags + [llvm_air_unwind]', text)

    def test_cloud_budget(self):
        self.assertEqual(build.MINUTES, 95)
        self.assertLess(build.MINUTES, 120)

    def test_repeat_refusal_no_overwrite(self):
        out = self.prepared()
        link.apply(out, 'native')
        before = snapshot(out)
        with self.assertRaises(ValueError):
            link.apply(out, 'native')
        self.assertEqual(snapshot(out), before)

    def test_wrong_kind_refusal(self):
        out = self.prepared()
        before = snapshot(out)
        with self.assertRaises(ValueError):
            link.apply(out, 'pe')
        self.assertEqual(snapshot(out), before)

    def test_source_drift_refusal(self):
        out = self.prepared()
        (out / 'source' / link.TARGET).write_bytes(b'foreign Meson input')
        before = snapshot(out)
        with self.assertRaises(ValueError):
            link.apply(out, 'native')
        self.assertEqual(snapshot(out), before)

    def test_transform_drift_refusal(self):
        with self.assertRaises(ValueError):
            link.transform(self.checkpoint.read_bytes() + b'\n')

    def test_backup_collision_refusal(self):
        out = self.prepared()
        (out / 'airconv-darwin.meson.before-llvm-link').write_bytes(b'preserve me')
        before = snapshot(out)
        with self.assertRaises(ValueError):
            link.apply(out, 'native')
        self.assertEqual(snapshot(out), before)

    def test_source_pin_real(self):
        pin = build.llvm_source_pin()
        self.assertEqual(pin['sha256'], build.LLVM_SHA)
        self.assertEqual(pin['url'], build.LLVM_URL)

    def test_cold_archive_real(self):
        archive = self.fixture_archive()
        source, graph = build.unpack_source(archive, self.root / 'unpacked', sha(archive))
        self.assertTrue((source / 'libunwind/CMakeLists.txt').is_file())
        self.assertIsInstance(graph, dict)

    def test_archive_negative_controls(self):
        archive = self.fixture_archive()
        out = self.root / 'unpacked'
        with self.assertRaises(ValueError):
            build.unpack_source(archive, out, '0' * 64)
        self.assertFalse(out.exists())
        build.unpack_source(archive, out, sha(archive))
        before = snapshot(out)
        with self.assertRaises(ValueError):
            build.unpack_source(archive, out, sha(archive))
        self.assertEqual(snapshot(out), before)

    def test_archive_foreign_root(self):
        archive = self.fixture_archive(root='other-release')
        with self.assertRaises(ValueError):
            build.unpack_source(archive, self.root / 'out', sha(archive))
        self.assertFalse((self.root / 'out').exists())

    def test_archive_missing_runtime(self):
        archive = self.fixture_archive(missing=True)
        with self.assertRaises(ValueError):
            build.unpack_source(archive, self.root / 'out', sha(archive))
        self.assertFalse((self.root / 'out').exists())

    def test_archive_traversal(self):
        archive = self.fixture_archive(traversal=True)
        with self.assertRaises(ValueError):
            build.unpack_source(archive, self.root / 'out', sha(archive))
        self.assertFalse((self.root / 'out').exists())

    def test_link_closure_real(self):
        prefix, values = self.config()
        result = build.validate_config(values, prefix)
        self.assertEqual(len(result['selected_archives']), 5)
        self.assertTrue(all(row['sha256'] == hashlib.sha256(b'own inert archive fixture\n').hexdigest()
                            for row in result['selected_archives']))

    def test_link_negative_controls(self):
        prefix, values = self.config()
        for key, value in [('version', '16.0.0'), ('host_target', 'x86_64-apple-darwin26.0.0'),
                           ('libfiles', ''), ('libfiles', '/usr/local/foreign.a'),
                           ('system_libs', '-lunknown'), ('system_libs', '-L/usr/local/lib')]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                build.validate_config(dict(values, **{key: value}), prefix)

    def test_real_config_child(self):
        prefix, values = self.config()
        (prefix / 'bin').mkdir()
        config = prefix / 'bin/llvm-config'
        config.write_text('#!' + sys.executable + '\nimport sys\nvalues=' + repr(values) + '\n'
                          "key='version' if '--version' in sys.argv else 'host_target' if '--host-target' in sys.argv else 'system_libs' if '--system-libs' in sys.argv else 'libfiles'\nprint(values[key])\n")
        config.chmod(0o755)
        calls = []
        def command(argv, log, *, env):
            calls.append(argv)
            with log.open('xb') as stream:
                subprocess.run(argv, env=env, stdin=subprocess.DEVNULL, stdout=stream,
                               stderr=subprocess.STDOUT, timeout=15, check=True)
        result = build.read_config(config, dict(os.environ), self.root, command)
        self.assertEqual(len(calls), 4)
        self.assertEqual(result['version'], '15.0.7')
        self.assertEqual(len(list(self.root.glob('llvm-config-*.log'))), 4)

    def test_existing_cmake_commands(self):
        dep = build.load('llvm_input_test_deps', build.REPO / 'repro109deps/build_deps.py')
        for row in build.llvm_rows():
            steps = dep.build_steps(row, Path('/source/llvm15'), Path('/fresh/prefix'),
                                    Path('/selected/sdk'), 4, ('/selected/clang', '/selected/clang++'))
            self.assertEqual(len(steps), 4 if row['name'] == 'llvm15' else 3)
            self.assertIn('-DCMAKE_OSX_ARCHITECTURES=arm64', steps[0])
            self.assertIn('-DCMAKE_C_COMPILER=/selected/clang', steps[0])
            self.assertIn('-DFETCHCONTENT_FULLY_DISCONNECTED=ON', steps[0])
            self.assertIn('-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local', steps[0])
            self.assertEqual(steps[2][:2], ['/fresh/prefix/bin/cmake', '--install'])

    def test_local_build_refused_before_work(self):
        out = self.root / 'cloud-work'
        with patch.dict(os.environ, {}, clear=True), self.assertRaises(ValueError):
            build.build(out, 4, 'xcode-cloud')
        self.assertFalse(out.exists())

    def test_cmake_queries_all_selected_sources(self):
        dep = build.load('llvm_query_test_deps', build.REPO / 'repro109deps/build_deps.py')
        rows = [next(row for row in dep.read_lock()['components'] if row['name'] == 'zstd'),
                *build.llvm_rows()]
        for row in rows:
            with self.subTest(component=row['name']):
                source = self.root / row['name']
                source.mkdir()
                argv = dep.build_steps(row, source, Path('/fresh/prefix'), Path('/selected/sdk'),
                                       4, ('/selected/clang', '/selected/clang++'))[0]
                directory = build.cmake_query(source, row, argv)
                query = source / directory / '.cmake/api/v1/query'
                self.assertEqual({x.name for x in query.iterdir()}, {'cmakeFiles-v1', 'codemodel-v2'})
                self.assertIsNone(build.cmake_query(source, row, argv[:1] + ['--build', directory]))
        with self.assertRaises(ValueError):
            build.cmake_query(self.root, rows[0], ['cmake', '-S', '.', '-B', '../foreign'])

    def test_cmake_real_ownership_and_failed_raw_preservation(self):
        dep = build.load('llvm_evidence_test_deps', build.REPO / 'repro109deps/build_deps.py')
        for foreign in [False, True]:
            with self.subTest(foreign=foreign):
                base = (self.root / ('foreign' if foreign else 'owned')).resolve()
                source, out, prefix, sdk = [base / x for x in ['source', 'reports', 'prefix', 'sdk']]
                for path in [source, out, prefix, sdk]:
                    path.mkdir(parents=True)
                row = build.llvm_rows()[0]
                configured = source / row['source_subdir']; configured.mkdir()
                (configured / 'CMakeLists.txt').write_text('# own inert fixture\n')
                argv = dep.build_steps(row, source, prefix, sdk, 4, ('/selected/clang', '/selected/clang++'))[0]
                directory = build.cmake_query(source, row, argv)
                build_dir = source / directory
                # Own child simulates only CMake's output contract; no external
                # source/compiler is executed. It refuses absent query files.
                code = r"""import json,sys
from pathlib import Path
build,configured,foreign=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]=='1'
query=build/'.cmake/api/v1/query'
if not all((query/name).is_file() for name in ['cmakeFiles-v1','codemodel-v2']): sys.exit(91)
reply=build/'.cmake/api/v1/reply'; reply.mkdir()
(build/'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL='+str(configured)+'\n')
input_path=str(configured.parent.parent/'foreign.c') if foreign else str(configured/'CMakeLists.txt')
(reply/'cmakeFiles-v1-own.json').write_text(json.dumps({'inputs':[{'path':input_path}]}))
(reply/'codemodel-v2-own.json').write_text(json.dumps({'configurations':[]}))
(build/'compile_commands.json').write_bytes(b'[]\n')
"""
                raw = out / 'own-configure.raw.log'
                with raw.open('xb') as stream:
                    run = subprocess.run([sys.executable, '-B', '-I', '-c', code, str(build_dir),
                                          str(configured), '1' if foreign else '0'], stdout=stream,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=15)
                self.assertEqual(run.returncode, 0, raw.read_text())
                if foreign:
                    with self.assertRaises(AssertionError):
                        build.cmake_evidence(dep, source, row, prefix, sdk, out, directory)
                else:
                    evidence = build.cmake_evidence(dep, source, row, prefix, sdk, out, directory)
                    self.assertEqual(evidence['ownership']['state'], 'PRESENT')
                    self.assertEqual(evidence['ownership']['foreign_files'], 0)
                    self.assertEqual(evidence['compile_commands']['state'], 'PRESENT')
                target = out / 'llvm15-compile_commands.json'
                self.assertEqual(target.read_bytes(), (build_dir / 'compile_commands.json').read_bytes())
                saved = out / ('llvm15-cmake-source-' + directory)
                self.assertEqual((saved / 'CMakeCache.txt').read_bytes(), (build_dir / 'CMakeCache.txt').read_bytes())

    def test_cmake_missing_database_and_collision_refusal(self):
        dep = build.load('llvm_missing_test_deps', build.REPO / 'repro109deps/build_deps.py')
        source, out = self.root / 'source', self.root / 'reports'
        source.mkdir(); out.mkdir()
        row = build.llvm_rows()[0]
        directory = '_repro_llvm'
        work = source / directory; work.mkdir()
        with self.assertRaises(ValueError):
            build.cmake_evidence(dep, source, row, self.root / 'prefix', self.root / 'sdk', out, directory)
        (work / 'compile_commands.json').write_bytes(b'[]\n')
        saved = out / 'llvm15-compile_commands.json'; saved.write_bytes(b'preserved\n')
        with self.assertRaises(ValueError):
            build.cmake_evidence(dep, source, row, self.root / 'prefix', self.root / 'sdk', out, directory)
        self.assertEqual(saved.read_bytes(), b'preserved\n')

    def test_job_real_shell_success_and_failure(self):
        job = HERE / 'run_llvm15.sh'
        for status in [0, 17]:
            with self.subTest(status=status):
                root = (self.root / ('exit-' + str(status))).resolve()
                root.mkdir()
                tools = root / 'tools'
                tools.mkdir()
                builder = tools / 'own-python'
                builder.write_text('#!' + sys.executable + '\n'
                    'import json,sys\nfrom pathlib import Path\n'
                    "work=Path(sys.argv[sys.argv.index('--work')+1])\n"
                    "(work/'reports').mkdir(parents=True)\n(work/'prefix').mkdir()\n"
                    "(work/'reports/RESULT.json').write_text(json.dumps({'state':'OWN_INERT_FIXTURE'}))\n"
                    "(work/'reports/compile.log').write_text('own fixture; install skipped\\n')\n"
                    "(work/'prefix/OWN_FIXTURE.txt').write_text('never compiled')\n"
                    'sys.exit(' + str(status) + ')\n')
                builder.chmod(0o755)
                work = root / 'work'
                env = dict(PATH=str(tools) + ':/usr/bin:/bin',
                           REPRO109_PYTHON=str(builder))
                run = subprocess.run(['/bin/bash', str(job), 'github-macos15-arm64', str(work)],
                    env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
                self.assertEqual(run.returncode, status, run.stderr.decode(errors='replace'))
                self.assertEqual(Path(str(work) + '.rc.txt').read_text(), str(status) + '\n')
                self.assertTrue((work / 'reports/RESULT.json').is_file())
                self.assertTrue((work / 'prefix/OWN_FIXTURE.txt').is_file())
                self.assertTrue(Path(str(work) + '.driver.log').is_file())
                self.assertNotIn('install skipped', run.stdout.decode())
                self.assertIn('install skipped', (work / 'reports/compile.log').read_text())

    def test_job_xcode_local_refusal(self):
        work = self.root / 'repro109-llvm15-work'
        env = dict(PATH='/usr/bin:/bin', REPRO109_PYTHON=sys.executable,
                   MACRUNNER_WINE_RECIPE_ROOT=str(build.REPO))
        run = subprocess.run(['/bin/bash', str(HERE / 'run_llvm15.sh'),
                              'xcode-cloud', str(work)], env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, timeout=30)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(work.exists())
        self.assertEqual(Path(str(work) + '.rc.txt').read_text(), str(run.returncode) + '\n')
        self.assertRegex(Path(str(work) + '.driver.log').read_text(),
                         'Cloud requires Darwin arm64|Source execution/download allowed only in the cloud')


if __name__ == '__main__':
    unittest.main()
