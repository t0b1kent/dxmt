"""Exercise the owned serializers without importing or running vendor/build code.

Meson's documented escapes match Python string escapes for these fixtures.
Actual Meson 1.11.0 acceptance remains the next cloud configure boundary.
"""
import ast
import configparser
import json
import os
from pathlib import Path
import unittest


SOURCE = Path(os.environ.get('REPRO109_QUOTING_SOURCE', str(Path(__file__).with_name('build_dxmt.py'))))
tree = ast.parse(SOURCE.read_text())
names = {'meson_string', 'native_machine_file', 'machine_file'}
nodes = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
namespace = {'Path': Path, 'json': json}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)


def values(text, section):
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(text)
    answer = {}
    for key, token in parser[section].items():
        if not (token.startswith("'") and token.endswith("'")):
            raise ValueError('Meson requires single quoted string: ' + key)
        answer[key] = ast.literal_eval(token)
    return answer


class MachineFileTests(unittest.TestCase):
    def test_native_actual_xcode27_paths(self):
        prefix = '/Applications/Xcode_27.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/'
        text = namespace['native_machine_file'](prefix + 'clang', prefix + 'clang++')
        self.assertEqual(values(text, 'binaries'), {'c': prefix + 'clang', 'cpp': prefix + 'clang++'})

    def test_native_spaces_quotes_unicode_and_backslash(self):
        path = '/work/Тест O\'Brien/"quoted"/back\\slash/clang'
        text = namespace['native_machine_file'](path, path + '++')
        self.assertEqual(values(text, 'binaries'), {'c': path, 'cpp': path + '++'})

    def test_cross_both_architectures_all_five_binaries(self):
        compiler = Path('/work/tool chain/O\'Brien/"quoted"/back\\slash')
        for arch in ['aarch64', 'x86_64']:
            with self.subTest(arch=arch):
                text = namespace['machine_file'](compiler, arch)
                expected = {key: str(compiler / 'bin' / (arch + '-w64-mingw32-' + suffix))
                            for key, suffix in [('c', 'gcc'), ('cpp', 'g++'), ('windres', 'windres')]}
                expected.update(ar=str(compiler / 'bin/llvm-ar'), strip=str(compiler / 'bin/llvm-strip'))
                self.assertEqual(values(text, 'binaries'), expected)
                self.assertEqual(values(text, 'host_machine'),
                                 {'system': 'windows', 'cpu_family': arch, 'cpu': arch, 'endian': 'little'})
                self.assertIn('needs_exe_wrapper = true', text)

    def test_string_edges_preserve_exact_values(self):
        for value in ['', "'", '"', '\\', '\\"', "\\'", '\n\r\t\b\f\x00', '😀']:
            with self.subTest(value=value):
                token = namespace['meson_string'](value)
                self.assertTrue(token.startswith("'") and token.endswith("'"))
                self.assertEqual(ast.literal_eval(token), value)
                self.assertNotIn('\n', token)

    def test_generated_native_has_two_values_and_no_injected_section(self):
        path = "/clang'\n[unexpected]\nc = 'bad"
        text = namespace['native_machine_file'](path, path)
        parser = configparser.ConfigParser(interpolation=None)
        parser.read_string(text)
        self.assertEqual(parser.sections(), ['binaries'])
        self.assertEqual(values(text, 'binaries'), {'c': path, 'cpp': path})


if __name__ == '__main__':
    unittest.main()
