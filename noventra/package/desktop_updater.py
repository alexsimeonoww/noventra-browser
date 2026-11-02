#!/usr/bin/env python3
"""Signed updates for the managed per-user Linux installation.

The verifier and installer are bundled, trusted code. Remote manifests contain
data only. Downloaded packages are never launched by the update checker.
"""
import argparse
import ctypes
from contextlib import contextmanager
import fcntl
import importlib.util
import http.client
import json
import lzma
import os
from pathlib import Path
import signal
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time

sys.dont_write_bytecode = True
# -I excludes environment/user Python imports. Only our inventoried siblings
# supply the verifier. OpenSSL and desktop helpers use system executable paths.
ORIGINAL_ENV = dict(os.environ)
os.environ['PATH'] = '/usr/bin:/bin'
sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_manifest as release
import update_delivery as delivery

INTERVAL = 6 * 3600
OPERATION_TIMEOUT = 900
SPACE_RESERVE = 64 * 1024**2
ERRORS = (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError,
          TimeoutError, tarfile.TarError, lzma.LZMAError, http.client.HTTPException)
READY = 'Update ready. Close all Noventra windows and open Noventra again to install it.'
require = release.require


class InterruptedUpdate(Exception):
    pass


def failure_message(error):
    reason = str(error)
    if 'Atomic updates' in reason:
        return 'This filesystem does not support automatic updates. Your current version was kept.'
    if 'free disk space' in reason:
        return 'Not enough free disk space to prepare the update. Free some space and try again.'
    if 'Manifest expired or from the future' in reason:
        return 'The update feed is out of date or your system clock is incorrect. Try again later.'
    if any(text in reason for text in ('Downgrade/replay', 'Sequence reused', 'Feed is older')):
        return 'The update feed offered an older or inconsistent release. Your current version was kept.'
    if any(text in reason for text in ('OpenSSL operation', 'signature', 'signer', 'hash mismatch')):
        return 'The update could not be authenticated. Your current version was kept.'
    return 'The update could not be verified or downloaded. Your current version was kept. Try again later.'


def configuration(folder):
    value = release.parse(release.read(folder / 'config.json', release.MANIFEST_LIMIT))
    release.keys(value, ('format', 'base_url', 'channel', 'architecture', 'sequence', 'public_key'))
    require(type(value['format']) is int and value['format'] == 1, 'Unsupported updater configuration')
    require(value['public_key'] == 'release-public.pem', 'Invalid pinned public key path')
    release.integer(value['sequence'], 1)
    cfg = {**value, 'public_key': str(folder / 'release-public.pem'),
           'install_root': '/unused', 'max_artifact_bytes': 2 * 1024**3,
           'max_unpacked_bytes': 4 * 1024**3}
    del cfg['sequence']
    # Reuse the client schema, origin restrictions and real Ed25519 parsing.
    with tempfile.TemporaryDirectory(prefix='noventra-update-config-') as temporary:
        path = Path(temporary) / 'client.json'
        path.write_bytes(release.encode(cfg))
        checked = delivery.config(path)
    return value, checked


