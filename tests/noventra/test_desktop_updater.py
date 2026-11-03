"""Signed loopback updates of disposable managed apps; never a real browser."""
import fcntl
import ctypes
from http.server import ThreadingHTTPServer
import importlib.util
import io
import select
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
sys.path.insert(0, str(ROOT / 'tests/noventra'))
import release_manifest as release
import update_delivery as delivery
import local_delivery_server as server
import test_package_distribution as fixtures

spec = importlib.util.spec_from_file_location('desktop_updater_fixture', ROOT / 'noventra/package/desktop_updater.py')
desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop)


class DesktopUpdateTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.PackageTests('test_hash_mismatch')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.key = self.root / 'disposable-test-only.pem'
        release.openssl('genpkey', '-algorithm', 'ED25519', '-out', self.key)
        self.key.chmod(0o600)
        self.public = self.root / 'disposable-test-only-public.pem'
        self.public.write_bytes(release.openssl('pkey', '-in', self.key, '-pubout'))
        self.http = ThreadingHTTPServer(('127.0.0.1', 0), server.DeliveryHandler)
        self.http.daemon_threads = True
        self.http.release_root = self.root
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.url = f'http://127.0.0.1:{self.http.server_port}/'
        self.home = self.root / 'owned home with spaces Café'
        self.data = self.home / '.local/share'
        self.data.mkdir(parents=True)
        self.profile = self.home / '.config/noventra-browser/Default/Preferences'
        self.profile.parent.mkdir(parents=True)
        self.profile.write_text('retained profile fixture')
        self.config = self.root / 'explicit-update-config.json'
        self.config_value = {'format': 1, 'base_url': self.url, 'channel': 'local',
                             'sequence': 1, 'public_key': str(self.public)}
        # A standalone test ELF ignores its filename and keeps the same numeric
        # duration API for these checks and their recovery-test callers.
        self.fixture.put('noventra-browser', fixtures.native_sleep_elf()
                         + b'\0noventra_crashpad_handler\0', executable=True)
        self.fixture.refresh()
        self.packages = {}
        self.releases = {}
        for version in (1, 2):
            self.make_version(version)
        self.http.release_root = self.releases[1]
        self.app = self.data / 'noventra-browser/app'
        self.installer = desktop.load_installer(self.packages[1])
        self.installer.install(self.packages[1], self.data)
        self.updater = desktop.DesktopUpdater(self.app, self.data)
        self.env = {**os.environ, 'HOME': str(self.home), 'XDG_DATA_HOME': str(self.data),
                    'XDG_CONFIG_HOME': str(self.home / '.config'),
                    'DBUS_SESSION_BUS_ADDRESS': f'unix:path={self.root / "no-notification-bus"}',
                    'NOVENTRA_VIDEO_BUFFER_WORKAROUND': 'off'}
        self.env.pop('APPIMAGE', None)
        self.env.pop('PYTHONPATH', None)
        self.env.pop('LD_LIBRARY_PATH', None)
        self.notification_patcher = patch.object(desktop, 'send_update_notification')
        self.notifier = self.notification_patcher.start()
        self.addCleanup(self.notification_patcher.stop)

    def stop_server(self):
        self.http.shutdown(); self.http.server_close(); self.thread.join(timeout=3)

    def make_version(self, version, *, config=None, signed_sequence=None, minimum_sequence=None):
        settings = dict(self.config_value if config is None else config)
        settings['sequence'] = version if minimum_sequence is None else minimum_sequence
        self.config.write_bytes(release.encode(settings))
        self.fixture.put('icudtl.dat', f'disposable version {version}'.encode())
        self.fixture.refresh()
        distribution = self.root / f'distribution-{version}'
        fixtures.package.package(self.fixture.source, distribution, f'{version}-test',
                                 archive=True, update_config=self.config)
        publication = self.root / f'published-{version}'
        release.publish(distribution, publication, self.key, signed_sequence or version,
                        channel='local', lifetime=86400)
        self.packages[version] = distribution / 'Noventra.AppDir'
        self.releases[version] = publication

    def current_version(self):
        return release.parse((self.app / 'noventra-package.json').read_bytes())['version_label']

    def state(self):
        return delivery.read_state(Path(self.updater.cfg['install_root']), self.updater.cfg)

    def to_second(self):
        self.http.release_root = self.releases[2]
        return self.updater.check()

    def cli(self, operation, *options):
        result = subprocess.run(['/usr/bin/python3', '-I', '-B',
                                 str(self.app / 'updater/desktop_updater.py'), *options, operation],
                                env=self.env, capture_output=True, text=True, timeout=30)
        return result, json.loads(result.stdout)

    def spawn(self, *args):
        process = subprocess.Popen([str(self.app / 'AppRun'), *args], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def cleanup():
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
            if process.stdout: process.stdout.close()
            if process.stderr: process.stderr.close()
        self.addCleanup(cleanup)
        return process

    def native_children(self, process):
        path = Path(f'/proc/{process.pid}/task/{process.pid}/children')
        children = path.read_text().split() if path.exists() else []
        found = []
        for pid in children:
            try:
                if os.readlink(f'/proc/{pid}/exe') == str(self.app / 'opt/noventra/noventra-browser'):
                    found.append(int(pid))
            except FileNotFoundError:
                pass
        return found

    def wait_native(self, process):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            children = self.native_children(process)
            if children:
                return children
            self.assertIsNone(process.poll(), 'Coordinator exited before native process')
            time.sleep(.02)
        self.fail('Native fixture did not start')

    def test_packaged_trust_is_explicit_normalized_and_inventoried(self):
        raw = (self.app / 'updater/release-public.pem').read_bytes()
        self.assertIn(b'BEGIN PUBLIC KEY', raw)
        self.assertNotIn(b'PRIVATE', raw)
        settings, cfg = desktop.configuration(self.app / 'updater')
        self.assertEqual(settings['public_key'], 'release-public.pem')
        self.assertEqual(settings['base_url'], self.url)
        self.assertEqual(cfg['signer_id'], release.sha(release.public_der(self.public)))
        for file in ['desktop_updater.py', 'release_manifest.py', 'update_delivery.py',
                     'config.json', 'release-public.pem']:
            self.assertIn(f'updater/{file}', release.parse((self.app / 'install-manifest.json').read_bytes())['files'])

    def test_packager_rejects_invalid_config_and_private_key_inputs(self):
        combined = self.root / 'public-and-private.pem'
        combined.write_bytes(self.public.read_bytes() + self.key.read_bytes())
        for index, change in enumerate([
                {'format': True}, {'sequence': True}, {'public_key': 123},
                {'base_url': 'http://example.invalid/'}, {'channel': 'UPPER'},
                {'base_url': 'https://user:dummy@example.invalid/'},
                {'public_key': str(self.key)}, {'public_key': str(combined)},
                {'hooks': ['never execute']} ]):
            with self.subTest(change=change):
                self.config.write_bytes(release.encode({**self.config_value, **change}))
                output = self.root / f'invalid-config-{index}'
                with self.assertRaises((ValueError, OSError)):
                    fixtures.package.package(self.fixture.source, output, 'invalid-config-test',
                                             update_config=self.config)
                self.assertFalse(output.exists())

    def test_only_normalized_public_material_is_embedded(self):
        self.public.write_bytes(b'\n' + self.public.read_bytes() + b'\n')
        self.config.write_bytes(release.encode(self.config_value))
        output = self.root / 'normalized-key-package'
        fixtures.package.package(self.fixture.source, output, 'normalized-test', update_config=self.config)
        self.assertNotIn(b'PRIVATE', (output / 'Noventra.AppDir/updater/release-public.pem').read_bytes())

    def test_current_release_does_not_download_archive_or_reinstall(self):
        with patch.object(delivery, 'download', wraps=delivery.download) as download:
            result = self.updater.check()
        self.assertEqual(result['status'], 'up_to_date')
        self.assertEqual([call.args[1] for call in download.call_args_list], ['manifest.json', 'manifest.sig'])
        self.assertEqual(self.current_version(), '1-test')
        self.notifier.assert_not_called()

    def test_download_then_closed_apply_uses_same_path_and_preserves_profile(self):
        desktop_before = (self.data / 'applications/noventra-browser.desktop').read_bytes()
        staged = self.to_second()
        self.assertEqual(staged['status'], 'ready')
        self.assertEqual(self.current_version(), '1-test')
        self.assertEqual(self.updater.apply()['status'], 'applied')
        self.assertEqual(self.current_version(), '2-test')
        self.assertEqual((self.app / 'opt/noventra/icudtl.dat').read_bytes(), b'disposable version 2')
        self.assertEqual(self.profile.read_text(), 'retained profile fixture')
        self.assertEqual((self.data / 'applications/noventra-browser.desktop').read_bytes(), desktop_before)
        self.installer.verify_installation(self.app.parent, self.data,
                                           self.installer.state_for(self.app.parent, self.data))
        self.assertEqual(self.state()['highest_sequence'], 2)
        self.assertIsNone(self.state()['staged'])

    def test_real_running_executable_defers_install_until_closed(self):
        self.to_second()
        installed = self.app / 'opt/noventra/noventra-browser'
        ready = self.root / 'running-native-ready'
        running = subprocess.Popen([str(installed), '10', str(ready)], env=self.env)
        self.addCleanup(lambda: running.poll() is None and running.kill())
        try:
            expected = installed.stat()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                self.assertIsNone(running.poll(), 'Renamed fixture executable exited before readiness')
                actual = (Path('/proc') / str(running.pid) / 'exe').stat()
                if ready.exists() and ready.read_text() == 'ready' and (actual.st_dev, actual.st_ino) == (expected.st_dev, expected.st_ino):
                    break
                time.sleep(.01)
            else:
                self.fail('Installed fixture executable did not become ready')
            result = self.updater.apply()
            self.assertEqual(result['status'], 'ready')
            self.assertEqual(self.current_version(), '1-test')
            self.assertIsNone(running.poll())
        finally:
            running.terminate(); running.wait(timeout=5)
        self.assertEqual(self.updater.apply()['status'], 'applied')

    def test_native_launch_is_nonblocking_and_download_worker_stops_on_close(self):
        # Hold the network/update mutex: startup must still reach the native
        # fixture immediately, and the busy background worker must be reaped.
        with desktop.file_lock(self.updater.root / '.desktop-lock'):
            process = self.spawn('.3')
            children = self.wait_native(process)
            stdout, stderr = process.communicate(timeout=6)
        self.assertEqual(process.returncode, 0, stderr)
        self.assertEqual(stdout, '')
        self.assertFalse(any(Path(f'/proc/{pid}').exists() for pid in children))

    def test_launcher_applies_ready_update_before_native_exec(self):
        self.to_second()
        process = self.spawn('.1')
        process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0)
        self.assertEqual(self.current_version(), '2-test')

    def test_simultaneous_apply_and_launch_share_transition_lock(self):
        self.to_second()
        with desktop.file_lock(self.app.parent / '.launch-lock'):
            launch = self.spawn('.2')
            apply = subprocess.Popen(['/usr/bin/python3', '-I', '-B',
                                      str(self.app / 'updater/desktop_updater.py'), 'apply'],
                                     env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(lambda: apply.poll() is None and apply.kill())
            time.sleep(.1)
            self.assertEqual(self.current_version(), '1-test')
            self.assertIsNone(launch.poll()); self.assertIsNone(apply.poll())
        launch.communicate(timeout=10)
        output, error = apply.communicate(timeout=10)
        self.assertEqual(launch.returncode, 0)
        self.assertEqual(apply.returncode, 0, error)
        self.assertIn(json.loads(output)['status'], ['applied', 'unchanged'])
        self.assertEqual(self.current_version(), '2-test')

    def test_background_throttle_and_manual_force(self):
        self.updater.check()
        with patch.object(delivery, 'download', wraps=delivery.download) as download:
            self.updater.check(background=True)
            self.assertEqual(download.call_count, 0)
            self.updater.check()
            self.assertEqual(download.call_count, 2)

    def test_ready_update_notifies_without_installing_and_records_private_attempt(self):
        self.assertEqual(self.to_second()['status'], 'ready')
        self.notifier.assert_called_once_with(self.app)
        path = self.updater.root / 'reminder-status.json'
        ledger = release.parse(path.read_bytes())
        self.assertEqual(set(ledger), {'format', 'last_attempt_at'})
        self.assertEqual(ledger['format'], 1)
        self.assertLessEqual(abs(ledger['last_attempt_at'] - time.time()), 5)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.current_version(), '1-test')
        self.assertIsNotNone(self.state()['staged'])

    def test_interrupted_initialization_does_not_publish_partial_cache(self):
        def interrupted(cfg):
            Path(cfg['install_root']).mkdir(mode=0o700)
            raise desktop.InterruptedUpdate('Browser closed during initialization')
        with patch.object(delivery, 'initialize', side_effect=interrupted):
            with self.assertRaises(desktop.InterruptedUpdate):
                self.updater.check()
        self.assertFalse(Path(self.updater.cfg['install_root']).exists())
        self.assertEqual(list(self.updater.root.glob('.initialize-*')), [])
        self.assertEqual(self.to_second()['status'], 'ready')
        self.assertEqual(self.state()['highest_sequence'], 2)
        self.notifier.assert_called_once_with(self.app)

    def test_interruption_after_cache_publication_keeps_it_usable(self):
        original = delivery.sync_directory
        def interrupted(path):
            if Path(path) == self.updater.root:
                raise desktop.InterruptedUpdate('Browser closed after cache publication')
            return original(path)
        with patch.object(delivery, 'sync_directory', side_effect=interrupted):
            with self.assertRaises(desktop.InterruptedUpdate):
                self.updater.check()
        self.assertTrue((Path(self.updater.cfg['install_root']) / '.lock').is_file())
        self.assertEqual(self.to_second()['status'], 'ready')
        self.assertEqual(self.state()['highest_sequence'], 2)
        self.notifier.assert_called_once_with(self.app)

    def test_ready_reminder_survives_reload_and_repeats_after_twelve_hours(self):
        now = int(time.time())
        with patch.object(desktop.time, 'time', return_value=now):
            self.to_second()
        self.updater = desktop.DesktopUpdater(self.app, self.data)
        with patch.object(desktop.time, 'time', return_value=now + desktop.INTERVAL):
            self.assertEqual(self.updater.check(background=True)['status'], 'ready')
        self.notifier.assert_called_once()
        with patch.object(desktop.time, 'time', return_value=now + desktop.REMINDER_INTERVAL):
            self.assertEqual(self.updater.check(background=True)['status'], 'ready')
        self.assertEqual(self.notifier.call_count, 2)
        with patch.object(desktop.time, 'time', return_value=now + desktop.REMINDER_INTERVAL + 1):
            self.updater.check()
        self.assertEqual(self.notifier.call_count, 2)

    def test_new_release_does_not_reset_reminder_interval(self):
        now = int(time.time())
        with patch.object(desktop.time, 'time', return_value=now):
            self.to_second()
        self.make_version(3)
        self.http.release_root = self.releases[3]
        with patch.object(desktop.time, 'time', return_value=now + desktop.INTERVAL):
            result = self.updater.check()
        self.assertEqual(result['version'], '3-test')
        self.notifier.assert_called_once()

    def test_clock_rollback_defers_reminder(self):
        now = int(time.time())
        with patch.object(desktop.time, 'time', return_value=now + 100):
            self.to_second()
        with patch.object(desktop.time, 'time', return_value=now):
            self.assertEqual(self.updater.check()['status'], 'ready')
        self.notifier.assert_called_once()

    def test_forged_cached_ready_status_cannot_trigger_reminder(self):
        self.updater.save_status('ready', 'untrusted cache', version='forged')
        self.assertEqual(self.updater.check(background=True)['status'], 'ready')
        self.assertEqual(self.updater.check()['status'], 'up_to_date')
        self.notifier.assert_not_called()
        self.assertFalse((self.updater.root / 'reminder-status.json').exists())

    def test_concurrent_checks_do_not_duplicate_reminder(self):
        started, finish = threading.Event(), threading.Event()
        results = []
        self.http.release_root = self.releases[2]
        def notify(app):
            started.set()
            self.assertTrue(finish.wait(timeout=10))
        def check():
            try:
                results.append(self.updater.check())
            except Exception as error:
                results.append(error)
        self.notifier.side_effect = notify
        thread = threading.Thread(target=check)
        thread.start()
        try:
            self.assertTrue(started.wait(timeout=10))
            with self.assertRaises(BlockingIOError):
                desktop.DesktopUpdater(self.app, self.data).check(background=True)
        finally:
            finish.set()
            thread.join(timeout=10)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(results), 1)
        self.assertIsInstance(results[0], dict)
        self.assertEqual(results[0]['status'], 'ready')
        self.notifier.assert_called_once_with(self.app)

    def test_notifier_failures_keep_ready_update_and_do_not_retry_immediately(self):
        for error in (FileNotFoundError('missing notifier'),
                      subprocess.CalledProcessError(1, '/usr/bin/notify-send'),
                      subprocess.TimeoutExpired('/usr/bin/notify-send', 3)):
            with self.subTest(error=type(error).__name__):
                (self.updater.root / 'reminder-status.json').unlink(missing_ok=True)
                self.notifier.reset_mock()
                self.notifier.side_effect = error
                self.assertEqual(self.to_second()['status'], 'ready')
                self.assertEqual(self.updater.check()['status'], 'ready')
                self.notifier.assert_called_once_with(self.app)
                self.assertEqual(self.current_version(), '1-test')
                self.assertIsNotNone(self.state()['staged'])

    def test_malformed_or_unsafe_reminder_ledger_keeps_ready_without_prompting(self):
        self.to_second()
        self.notifier.reset_mock()
        path = self.updater.root / 'reminder-status.json'
        for raw in (b'not JSON', b'{}', b'{"format":true,"last_attempt_at":0}',
                    b'{"format":1,"last_attempt_at":false}',
                    b'{"format":1,"last_attempt_at":-1}',
                    b'{"format":1,"last_attempt_at":0,"extra":0}', b' ' * 1025):
            with self.subTest(raw=raw[:60]):
                path.write_bytes(raw)
                self.assertEqual(self.updater.check()['status'], 'ready')
                self.assertEqual(path.read_bytes(), raw)
                self.notifier.assert_not_called()
        path.chmod(0o666)
        self.assertEqual(self.updater.check()['status'], 'ready')
        self.notifier.assert_not_called()
        path.unlink()
        outside = self.root / 'outside-reminder'
        outside.write_bytes(b'keep')
        path.symlink_to(outside)
        self.assertEqual(self.updater.check()['status'], 'ready')
        self.assertEqual(outside.read_bytes(), b'keep')
        self.notifier.assert_not_called()

    def test_reminder_ledger_write_failure_keeps_update_ready(self):
        original = desktop.tempfile.mkstemp
        def fail_reminder(*args, **kwargs):
            if kwargs.get('prefix') == '.reminder-':
                raise OSError('injected reminder ledger failure')
            return original(*args, **kwargs)
        with patch.object(desktop.tempfile, 'mkstemp', side_effect=fail_reminder):
            self.assertEqual(self.to_second()['status'], 'ready')
        self.notifier.assert_not_called()
        self.assertIsNotNone(self.state()['staged'])

    def test_install_space_check_precedes_ready_notification(self):
        with patch.object(self.updater, 'require_install_space',
                          side_effect=ValueError('Not enough free disk space')):
            with self.assertRaisesRegex(ValueError, 'free disk space'):
                self.to_second()
        self.notifier.assert_not_called()
        self.assertFalse((self.updater.root / 'reminder-status.json').exists())

    def test_system_notifier_uses_fixed_message_and_system_libraries(self):
        self.notification_patcher.stop()
        env = {'PATH': '/untrusted', 'LD_LIBRARY_PATH': '/bundled-libs',
               'LD_PRELOAD': '/bundled.so', 'LD_AUDIT': '/bundled-audit.so',
               'GTK_PATH': '/bundled-gtk', 'GIO_EXTRA_MODULES': '/bundled-gio',
               'GIO_MODULE_DIR': '/bundled-gio-dir', 'GI_TYPELIB_PATH': '/bundled-gi',
               'GDK_PIXBUF_MODULE_FILE': '/bundled-pixbuf', 'GSETTINGS_SCHEMA_DIR': '/schemas',
               'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/owned-private-bus',
               'XDG_RUNTIME_DIR': '/owned-runtime', 'HOME': str(self.home)}
        with patch.object(desktop, 'ORIGINAL_ENV', env), \
                patch.object(desktop.subprocess, 'run') as run:
            desktop.send_update_notification(self.app)
        args, kwargs = run.call_args
        self.assertEqual(args[0], ['/usr/bin/notify-send', '--app-name=Noventra',
                                  f'--icon={self.app / "noventra-browser.png"}', '--urgency=normal',
                                  '--hint=string:desktop-entry:noventra-browser',
                                  '--hint=boolean:suppress-sound:true', '--',
                                  'Noventra update ready',
                                  'Close all Noventra windows and open Noventra again to install the verified update.'])
        self.assertEqual(kwargs['env'], {'PATH': '/usr/bin:/bin',
                                        'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/owned-private-bus',
                                        'XDG_RUNTIME_DIR': '/owned-runtime', 'HOME': str(self.home)})
        self.assertEqual(kwargs['timeout'], 3)
        self.assertTrue(kwargs['check'])
        self.assertNotIn('shell', kwargs)

    def test_invalid_signature_and_expired_manifest_do_not_modify_app(self):
        self.http.release_root = self.releases[2]
        manifest = self.releases[2] / 'manifest.json'
        original = manifest.read_bytes()
        manifest.write_bytes(original.replace(b'2-test', b'3-test'))
        with self.assertRaises(ValueError): self.updater.check()
        self.assertEqual(self.current_version(), '1-test')
        self.notifier.assert_not_called()
        value = release.parse(original)
        value.update(created_at=int(time.time()) - 10, expires_at=int(time.time()) - 1)
        manifest.write_bytes(release.encode(value))
        release.openssl('pkeyutl', '-sign', '-inkey', self.key, '-rawin', '-in', manifest,
                        '-out', self.releases[2] / 'manifest.sig')
        with self.assertRaises(ValueError): self.updater.check()
        self.assertEqual(self.current_version(), '1-test')
        self.notifier.assert_not_called()
        result, value = self.cli('check')
        self.assertEqual(result.returncode, 1)
        self.assertIn('out of date', value['message'])

    def test_tampered_archive_or_staged_file_cannot_be_installed(self):
        self.http.release_root = self.releases[2]
        artifact = next(self.releases[2].glob('*.tar.xz'))
        original = artifact.read_bytes()
        artifact.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        with self.assertRaises(ValueError): self.updater.check()
        self.notifier.assert_not_called()
        self.assertEqual(self.current_version(), '1-test')
        artifact.write_bytes(original)
        self.to_second()
        self.notifier.reset_mock()
        staged = Path(self.updater.cfg['install_root']) / 'releases' / self.state()['staged']
        (staged / 'Noventra.AppDir/opt/noventra/icudtl.dat').write_bytes(b'changed')
        with self.assertRaises(ValueError): self.updater.check()
        self.notifier.assert_not_called()
        with self.assertRaises(ValueError): self.updater.apply()
        self.assertEqual(self.current_version(), '1-test')

    def test_replay_and_installed_floor_reject_older_signed_releases(self):
        self.to_second()
        self.http.release_root = self.releases[1]
        with self.assertRaises(ValueError): self.updater.check()
        self.assertEqual(self.state()['highest_sequence'], 2)
        self.updater.apply()
        self.assertEqual(self.current_version(), '2-test')
        # The package's minimum protects a fresh install even without prior
        # observations. Use a separate empty updater state for this exercise.
        newer = desktop.DesktopUpdater(self.app, self.data)
        newer.root = self.root / 'fresh updater state'; newer.root.mkdir(mode=0o700)
        newer.cfg['install_root'] = str(newer.root / 'delivery')
        with self.assertRaises(ValueError): newer.check()
        self.assertEqual(delivery.read_state(Path(newer.cfg['install_root']), newer.cfg)['highest_sequence'], 0)

    def test_updater_binding_change_is_rejected_before_app_replacement(self):
        config = {**self.config_value, 'base_url': 'https://different.invalid/'}
        self.make_version(3, config=config)
        self.http.release_root = self.releases[3]
        with self.assertRaises(ValueError): self.updater.check()
        with self.assertRaises(ValueError): self.updater.apply()
        self.assertEqual(self.current_version(), '1-test')

    def test_new_payload_requires_a_new_packaged_minimum_sequence(self):
        self.to_second(); self.updater.apply()
        self.updater = desktop.DesktopUpdater(self.app, self.data)
        self.make_version(3, minimum_sequence=2)
        self.http.release_root = self.releases[3]
        with self.assertRaisesRegex(ValueError, 'minimum sequence'):
            self.updater.check()
        with self.assertRaisesRegex(ValueError, 'minimum sequence'):
            self.updater.apply()
        self.assertEqual(self.current_version(), '2-test')

    def test_completed_install_with_failed_delivery_state_commit_recovers(self):
        self.to_second()
        original = delivery.atomic_state
        def fail_final_state(root, state):
            if state['staged'] is None:
                raise OSError('injected final state failure')
            return original(root, state)
        with patch.object(delivery, 'atomic_state', side_effect=fail_final_state):
            with self.assertRaises(OSError): self.updater.apply()
        self.assertEqual(self.current_version(), '2-test')
        self.assertIsNotNone(self.state()['staged'])
        recovered = desktop.DesktopUpdater(self.app, self.data)
        # No payload copy is needed after the installer already committed.
        # Recovery must not demand another full app's worth of free space.
        usage = shutil.disk_usage(self.root)
        with patch.object(desktop.shutil, 'disk_usage', return_value=usage._replace(free=1)):
            self.assertEqual(recovered.apply()['status'], 'applied')
        self.assertIsNone(self.state()['staged'])

    def test_background_lock_is_exclusive_and_status_cli_is_bounded(self):
        with desktop.file_lock(self.updater.root / '.desktop-lock'):
            result, value = self.cli('background')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(value['status'], 'busy')
        self.assertLess(len(result.stdout), 1024)

    def test_low_space_stops_before_archive_download_and_preserves_retry(self):
        self.http.release_root = self.releases[2]
        usage = shutil.disk_usage(self.root)
        with patch.object(delivery.shutil, 'disk_usage', return_value=usage._replace(free=1)), \
                patch.object(delivery, 'download', wraps=delivery.download) as download:
            with self.assertRaisesRegex(ValueError, 'free disk space'):
                self.updater.check()
        self.assertFalse(any(call.args[1].endswith('.tar.xz') for call in download.call_args_list))
        self.assertEqual(self.state()['highest_sequence'], 2)
        self.assertIsNone(self.state()['staged'])
        self.assertEqual(self.current_version(), '1-test')
        self.assertEqual(list((Path(self.updater.cfg['install_root']) / '.staging').iterdir()), [])
        self.notifier.assert_not_called()
        self.assertEqual(self.updater.check()['status'], 'ready')

    def test_low_space_apply_retains_app_and_pending_version(self):
        self.to_second()
        staged = self.state()['staged']
        usage = shutil.disk_usage(self.root)
        with patch.object(desktop.shutil, 'disk_usage', return_value=usage._replace(free=1)):
            with self.assertRaisesRegex(ValueError, 'free disk space'):
                self.updater.apply()
        self.assertEqual(self.current_version(), '1-test')
        self.assertEqual(self.state()['staged'], staged)
        self.assertEqual(self.updater.apply()['status'], 'applied')

    def test_successful_apply_reclaims_only_download_cache_and_keeps_high_water(self):
        self.to_second()
        result = self.updater.apply()
        self.assertEqual(result['retained_cache_entries'], 0)
        self.assertEqual(list((Path(self.updater.cfg['install_root']) / 'releases').iterdir()), [])
        state = self.state()
        self.assertEqual(state['highest_sequence'], 2)
        self.assertIsNone(state['active']); self.assertIsNone(state['previous']); self.assertIsNone(state['staged'])
        newer = desktop.DesktopUpdater(self.app, self.data)
        self.assertEqual(newer.check()['status'], 'up_to_date')
        self.http.release_root = self.releases[1]
        with self.assertRaises(ValueError): newer.check()

    def test_about_worker_exits_and_cleans_temporary_download_after_parent_death(self):
        started, release_response = threading.Event(), threading.Event()
        class SlowHandler(server.DeliveryHandler):
            def do_GET(self):
                started.set()
                release_response.wait(timeout=10)
                super().do_GET()
        self.http.RequestHandlerClass = SlowHandler
        # This test owns the supervisor and worker. Adopt/reap the worker when
        # its supervisor exits instead of relying on the host's PID1 reaper.
        libc = ctypes.CDLL(None)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        argv = ['/usr/bin/python3', '-I', '-B', str(self.app / 'updater/desktop_updater.py'), 'check']
        code = ('import subprocess,time; p=subprocess.Popen(' + repr(argv)
                + ',stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);'
                'print(p.pid,flush=True);time.sleep(30)')
        owner = subprocess.Popen(['/usr/bin/python3', '-I', '-B', '-c', code],
                                 env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        worker = int(owner.stdout.readline())
        try:
            self.assertTrue(started.wait(timeout=5), 'Worker did not reach owned HTTP fixture')
            owner.terminate(); owner.wait(timeout=5)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                path = Path(f'/proc/{worker}/stat')
                if not path.exists() or path.read_text().split(') ', 1)[1].startswith('Z '):
                    break
                time.sleep(.02)
            else:
                self.fail('Update worker survived its owner')
            self.assertEqual(list((Path(self.updater.cfg['install_root']) / '.staging').iterdir()), [])
            self.assertEqual(os.waitpid(worker, 0)[0], worker)
        finally:
            release_response.set()
            if owner.poll() is None: owner.terminate(); owner.wait(timeout=5)
            owner.stdout.close(); owner.stderr.close()
            try:
                path = Path(f'/proc/{worker}/cmdline')
                if path.exists() and str(self.app).encode() in path.read_bytes():
                    os.kill(worker, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(worker, 0)
            except ChildProcessError:
                pass
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def test_explicit_owner_cli_accepts_its_actual_parent(self):
        run, value = self.cli('check', '--owner-pid', str(os.getpid()))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(value['status'], 'up_to_date')
        self.assertTrue((self.updater.root / 'desktop-status.json').is_file())

    def test_explicit_owner_cli_started_after_parent_exit_rejects_subreaper(self):
        requests = []
        class ObservedHandler(server.DeliveryHandler):
            def do_GET(self):
                requests.append(self.path)
                super().do_GET()
        self.http.RequestHandlerClass = ObservedHandler
        def snapshot():
            return {str(path.relative_to(self.updater.root)):
                    path.read_bytes() if path.is_file() else None
                    for path in self.updater.root.rglob('*')}
        before = snapshot()
        libc = ctypes.CDLL(None)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        gate_read, gate_write = os.pipe()
        stdout, stderr = self.root / 'late-owner.stdout', self.root / 'late-owner.stderr'
        bootstrap = ("import os,sys; fd=int(sys.argv[1]); owner=sys.argv[2]; helper=sys.argv[3];"
                     "assert os.read(fd,1)==b'x'; os.close(fd);"
                     "os.execv('/usr/bin/python3',['/usr/bin/python3','-I','-B',helper,'--owner-pid',owner,'check'])")
        owner_code = ("import os,subprocess; out=open(" + repr(str(stdout)) + ",'w'); err=open("
                      + repr(str(stderr)) + ",'w'); p=subprocess.Popen(['/usr/bin/python3','-I','-B','-c',"
                      + repr(bootstrap) + "," + repr(str(gate_read)) + ",str(os.getpid()),"
                      + repr(str(self.app / 'updater/desktop_updater.py')) + "],pass_fds=("
                      + str(gate_read) + ",),stdout=out,stderr=err); print(p.pid,flush=True)")
        owner = subprocess.Popen(['/usr/bin/python3', '-I', '-B', '-c', owner_code],
            pass_fds=(gate_read,), env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        worker = worker_start = None
        try:
            self.assertTrue(select.select([owner.stdout], [], [], 5)[0], 'Short parent did not publish its child')
            worker = int(owner.stdout.readline())
            owner.wait(timeout=5)
            # The helper has not exec'd yet. Observe the actual reparenting,
            # then release the gate so Python starts under the wrong supervisor.
            fields = Path(f'/proc/{worker}/stat').read_text().rsplit(') ', 1)[1].split()
            self.assertEqual(int(fields[1]), os.getpid())
            worker_start = fields[19]
            os.write(gate_write, b'x'); os.close(gate_write); gate_write = None
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                pid, status = os.waitpid(worker, os.WNOHANG)
                if pid:
                    break
                time.sleep(.02)
            else:
                self.fail('Late helper did not reject its dead owner promptly')
            self.assertEqual(pid, worker)
            self.assertEqual(os.waitstatus_to_exitcode(status), 0, stderr.read_text())
            self.assertEqual(json.loads(stdout.read_text())['status'], 'unchecked')
            self.assertEqual(snapshot(), before, 'Stale ownership must precede managed metadata/state access')
            self.assertEqual(requests, [], 'Stale ownership must be rejected before any HTTP request')
        finally:
            if owner.poll() is None:
                owner.terminate(); owner.wait(timeout=5)
            owner.stdout.close(); owner.stderr.close()
            os.close(gate_read)
            if gate_write is not None:
                os.close(gate_write)
            if worker is not None:
                try:
                    fields = Path(f'/proc/{worker}/stat').read_text().rsplit(') ', 1)[1].split()
                    if worker_start is not None and fields[19] == worker_start:
                        os.kill(worker, signal.SIGTERM)
                except (FileNotFoundError, ProcessLookupError):
                    pass
                try:
                    os.waitpid(worker, 0)
                except ChildProcessError:
                    pass
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def test_unmanaged_extraction_does_not_create_update_state(self):
        env = {**self.env, 'XDG_DATA_HOME': str(self.home / 'unrelated data')}
        process = subprocess.run([str(self.packages[1] / 'AppRun'), '0'], env=env,
                                 capture_output=True, text=True, timeout=5)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertFalse((self.home / 'unrelated data').exists())


class ParentBindingTests(unittest.TestCase):
    def test_matching_explicit_owner_binds_then_rechecks(self):
        with patch.object(desktop.os, 'getppid', side_effect=[812, 812]), \
                patch.object(desktop.ctypes, 'CDLL') as library:
            library.return_value.prctl.return_value = 0
            desktop.follow_parent(812)
        library.return_value.prctl.assert_called_once_with(1, signal.SIGTERM, 0, 0, 0)

    def test_mismatched_owner_is_rejected_before_prctl(self):
        with patch.object(desktop.os, 'getppid', return_value=991), \
                patch.object(desktop.ctypes, 'CDLL') as library:
            with self.assertRaises(desktop.InterruptedUpdate):
                desktop.follow_parent(812)
        library.assert_not_called()

    def test_parent_change_during_binding_is_rejected(self):
        with patch.object(desktop.os, 'getppid', side_effect=[812, 991]), \
                patch.object(desktop.ctypes, 'CDLL') as library:
            library.return_value.prctl.return_value = 0
            with self.assertRaises(desktop.InterruptedUpdate):
                desktop.follow_parent(812)
        library.return_value.prctl.assert_called_once_with(1, signal.SIGTERM, 0, 0, 0)

    def test_invalid_explicit_owner_is_rejected_before_prctl(self):
        for value in (0, 1, -1, True, '812'):
            with self.subTest(owner=value), patch.object(desktop.os, 'getppid', return_value=812), \
                    patch.object(desktop.ctypes, 'CDLL') as library:
                with self.assertRaisesRegex(ValueError, 'owner PID'):
                    desktop.follow_parent(value)
                library.assert_not_called()

    def test_legacy_parent_binding_still_works_without_argument(self):
        with patch.object(desktop.os, 'getppid', side_effect=[812, 812]), \
                patch.object(desktop.ctypes, 'CDLL') as library:
            library.return_value.prctl.return_value = 0
            desktop.follow_parent()
        library.return_value.prctl.assert_called_once_with(1, signal.SIGTERM, 0, 0, 0)

    def test_stale_owner_cli_returns_before_prctl_metadata_or_network(self):
        output = io.StringIO()
        with patch.object(desktop.sys, 'argv', ['desktop_updater.py', '--owner-pid', '812', 'check']), \
                patch.object(desktop.os, 'getppid', return_value=991), \
                patch.object(desktop.ctypes, 'CDLL') as library, \
                patch.object(desktop, 'DesktopUpdater') as updater, \
                patch.object(desktop.signal, 'signal'), patch.object(desktop.sys, 'stdout', output):
            result = desktop.main()
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(output.getvalue())['status'], 'unchecked')
        library.assert_not_called(); updater.assert_not_called()


if __name__ == '__main__':
    unittest.main()
