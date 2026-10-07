"""Own inert layout fixtures; no source compilation, network or vendor execution."""
import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wine_layout as layout


class WineLayoutTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ['REPRO109_TEST_TMP']).resolve()
        self.temp = tempfile.TemporaryDirectory(prefix='wine-layout-', dir=base)
        self.directory = Path(self.temp.name)
        self.root = self.directory / 'checkout/build'
        self.root.mkdir(parents=True)
        self.here = self.directory / 'recipe'
        self.here.mkdir()
        self.rows = []
        for number in range(87):
            name = 'build/owned-' + str(number) + '.py'
            target = self.root.parent / name
            raw = b'# own inert bytes\n'
            target.write_bytes(raw)
            self.rows.append(dict(path=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
        self.lock = dict(schema=1, changed_count=87, files=self.rows)
        self.save()
        self.env_patch = patch.dict(os.environ, MACRUNNER_WINE_RECIPE_ROOT=str(self.root))
        self.env_patch.start()
        self.here_patch = patch.object(layout, 'HERE', self.here)
        self.here_patch.start()

    def tearDown(self):
        self.here_patch.stop()
        self.env_patch.stop()
        self.temp.cleanup()

    def save(self):
        (self.here / 'wine-recipe.lock.json').write_text(json.dumps(self.lock))

    def reject(self, text):
        with self.assertRaisesRegex(ValueError, text):
            layout.recipe_root()

    def test_exact_external_recipe(self):
        self.assertEqual(layout.recipe_root(), self.root)

    def test_missing_root(self):
        with patch.dict(os.environ, {}, clear=True):
            self.reject('Set MACRUNNER')

    def test_root_symlink_and_symlink_parent(self):
        alias = self.directory / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        with patch.dict(os.environ, MACRUNNER_WINE_RECIPE_ROOT=str(alias)):
            self.reject('without symlink')
        alias.unlink()
        alias.symlink_to(self.root.parent, target_is_directory=True)
        with patch.dict(os.environ, MACRUNNER_WINE_RECIPE_ROOT=str(alias / 'build')):
            self.reject('without symlink')

    def test_file_drift_and_missing_file(self):
        target = self.root.parent / self.rows[0]['path']
        target.write_bytes(b'different')
        self.reject('input drift')
        target.unlink()
        self.reject('Missing or foreign')

    def test_foreign_file_symlink(self):
        target = self.root.parent / self.rows[0]['path']
        target.unlink()
        target.symlink_to(self.directory / 'outside')
        (self.directory / 'outside').write_bytes(b'# own inert bytes\n')
        self.reject('Missing or foreign')

    def test_duplicate_path_and_wrong_coverage(self):
        self.rows[-1] = copy.deepcopy(self.rows[0])
        self.save()
        self.reject('duplicate')
        self.lock['files'] = self.rows[:-1]
        self.save()
        self.reject('coverage')

    def test_path_traversal_and_foreign_namespace(self):
        for value in ('../outside', '/outside', 'build/../outside', 'build//owned-0.py', 'foreign/owned.py'):
            self.rows[0]['path'] = value
            self.save()
            self.reject('pin path')

    def test_workflow_pin_is_included(self):
        old = self.root.parent / self.rows[0]['path']
        target = self.root.parent / '.github/workflows/own.yml'
        target.parent.mkdir(parents=True)
        target.write_bytes(old.read_bytes())
        self.rows[0]['path'] = '.github/workflows/own.yml'
        self.save()
        self.assertEqual(layout.recipe_root(), self.root)
        target.write_bytes(b'changed')
        self.reject('input drift')


if __name__ == '__main__':
    unittest.main()
