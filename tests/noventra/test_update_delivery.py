"""Local delivery tests: generated test keys, loopback HTTP, fake browser AppDirs."""

import copy
from http.client import HTTPResponse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import lzma
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
sys.path.insert(0, str(ROOT / 'tests/noventra'))
import release_manifest as release
import update_delivery as client
import local_delivery_server as server
import test_package_distribution as package_fixture


class DeliveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='noventra-delivery-tests-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.key = cls.base / 'disposable-test-key.pem'
        release.openssl('genpkey', '-algorithm', 'ED25519', '-out', cls.key)
        cls.key.chmod(0o600)
        cls.public = cls.base / 'test-public.pem'
        cls.public.write_bytes(release.openssl('pkey', '-in', cls.key, '-pubout'))
        cls.wrong_key = cls.base / 'other-test-key.pem'
        release.openssl('genpkey', '-algorithm', 'ED25519', '-out', cls.wrong_key)
        cls.wrong_key.chmod(0o600)
        fixture = package_fixture.PackageTests('test_hash_mismatch')
        fixture.setUp()
        cls.addClassCleanup(fixture.doCleanups)
        cls.distributions = []
        cls.published = []
        for version in (1, 2):
            fixture.put('icudtl.dat', f'fake version {version}\n'.encode())
            fixture.refresh()
            distribution = cls.base / f'distribution-{version}'
            package_fixture.package.package(fixture.source, distribution, f'{version}-local', archive=True)
            published = cls.base / f'published-{version}'
            process = subprocess.run([
                sys.executable, '-B', str(ROOT / 'tools/noventra/release_manifest.py'),
                '--distribution', str(distribution), '--output', str(published),
                '--private-key', str(cls.key), '--sequence', str(version)],
                capture_output=True, text=True, timeout=30)
            if process.returncode:
                raise RuntimeError(process.stderr)
            if json.loads(process.stdout)['sequence'] != version:
                raise RuntimeError('Publisher returned wrong sequence')
            cls.distributions.append(distribution)
            cls.published.append(published)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='case-', dir=self.base)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.feed = self.root / 'feed'
        shutil.copytree(self.published[0], self.feed)
        self.http = server.make_server(self.feed)
        thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.stop_http, self.http, thread)
        self.cfg_path = self.root / 'client.json'
        self.config_value = {
            'format': 1, 'base_url': f'http://127.0.0.1:{self.http.server_port}/',
            'public_key': str(self.public), 'install_root': str(self.root / 'install'),
            'channel': 'local', 'architecture': 'x86_64',
            'max_artifact_bytes': 8 * 1024**2, 'max_unpacked_bytes': 32 * 1024**2,
        }
        self.save_config()
        client.operate(self.cfg, 'init')

    @staticmethod
    def stop_http(http, thread):
        http.shutdown()
        http.server_close()
        thread.join(timeout=3)

    def save_config(self):
        self.cfg_path.write_bytes(release.encode(self.config_value))
        self.cfg = client.config(self.cfg_path)

    def version(self, number):
        self.http.release_root = self.published[number - 1]

    def state(self):
        return release.parse((Path(self.cfg['install_root']) / 'state.json').read_bytes())

    def sign(self, value=None, raw=None, key=None):
        if raw is None:
            raw = release.encode(value)
        (self.feed / 'manifest.json').write_bytes(raw)
        release.openssl('pkeyutl', '-sign', '-inkey', key or self.key, '-rawin',
                        '-in', self.feed / 'manifest.json', '-out', self.feed / 'manifest.sig')
        self.http.release_root = self.feed

    def manifest(self):
        return release.parse((self.feed / 'manifest.json').read_bytes())

    def activate(self, number):
        self.version(number)
        client.operate(self.cfg, 'stage')
        return client.operate(self.cfg, 'activate')

    def assert_rejected(self, operation='stage', message=None):
        before = self.state()
        with self.assertRaises((ValueError, OSError, tarfile.TarError, lzma.LZMAError,
                                urllib.error.URLError), msg=message):
            client.operate(self.cfg, operation)
        after = self.state()
        self.assertEqual(after['active'], before['active'])
        self.assertEqual(after['previous'], before['previous'])
        self.assertEqual(after['staged'], before['staged'])
        self.assertEqual(list((Path(self.cfg['install_root']) / '.staging').iterdir()), [])

    def cli(self, operation):
        process = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/noventra/update_delivery.py'),
                                  '--config', str(self.cfg_path), operation],
                                 capture_output=True, text=True, timeout=30)
        self.assertEqual(process.returncode, 0, process.stderr)
        return json.loads(process.stdout)

    def test_complete_cli_publish_download_stage_activate_update_rollback(self):
        self.assertEqual(self.cli('check')['status'], 'available')
        self.assertEqual(self.cli('stage')['status'], 'staged')
        first = self.cli('activate')
        self.assertEqual(Path(first['active_path']).name, 'AppRun')
        root = Path(first['active_path']).parent
        self.assertEqual((root / 'opt/noventra/icudtl.dat').read_bytes(), b'fake version 1\n')
        self.assertEqual(os.readlink(root / 'usr/bin/noventra-browser'), release.LINK_TARGET)
        self.version(2)
        self.cli('stage')
        second = self.cli('activate')
        self.assertNotEqual(second['active'], first['active'])
        self.assertEqual(self.cli('active-path')['active_path'], second['active_path'])
        rolled = self.cli('rollback')
        self.assertEqual(rolled['active'], first['active'])
        self.assertEqual(rolled['highest_sequence'], 2)
        self.assertTrue(Path(second['active_path']).exists())
        self.assertEqual(list(self.root.glob('*profile*')), [])

    def test_publisher_refuses_existing_output_bad_archive_and_non_ed25519(self):
        with self.assertRaises(ValueError):
            release.publish(self.distributions[0], self.published[0], self.key, 3)
        broken = self.root / 'broken'
        shutil.copytree(self.distributions[0], broken)
        next(broken.glob('*.tar.xz')).write_bytes(b'bad')
        output = self.root / 'not-published'
        with self.assertRaises(ValueError):
            release.publish(broken, output, self.key, 3)
        self.assertFalse(output.exists())
        rsa = self.root / 'test-rsa.pem'
        release.openssl('genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', rsa)
        with self.assertRaises(ValueError):
            release.publish(self.distributions[0], output, rsa, 3)
        self.assertFalse(output.exists())

    def test_native_external_extension_directory_is_inventoried_before_launch(self):
        staged = client.operate(self.cfg, 'stage')
        installed = Path(self.cfg['install_root']) / 'releases' / staged['release']
        directory = installed / release.RUNTIME / 'extensions'
        marker = directory / 'README.noventra.txt'
        self.assertTrue(marker.is_file())
        self.assertEqual(list(directory.iterdir()), [marker])
        self.assertFalse(directory.stat().st_mode & 0o022)
        inventory = release.parse((installed / 'distribution.json').read_bytes())['files']
        self.assertEqual(inventory[marker.relative_to(installed).as_posix()]['sha256'],
                         release.sha(marker.read_bytes()))
        # Match PathProvider's native "create only if absent" operation. No
        # directory appears outside the inventory when the browser queries it.
        if not directory.exists():
            directory.mkdir(mode=0o755)
        first = client.operate(self.cfg, 'activate')
        self.assertEqual(client.operate(self.cfg, 'active-path')['active'], first['active'])
        self.activate(2)
        self.assertEqual(client.operate(self.cfg, 'rollback')['active'], first['active'])
        self.assertEqual(self.state()['highest_sequence'], 2)
        # The marker does not grant an exception for arbitrary directory/file
        # additions or permit unlisted extension installation metadata.
        (directory / 'unlisted.json').write_text('{}')
        with self.assertRaisesRegex(ValueError, 'Unexpected installed file'):
            client.operate(self.cfg, 'active-path')

    def test_signature_tamper_wrong_key_and_missing_signature(self):
        raw = (self.feed / 'manifest.json').read_bytes()
        (self.feed / 'manifest.json').write_bytes(raw + b' ')
        self.assert_rejected()
        self.sign(raw=raw, key=self.wrong_key)
        self.assert_rejected()
        (self.feed / 'manifest.sig').unlink()
        self.assert_rejected()
        self.assertEqual(self.state()['highest_sequence'], 0)

    def test_duplicate_json_unknown_fields_and_wrong_identity(self):
        good = self.manifest()
        raw = release.encode(good).replace(b'"sequence": 1,', b'"sequence": 1, "sequence": 1,')
        self.sign(raw=raw)
        self.assert_rejected()
        for field, value in [('product', 'helium'), ('platform', 'windows'), ('architecture', 'aarch64'),
                             ('channel', 'stable'), ('sequence', True), ('hooks', ['run something']),
                             ('signer_id', '0' * 64)]:
            with self.subTest(field=field):
                mutated = copy.deepcopy(good)
                mutated[field] = value
                self.sign(value=mutated)
                self.assert_rejected()

    def test_expired_future_and_excess_lifetime(self):
        good = self.manifest()
        now = int(time.time())
        for created, expires in [(now - 10, now - 1), (now + 1000, now + 1100),
                                 (now, now + release.MAX_LIFETIME + 1)]:
            mutated = copy.deepcopy(good)
            mutated.update(created_at=created, expires_at=expires)
            self.sign(value=mutated)
            self.assert_rejected()

    def test_replay_equivocation_and_idempotent_stage(self):
        one = self.activate(1)
        two = self.activate(2)
        again = client.operate(self.cfg, 'stage')
        self.assertEqual(again['status'], 'up_to_date')
        self.assertEqual(self.state()['previous'], one['active'])
        client.operate(self.cfg, 'rollback')
        self.version(1)
        self.assert_rejected()
        mutated = release.parse((self.published[1] / 'manifest.json').read_bytes())
        mutated['version_label'] = 'different'
        self.sign(value=mutated)
        self.assert_rejected()
        self.assertEqual(self.state()['highest_sequence'], 2)
        self.assertEqual(self.state()['previous'], two['active'])

    def test_newer_observation_blocks_old_staged_activation(self):
        client.operate(self.cfg, 'stage')
        self.version(2)
        client.operate(self.cfg, 'check')
        self.assert_rejected('activate')

    def test_download_hash_sizes_encoding_and_cap(self):
        good = self.manifest()
        dist_path = self.feed / 'distribution.json'
        original_dist = dist_path.read_bytes()
        inconsistent = copy.deepcopy(good)
        inconsistent['artifact']['sha256'] = '0' * 64
        self.sign(value=inconsistent)
        cfg = dict(self.cfg, install_root=str(self.root / 'inconsistent-inventory-install'))
        client.operate(cfg, 'init')
        with patch.object(client, 'download', wraps=client.download) as downloads:
            with self.assertRaisesRegex(ValueError, 'Archive inventory mismatch'):
                client.operate(cfg, 'stage')
        self.assertFalse(any(call.args[1].endswith('.tar.xz') for call in downloads.call_args_list))
        for field in ('distribution', 'artifact'):
            dist_path.write_bytes(original_dist)
            mutated = copy.deepcopy(good)
            mutated[field]['sha256'] = '0' * 64
            if field == 'artifact':
                # Keep the signed inventory consistent so this variation gets
                # past inventory validation to the actual download hash check.
                dist = release.parse(original_dist)
                dist['files'][good['artifact']['filename']]['sha256'] = '0' * 64
                raw_dist = release.encode(dist)
                dist_path.write_bytes(raw_dist)
                mutated['distribution'].update(size=len(raw_dist), sha256=release.sha(raw_dist))
            self.sign(value=mutated)
            # Each variation has its own root: same-sequence equivocation must not mask download validation.
            cfg = dict(self.cfg, install_root=str(self.root / f'{field}-install'))
            client.operate(cfg, 'init')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                client.operate(cfg, 'stage')
        dist_path.write_bytes(original_dist)
        cfg = dict(self.cfg, install_root=str(self.root / 'cap-install'), max_artifact_bytes=1)
        client.operate(cfg, 'init')
        self.sign(value=good)
        with self.assertRaisesRegex(ValueError, 'exceeds client limit'):
            client.operate(cfg, 'stage')
        (self.feed / good['artifact']['filename']).write_bytes(b'short')
        self.assert_rejected()

    def rewrite_archive(self, transform, inventory=None):
        # Deliberately sign malformed tar bytes with the test key to test the client
        # independently of the publisher's own refusal to publish them.
        manifest = self.manifest()
        archive = self.feed / manifest['artifact']['filename']
        with tarfile.open(archive, 'r:xz') as old:
            members = [(copy.copy(i), old.extractfile(i).read() if i.isreg() else None) for i in old]
        members = transform(members)
        with tarfile.open(archive, 'w:xz', format=tarfile.PAX_FORMAT) as out:
            for info, data in members:
                out.addfile(info, io.BytesIO(data) if data is not None else None)
        dist_path = self.feed / 'distribution.json'
        dist = release.parse(dist_path.read_bytes())
        if inventory:
            inventory(dist)
        data = archive.read_bytes()
        manifest['artifact']['sha256'] = release.sha(data)
        manifest['artifact']['size'] = len(data)
        dist['files'][archive.name] = {'sha256': release.sha(data), 'size': len(data), 'mode': '0o644'}
        raw_dist = release.encode(dist)
        dist_path.write_bytes(raw_dist)
        manifest['distribution'].update(size=len(raw_dist), sha256=release.sha(raw_dist))
        self.sign(value=manifest)

    def test_malicious_archive_entries_never_escape(self):
        outside = self.root / 'outside'
        outside.write_bytes(b'keep')
        for kind in ('traversal', 'absolute', 'duplicate', 'hardlink', 'symlink', 'fifo', 'setuid', 'wronghash', 'missing'):
            with self.subTest(kind=kind):
                for path in self.feed.iterdir():
                    path.unlink()
                for path in self.published[0].iterdir():
                    shutil.copyfile(path, self.feed / path.name)
                def transform(items):
                    info, data = next((i, d) for i, d in items if i.name == release.RUNTIME + 'icudtl.dat')
                    bad = copy.copy(info)
                    if kind == 'missing':
                        return [(i, d) for i, d in items if i.name != info.name]
                    if kind == 'traversal':
                        bad.name = '../outside'
                    elif kind == 'absolute':
                        bad.name = str(outside)
                    elif kind == 'hardlink':
                        bad.type, bad.linkname, bad.size, data = tarfile.LNKTYPE, str(outside), 0, None
                    elif kind == 'symlink':
                        bad.type, bad.linkname, bad.size, data = tarfile.SYMTYPE, str(self.root), 0, None
                    elif kind == 'fifo':
                        bad.type, bad.size, data = tarfile.FIFOTYPE, 0, None
                    elif kind == 'setuid':
                        bad.mode = 0o4755
                    elif kind == 'wronghash':
                        data = b'X' * len(data)
                    if kind == 'duplicate':
                        return items + [(bad, data)]
                    return [(bad, data) if i.name == info.name else (i, d) for i, d in items]
                self.rewrite_archive(transform)
                cfg = dict(self.cfg, install_root=str(self.root / f'bad-{kind}'))
                client.operate(cfg, 'init')
                with self.assertRaises((ValueError, tarfile.TarError)):
                    client.operate(cfg, 'stage')
                self.assertIsNone(release.parse((Path(cfg['install_root']) / 'state.json').read_bytes())['active'])
                self.assertEqual(list((Path(cfg['install_root']) / 'releases').iterdir()), [])
                self.assertEqual(outside.read_bytes(), b'keep')

    def test_inventory_prefix_collision_and_unexpected_symlink(self):
        def alter(dist):
            dist['files']['Noventra.AppDir/opt'] = {'symlink': '/tmp'}
        self.rewrite_archive(lambda items: items, alter)
        self.assert_rejected()

    def test_consistent_outer_hashes_cannot_hide_wrong_package_identity(self):
        changed = {}
        def transform(items):
            result = []
            for info, data in items:
                if info.name == release.APP + 'noventra-package.json':
                    metadata = release.parse(data)
                    metadata['architecture'] = 'aarch64'
                    data = release.encode(metadata)
                    info.size = len(data)
                    changed[info.name] = {'size': len(data), 'sha256': release.sha(data), 'mode': '0o644'}
                result.append((info, data))
            return result
        self.rewrite_archive(transform, lambda dist: dist['files'].update(changed))
        with self.assertRaisesRegex(ValueError, 'Packaged identity mismatch'):
            client.operate(self.cfg, 'stage')

    def test_pax_linkpath_sparse_and_huge_metadata(self):
        for kind in ('linkpath', 'sparse', 'huge'):
            with self.subTest(kind=kind):
                for path in self.published[0].iterdir():
                    shutil.copyfile(path, self.feed / path.name)
                def transform(items):
                    i, data = next((i, d) for i, d in items if i.name == release.RUNTIME + 'icudtl.dat')
                    i.pax_headers = ({'linkpath': '/tmp/elsewhere'} if kind == 'linkpath' else
                                     {'GNU.sparse.map': '0,1', 'GNU.sparse.size': '1'} if kind == 'sparse' else
                                     {'comment': 'x' * 70000})
                    return items
                self.rewrite_archive(transform)
                cfg = dict(self.cfg, install_root=str(self.root / f'pax-{kind}'))
                client.operate(cfg, 'init')
                with self.assertRaises((ValueError, tarfile.TarError)):
                    client.operate(cfg, 'stage')

    def test_installed_tampering_blocks_activation_and_rollback(self):
        result = client.operate(self.cfg, 'stage')
        binary = Path(self.cfg['install_root']) / 'releases' / result['release'] / release.RUNTIME / 'noventra-browser'
        original = binary.read_bytes()
        binary.write_bytes(b'changed')
        self.assert_rejected('activate')
        binary.write_bytes(original)
        one = client.operate(self.cfg, 'activate')
        self.activate(2)
        binary.write_bytes(b'changed again')
        self.assert_rejected('rollback')
        self.assertEqual(self.state()['previous'], one['active'])

    def test_state_update_failure_preserves_selection_and_retains_release(self):
        first = self.activate(1)
        self.version(2)
        staged = client.operate(self.cfg, 'stage')
        with patch.object(client.os, 'replace', side_effect=OSError('injected state write failure')):
            self.assert_rejected('activate')
        self.assertEqual(self.state()['active'], first['active'])
        self.assertTrue((Path(self.cfg['install_root']) / 'releases' / staged['release']).exists())
        self.assertEqual(client.operate(self.cfg, 'activate')['active'], staged['release'])

    def test_recover_complete_orphan_after_stage_state_failure(self):
        original = client.atomic_state
        calls = 0
        def fail_second(root, state):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('injected state failure after rename')
            original(root, state)
        with patch.object(client, 'atomic_state', side_effect=fail_second):
            self.assert_rejected()
        self.assertEqual(len(list((Path(self.cfg['install_root']) / 'releases').iterdir())), 1)
        self.assertEqual(client.operate(self.cfg, 'stage')['status'], 'staged')

    def test_orphan_retry_flushes_release_directory_before_success_state(self):
        releases = Path(self.cfg['install_root']) / 'releases'
        original_sync = client.sync_directory
        original_state = client.atomic_state
        events = []
        fail_barrier = True

        def flush(path):
            if Path(path) == releases:
                events.append('release-flush-attempt')
                if fail_barrier:
                    raise OSError('injected release-directory fsync failure')
                original_sync(path)
                events.append('release-flush-complete')
            else:
                original_sync(path)

        def save(root, state):
            if state['staged'] is not None:
                self.assertIn('release-flush-complete', events,
                              'Success state must follow the release-directory durability barrier')
                events.append('success-state')
            original_state(root, state)

        with patch.object(client, 'sync_directory', side_effect=flush), \
                patch.object(client, 'atomic_state', side_effect=save):
            # The first rename succeeds, but its directory flush fails.
            with self.assertRaisesRegex(OSError, 'release-directory fsync failure'):
                client.operate(self.cfg, 'stage')
            orphan, = releases.iterdir()
            self.assertEqual(events, ['release-flush-attempt'])
            self.assertIsNone(self.state()['staged'])
            self.assertEqual(self.state()['highest_sequence'], 1)
            client.verify_installed(releases.parent, orphan.name, self.cfg)

            # A retry must attempt the barrier again, and still fail closed if
            # it fails again. Reusing the visible directory is not durability.
            events.clear()
            with self.assertRaisesRegex(OSError, 'release-directory fsync failure'):
                client.operate(self.cfg, 'stage')
            self.assertEqual(events, ['release-flush-attempt'])
            self.assertIsNone(self.state()['staged'])

            events.clear()
            fail_barrier = False
            self.assertEqual(client.operate(self.cfg, 'stage')['release'], orphan.name)
            self.assertEqual(events, ['release-flush-attempt', 'release-flush-complete', 'success-state'])
            self.assertEqual(self.state()['staged'], orphan.name)
            self.assertEqual(list(releases.iterdir()), [orphan])

    def test_flush_failure_after_atomic_selection_has_one_complete_state(self):
        first = self.activate(1)
        self.version(2)
        staged = client.operate(self.cfg, 'stage')
        with patch.object(client, 'sync_directory', side_effect=OSError('injected post-rename flush failure')):
            with self.assertRaises(OSError):
                client.operate(self.cfg, 'activate')
        # Rename already happened: report failure, but never leave a torn state or
        # a mismatching second "current" pointer. Caller must inspect status.
        status = client.operate(self.cfg, 'status')
        self.assertEqual(status['active'], staged['release'])
        self.assertEqual(status['previous'], first['active'])
        self.assertIsNone(status['staged'])
        self.assertEqual(status['highest_sequence'], 2)
        self.assertTrue(Path(status['active_path']).exists())

    def test_explicit_root_binding_no_reinitialize_no_symlink(self):
        with self.assertRaises(ValueError):
            client.operate(self.cfg, 'init')
        changed = dict(self.cfg, channel='different')
        with self.assertRaises(ValueError):
            client.operate(changed, 'status')
        link = self.root / 'linked-install'
        link.symlink_to(self.cfg['install_root'])
        with self.assertRaises(ValueError):
            client.operate(dict(self.cfg, install_root=str(link)), 'status')
        lock = Path(self.cfg['install_root']) / '.lock'
        lock.unlink()
        target = self.root / 'untouched'
        target.write_bytes(b'preserve')
        lock.symlink_to(target)
        with self.assertRaises(OSError):
            client.operate(self.cfg, 'status')
        self.assertEqual(target.read_bytes(), b'preserve')

    def test_lock_excludes_concurrent_mutation(self):
        with client.locked(self.cfg['install_root']):
            with self.assertRaises(BlockingIOError):
                client.operate(self.cfg, 'stage')

    def test_server_exposes_only_release_files_and_no_writes(self):
        (self.feed / 'secret.pem').write_bytes(b'test sentinel')
        for name in ('secret.pem', '../secret.pem', '%2e%2e/secret.pem', '?listing', 'manifest.json?x=1'):
            with self.subTest(name=name):
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    urllib.request.urlopen(self.cfg['base_url'] + name)
                caught.exception.close()
        request = urllib.request.Request(self.cfg['base_url'] + 'manifest.json', method='POST', data=b'replace')
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request)
        caught.exception.close()
        with self.assertRaises(ValueError):
            server.make_server(self.feed, '0.0.0.0')

    def test_redirect_origin_path_and_https_downgrade_rules(self):
        handler = client.ConstrainedRedirect('https://release.invalid/owned/')
        request = urllib.request.Request('https://release.invalid/owned/manifest.json')
        for target in ('http://release.invalid/owned/x', 'https://else.invalid/owned/x',
                       'https://release.invalid:444/owned/x', 'https://release.invalid/other/x',
                       'https://release.invalid/owned/%2e%2e/x'):
            with self.subTest(target=target), self.assertRaises(ValueError):
                handler.redirect_request(request, None, 302, '', {}, target)
        self.assertIsNotNone(handler.redirect_request(request, None, 302, '', {}, 'https://release.invalid/owned/next.json'))
        for url in ('http://example.invalid/', 'http://localhost/', 'https://u:p@example.invalid/',
                    'file:///tmp/', 'https://example.invalid/?x=1'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                client.origin(url)

    def test_descriptive_user_agent_reaches_server_before_and_after_redirect(self):
        requests = []

        class AgentAware(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                requests.append((self.path, self.headers.get('User-Agent'),
                                 self.headers.get('Accept-Encoding')))
                if self.path.startswith('/owned/start-'):
                    code = int(self.path.rsplit('-', 1)[1])
                    self.send_response(code)
                    self.send_header('Location', f'target-{code}')
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                else:
                    self.send_response(200)
                    self.send_header('Content-Length', '2')
                    self.end_headers()
                    self.wfile.write(b'ok')

        http = ThreadingHTTPServer(('127.0.0.1', 0), AgentAware)
        thread = threading.Thread(target=http.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.stop_http, http, thread)
        cfg = dict(self.cfg, base_url=f'http://127.0.0.1:{http.server_port}/owned/')
        for code in (301, 302, 303, 307, 308):
            with self.subTest(code=code):
                requests.clear()
                destination = self.root / f'agent-{code}'
                client.download(cfg, f'start-{code}', destination, 2,
                                expected_size=2, expected_hash=release.sha(b'ok'))
                self.assertEqual(destination.read_bytes(), b'ok')
                self.assertEqual(requests, [
                    (f'/owned/start-{code}', 'NoventraUpdater/1 (+https://noventrastudio.com)', 'identity'),
                    (f'/owned/target-{code}', 'NoventraUpdater/1 (+https://noventrastudio.com)', 'identity'),
                ])

    def test_real_http_redirect_bounded_stream_and_encoding(self):
        class Hostile(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                if self.path == '/redirect':
                    self.send_response(302)
                    self.send_header('Location', 'http://127.0.0.1:1/stolen')
                    self.end_headers()
                    return
                self.send_response(200)
                if self.path == '/encoding':
                    self.send_header('Content-Encoding', 'gzip')
                if self.path == '/truncated':
                    self.send_header('Content-Length', '4096')
                if self.path == '/ambiguous':
                    self.send_header('Content-Length', '2048')
                    self.send_header('Content-Length', '2048')
                self.end_headers()
                self.wfile.write(b'x' * 2048)
        http = ThreadingHTTPServer(('127.0.0.1', 0), Hostile)
        thread = threading.Thread(target=http.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.stop_http, http, thread)
        cfg = dict(self.cfg, base_url=f'http://127.0.0.1:{http.server_port}/')
        for name, limit in [('redirect', 4096), ('encoding', 4096), ('bounded', 1024),
                            ('truncated', 4096), ('ambiguous', 4096)]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                client.download(cfg, name, self.root / f'http-{name}', limit)

    def test_real_http_redirect_bodies_are_closed_without_reading(self):
        for code in (301, 302, 303, 307, 308):
            for kind in ('oversized', 'streaming'):
                with self.subTest(code=code, kind=kind):
                    stopped = threading.Event()
                    disconnected = threading.Event()
                    body_reads = []

                    class Hostile(BaseHTTPRequestHandler):
                        def log_message(self, *args):
                            pass

                        def do_GET(self):
                            if self.path == '/owned/target':
                                self.send_response(200)
                                self.send_header('Content-Length', '2')
                                self.end_headers()
                                self.wfile.write(b'ok')
                                return
                            self.send_response(code)
                            self.send_header('Location', 'target')
                            if kind == 'oversized':
                                self.send_header('Content-Length', str(1024**4))
                            self.end_headers()
                            # An oversized declared body or a stream with no
                            # end/length. It only ends on disconnect/teardown.
                            self.connection.settimeout(0.5)
                            try:
                                while not stopped.wait(0.001):
                                    self.wfile.write(b'x' * 8192)
                            except OSError:
                                disconnected.set()

                    http = ThreadingHTTPServer(('127.0.0.1', 0), Hostile)
                    thread = threading.Thread(target=http.serve_forever, daemon=True)
                    thread.start()
                    cfg = dict(self.cfg, base_url=f'http://127.0.0.1:{http.server_port}/owned/')
                    destination = self.root / f'redirect-{code}-{kind}'
                    original_read = HTTPResponse.read
                    original_read1 = HTTPResponse.read1

                    def guarded(original):
                        def read(response, *args, **kwargs):
                            if response.status in (301, 302, 303, 307, 308):
                                body_reads.append((response.status, args))
                                response.close()
                                # Fail safely on the original vulnerability:
                                # never let a regression exhaust test memory.
                                raise AssertionError('Reading the real HTTP redirect body')
                            return original(response, *args, **kwargs)
                        return read

                    try:
                        with patch.object(HTTPResponse, 'read', guarded(original_read)), \
                                patch.object(HTTPResponse, 'read1', guarded(original_read1)):
                            client.download(cfg, 'start', destination, 2, expected_size=2,
                                            expected_hash=release.sha(b'ok'))
                        self.assertEqual(destination.read_bytes(), b'ok')
                        self.assertEqual(body_reads, [])
                        self.assertTrue(disconnected.wait(2), 'Redirect connection was not closed')
                    finally:
                        stopped.set()
                        self.stop_http(http, thread)

    def test_real_http_redirect_chain_shares_download_deadline(self):
        requests = []

        class SlowRedirect(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                hop = int(self.path.rsplit('-', 1)[1])
                requests.append(hop)
                time.sleep(0.08)
                try:
                    self.send_response(302 if hop < 8 else 200)
                    if hop < 8:
                        self.send_header('Location', f'hop-{hop + 1}')
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                except OSError:
                    pass

        http = ThreadingHTTPServer(('127.0.0.1', 0), SlowRedirect)
        thread = threading.Thread(target=http.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.stop_http, http, thread)
        cfg = dict(self.cfg, base_url=f'http://127.0.0.1:{http.server_port}/owned/')
        started = time.monotonic()
        # A shorter test budget, with real socket delays. Each hop fits the
        # budget separately; the chain must not receive a fresh budget per hop.
        with patch.object(client, 'DOWNLOAD_TIMEOUT', 0.35, create=True):
            with self.assertRaises((ValueError, OSError, urllib.error.URLError)):
                client.download(cfg, 'hop-0', self.root / 'redirect-timeout', 2)
        self.assertLess(time.monotonic() - started, 1.5)
        self.assertGreaterEqual(len(requests), 2)
        self.assertNotIn(8, requests)
        self.assertFalse((self.root / 'redirect-timeout').exists())

    def test_uninitialized_nonempty_root_is_never_modified(self):
        other = self.root / 'unrelated'
        other.mkdir(mode=0o700)
        (other / 'sentinel').write_bytes(b'keep')
        cfg = dict(self.cfg, install_root=str(other))
        for operation in ('init', 'status', 'stage'):
            with self.subTest(operation=operation), self.assertRaises((ValueError, OSError)):
                client.operate(cfg, operation)
            self.assertEqual(set(p.name for p in other.iterdir()), {'sentinel'})

    def test_xz_decompression_bound_and_concatenation(self):
        good_manifest = self.manifest()
        original = (self.feed / good_manifest['artifact']['filename']).read_bytes()
        raw = lzma.decompress(original)
        for kind in ('expansion', 'concatenated', 'truncated'):
            with self.subTest(kind=kind):
                for path in self.published[0].iterdir():
                    shutil.copyfile(path, self.feed / path.name)
                manifest = self.manifest()
                data = (lzma.compress(raw + b'\0' * (4 * 1024**2)) if kind == 'expansion' else
                        original + lzma.compress(b'extra') if kind == 'concatenated' else original[:-12])
                archive = self.feed / manifest['artifact']['filename']
                archive.write_bytes(data)
                dist = release.parse((self.feed / 'distribution.json').read_bytes())
                manifest['artifact'].update(size=len(data), sha256=release.sha(data))
                dist['files'][archive.name] = {'size': len(data), 'sha256': release.sha(data), 'mode': '0o644'}
                raw_dist = release.encode(dist)
                (self.feed / 'distribution.json').write_bytes(raw_dist)
                manifest['distribution'].update(size=len(raw_dist), sha256=release.sha(raw_dist))
                self.sign(value=manifest)
                cfg = dict(self.cfg, install_root=str(self.root / kind), max_unpacked_bytes=len(raw) + 10240)
                client.operate(cfg, 'init')
                with self.assertRaises((ValueError, lzma.LZMAError)):
                    client.operate(cfg, 'stage')

    def test_deleted_extra_linked_and_replaced_payload_is_rejected(self):
        staged = client.operate(self.cfg, 'stage')
        directory = Path(self.cfg['install_root']) / 'releases' / staged['release']
        victim = directory / release.RUNTIME / 'icudtl.dat'
        original = victim.read_bytes()
        victim.unlink()
        self.assert_rejected('activate')
        victim.write_bytes(original)
        extra = directory / 'Noventra.AppDir' / 'unexpected'
        extra.write_bytes(b'not in inventory')
        self.assert_rejected('activate')
        extra.unlink()
        outside = self.root / 'external-hardlink'
        os.link(victim, outside)
        self.assert_rejected('activate')
        outside.unlink()
        victim.unlink()
        outside.write_bytes(original)
        victim.symlink_to(outside)
        self.assert_rejected('activate')

    def test_expired_installed_release_is_available_for_explicit_rollback_only(self):
        self.activate(1)
        self.activate(2)
        future = int(time.time()) + release.MAX_LIFETIME
        with patch.object(release.time, 'time', return_value=future):
            self.assert_rejected('stage')
            result = client.operate(self.cfg, 'rollback')
            self.assertEqual(result['highest_sequence'], 2)
            self.assertTrue(result['active'].startswith('r-1-'))

    def test_failed_update_keeps_running_selection_and_no_pending_release(self):
        first = self.activate(1)
        for path in self.published[1].iterdir():
            shutil.copyfile(path, self.feed / path.name)
        self.http.release_root = self.feed
        artifact = self.feed / self.manifest()['artifact']['filename']
        artifact.write_bytes(b'bad update')
        self.assert_rejected()
        self.assertEqual(self.state()['highest_sequence'], 2)
        self.assertEqual(self.state()['active'], first['active'])
        self.assertIsNone(self.state()['staged'])


if __name__ == '__main__':
    unittest.main()
