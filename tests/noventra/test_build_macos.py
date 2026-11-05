"""Pure source/configuration/ownership checks; no simulated native build success."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/noventra'))
import build_macos as build


def digest(data):
    return hashlib.sha256(data).hexdigest()


class SourceChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def put(self, name, data=b'original'):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_exact_preimages_and_absent_additions(self):
        self.put('chrome/file.cc')
        expected = {'chrome/file.cc': digest(b'original'), 'chrome/new.cc': None}
        self.assertEqual(build.verify_files(self.source, expected), expected)

    def test_all_hash_and_absence_mismatches_are_reported(self):
        self.put('chrome/old.cc', b'changed')
        self.put('chrome/new.cc', b'unexpected')
        with self.assertRaises(build.SourceMismatch) as failure:
            build.verify_files(self.source, {'chrome/old.cc': digest(b'old'), 'chrome/new.cc': None})
        self.assertEqual(len(failure.exception.mismatches), 2)
        self.assertEqual(failure.exception.mismatches[0]['actual'], digest(b'changed'))

    def test_symlink_parent_is_never_followed(self):
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'file.cc').write_bytes(b'original')
        (self.source / 'linked').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(build.SourceMismatch) as failure:
            build.verify_files(self.source, {'linked/file.cc': digest(b'original')})
        self.assertIn('Symlink', failure.exception.mismatches[0]['actual']['error'])

    def test_special_file_is_rejected_without_reading(self):
        import os
        os.mkfifo(self.source / 'pipe')
        with self.assertRaises(build.SourceMismatch):
            build.verify_files(self.source, {'pipe': digest(b'')})

    def test_unsafe_source_paths_rejected(self):
        for name in ['/tmp/file', '../file', 'a/../file', 'a//file', '.git/config', 'a\\file', 'a:stream', 'a\nfile']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                build.relative(name)

    def make_layer(self):
        patch = b'diff --git a/chrome/file.cc b/chrome/file.cc\n--- a/chrome/file.cc\n+++ b/chrome/file.cc\n@@ -1 +1 @@\n-old\n+new\n'
        folder = self.root / 'layer'
        folder.mkdir()
        (folder / 'port.patch').write_bytes(patch)
        (folder / 'series').write_text('port.patch\n')
        manifest = {'schema': 'noventra-macos-source-port-v1', 'engine': build.ENGINE,
                    'pristine_files': {'chrome/file.cc': digest(b'old\n')},
                    'final_files': {'chrome/file.cc': digest(b'new\n')},
                    'patches': [{'file': 'port.patch', 'sha256': digest(patch)}],
                    'series_sha256': digest(b'port.patch\n')}
        (folder / 'manifest.json').write_text(json.dumps(manifest))
        return folder, manifest

    def test_manifest_patch_and_series_hashes_bind_inputs(self):
        layer, manifest = self.make_layer()
        self.assertEqual(build.load_layer(layer)[0], manifest)
        (layer / 'port.patch').write_bytes((layer / 'port.patch').read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'Patch hash mismatch'):
            build.load_layer(layer)

    def test_manifest_cannot_hide_patch_paths_or_change_platform(self):
        layer, manifest = self.make_layer()
        manifest['engine'] = {**build.ENGINE, 'platform': '0' * 40}
        (layer / 'manifest.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'engine pins'):
            build.load_layer(layer)
        manifest['engine'] = build.ENGINE
        manifest['pristine_files'] = {'elsewhere.cc': digest(b'old')}
        manifest['final_files'] = {'elsewhere.cc': digest(b'new')}
        (layer / 'manifest.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'Undeclared'):
            build.load_layer(layer)

    def test_duplicate_json_keys_are_rejected(self):
        path = self.root / 'duplicate.json'
        path.write_text('{"engine":1,"engine":2}')
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON key'):
            build.json_object(path)

    def test_actual_candidate_remains_explicitly_uncompiled(self):
        manifest, identity = build.load_layer()
        self.assertEqual(len(manifest['pristine_files']), 677)
        self.assertFalse(manifest['native_build_verified'])
        self.assertFalse(manifest['native_ui_verified'])
        self.assertTrue(manifest['native_preimage_check_required'])
        self.assertRegex(identity, r'^[0-9a-f]{64}$')

    def test_source_version_and_lastchange_must_match_engine(self):
        self.put('chrome/VERSION', b'MAJOR=156\nMINOR=0\nBUILD=8078\nPATCH=79\nHELIUM_MAJOR=0\n')
        stamp = self.put('build/util/LASTCHANGE',
                         ('LASTCHANGE=' + build.ENGINE['chromium'] + '-refs/branch-heads/8078@{#1704}\n').encode())
        self.assertEqual(build.verify_chromium(self.source)['commit'], build.ENGINE['chromium'])
        stamp.write_text('LASTCHANGE=' + '0' * 40 + '\n')
        with self.assertRaisesRegex(ValueError, 'LASTCHANGE'):
            build.verify_chromium(self.source)


class ConfigurationChecks(unittest.TestCase):
    def test_native_gates_override_upstream_release_and_component_flags(self):
        rendered = build.render_gn_args('chrome_pgo_phase=2\nuse_remoteexec=true\n',
            'enable_updater=true\nis_component_build=true\nis_debug=true\n')
        self.assertEqual(build.verify_gn_gates(rendered), build.GN_REQUIRED)
        assignments = build.literal_assignments(rendered)
        self.assertEqual(assignments['chrome_pgo_phase'], '0')
        self.assertEqual(assignments['is_debug'], 'false')

    def test_ambiguous_expression_or_enabled_gate_is_rejected(self):
        text = build.render_gn_args('', '')
        invalid = [text + 'enable_sparkle = true\n',
                   text.replace('enable_sparkle = false', 'enable_sparkle = true'),
                   text.replace('target_cpu = "arm64"', 'target_cpu = host_cpu'),
                   text.replace('is_component_build = false\n', '')]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                build.verify_gn_gates(value)


class OwnershipChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.path = self.root / 'native-build'
        self.platform = self.path / 'platform'

    def tearDown(self):
        self.temporary.cleanup()

    def workspace(self, create=False):
        return build.Workspace(self.path, self.platform, '1' * 64, create=create)

    def test_fresh_workspace_can_own_future_platform_path(self):
        workspace = self.workspace(create=True)
        self.assertEqual(workspace.state['phase'], 'created')
        self.assertEqual(self.workspace().state['manifest_sha256'], '1' * 64)

    def test_existing_unowned_directory_is_not_adopted(self):
        self.path.mkdir()
        with self.assertRaisesRegex(ValueError, 'ownership receipt'):
            self.workspace(create=True)

    def test_changed_workspace_bindings_refuse_resume(self):
        workspace = self.workspace(create=True)
        workspace.state['engine'] = {**build.ENGINE, 'target_cpu': 'x64'}
        workspace.save()
        with self.assertRaisesRegex(ValueError, 'changed'):
            self.workspace()

    def test_source_symlink_refuses_resume(self):
        self.workspace(create=True)
        (self.path / 'src').symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Symlink'):
            self.workspace()

    def test_partial_preparation_is_not_blindly_restarted(self):
        workspace = self.workspace(create=True)
        workspace.begin('upstream-preparation')
        with self.assertRaisesRegex(ValueError, 'Partial preparation'):
            build.prepare(workspace, {}, False)

    def test_non_mac_host_fails_before_tool_or_build_commands(self):
        workspace = self.workspace(create=True)
        with mock.patch.object(build.platform, 'system', return_value='Linux'), \
             mock.patch.object(workspace, 'command') as command:
            with self.assertRaisesRegex(ValueError, 'Native Apple-silicon'):
                build.prerequisites(workspace, 180)
        command.assert_not_called()


if __name__ == '__main__':
    unittest.main()
