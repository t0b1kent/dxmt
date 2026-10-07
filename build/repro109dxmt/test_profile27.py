"""Check the Wine27 consumer profile across parent and DirectX child CLIs."""
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_dxmt as builder
import prepare_directx as directx


class Profile27(unittest.TestCase):
    def test_exact_wine27_profile_has_no_legacy_sdk_substitution(self):
        value = builder.full.apply_profile({'toolchain': {}}, 'github-xcode27-arm64')['toolchain']
        self.assertEqual(value['xcode'], '27.0')
        self.assertEqual(value['xcode_build'], '27A266a')
        self.assertEqual(value['sdk'], '27.0')
        self.assertEqual(value['deployment_target'], '14.0')

    def test_parent_cli_preserves_all_sealed_inputs_and_profile(self):
        argv = ['build_dxmt.py', '--build', '--profile', 'github-xcode27-arm64', '--work', 'owned-output']
        names = ['wine-prefix', 'wine-result', 'wine-files', 'deps-prefix', 'deps-manifest', 'llvm-prefix', 'llvm-result']
        for name in names:
            argv.extend(['--' + name, 'owned-' + name])
        for name in ['wine-result-sha256', 'wine-files-sha256', 'deps-manifest-sha256', 'llvm-result-sha256']:
            argv.extend(['--' + name, 'a' * 64])
        with patch.object(sys, 'argv', argv), patch.object(builder, 'build') as build:
            self.assertEqual(builder.main(), 0)
        build.assert_called_once()
        args = build.call_args.args[0]
        self.assertEqual(args.profile, 'github-xcode27-arm64')
        self.assertEqual(args.llvm_result_sha256, 'a' * 64)
        self.assertEqual(args.wine_result_sha256, 'a' * 64)
        self.assertEqual(args.deps_manifest_sha256, 'a' * 64)

    def test_directx_child_passes_same_profile_to_existing_cloud_guard(self):
        argv = ['prepare_directx.py', '--prepared', 'owned-prepared', '--checkout', 'owned-headers',
                '--kind', 'pe', '--fetch', '--profile', 'github-xcode27-arm64']
        result = {'status': 'OWNED_FIXTURE', 'source_after': {}, 'headers': {'census': {}}}
        with patch.object(sys, 'argv', argv), patch.object(directx, 'cloud_fetch') as fetch, \
             patch.object(directx, 'prepare', return_value=result):
            self.assertEqual(directx.main(), 0)
        fetch.assert_called_once_with(Path('owned-headers'), 'github-xcode27-arm64')

    def test_profile_does_not_authorize_local_source_execution(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, 'cloud'):
                builder.full.cloud_guard('github-xcode27-arm64')


if __name__ == '__main__':
    unittest.main()
