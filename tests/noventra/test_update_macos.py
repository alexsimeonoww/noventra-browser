"""Source migration fixtures only; these do not simulate a successful native build."""

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/noventra'))
import build_macos as build
import update_macos as update


def digest(data):
    return hashlib.sha256(data).hexdigest()


def put(root, name, data):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class UpdateChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.old_kit, self.new_kit = self.root / 'old-kit', self.root / 'new-kit'
        self.workspace_path = self.root / 'native-workspace'
        self.platform = self.workspace_path / 'platform'
        self.pristine = {'chrome/file.cc': b'base\n', 'chrome/stable.cc': b'base stable\n',
                         'chrome/existing.cc': b'existing pristine\n', 'new/fresh.cc': None}
        self.old_final = {'chrome/file.cc': b'old layer\n', 'chrome/stable.cc': b'shared layer\n'}
        self.new_final = {'chrome/file.cc': b'new layer\n', 'chrome/stable.cc': b'shared layer\n',
                          'chrome/existing.cc': b'existing updated\n', 'new/fresh.cc': b'new addition\n'}
        self.old, self.old_identity = self.layer(self.old_kit, self.old_final)
        self.new, self.new_identity = self.layer(self.new_kit, self.new_final)
        # Neither kit's tool is imported or executed. Its bytes are ownership inputs only.
        put(self.old_kit, 'tools/noventra/build_macos.py', b'raise RuntimeError("never execute old code")\n')
        put(self.new_kit, 'tools/noventra/build_macos.py', b'new builder input\n')
        for kit in [self.old_kit, self.new_kit]:
            put(kit, 'noventra/resources/initial_preferences.json', b'{"fixture":true}\n')
        self.pins = {'platform': build.ENGINE['platform'], 'helium': build.ENGINE['helium'], 'tracked_clean': True}
        # Pin validation needs real pinned upstream checkouts on a native host. Everything
        # else here (hashes, patch checks, identity text, journal, filesystem) runs for real.
        self.platform_check = mock.patch.object(build, 'verify_platform', return_value=self.pins)
        self.platform_check.start()
        self.addCleanup(self.platform_check.stop)
        self.workspace = build.Workspace(self.workspace_path, self.platform, self.old_identity,
            create=True, project=self.new_kit, tool_path=self.old_kit / 'tools/noventra/build_macos.py',
            preferences_path=self.old_kit / 'noventra/resources/initial_preferences.json')
        self.source = self.workspace.source
        for name, data in self.pristine.items():
            if data is not None:
                put(self.source, name, data)
        for name, data in self.old_final.items():
            put(self.source, name, data)
        identities = {
            'chrome/app/theme/chromium/BRANDING': b'PRODUCT_FULLNAME=Noventra\nPRODUCT_SHORTNAME=Noventra\nMAC_BUNDLE_ID=local.noventra.browser\n',
            'chrome/common/chrome_paths_mac.mm': b'product_dir_name = "local.noventra.browser";\n',
            'components/os_crypt/common/keychain_password_mac.mm': b'kDefaultServiceName[] = "Noventra Storage Key";\nkDefaultAccountName[] = "Noventra";\nKeychainPassword::GetPasswordWithError() const\n',
            'chrome/browser/browser_process_impl.cc': b'ShowKeychainStartupError()\nbase::Process::TerminateCurrentProcessImmediately(EXIT_FAILURE);\n',
            'chrome/browser/mac/initial_prefs.mm': b'base::apple::OuterBundlePath()\n.Append("initial_preferences")\n',
            'chrome/VERSION': b'MAJOR=156\nMINOR=0\nBUILD=8078\nPATCH=79\n',
            'build/util/LASTCHANGE': ('LASTCHANGE=' + build.ENGINE['chromium'] + '\n').encode(),
            update.ASSETS: b'fixture catalog, never compiled\n',
        }
        for name, data in identities.items():
            put(self.source, name, data)
        self.sentinels = [put(self.source, 'out/Default/obj/sentinel.o', b'original compiler object'),
                          put(self.source, 'out/Default/.siso_deps', b'original siso cache'),
                          put(self.source, 'out/Default/build.ninja', b'original gn paths'),
                          put(self.workspace.root, 'download_cache/source.tar', b'original download')]
        put(self.workspace.root, '.build.lock', b'')
        old_time = 1_600_000_000_123_456_789
        for path in self.sentinels + [self.source / name for name in self.old_final]:
            os.utime(path, ns=(old_time, old_time))
        pristine_path = self.workspace.root / 'evidence/source-pristine.json'
        build.write_json(pristine_path, {'format': 1, 'status': 'passed',
            'manifest_sha256': self.old_identity, 'files': self.old['pristine_files']})
        self.workspace.state['source_report'] = self.workspace.report('source_verified',
            manifest_sha256=self.old_identity, files=self.old['final_files'],
            pristine_report_sha256=build.sha(pristine_path), platform_pins=self.pins,
            chromium=build.verify_chromium(self.source), native_host='Darwin/arm64')
        self.workspace.state['identity_report'] = self.workspace.report('keychain_identity_verified',
            source_files=build.verify_identity(self.source))
        self.workspace.state['assets_car_sha256'] = build.sha(self.source / update.ASSETS)
        self.workspace.state['assets_report'] = self.workspace.report('assets_car_actool',
            assets_car_sha256=self.workspace.state['assets_car_sha256'])
        self.workspace.state['sparkle_report'] = self.workspace.report('sparkle_disabled', fixture=True)
        self.workspace.state['build_jobs'] = 2
        self.workspace.complete('complete')
        put(self.workspace.root, 'build-receipt.json', b'{"fixture":"not-native-build-evidence"}\n')

    def tearDown(self):
        self.temporary.cleanup()

    def layer(self, kit, final):
        scope = set(final)
        pristine = {name: self.pristine[name] for name in scope}
        scratch = kit / 'patch-fixture'
        scratch.mkdir(parents=True)
        subprocess.run(['git', 'init', '--quiet', str(scratch)], check=True)
        for name, data in pristine.items():
            if data is not None:
                put(scratch, name, data)
        subprocess.run(['git', '-C', str(scratch), 'add', '.'], check=True)
        tree = subprocess.check_output(['git', '-C', str(scratch), 'write-tree'], text=True).strip()
        for name, data in final.items():
            put(scratch, name, data)
        subprocess.run(['git', '-C', str(scratch), 'add', '-A'], check=True)
        patch = subprocess.check_output(['git', '-C', str(scratch), 'diff', '--src-prefix=a/', '--dst-prefix=b/', '--binary', '--cached', tree])
        layer = kit / update.LAYER_NAME
        put(layer, 'layer.patch', patch)
        put(layer, 'series', b'layer.patch\n')
        manifest = {'schema': 'noventra-macos-source-port-v1', 'engine': build.ENGINE,
            'pristine_files': {name: digest(data) if data is not None else None for name, data in pristine.items()},
            'final_files': {name: digest(data) for name, data in final.items()},
            'patches': [{'file': 'layer.patch', 'sha256': digest(patch)}], 'series_sha256': digest(b'layer.patch\n')}
        path = put(layer, 'manifest.json', json.dumps(manifest).encode())
        return manifest, build.sha(path)

    def run_update(self):
        return update.update(self.workspace_path, self.old_kit, project=self.new_kit)

    def snapshot(self):
        result = {}
        for path in self.workspace.root.rglob('*'):
            if path.is_file() and 'source-updates' not in path.relative_to(self.workspace.root).parts:
                result[str(path.relative_to(self.workspace.root))] = (path.read_bytes(), stat.S_IMODE(path.stat().st_mode), path.stat().st_mtime_ns)
        return result

    def test_update_changes_only_needed_source_and_archives_old_evidence(self):
        unchanged_time = (self.source / 'chrome/stable.cc').stat().st_mtime_ns
        sentinels = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.sentinels}
        old_state = self.workspace.state_path.read_bytes()
        old_receipt = (self.workspace.root / 'build-receipt.json').read_bytes()
        history = self.run_update()
        self.assertEqual(build.verify_files(self.source, self.new['final_files']), self.new['final_files'])
        self.assertEqual((self.source / 'chrome/stable.cc').stat().st_mtime_ns, unchanged_time)
        self.assertGreater((self.source / 'chrome/file.cc').stat().st_mtime_ns, unchanged_time)
        self.assertEqual({p: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.sentinels}, sentinels)
        self.assertEqual((history / 'before/workspace/workspace.json').read_bytes(), old_state)
        self.assertEqual((history / 'before/workspace/build-receipt.json').read_bytes(), old_receipt)
        self.assertFalse((self.workspace.root / 'build-receipt.json').exists())
        self.assertFalse((history / 'stage').exists())
        state = build.json_object(self.workspace.state_path)
        self.assertEqual(state['phase'], 'prepared')
        self.assertIsNone(state['in_progress'])
        self.assertNotIn('sparkle_report', state)
        self.assertEqual(state['manifest_sha256'], self.new_identity)
        self.assertEqual(state['tool_sha256'], build.sha(self.new_kit / 'tools/noventra/build_macos.py'))
        self.assertEqual(build.json_object(history / 'journal.json')['status'], 'committed')
        new_workspace = build.Workspace(self.workspace_path, self.platform, self.new_identity, project=self.new_kit,
            tool_path=self.new_kit / 'tools/noventra/build_macos.py',
            preferences_path=self.new_kit / 'noventra/resources/initial_preferences.json')
        build.validate_prepared(new_workspace, self.new)
        with self.assertRaisesRegex(ValueError, 'changed'):
            build.Workspace(self.workspace_path, self.platform, self.old_identity, project=self.new_kit,
                tool_path=self.old_kit / 'tools/noventra/build_macos.py',
                preferences_path=self.old_kit / 'noventra/resources/initial_preferences.json')

    def test_unexpected_edit_refuses_without_workspace_writes(self):
        (self.source / 'chrome/file.cc').write_bytes(b'unreviewed edit\n')
        before = self.snapshot()
        with self.assertRaises(build.SourceMismatch):
            self.run_update()
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.workspace.root / 'source-updates').exists())

    def test_new_scope_edit_after_replay_is_preserved_and_refused(self):
        before = self.snapshot()
        replay = update.replay
        def race(*args):
            replay(*args)
            (self.source / 'chrome/existing.cc').write_bytes(b'user edit after replay\n')
        with mock.patch.object(update, 'replay', side_effect=race):
            with self.assertRaisesRegex(ValueError, 'Source changed during update validation'):
                self.run_update()
        after = self.snapshot()
        before['src/chrome/existing.cc'] = after['src/chrome/existing.cc']
        self.assertEqual(after, before)
        self.assertEqual((self.source / 'chrome/existing.cc').read_bytes(), b'user edit after replay\n')

    def test_new_scope_absent_file_created_after_replay_is_preserved_and_refused(self):
        before = self.snapshot()
        replay = update.replay
        def race(*args):
            replay(*args)
            put(self.source, 'new/fresh.cc', b'user addition after replay\n')
        with mock.patch.object(update, 'replay', side_effect=race):
            with self.assertRaisesRegex(ValueError, 'Source changed during update validation'):
                self.run_update()
        after = self.snapshot()
        before['src/new/fresh.cc'] = after['src/new/fresh.cc']
        self.assertEqual(after, before)
        self.assertEqual((self.source / 'new/fresh.cc').read_bytes(), b'user addition after replay\n')

    def test_changed_engine_or_previous_tool_or_preferences_binding_refuses(self):
        for name in ['tools/noventra/build_macos.py', 'noventra/resources/initial_preferences.json']:
            path = self.old_kit / name
            original = path.read_bytes()
            path.write_bytes(original + b'changed')
            before = self.snapshot()
            with self.assertRaisesRegex(ValueError, 'changed'):
                self.run_update()
            self.assertEqual(self.snapshot(), before)
            path.write_bytes(original)
        path = self.new_kit / update.LAYER_NAME / 'manifest.json'
        manifest = build.json_object(path)
        manifest['engine']['chromium'] = '0' * 40
        path.write_text(json.dumps(manifest))
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'engine pins'):
            self.run_update()
        self.assertEqual(self.snapshot(), before)

    def test_wrong_platform_path_and_unowned_workspace_refuse(self):
        with self.assertRaisesRegex(ValueError, 'changed'):
            update.update(self.workspace_path, self.old_kit, self.root, project=self.new_kit)
        (self.workspace.root / 'workspace.json').unlink()
        with self.assertRaises(OSError):
            self.run_update()

    def test_saved_gate_must_bind_exact_old_source(self):
        report_path = self.workspace.root / 'evidence/source_verified.json'
        report = build.json_object(report_path)
        report['files'] = {}
        build.write_json(report_path, report)
        self.workspace.state['source_report']['sha256'] = build.sha(report_path)
        self.workspace.save()
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'Saved source gate'):
            self.run_update()
        self.assertEqual(self.snapshot(), before)

    def test_new_pristine_mismatch_and_patch_mismatch_leave_source_unchanged(self):
        path = self.new_kit / update.LAYER_NAME / 'manifest.json'
        manifest = build.json_object(path)
        manifest['pristine_files']['chrome/existing.cc'] = '0' * 64
        path.write_text(json.dumps(manifest))
        before = self.snapshot()
        with self.assertRaises(build.SourceMismatch):
            self.run_update()
        self.assertEqual(self.snapshot(), before)
        patch = self.old_kit / update.LAYER_NAME / 'layer.patch'
        patch.write_bytes(patch.read_bytes() + b'changed\n')
        with self.assertRaisesRegex(ValueError, 'Patch hash mismatch'):
            self.run_update()
        self.assertEqual(self.snapshot(), before)

    def test_failed_reverse_patch_is_reported_without_source_mutation(self):
        patch = self.old_kit / update.LAYER_NAME / 'layer.patch'
        patch.write_bytes(patch.read_bytes().replace(b'+old layer', b'+wrong layer'))
        path = self.old_kit / update.LAYER_NAME / 'manifest.json'
        manifest = build.json_object(path)
        manifest['patches'][0]['sha256'] = build.sha(patch)
        path.write_text(json.dumps(manifest))
        self.workspace.state['manifest_sha256'] = build.sha(path)
        self.workspace.bindings['manifest_sha256'] = build.sha(path)
        pristine_path = self.workspace.root / 'evidence/source-pristine.json'
        pristine = build.json_object(pristine_path)
        pristine['manifest_sha256'] = build.sha(path)
        build.write_json(pristine_path, pristine)
        report_path = self.workspace.root / 'evidence/source_verified.json'
        report = build.json_object(report_path)
        report.update(manifest_sha256=build.sha(path), pristine_report_sha256=build.sha(pristine_path))
        build.write_json(report_path, report)
        self.workspace.state['source_report']['sha256'] = build.sha(report_path)
        self.workspace.save()
        before = self.snapshot()
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_update()
        self.assertEqual(self.snapshot(), before)

    def test_symlink_source_parent_or_output_or_history_is_rejected(self):
        outside = self.root / 'outside'
        outside.mkdir()
        for name in ['new', 'out', None]:
            if name == 'out':
                (self.source / 'out').rename(self.source / 'out-saved')
            link = self.source / name if name else self.workspace.root / 'source-updates'
            link.symlink_to(outside, target_is_directory=True)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'Symlink'):
                self.run_update()
            link.unlink()
            if name == 'out':
                (self.source / 'out-saved').rename(self.source / 'out')
        self.assertEqual(list(outside.iterdir()), [])

    def test_special_lock_file_is_rejected_without_opening_it(self):
        lock = self.workspace.root / '.build.lock'
        lock.unlink()
        os.mkfifo(lock)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.run_update()

    def test_synchronous_failure_restores_source_modes_times_and_old_receipts(self):
        os.chmod(self.source / 'chrome/file.cc', 0o755)
        before = self.snapshot()
        with mock.patch.object(update, 'prepare_metadata', side_effect=ValueError('injected failure')):
            with self.assertRaisesRegex(ValueError, 'injected failure'):
                self.run_update()
        self.assertEqual(self.snapshot(), before)
        history = next((self.workspace.root / 'source-updates').iterdir())
        self.assertEqual(build.json_object(history / 'journal.json')['status'], 'rolled-back')
        self.assertFalse((self.source / 'new').exists())

    def test_failure_during_gate_publication_restores_old_receipts(self):
        before = self.snapshot()
        replace = update.replace_file
        def fail(workspace, directory, entry, version):
            if entry['target'] == 'workspace' and entry['path'] == 'evidence/source_verified.json' and version == 'after':
                raise ValueError('injected gate publication failure')
            return replace(workspace, directory, entry, version)
        with mock.patch.object(update, 'replace_file', side_effect=fail):
            with self.assertRaisesRegex(ValueError, 'gate publication failure'):
                self.run_update()
        self.assertEqual(self.snapshot(), before)

    def test_crash_after_durable_commit_finishes_publication_before_build_can_resume(self):
        sentinels = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.sentinels}
        replace = update.replace_file
        def fail(workspace, directory, entry, version):
            if entry['target'] == 'workspace' and entry['path'] == 'workspace.json' and version == 'after':
                raise OSError('injected final publication interruption')
            return replace(workspace, directory, entry, version)
        with mock.patch.object(update, 'replace_file', side_effect=fail):
            with self.assertRaisesRegex(OSError, 'final publication interruption'):
                self.run_update()
        history = next((self.workspace.root / 'source-updates').iterdir())
        self.assertEqual(build.json_object(history / 'journal.json')['status'], 'committed')
        state = build.json_object(self.workspace.state_path)
        self.assertEqual(state['in_progress'], 'source-update')
        self.workspace.state = state
        with self.assertRaisesRegex(ValueError, 'built/resumed'):
            build.build(self.workspace, self.old, 2)
        self.assertEqual(update.recover(self.workspace_path, history), 'committed')
        state = build.json_object(self.workspace.state_path)
        self.assertEqual(state['manifest_sha256'], self.new_identity)
        self.assertEqual(state['phase'], 'prepared')
        self.assertIsNone(state['in_progress'])
        self.assertEqual({p: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.sentinels}, sentinels)
        self.assertEqual(update.recover(self.workspace_path, history), 'committed')

    def interrupt(self):
        with mock.patch.object(update, 'prepare_metadata', side_effect=ValueError('interruption fixture')), \
             mock.patch.object(update, 'rollback', side_effect=OSError('simulate process loss before rollback')):
            with self.assertRaisesRegex(OSError, 'process loss'):
                self.run_update()
        history = next((self.workspace.root / 'source-updates').iterdir())
        self.assertEqual(build.json_object(history / 'journal.json')['status'], 'applying')
        return history

    def test_interrupted_update_recovery_restores_original_and_is_repeatable(self):
        before = self.snapshot()
        history = self.interrupt()
        with self.assertRaisesRegex(ValueError, 'Interrupted source update'):
            self.run_update()
        update.recover(self.workspace_path, history)
        self.assertEqual(self.snapshot(), before)
        update.recover(self.workspace_path, history)
        self.assertEqual(self.snapshot(), before)
        self.run_update()

    def test_recovery_restores_a_partially_written_source_transaction(self):
        before = self.snapshot()
        replace = update.replace_file
        count = 0
        def interrupt(workspace, directory, entry, version):
            nonlocal count
            if entry['target'] == 'source' and version == 'after':
                count += 1
                if count == 2:
                    raise OSError('partial source interruption')
            return replace(workspace, directory, entry, version)
        with mock.patch.object(update, 'replace_file', side_effect=interrupt), \
             mock.patch.object(update, 'rollback', side_effect=OSError('process loss')):
            with self.assertRaisesRegex(OSError, 'process loss'):
                self.run_update()
        history = next((self.workspace.root / 'source-updates').iterdir())
        self.assertEqual((self.source / 'chrome/existing.cc').read_bytes(), self.new_final['chrome/existing.cc'])
        self.assertEqual((self.source / 'chrome/file.cc').read_bytes(), self.old_final['chrome/file.cc'])
        update.recover(self.workspace_path, history)
        self.assertEqual(self.snapshot(), before)

    def test_recovery_conflict_rejects_every_write_and_preserves_user_edit(self):
        history = self.interrupt()
        (self.source / 'chrome/file.cc').write_bytes(b'post-interruption user edit\n')
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'Recovery conflict'):
            update.recover(self.workspace_path, history)
        self.assertEqual(self.snapshot(), before)
        with self.assertRaisesRegex(ValueError, 'owned source-updates'):
            update.recover(self.workspace_path, self.root)

    def test_scope_shrink_is_explicitly_rejected(self):
        path = self.new_kit / update.LAYER_NAME / 'manifest.json'
        manifest = build.json_object(path)
        # A valid smaller new layer must still be refused before source mutation.
        self.new, self.new_identity = self.layer(self.root / 'smaller-kit', {'chrome/file.cc': b'new layer\n'})
        smaller_layer = self.root / 'smaller-kit' / update.LAYER_NAME
        path.write_bytes((smaller_layer / 'manifest.json').read_bytes())
        (path.parent / 'layer.patch').write_bytes((smaller_layer / 'layer.patch').read_bytes())
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'scope shrank'):
            self.run_update()
        self.assertEqual(self.snapshot(), before)

    def test_changed_asset_catalog_is_rejected_before_source_mutation(self):
        catalog = update.CATALOG + 'Contents.json'
        self.pristine[catalog] = b'base catalog\n'
        self.new_final[catalog] = b'new catalog\n'
        replacement, _ = self.layer(self.root / 'catalog-kit', self.new_final)
        source = self.root / 'catalog-kit' / update.LAYER_NAME
        destination = self.new_kit / update.LAYER_NAME
        for name in ['manifest.json', 'layer.patch', 'series']:
            (destination / name).write_bytes((source / name).read_bytes())
        put(self.source, catalog, self.pristine[catalog])
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'actool regeneration'):
            self.run_update()
        self.assertEqual(self.snapshot(), before)


if __name__ == '__main__':
    unittest.main()