def data_home():
    value = Path(ORIGINAL_ENV.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    require(value.is_absolute(), 'XDG_DATA_HOME must be absolute')
    return value.resolve()


def load_installer(app):
    spec = importlib.util.spec_from_file_location('noventra_desktop_installer', app / 'installer.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@contextmanager
def file_lock(path, *, blocking=False):
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                and info.st_nlink == 1 and not info.st_mode & 0o077, 'Unsafe updater lock')
        fcntl.flock(fd, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        yield
    finally:
        os.close(fd)


class DesktopUpdater:
    def __init__(self, app, home=None):
        self.app = Path(app).resolve(strict=True)
        self.data = data_home() if home is None else Path(home).resolve(strict=True)
        self.managed = self.data / 'noventra-browser'
        require(self.app == self.managed / 'app', 'Updates are available after installing Noventra')
        self.installer = load_installer(self.app)
        self.root = self.managed / 'updates'
        self.refresh_installed()
        if not self.root.exists():
            self.root.mkdir(mode=0o700)
        delivery.directory(self.root)

    def refresh_installed(self):
        state = self.installer.state_for(self.managed, self.data)
        require(state.get('status') == 'installed', 'Updates require a completed managed installation')
        self.meta = release.parse(release.read(self.app / 'noventra-package.json', release.MANIFEST_LIMIT))
        self.settings, self.cfg = configuration(self.app / 'updater')
        require(self.meta['architecture'] == self.settings['architecture'] == os.uname().machine,
                'Update architecture does not match this installation')
        self.cfg['install_root'] = str(self.root / 'delivery')

    def cached(self):
        path = self.root / 'desktop-status.json'
        if not path.exists():
            return {'status': 'unchecked', 'message': 'Updates have not been checked yet.', 'checked_at': 0}
        value = release.parse(release.read(path, release.MANIFEST_LIMIT))
        require(value.get('format') == 1 and isinstance(value.get('checked_at'), (int, float)),
                'Invalid updater status')
        return value

    def save_status(self, status, message, *, version='', checked_at=None):
        value = {'format': 1, 'status': status, 'message': message, 'version': version,
                 'checked_at': time.time() if checked_at is None else checked_at}
        fd, name = tempfile.mkstemp(prefix='.status-', dir=self.root)
        try:
            with os.fdopen(fd, 'wb') as stream:
                stream.write(release.encode(value)); stream.flush(); os.fsync(stream.fileno())
            os.replace(name, self.root / 'desktop-status.json')
            delivery.sync_directory(self.root)
        finally:
            Path(name).unlink(missing_ok=True)
        return value

    def initialize(self):
        root = Path(self.cfg['install_root'])
        if not root.exists():
            delivery.initialize(self.cfg)

    def remote_manifest(self, root, state):
        with tempfile.TemporaryDirectory(prefix='check-', dir=root / '.staging') as folder:
            path = Path(folder)
            delivery.download(self.cfg, 'manifest.json', path / 'manifest.json', release.MANIFEST_LIMIT)
            delivery.download(self.cfg, 'manifest.sig', path / 'manifest.sig', 64, expected_size=64)
            raw = release.read(path / 'manifest.json', release.MANIFEST_LIMIT)
            manifest = delivery.authenticated(self.cfg, raw, release.read(path / 'manifest.sig', 64))
            require(manifest['sequence'] >= self.settings['sequence'], 'Feed is older than the installed release')
            delivery.check_sequence(state, manifest, raw)
            delivery.atomic_state(root, state)
            return manifest

    def matches_current(self, manifest):
        return all(manifest[key] == self.meta[key] for key in ('version_label', 'snapshot_sha256'))

    def check(self, *, background=False):
        with file_lock(self.root / '.desktop-lock', blocking=not background):
            cached = self.cached()
            if background and 0 <= time.time() - cached['checked_at'] < INTERVAL:
                return cached
            self.initialize()
            with delivery.locked(self.cfg['install_root']) as root:
                state = delivery.read_state(root, self.cfg)
                manifest = self.remote_manifest(root, state)
                if self.matches_current(manifest):
                    # A signed manifest renewal can advance the high-water
                    # without forcing a reinstall of the same browser payload.
                    state['staged'] = None
                    delivery.atomic_state(root, state)
                    return self.save_status('up_to_date', 'Noventra is up to date.')
                # Do not offer a ready automatic update on a filesystem that
                # cannot preserve the installed launcher atomically.
                with tempfile.TemporaryDirectory(prefix='.update-capability-', dir=self.managed) as probe:
                    self.installer.require_atomic_exchange(Path(probe))
                delivery.inspect_remote(self.cfg, root, state, True, space_reserve=SPACE_RESERVE)
                manifest, _ = delivery.verify_installed(root, state['staged'], self.cfg)
                require(manifest['sequence'] >= self.settings['sequence'], 'Staged release is older than installed')
                target = root / 'releases' / state['staged'] / 'Noventra.AppDir'
                self.check_target(target, manifest)
                self.require_install_space(target)
                return self.save_status('ready', READY, version=manifest['version_label'])

    def discard_downloads(self, root):
        # All downloaded versions are cache copies. The transactional installer
        # has independently copied/verified the selected app. Anti-replay state
        # remains, without accumulating an unbounded set of old browser copies.
        retained = 0
        for path in (root / 'releases').iterdir():
            try:
                delivery.verify_installed(root, path.name, self.cfg, allow_expired=True)
                shutil.rmtree(path)
            except ERRORS:
                retained += 1  # Never remove changed/unknown files or versions.
        delivery.sync_directory(root / 'releases')
        return retained

    def check_target(self, target, manifest):
        settings, cfg = configuration(target / 'updater')
        require(delivery.binding(cfg) == delivery.binding(self.cfg), 'Update changed its trusted feed or signer')
        newer_floor = (settings['sequence'] >= self.settings['sequence'] if self.matches_current(manifest)
                       else settings['sequence'] > self.settings['sequence'])
        require(newer_floor and settings['sequence'] <= manifest['sequence'],
                'Update has an invalid minimum sequence')

    def require_install_space(self, target):
        inventory = release.parse(release.read(target / 'install-manifest.json', release.DISTRIBUTION_LIMIT))
        required = sum(row.get('size', 0) for row in inventory['files'].values()) + SPACE_RESERVE
        require(shutil.disk_usage(self.managed).free >= required,
                'Not enough free disk space to safely install this update')

    def apply(self, *, transition_locked=False, installation_locked=False):
        if not transition_locked:
            with file_lock(self.managed / '.launch-lock', blocking=True):
                return self.apply(transition_locked=True)
        if not installation_locked:
            with self.installer.lock(self.managed, blocking=True):
                # A prior installer transaction is independent of the delivery
                # cache. Recover before trusting the current metadata/floor.
                self.installer.recover_update(self.managed, self.data)
                self.refresh_installed()
                return self.apply(transition_locked=True, installation_locked=True)
        # Never wait for a network download before opening a browser. A check
        # in progress will finish and its ready version waits for the next run.
        with file_lock(self.root / '.desktop-lock'):
            if not Path(self.cfg['install_root']).exists():
                return {'status': 'unchanged'}
            with delivery.locked(self.cfg['install_root']) as root:
                state = delivery.read_state(root, self.cfg)
                if state['staged'] is None:
                    return {'status': 'unchanged'}
                # Check before hashing a large payload, and again transactionally
                # in the existing installer. Never terminate an open browser.
                try:
                    self.installer.require_closed(self.app)
                except ValueError:
                    cached = self.cached()
                    return self.save_status('ready', READY, version=cached.get('version', ''),
                                            checked_at=cached['checked_at'])
                manifest, raw = delivery.verify_installed(root, state['staged'], self.cfg)
                require(manifest['sequence'] >= self.settings['sequence']
                        and manifest['sequence'] == state['highest_sequence']
                        and release.sha(raw) == state['highest_manifest'], 'Staged release is superseded')
                target = root / 'releases' / state['staged'] / 'Noventra.AppDir'
                self.check_target(target, manifest)
                installed = self.installer.state_for(self.managed, self.data)
                target_fingerprint = self.installer.describe(target / 'install-manifest.json')['sha256']
                if installed['manifest_sha256'] != target_fingerprint:
                    self.require_install_space(target)
                # Call already-installed trusted installer code. The remote
                # install.sh/installer.py are copied as data, never executed.
                result = self.installer.install(target, self.data, lock_held=True)
                require(result.get('installed') is True, 'Update installation did not complete')
                state['previous'] = state['active'] = state['staged'] = None
                delivery.atomic_state(root, state)
                result = self.save_status('applied', 'Noventra was updated.', version=manifest['version_label'])
                try:
                    result['retained_cache_entries'] = self.discard_downloads(root)
                except ERRORS:
                    result['retained_cache_entries'] = 1
                return result


def deadline(signum, frame):
    raise TimeoutError('Update operation timed out')


def interrupted(signum, frame):
    raise InterruptedUpdate('The browser closed before the download finished')


def follow_parent(expected_pid=None):
    """Bind to the real owner before reading metadata or making requests."""
    parent = os.getppid()
    require(parent > 1, 'Update owner is no longer running')
    if expected_pid is not None:
        require(type(expected_pid) is int and expected_pid > 1, 'Invalid update owner PID')
        if parent != expected_pid:
            raise InterruptedUpdate('Update owner already exited')
    libc = ctypes.CDLL(None, use_errno=True)
    require(libc.prctl(1, signal.SIGTERM, 0, 0, 0) == 0, 'Could not bind update worker lifetime')
    current = os.getppid()
    if current != parent or (expected_pid is not None and current != expected_pid):
        raise InterruptedUpdate('Update owner already exited')


def launch(app, arguments):
    binary = app / 'opt/noventra/noventra-browser'
    managed = data_home() / 'noventra-browser'
    if app != managed / 'app' or not (managed / 'installation.json').is_file():
        os.execve(binary, [str(binary), *arguments], ORIGINAL_ENV)
    # Coordinated launches cannot start the native executable in the middle of
    # replacement. Popen's exec handshake completes before releasing the lock.
    with file_lock(managed / '.launch-lock', blocking=True):
        # Manual install/uninstall uses this same lock. Hold it through the
        # native exec handshake, not just the automated install decision.
        with file_lock(managed / '.install-lock', blocking=True):
            try:
                signal.alarm(OPERATION_TIMEOUT)
                installer = load_installer(app)
                installer.recover_update(managed, data_home())
                try:
                    updater = DesktopUpdater(app)
                    updater.apply(transition_locked=True, installation_locked=True)
                except ERRORS as error:
                    if 'updater' in locals() and not isinstance(error, BlockingIOError):
                        try:
                            updater.save_status('failed', failure_message(error))
                        except ERRORS:
                            pass
                # If install failed during recovery, do not launch mismatched
                # app/state. A healthy rollback or unsupported-exchange refusal
                # leaves no journal and starts the previous browser normally.
                installer.recover_update(managed, data_home())
                process = subprocess.Popen([str(binary), *arguments], env=ORIGINAL_ENV)
            finally:
                signal.alarm(0)
    # Forward termination of the coordinator to its own native child. Closing
    # a terminal cannot leave an unattended native process behind.
    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, lambda number, frame: process.send_signal(number))
    checker = None
    next_check = 0
    try:
        while True:
            if checker is not None and checker.poll() is not None:
                checker = None
            if checker is None and time.monotonic() >= next_check:
                try:
                    checker = subprocess.Popen(['/usr/bin/python3', '-I', '-B',
                                                str(app / 'updater/desktop_updater.py'), 'background'],
                                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL, env=ORIGINAL_ENV)
                except OSError:
                    pass  # A failed checker must never prevent normal browsing.
                next_check = time.monotonic() + INTERVAL
            try:
                result = process.wait(timeout=30)
                return result if result >= 0 else 128 - result
            except subprocess.TimeoutExpired:
                pass
    finally:
        if checker is not None and checker.poll() is None:
            checker.terminate()
            try:
                checker.wait(timeout=5)
            except subprocess.TimeoutExpired:
                checker.kill(); checker.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--owner-pid', type=int, help='Expected native parent for an About check')
    parser.add_argument('operation', choices=('launch', 'background', 'check', 'apply', 'status'))
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    signal.signal(signal.SIGALRM, deadline)
    # SIGTERM unwinds owned download/extraction temporary directories when the
    # launch coordinator exits. No background service survives browser closure.
    signal.signal(signal.SIGTERM, interrupted)
    app = Path(__file__).resolve().parent.parent
    try:
        if args.operation == 'launch':
            argv = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
            if sys.version_info < (3, 11):
                binary = app / 'opt/noventra/noventra-browser'
                os.execve(binary, [str(binary), *argv], ORIGINAL_ENV)
            return launch(app, argv)
        require(sys.version_info >= (3, 11), 'Updates require Python 3.11 or newer')
        follow_parent(args.owner_pid)
        signal.alarm(OPERATION_TIMEOUT)
        updater = DesktopUpdater(app)
        if args.operation in ('check', 'background'):
            result = updater.check(background=args.operation == 'background')
        elif args.operation == 'apply':
            result = updater.apply()
        else:
            result = updater.cached()
    except BlockingIOError:
        result = {'status': 'busy', 'message': 'Another update check is running.'}
    except InterruptedUpdate:
        result = {'status': 'unchecked', 'message': 'The update will be checked on the next launch.'}
        if 'updater' in locals():
            updater.save_status('unchecked', result['message'], checked_at=0)
    except ERRORS as error:
        result = {'status': 'failed', 'message': failure_message(error)}
        if 'updater' in locals():
            try:
                updater.save_status('failed', result['message'])
            except ERRORS:
                pass
    print(json.dumps(result))
    return 0 if result['status'] != 'failed' else 1


if __name__ == '__main__':
    sys.exit(main())
