"""Offline controls for the one-line Metal shader fix of the native scenarios (the shader file is the recipe's own input)."""
import hashlib
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import native_metal_fix as fix
PINNED = HERE / 'inputs/basis-overlay' / fix.PATH


class NativeMetalFix(unittest.TestCase):
    def setUp(self):
        root = os.environ.get('REPRO109_TEST_TMP')
        if not root or not Path(root).is_dir():
            raise RuntimeError('REPRO109_TEST_TMP must name an owned existing Flash/cloud fixture root')
        self.temp = tempfile.TemporaryDirectory(prefix='metal-fix-', dir=root)
        self.addCleanup(self.temp.cleanup)
        self.tree = Path(self.temp.name) / 'source'
        (self.tree / fix.PATH).parent.mkdir(parents=True)

    def test_the_recipe_input_is_the_pinned_pre_image(self):
        self.assertEqual(hashlib.sha256(PINNED.read_bytes()).hexdigest(), fix.PRE_SHA256)

    def test_exactly_one_line_changes_and_the_result_is_the_pinned_post_image(self):
        shutil.copyfile(PINNED, self.tree / fix.PATH)
        before = (self.tree / fix.PATH).read_text().splitlines()
        report = fix.apply(self.tree)
        after = (self.tree / fix.PATH).read_text().splitlines()
        self.assertEqual(hashlib.sha256((self.tree / fix.PATH).read_bytes()).hexdigest(), fix.POST_SHA256)
        self.assertEqual([i for i, (a, b) in enumerate(zip(before, after)) if a != b], [152])
        self.assertEqual((len(before), len(after)), (len(after), len(before)))
        self.assertEqual((report['changed_lines'], report['upstream_commit']), (1, fix.UPSTREAM))
        self.assertNotIn('__metal_atomic_fetch_add_explicit', '\n'.join(after))

    def test_any_other_bytes_are_refused_and_a_second_application_too(self):
        (self.tree / fix.PATH).write_text('// something else\n')
        with self.assertRaisesRegex(ValueError, 'pinned bytes'):
            fix.apply(self.tree)
        shutil.copyfile(PINNED, self.tree / fix.PATH)
        fix.apply(self.tree)
        with self.assertRaisesRegex(ValueError, 'pinned bytes'):
            fix.apply(self.tree)

    def test_a_tree_without_the_shader_is_left_alone(self):
        self.assertIsNone(fix.apply(self.tree))


if __name__ == '__main__':
    unittest.main(verbosity=2)
