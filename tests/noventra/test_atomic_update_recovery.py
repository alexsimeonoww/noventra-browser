"""Crash/signal/lock recovery using signed, disposable installed sleep ELFs."""
from contextlib import contextmanager, ExitStack
import errno
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
sys.path.insert(0, str(ROOT / 'tests/noventra'))
import test_desktop_updater as fixtures


class PowerFailure(BaseException):
    """Skip graceful rollback, like process loss, while retaining the journal."""


class AtomicRecoveryTests(unittest.TestCase):
    @contextmanager
    def case(self):
        fixture = fixtures.DesktopUpdateTests('test_packaged_trust_is_explicit_normalized_and_inventoried')
        fixture.setUp()
        try:
            yield fixture
        finally:
            fixture.doCleanups()

    def launchable(self, fixture):
        self.assertTrue((fixture.app / 'AppRun').is_file(), 'The installed launcher disappeared')
        self.assertTrue((fixture.app / 'opt/noventra/noventra-browser').is_file())

    def verify(self, fixture):
        module = fixtures.desktop.load_installer(fixture.app)
        module.verify_installation(fixture.app.parent, fixture.data,
                                   module.state_for(fixture.app.parent, fixture.data))
        self.assertEqual(fixture.profile.read_text(), 'retained profile fixture')

    def ordinary_launch(self, fixture):
        self.launchable(fixture)
        process = fixture.spawn('.05')
        output, error = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, error + output)
        self.launchable(fixture)

    def clear_delivery_stage(self, fixture):
        with fixtures.delivery.locked(fixture.updater.cfg['install_root']) as root:
            state = fixtures.delivery.read_state(root, fixture.updater.cfg)
            state['staged'] = None
            fixtures.delivery.atomic_state(root, state)

    @contextmanager
    def fail_transition(self, fixture, wanted):
        module = fixture.updater.installer
        root = fixture.app.parent
        transaction = root / '.update'
        counts = {}
        reached = []

        def event(kind, when, index=None):
            self.launchable(fixture)
            if (kind, when, index) == wanted:
                reached.append(wanted)
                raise PowerFailure(wanted)

        original_save = module.save
        def save(path, value, name=module.RECORD):
            kind = 'journal' if name == 'journal.json' else None
            if kind: event(kind, 'before')
            result = original_save(path, value, name)
            if kind: event(kind, 'after')
            return result

        original_rename = Path.rename
        def rename(path, target):
            target = Path(target)
            kind = ('publish' if path.name.startswith('.prepare-update-') and target == transaction
                    else 'backup-name' if path == transaction / 'new-app' and target == transaction / 'old-app'
                    else None)
            if kind: event(kind, 'before')
            result = original_rename(path, target)
            self.launchable(fixture)
            if kind: event(kind, 'after')
            return result

        original_exchange = module.exchange_paths
        def exchange(first, second):
            active = Path(first) == fixture.app
            if active: event('exchange', 'before')
            result = original_exchange(first, second)
            self.launchable(fixture)
            if active: event('exchange', 'after')
            return result

        original_sync = module.sync_directory
        def sync(path):
            kind = 'sync-root' if Path(path) == root else 'sync-transaction' if Path(path) == transaction else None
            index = None
            if kind:
                counts[kind] = counts.get(kind, 0) + 1
                index = counts[kind]
                event(kind, 'before', index)
            result = original_sync(path)
            self.launchable(fixture)
            if kind: event(kind, 'after', index)
            return result

        original_replace = module.os.replace
        def replace(source, target):
            source, target = Path(source), Path(target)
            kind, index = None, None
            if source.name.startswith('new-external-'):
                kind, index = 'external', int(source.name.rsplit('-', 1)[1])
            elif target == root / module.RECORD:
                kind = 'state-commit'
            if kind: event(kind, 'before', index)
            result = original_replace(source, target)
            self.launchable(fixture)
            if kind: event(kind, 'after', index)
            return result

        original_remove = module.remove_tree
        def remove(path, files, directories):
            if Path(path) == transaction / 'old-app': event('old-cleanup', 'before')
            result = original_remove(path, files, directories)
            self.launchable(fixture)
            if Path(path) == transaction / 'old-app': event('old-cleanup', 'after')
            return result

        original_unlink, original_rmdir = Path.unlink, Path.rmdir
        def unlink(path, *args, **kwargs):
            selected = path == transaction / 'journal.json'
            if selected: event('journal-remove', 'before')
            result = original_unlink(path, *args, **kwargs)
            if selected: event('journal-remove', 'after')
            return result
        def rmdir(path):
            selected = path == transaction
            if selected: event('transaction-remove', 'before')
            result = original_rmdir(path)
            if selected: event('transaction-remove', 'after')
            return result

        with ExitStack() as stack:
            for owner, name, function in [(module, 'save', save), (Path, 'rename', rename),
                    (module, 'exchange_paths', exchange), (module, 'sync_directory', sync),
                    (module.os, 'replace', replace), (module, 'remove_tree', remove),
                    (Path, 'unlink', unlink), (Path, 'rmdir', rmdir)]:
                stack.enter_context(patch.object(owner, name, side_effect=function, autospec=owner is Path))
            yield reached

    def test_fault_matrix_keeps_launcher_and_recovers_without_delivery_stage(self):
        points = []
        for kind in ['journal', 'publish', 'exchange', 'backup-name', 'state-commit',
                     'old-cleanup', 'journal-remove', 'transaction-remove']:
            points += [(kind, when, None) for when in ['before', 'after']]
        points += [('sync-root', when, index) for index in range(1, 5) for when in ['before', 'after']]
        points += [('sync-transaction', when, index) for index in range(1, 3) for when in ['before', 'after']]
        points += [('external', when, index) for index in range(10) for when in ['before', 'after']]
        self.assertEqual(len(points), 48)
        for point in points:
            with self.subTest(point=point), self.case() as fixture:
                fixture.to_second()
                old = fixture.updater.installer.state_for(fixture.app.parent, fixture.data)
                with self.fail_transition(fixture, point) as reached:
                    with self.assertRaises(PowerFailure):
                        fixture.updater.installer.install(fixture.packages[2], fixture.data)
                self.assertEqual(reached, [point])
                self.launchable(fixture)
                state = fixture.updater.installer.state_for(fixture.app.parent, fixture.data)
                expected = '1-test' if state['manifest_sha256'] == old['manifest_sha256'] else '2-test'
                self.clear_delivery_stage(fixture)
                self.ordinary_launch(fixture)
                self.assertEqual(fixture.current_version(), expected)
                self.assertFalse((fixture.app.parent / '.update').exists())
                self.verify(fixture)

    def stopped_apply(self, fixture):
        helper = fixture.app / 'updater/desktop_updater.py'
        code = '''import importlib.util,os,signal,sys
from pathlib import Path
helper=Path(sys.argv[1]); spec=importlib.util.spec_from_file_location('owned_signal_fixture',helper)
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
original=m.load_installer; fired=False
def load(app):
    installer=original(app); exchange=installer.exchange_paths
    def interrupt(first,second):
        global fired
        result=exchange(first,second)
        if Path(first)==app and not fired:
            fired=True; print('EXCHANGED',flush=True); os.kill(os.getpid(),signal.SIGSTOP)
        return result
    installer.exchange_paths=interrupt
    return installer
m.load_installer=load; sys.argv=[str(helper),'apply']; sys.exit(m.main())
'''
        process = subprocess.Popen(['/usr/bin/python3', '-I', '-B', '-c', code, str(helper)],
                                   env=fixture.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def cleanup():
            if process.poll() is None:
                process.kill(); process.wait(timeout=5)
            process.stdout.close(); process.stderr.close()
        fixture.addCleanup(cleanup)
        self.assertEqual(process.stdout.readline().strip(), 'EXCHANGED')
        pid, status = os.waitpid(process.pid, os.WUNTRACED)
        self.assertEqual(pid, process.pid); self.assertTrue(os.WIFSTOPPED(status))
        self.launchable(fixture)
        return process

    def test_actual_sigterm_rolls_back_signed_update_and_next_launch_retries(self):
        with self.case() as fixture:
            fixture.to_second()
            old = (fixture.app.parent / 'installation.json').read_bytes()
            process = self.stopped_apply(fixture)
            process.send_signal(signal.SIGTERM); process.send_signal(signal.SIGCONT)
            output, error = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, error)
            self.assertEqual(json.loads(output)['status'], 'unchecked')
            self.assertEqual((fixture.app.parent / 'installation.json').read_bytes(), old)
            self.assertEqual(fixture.current_version(), '1-test')
            self.launchable(fixture)
            self.assertFalse((fixture.app.parent / '.update').exists())
            self.ordinary_launch(fixture)
            self.assertEqual(fixture.current_version(), '2-test')
            self.verify(fixture)

    def test_actual_sigkill_after_exchange_recovers_on_next_ordinary_launch(self):
        with self.case() as fixture:
            fixture.to_second()
            process = self.stopped_apply(fixture)
            process.kill(); process.communicate(timeout=5)
            self.assertEqual(process.returncode, -signal.SIGKILL)
            self.assertTrue((fixture.app.parent / '.update/journal.json').is_file())
            self.assertEqual(fixture.current_version(), '2-test')
            self.ordinary_launch(fixture)
            self.assertEqual(fixture.current_version(), '2-test')
            self.assertFalse((fixture.app.parent / '.update').exists())
            self.verify(fixture)

    def test_unsupported_exchange_never_replaces_app_or_downloads_artifact(self):
        with self.case() as fixture:
            fixture.http.release_root = fixture.releases[2]
            old = (fixture.app.parent / 'installation.json').read_bytes()
            unsupported = OSError(errno.ENOSYS, 'Atomic updates are unsupported in this fixture')
            with patch.object(fixture.updater.installer, 'exchange_paths', side_effect=unsupported), \
                    patch.object(fixtures.delivery, 'download', wraps=fixtures.delivery.download) as requests:
                with self.assertRaises(OSError): fixture.updater.check()
            self.assertFalse(any(call.args[1].endswith('.tar.xz') for call in requests.call_args_list))
            self.assertEqual((fixture.app.parent / 'installation.json').read_bytes(), old)
            self.assertEqual(fixture.current_version(), '1-test')
            fixture.to_second()
            with patch.object(fixture.updater.installer, 'exchange_paths', side_effect=unsupported):
                with self.assertRaises(OSError): fixture.updater.apply()
            self.launchable(fixture)
            self.assertEqual(fixture.current_version(), '1-test')
            self.assertFalse((fixture.app.parent / '.update').exists())
            # Exercise the real coordinator's failure path while the syscall
            # alone is unsupported. Its native child must still start old code.
            helper = fixture.app / 'updater/desktop_updater.py'
            code = '''import importlib.util,sys,errno
from pathlib import Path
helper=Path(sys.argv[1]); spec=importlib.util.spec_from_file_location('unsupported_fixture',helper)
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
original=m.load_installer
def load(app):
    installer=original(app)
    def unsupported(*args): raise OSError(errno.ENOSYS,'Atomic updates are unsupported')
    installer.exchange_paths=unsupported
    return installer
m.load_installer=load; sys.argv=[str(helper),'launch','--','.1']; sys.exit(m.main())
'''
            started = subprocess.run(['/usr/bin/python3', '-I', '-B', '-c', code, str(helper)],
                                     env=fixture.env, capture_output=True, text=True, timeout=10)
            self.assertEqual(started.returncode, 0, started.stderr)
            self.assertEqual(fixture.current_version(), '1-test')
            self.verify(fixture)

    def test_native_start_waits_for_manual_installer_lock_without_deadlock(self):
        with self.case() as fixture:
            with fixture.updater.installer.lock(fixture.app.parent):
                process = fixture.spawn('.1')
                time.sleep(.1)
                self.assertIsNone(process.poll())
                self.assertEqual(fixture.native_children(process), [])
            output, error = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, error + output)
            self.verify(fixture)

    def test_native_exec_handshake_excludes_manual_update(self):
        with self.case() as fixture:
            helper = fixture.app / 'updater/desktop_updater.py'
            code = '''import importlib.util,os,signal,sys
from pathlib import Path
helper=Path(sys.argv[1]); spec=importlib.util.spec_from_file_location('owned_spawn_fixture',helper)
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
original=m.subprocess.Popen
def spawn(argv,*args,**kwargs):
    if Path(argv[0])==helper.parent.parent/'opt/noventra/noventra-browser':
        print('SPAWN_READY',flush=True); os.kill(os.getpid(),signal.SIGSTOP)
    return original(argv,*args,**kwargs)
m.subprocess.Popen=spawn; sys.argv=[str(helper),'launch','--','.1']; sys.exit(m.main())
'''
            process = subprocess.Popen(['/usr/bin/python3', '-I', '-B', '-c', code, str(helper)],
                                       env=fixture.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            def cleanup():
                if process.poll() is None: process.kill(); process.wait(timeout=5)
                process.stdout.close(); process.stderr.close()
            fixture.addCleanup(cleanup)
            self.assertEqual(process.stdout.readline().strip(), 'SPAWN_READY')
            self.assertTrue(os.WIFSTOPPED(os.waitpid(process.pid, os.WUNTRACED)[1]))
            manual = subprocess.run([str(fixture.packages[2] / 'install.sh'), '--data-home', str(fixture.data)],
                                    env=fixture.env, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(manual.returncode, 0)
            self.assertIn('Resource temporarily unavailable', manual.stderr)
            self.assertEqual(fixture.current_version(), '1-test')
            process.send_signal(signal.SIGCONT)
            output, error = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, error + output)
            self.verify(fixture)

    def test_changed_backup_files_are_retained_and_not_executed(self):
        with self.case() as fixture:
            fixture.to_second()
            with self.fail_transition(fixture, ('backup-name', 'after', None)):
                with self.assertRaises(PowerFailure):
                    fixture.updater.installer.install(fixture.packages[2], fixture.data)
            note = fixture.app.parent / '.update/old-app/user-owned-note'
            note.write_text('preserve this unexpected file')
            process = fixture.spawn('.1')
            output, error = process.communicate(timeout=10)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(note.read_text(), 'preserve this unexpected file')
            self.launchable(fixture)
            self.assertTrue((fixture.app.parent / '.update/journal.json').is_file())

    def test_legacy_format1_journals_remain_recoverable(self):
        for boundary in ('old-rename-gap', 'after-both-renames'):
            with self.subTest(boundary=boundary), self.case() as fixture:
                # Produce the unchanged format1 journal/real inode inventory,
                # then reproduce the former pair of renames on those trees.
                # No task scratch preimage is needed on a fresh checkout.
                with self.fail_transition(fixture, ('publish', 'after', None)):
                    with self.assertRaises(PowerFailure):
                        fixture.updater.installer.install(fixture.packages[2], fixture.data)
                transaction = fixture.app.parent / '.update'
                fixture.app.rename(transaction / 'old-app')
                if boundary == 'after-both-renames':
                    (transaction / 'new-app').rename(fixture.app)
                if boundary == 'old-rename-gap':
                    self.assertFalse((fixture.app / 'AppRun').exists())
                with fixture.updater.installer.lock(fixture.app.parent):
                    fixture.updater.installer.recover_update(fixture.app.parent, fixture.data)
                self.launchable(fixture)
                self.assertEqual(fixture.current_version(), '1-test')
                self.assertFalse((fixture.app.parent / '.update').exists())
                self.verify(fixture)

    def test_configured_uninstall_reinstall_preserves_high_water_and_profile(self):
        with self.case() as fixture:
            self.ordinary_launch(fixture)
            fixture.to_second()
            cache = Path(fixture.updater.cfg['install_root']) / 'state.json'
            accepted = cache.read_bytes()
            note = fixture.app.parent / 'user-note'
            note.write_text('preserve this unrelated sibling')
            defaults = fixture.home / '.config/mimeapps.list'
            defaults.write_text('[Default Applications]\nx-scheme-handler/http=owned-example.desktop\n')
            default_bytes = defaults.read_bytes()
            source = fixture.packages[1]
            removed = subprocess.run([str(source / 'uninstall.sh'), '--data-home', str(fixture.data)],
                                     env=fixture.env, capture_output=True, text=True, timeout=15)
            self.assertEqual(removed.returncode, 0, removed.stderr)
            self.assertFalse(fixture.app.exists())
            self.assertEqual(cache.read_bytes(), accepted)
            reinstalled = subprocess.run([str(source / 'install.sh'), '--data-home', str(fixture.data)],
                                         env=fixture.env, capture_output=True, text=True, timeout=15)
            self.assertEqual(reinstalled.returncode, 0, reinstalled.stderr)
            self.assertEqual(cache.read_bytes(), accepted)
            self.assertEqual(fixture.current_version(), '1-test')
            self.assertEqual(note.read_text(), 'preserve this unrelated sibling')
            self.ordinary_launch(fixture)
            self.assertEqual(fixture.current_version(), '2-test')
            self.assertEqual(fixture.state()['highest_sequence'], 2)
            self.assertEqual(defaults.read_bytes(), default_bytes)
            self.verify(fixture)

    def test_reinstall_refuses_unexpected_app_and_changed_uninstalled_metadata(self):
        with self.case() as fixture:
            fixture.updater.check()
            fixture.updater.installer.uninstall(fixture.data)
            record = fixture.app.parent / 'installation.json'
            original = record.read_bytes()
            source = fixture.packages[1]
            def refused():
                result = subprocess.run([str(source / 'install.sh'), '--data-home', str(fixture.data)],
                                        env=fixture.env, capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                return result
            fixture.app.mkdir()
            note = fixture.app / 'user-file'; note.write_text('do not adopt')
            self.assertIn('app subtree', refused().stderr)
            self.assertEqual(note.read_text(), 'do not adopt')
            self.assertEqual(record.read_bytes(), original)
            note.unlink(); fixture.app.rmdir()
            for change in [{'format': True}, {'root_identity': [0, 0]},
                           {'data_home': str(fixture.root / 'other-home')},
                           {'manifest_sha256': 'invalid'}, {'unknown_metadata': True}]:
                with self.subTest(change=change):
                    value = fixtures.release.parse(original); value.update(change)
                    changed = fixtures.release.encode(value); record.write_bytes(changed)
                    refused()
                    self.assertEqual(record.read_bytes(), changed)
                    self.assertFalse(fixture.app.exists())
            record.write_bytes(original)
            # Copying an extracted configured app into that namespace does not
            # adopt it into an explicitly uninstalled installation record.
            shutil.copytree(source, fixture.app, symlinks=True)
            with self.assertRaisesRegex(ValueError, 'completed managed installation'):
                fixtures.desktop.DesktopUpdater(fixture.app, fixture.data)
            self.assertEqual(record.read_bytes(), original)

    def test_configured_install_does_not_adopt_unmanaged_sibling_directory(self):
        with self.case() as fixture:
            data = fixture.home / 'unmanaged-data'
            root = data / 'noventra-browser'; root.mkdir(parents=True)
            note = root / 'user-file'; note.write_text('unmanaged content')
            result = subprocess.run([str(fixture.packages[1] / 'install.sh'), '--data-home', str(data)],
                                    env=fixture.env, capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Unmanaged', result.stderr)
            self.assertEqual(note.read_text(), 'unmanaged content')
            self.assertFalse((root / 'app').exists())
            self.assertFalse((root / 'installation.json').exists())


if __name__ == '__main__':
    unittest.main()
