"""Standalone install/uninstall fixtures in disposable homes; never a browser."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import tarfile
import time
import unittest
from unittest.mock import patch

import test_package_distribution as fixtures

package = fixtures.package
spec = importlib.util.spec_from_file_location('noventra_installer', fixtures.ROOT / 'noventra/package/installer.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InterruptedUpdate(BaseException):
    """Model process termination without running the ordinary exception rollback."""


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.PackageTests(methodName='test_hash_mismatch')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.home = self.root / 'home with spaces Café'
        self.home.mkdir()
        self.data = self.home / '.local/share'
        self.profile = self.home / '.config/noventra-browser/Default/Preferences'
        self.profile.parent.mkdir(parents=True)
        self.profile.write_text('private profile fixture; keep unchanged')
        self.env = dict(os.environ, HOME=str(self.home), XDG_DATA_HOME=str(self.data),
                        XDG_CONFIG_HOME=str(self.home / '.config'), PATH='/usr/bin:/bin',
                        NOVENTRA_VIDEO_BUFFER_WORKAROUND='off')
        self.env.pop('APPIMAGE', None)
        self.env.pop('LD_LIBRARY_PATH', None)
        self.env.pop('PYTHONPATH', None)

    def make_package(self, archive=False):
        self.fixture.run_package(archive=archive)
        return self.fixture.output / 'Noventra.AppDir'

    def run_helper(self, source, operation='install', success=True, data=None):
        result = subprocess.run([str(source / (operation + '.sh')),
                                 '--data-home', str(data or self.data)],
                                env=self.env, capture_output=True, text=True, timeout=45)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0)
        return result

    @property
    def managed(self):
        return self.data / 'noventra-browser'

    def test_moved_archive_installs_independent_copy_without_checkout(self):
        source = self.make_package(archive=True)
        moved = self.root / 'archive moved to another folder'
        with tarfile.open(next(self.fixture.output.glob('*.tar.xz'))) as bundle:
            bundle.extractall(moved, filter='data')
        source = moved / 'Noventra.AppDir'
        result = self.run_helper(source)
        self.assertEqual(result['location'], str(self.managed / 'app'))
        manifest = json.loads((source / 'install-manifest.json').read_text())
        for name, record in manifest['files'].items():
            if 'symlink' not in record:
                installed = self.managed / 'app' / name
                self.assertEqual(installed.read_bytes(), (source / name).read_bytes())
                self.assertNotEqual(installed.stat().st_ino, (source / name).stat().st_ino)
        desktop = self.data / 'applications/noventra-browser.desktop'
        if shutil.which('desktop-file-validate'):
            subprocess.run(['desktop-file-validate', str(desktop)], check=True)
        shutil.rmtree(moved)
        shutil.rmtree(self.fixture.output)
        self.run_helper(self.managed / 'app', 'uninstall')
        self.assertFalse(self.managed.exists())
        self.assertFalse(desktop.exists())
        self.assertEqual(self.profile.read_text(), 'private profile fixture; keep unchanged')

    def another_package(self, version):
        output = self.root / ('package-' + version)
        package.package(self.fixture.source, output, version)
        return output / 'Noventra.AppDir'

    def test_repeat_is_idempotent_and_changed_install_refused(self):
        source = self.make_package()
        self.run_helper(source)
        binary = self.managed / 'app/opt/noventra/noventra-browser'
        before = binary.stat().st_ino
        self.assertTrue(self.run_helper(source)['already_installed'])
        self.assertEqual(binary.stat().st_ino, before)
        binary.write_text('user changed runtime')
        refused = self.run_helper(source, success=False)
        self.assertIn('Changed installed file', refused.stderr)
        self.assertEqual(binary.read_text(), 'user changed runtime')

    def test_multiple_updates_preserve_profiles_and_original_desktop_backups(self):
        desktop = self.data / 'applications/noventra-browser.desktop'
        icon = self.data / 'icons/hicolor/64x64/apps/noventra-browser.png'
        for path, value in ((desktop, b'original development launcher'), (icon, b'original icon')):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
            path.chmod(0o640)
        source = self.make_package()
        self.run_helper(source)
        first = json.loads((self.managed / 'installation.json').read_text())
        for number in (2, 3):
            with self.subTest(version=number):
                self.fixture.put('icudtl.dat', ('fixture data v' + str(number)).encode())
                self.fixture.refresh()
                updated = self.another_package(str(number) + '.0-local')
                result = self.run_helper(updated)
                self.assertTrue(result['updated'])
                self.assertEqual(result['location'], str(self.managed / 'app'))
                self.assertEqual((self.managed / 'app/opt/noventra/icudtl.dat').read_bytes(),
                                 ('fixture data v' + str(number)).encode())
                current = json.loads((self.managed / 'installation.json').read_text())
                self.assertEqual({n: r['backup'] for n, r in first['external'].items()},
                                 {n: r['backup'] for n, r in current['external'].items()})
                self.assertFalse((self.managed / '.update').exists())
                self.assertFalse(list(self.managed.glob('.prepare-update-*')))
                self.assertTrue(self.run_helper(updated)['already_installed'])
        self.run_helper(self.managed / 'app', 'uninstall')
        self.assertFalse(self.managed.exists())
        self.assertEqual(desktop.read_bytes(), b'original development launcher')
        self.assertEqual(icon.read_bytes(), b'original icon')
        self.assertEqual(stat.S_IMODE(desktop.stat().st_mode), 0o640)
        self.assertEqual(self.profile.read_text(), 'private profile fixture; keep unchanged')

    def test_update_refuses_extras_changed_external_and_changed_backup(self):
        source = self.make_package()
        other = self.another_package('2.0-local')
        for number, kind in enumerate(('file', 'directory', 'external', 'backup', 'replaced-directory')):
            with self.subTest(kind=kind):
                data = self.home / ('refused-' + str(number))
                desktop = data / 'applications/noventra-browser.desktop'
                desktop.parent.mkdir(parents=True)
                desktop.write_text('original desktop')
                self.run_helper(source, data=data)
                managed = data / 'noventra-browser'
                before = (managed / 'installation.json').read_bytes()
                if kind == 'file':
                    (managed / 'app/keep').write_text('extra file')
                elif kind == 'directory':
                    (managed / 'app/keep-empty-directory').mkdir()
                elif kind == 'external':
                    desktop.write_text('custom desktop')
                elif kind == 'backup':
                    value = json.loads(before)['external']['applications/noventra-browser.desktop']['backup']
                    (managed / value['path']).write_text('custom backup')
                else:
                    directory = managed / 'app/opt/noventra/resources'
                    directory.rename(directory.with_name('resources-original'))
                    shutil.copytree(directory.with_name('resources-original'), directory)
                refused = self.run_helper(other, data=data, success=False)
                self.assertIn('nothing was replaced', refused.stderr)
                self.assertEqual((managed / 'installation.json').read_bytes(), before)
                self.assertFalse((managed / '.update').exists())
                self.assertEqual(self.profile.read_text(), 'private profile fixture; keep unchanged')

    def test_wrong_architecture_is_refused_before_install_or_update(self):
        source = self.make_package()
        self.run_helper(source)
        before = (self.managed / 'installation.json').read_bytes()
        self.fixture.put('args.gn', b'target_cpu = "arm64"\n')
        self.fixture.refresh()
        other = self.another_package('wrong-architecture')
        for data in (self.data, self.home / 'uninstalled-data'):
            refused = self.run_helper(other, data=data, success=False)
            self.assertIn('architecture does not match', refused.stderr)
        self.assertEqual((self.managed / 'installation.json').read_bytes(), before)
        self.assertFalse((self.home / 'uninstalled-data').exists())
        self.assertTrue(self.run_helper(source)['already_installed'])

    def test_failed_staging_preserves_active_installation(self):
        source = self.make_package()
        self.run_helper(source)
        other = self.another_package('2.0-local')
        before = (self.managed / 'installation.json').read_bytes()
        original_copy = installer.copy_file
        def fail_copy(source, target, mode=None):
            if '.prepare-update-' in str(target) and target.name == 'icudtl.dat':
                raise OSError('fixture staging failure')
            return original_copy(source, target, mode)
        with patch.object(installer, 'copy_file', side_effect=fail_copy):
            with self.assertRaisesRegex(OSError, 'fixture staging failure'):
                installer.install(other, self.data)
        self.assertEqual((self.managed / 'installation.json').read_bytes(), before)
        self.assertTrue(self.run_helper(source)['already_installed'])
        self.assertFalse(list(self.managed.glob('.prepare-update-*')))

    def test_failed_activation_rolls_back_app_and_desktop_inodes(self):
        source = self.make_package()
        self.run_helper(source)
        other = self.another_package('2.0-local')
        before = (self.managed / 'installation.json').read_bytes()
        original_replace = installer.os.replace
        def fail_replace(source, target):
            if Path(source).name == 'new-external-1':
                raise OSError('fixture activation failure')
            return original_replace(source, target)
        with patch.object(installer.os, 'replace', side_effect=fail_replace):
            with self.assertRaisesRegex(OSError, 'fixture activation failure'):
                installer.install(other, self.data)
        self.assertEqual((self.managed / 'installation.json').read_bytes(), before)
        self.assertTrue(self.run_helper(source)['already_installed'])
        self.assertFalse((self.managed / '.update').exists())

    def test_interrupted_activation_recovers_on_next_install(self):
        source = self.make_package()
        self.run_helper(source)
        other = self.another_package('2.0-local')
        before = (self.managed / 'installation.json').read_bytes()
        original_replace = installer.os.replace
        def stop_replace(source, target):
            if Path(source).name == 'new-external-1':
                raise InterruptedUpdate()
            return original_replace(source, target)
        with patch.object(installer.os, 'replace', side_effect=stop_replace):
            with self.assertRaises(InterruptedUpdate):
                installer.install(other, self.data)
        self.assertTrue((self.managed / '.update/journal.json').exists())
        self.assertEqual((self.managed / 'installation.json').read_bytes(), before)
        self.assertTrue(self.run_helper(source)['already_installed'])
        self.assertFalse((self.managed / '.update').exists())
        self.assertEqual((self.managed / 'installation.json').read_bytes(), before)
        self.assertTrue(self.run_helper(other)['updated'])

    def test_interrupted_committed_cleanup_resumes_without_reverting_update(self):
        source = self.make_package()
        self.run_helper(source)
        other = self.another_package('2.0-local')
        with patch.object(installer, 'remove_tree', side_effect=InterruptedUpdate()):
            with self.assertRaises(InterruptedUpdate):
                installer.install(other, self.data)
        current = (self.managed / 'installation.json').read_bytes()
        self.assertTrue((self.managed / '.update/old-app').is_dir())
        self.assertTrue(self.run_helper(other)['already_installed'])
        self.assertEqual((self.managed / 'installation.json').read_bytes(), current)
        self.assertFalse((self.managed / '.update').exists())

    def test_running_installed_executable_blocks_update_and_uninstall(self):
        # A multicall coreutils executable can reject the installed filename.
        # Use the already-required system Python as a real renamed ELF instead.
        executable = Path('/usr/bin/python3').resolve(strict=True)
        self.fixture.put(package.BINARY, executable.read_bytes() + b'\0' + self.fixture.helper.encode() + b'\0', executable=True)
        self.fixture.refresh()
        source = self.make_package()
        self.run_helper(source)
        other = self.another_package('2.0-local')
        installed = self.managed / 'app/opt/noventra/noventra-browser'
        ready = self.root / 'running-native-ready'
        code = 'import sys,time;open(sys.argv[1],"w").write("ready");time.sleep(30)'
        process = subprocess.Popen([str(installed), '-I', '-S', '-c', code, str(ready)], env=self.env)
        try:
            expected = installed.stat()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                self.assertIsNone(process.poll(), 'Renamed fixture executable exited before readiness')
                actual = (Path('/proc') / str(process.pid) / 'exe').stat()
                if ready.exists() and ready.read_text() == 'ready' and (actual.st_dev, actual.st_ino) == (expected.st_dev, expected.st_ino):
                    break
                time.sleep(.01)
            else:
                self.fail('Installed fixture executable did not become ready')
            for operation, candidate in (('install', other), ('uninstall', source)):
                refused = self.run_helper(candidate, operation, success=False)
                self.assertIn('Close Noventra', refused.stderr)
                self.assertIsNone(process.poll())
            self.assertFalse((self.managed / '.update').exists())
        finally:
            process.terminate()
            process.wait(timeout=5)
        self.assertTrue(self.run_helper(other)['updated'])

    def test_existing_desktop_and_icon_are_restored_with_modes(self):
        desktop = self.data / 'applications/noventra-browser.desktop'
        icon = self.data / 'icons/hicolor/64x64/apps/noventra-browser.png'
        for path, content in [(desktop, b'previous desktop entry'), (icon, b'previous icon')]:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            path.chmod(0o640)
        source = self.make_package()
        self.run_helper(source)
        self.assertNotEqual(desktop.read_bytes(), b'previous desktop entry')
        self.run_helper(source, 'uninstall')
        self.assertEqual(desktop.read_bytes(), b'previous desktop entry')
        self.assertEqual(icon.read_bytes(), b'previous icon')
        self.assertEqual(stat.S_IMODE(desktop.stat().st_mode), 0o640)
        self.assertEqual(stat.S_IMODE(icon.stat().st_mode), 0o640)
        self.assertFalse(self.managed.exists())

    def test_uninstall_keeps_modified_extra_and_profile_files(self):
        icon = self.data / 'icons/hicolor/64x64/apps/noventra-browser.png'
        icon.parent.mkdir(parents=True)
        icon.write_bytes(b'original icon backup')
        source = self.make_package()
        self.run_helper(source)
        changed = self.managed / 'app/opt/noventra/icudtl.dat'
        changed.write_bytes(b'user changed file')
        extra = self.managed / 'app/extra-user-file'
        extra.write_bytes(b'keep extra')
        inside_profile = self.managed / 'app/opt/noventra/Default/Preferences'
        inside_profile.parent.mkdir()
        inside_profile.write_text('keep profile created inside runtime')
        desktop = self.data / 'applications/noventra-browser.desktop'
        desktop.write_bytes(b'user changed desktop')
        # Even an outside replacement with identical bytes is no longer owned.
        current = icon.read_bytes()
        replacement = icon.with_name('replacement.png')
        replacement.write_bytes(current)
        replacement.replace(icon)
        result = self.run_helper(source, 'uninstall')
        self.assertTrue(result['retained'])
        self.assertEqual(changed.read_bytes(), b'user changed file')
        self.assertEqual(extra.read_bytes(), b'keep extra')
        self.assertEqual(inside_profile.read_text(), 'keep profile created inside runtime')
        self.assertEqual(desktop.read_bytes(), b'user changed desktop')
        self.assertEqual(icon.read_bytes(), current)
        self.assertFalse((self.managed / 'app/opt/noventra/noventra-browser').exists())
        self.assertTrue(any(x.read_bytes() == b'original icon backup'
                            for x in (self.managed / 'backups').iterdir()))
        self.assertEqual(self.profile.read_text(), 'private profile fixture; keep unchanged')

    def test_tampered_unexpected_link_and_unsafe_inventory_are_rejected_before_install(self):
        original = self.make_package()
        for case in ('tampered', 'extra', 'symlink', 'traversal', 'duplicate'):
            with self.subTest(case=case):
                source = self.root / case
                shutil.copytree(original, source, symlinks=True)
                if case == 'tampered':
                    (source / 'opt/noventra/icudtl.dat').write_bytes(b'tampered')
                elif case == 'extra':
                    (source / 'unlisted').write_text('extra')
                elif case == 'symlink':
                    (source / 'unlisted').symlink_to(self.profile)
                else:
                    manifest = source / 'install-manifest.json'
                    value = json.loads(manifest.read_text())
                    if case == 'traversal':
                        value['files']['../outside'] = value['files']['AppRun']
                        manifest.write_text(json.dumps(value))
                    else:
                        manifest.write_text('{"format":1,"format":1,"id":"local.noventra.browser","files":{}}')
                self.run_helper(source, success=False)
                self.assertFalse(self.managed.exists())
                self.assertEqual(self.profile.read_text(), 'private profile fixture; keep unchanged')

    def test_unmanaged_root_and_external_symlink_are_preserved(self):
        source = self.make_package()
        self.managed.mkdir(parents=True)
        marker = self.managed / 'keep'
        marker.write_text('unmanaged')
        self.run_helper(source, success=False)
        self.assertEqual(list(self.managed.iterdir()), [marker])
        second_data = self.home / 'other data'
        desktop = second_data / 'applications/noventra-browser.desktop'
        desktop.parent.mkdir(parents=True)
        desktop.symlink_to(self.profile)
        self.run_helper(source, success=False, data=second_data)
        self.assertTrue(desktop.is_symlink())
        self.assertFalse((second_data / 'noventra-browser').exists())
        self.assertEqual(self.profile.read_text(), 'private profile fixture; keep unchanged')

    def test_uninstall_never_follows_replaced_app_directory(self):
        source = self.make_package()
        self.run_helper(source)
        outside = self.root / 'outside directory'
        moved = self.managed / 'app/opt'
        moved.rename(outside)
        moved.symlink_to(outside, target_is_directory=True)
        expected = {x.relative_to(outside): x.read_bytes() for x in outside.rglob('*') if x.is_file()}
        self.run_helper(source, 'uninstall')
        self.assertEqual({x.relative_to(outside): x.read_bytes() for x in outside.rglob('*') if x.is_file()}, expected)
        self.assertTrue(moved.is_symlink())

    def test_uninstall_never_follows_replaced_whole_app_root(self):
        source = self.make_package()
        self.run_helper(source)
        outside = self.root / 'outside whole app'
        moved = self.managed / 'app'
        moved.rename(outside)
        moved.symlink_to(outside, target_is_directory=True)
        expected = {x.relative_to(outside): x.read_bytes() for x in outside.rglob('*')
                    if x.is_file() and not x.is_symlink()}
        self.run_helper(source, 'uninstall')
        self.assertEqual({x.relative_to(outside): x.read_bytes() for x in outside.rglob('*')
                          if x.is_file() and not x.is_symlink()}, expected)
        self.assertTrue(moved.is_symlink())

    def test_uninstall_keeps_changed_backup_and_does_not_follow_desktop_directory(self):
        desktop = self.data / 'applications/noventra-browser.desktop'
        desktop.parent.mkdir(parents=True)
        desktop.write_text('original desktop')
        source = self.make_package()
        self.run_helper(source)
        record = json.loads((self.managed / 'installation.json').read_text())
        backup = self.managed / record['external']['applications/noventra-browser.desktop']['backup']['path']
        backup.write_text('user edited backup')
        expected = desktop.read_bytes()
        self.run_helper(source, 'uninstall')
        self.assertEqual(desktop.read_bytes(), expected)
        self.assertEqual(backup.read_text(), 'user edited backup')

        other_data = self.home / 'second-install'
        self.run_helper(source, data=other_data)
        outside = self.root / 'outside desktop directory'
        outside.mkdir()
        (outside / 'keep').write_text('unchanged')
        (other_data / 'applications').rename(self.root / 'old-applications')
        (other_data / 'applications').symlink_to(outside, target_is_directory=True)
        self.run_helper(source, 'uninstall', data=other_data)
        self.assertEqual(list(outside.iterdir()), [outside / 'keep'])
        self.assertEqual((outside / 'keep').read_text(), 'unchanged')

    def test_tampered_ownership_record_is_rejected_without_removing_files(self):
        source = self.make_package()
        self.run_helper(source)
        record = self.managed / 'installation.json'
        before = record.read_bytes()
        value = json.loads(before)
        value['app_files']['../../profile'] = value['app_files']['AppRun']
        record.write_text(json.dumps(value))
        self.run_helper(source, 'uninstall', success=False)
        self.assertTrue((self.managed / 'app/opt/noventra/noventra-browser').is_file())
        self.assertEqual(self.profile.read_text(), 'private profile fixture; keep unchanged')
        record.write_bytes(before)
        self.run_helper(source, 'uninstall')

    @unittest.skipUnless(shutil.which('gio'), 'GIO desktop launcher unavailable')
    def test_desktop_exec_preserves_urls_and_metacharacter_paths(self):
        record = self.root / 'fixture-args.json'
        code = 'import json,os,sys;open(os.environ["NOVENTRA_INSTALL_FIXTURE_RECORD"],"w").write(json.dumps(sys.argv[1:]))'
        recorder = (f'#!/bin/sh\nexec /usr/bin/python3 -c {shlex.quote(code)} "$@"\n# ').encode()
        self.fixture.put(package.BINARY, recorder + self.fixture.helper.encode() + b'\0\n', executable=True)
        self.fixture.refresh()
        source = self.make_package()
        data = self.home / 'data $literal `literal` %value ; quotation"and\''
        self.run_helper(source, data=data)
        desktop = data / 'applications/noventra-browser.desktop'
        subprocess.run(['desktop-file-validate', str(desktop)], check=True)
        env = dict(self.env, NOVENTRA_INSTALL_FIXTURE_RECORD=str(record))
        url = 'https://example.invalid/a%20b?literal=$value;keep'
        launched = subprocess.run(['gio', 'launch', str(desktop), url], env=env,
                                  capture_output=True, text=True, timeout=10)
        self.assertEqual(launched.returncode, 0, launched.stderr)
        deadline = time.monotonic() + 5
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertEqual(json.loads(record.read_text()), [url])
        self.run_helper(source, 'uninstall', data=data)
        self.assertFalse((data / 'noventra-browser').exists())


if __name__ == '__main__':
    unittest.main()
