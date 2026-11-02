"""Public release preparation/ordering tests; fake payloads, no live account."""

import base64
import copy
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import lzma
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
sys.path.insert(0, str(ROOT / 'tests/noventra'))
import publish_release as publisher
import release_manifest as release
import test_package_distribution as package_fixture
import public_release_fixture as public_fixture


class Store:
    def __init__(self):
        self.objects = {}
        self.writes = []
        self.catalog_reads = 0
        self.change_before_commit = False

    def read(self, key, limit):
        if key == publisher.CATALOG_KEY:
            self.catalog_reads += 1
            if self.change_before_commit and self.catalog_reads == 2:
                self.objects[key] = b'concurrent publication'
        value = self.objects.get(key)
        if value is not None and len(value) > limit:
            raise ValueError('Remote response exceeds limit')
        return value

    def verify(self, key, record):
        if key not in self.objects:
            return False
        value = self.objects[key]
        if len(value) != record['size'] or release.sha(value) != record['sha256']:
            raise ValueError('Existing remote object has different bytes')
        return True

    def put(self, key, path, record):
        value = path.read_bytes()
        if len(value) != record['size'] or release.sha(value) != record['sha256']:
            raise ValueError('Changed input')
        if key != publisher.CATALOG_KEY and key in self.objects:
            raise ValueError('Immutable object exists')
        self.objects[key] = value
        self.writes.append(key)


class PublicReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='noventra-public-release-tests-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.key = cls.base / 'test-only-private.pem'
        release.openssl('genpkey', '-algorithm', 'ED25519', '-out', cls.key)
        cls.key.chmod(0o600)
        cls.public = cls.base / 'test-only-public.pem'
        cls.public.write_bytes(release.openssl('pkey', '-in', cls.key, '-pubout'))
        fixture = package_fixture.PackageTests('test_hash_mismatch')
        fixture.setUp()
        cls.addClassCleanup(fixture.doCleanups)
        cls.releases = []
        cls.distributions = []
        cls.sources = []
        cls.source_infos = []
        for version in (1, 2):
            fixture.put('icudtl.dat', f'fake public version {version}\n'.encode())
            fixture.refresh()
            dist = cls.base / f'distribution-{version}'
            config = public_fixture.update_configuration(cls.base / f'update-{version}.json', cls.public, version)
            package_fixture.package.package(fixture.source, dist, f'{version}-public-test', archive=True,
                                            update_config=config)
            source = cls.base / f'noventra-{version}-source.zip'
            with zipfile.ZipFile(source, 'x') as archive:
                archive.writestr('SOURCE.txt', 'Disposable test fixture, not browser source')
            inventory = release.parse((dist / 'distribution.json').read_bytes())
            info = cls.base / f'source-info-{version}.json'
            info.write_bytes(release.encode({'format': 1, 'product': release.PRODUCT,
                'version_label': inventory['version_label'], 'snapshot_sha256': inventory['snapshot_sha256'],
                'source_archive': publisher.file_record(source)}))
            output = cls.base / f'release-{version}'
            publisher.prepare(dist, output, cls.key, source, info, version,
                              max_unpacked_bytes=32 * 1024**2)
            cls.releases.append(output)
            cls.distributions.append(dist)
            cls.sources.append(source)
            cls.source_infos.append(info)

    def setUp(self):
        self.temporary_case = tempfile.TemporaryDirectory(dir=self.base, prefix='case-')
        self.addCleanup(self.temporary_case.cleanup)
        self.case = Path(self.temporary_case.name)
        self.path = self.case / 'release'
        shutil.copytree(self.releases[0], self.path)

    def upload(self, store, expected=0, path=None):
        return publisher.upload(path or self.path, self.public, store,
                                expected_sequence=expected, max_unpacked_bytes=32 * 1024**2)

    def invalid_package(self, name, **changes):
        dist = self.case / (name + '-distribution')
        shutil.copytree(self.distributions[0], dist, symlinks=True)
        public_fixture.rewrite_updater(dist, **changes)
        return dist

    def rejected_public_package(self, name, message, **changes):
        dist = self.invalid_package(name, **changes)
        output = self.case / (name + '-prepare')
        # Exactly the same archive is still supported by the manual/fork
        # protocol. Its authentic signature cannot bypass the public gate.
        legacy = self.case / (name + '-legacy')
        public_fixture.legacy_prepared(dist, legacy, self.key, self.sources[0], self.source_infos[0])
        raw = (legacy / 'manifest.json').read_bytes()
        self.assertEqual(release.verify_signature(raw, (legacy / 'manifest.sig').read_bytes(), self.public),
                         release.parse(raw)['signer_id'])
        with patch.object(release, 'openssl', wraps=release.openssl) as openssl:
            with self.assertRaisesRegex(ValueError, message):
                publisher.prepare(dist, output, self.key, self.sources[0], self.source_infos[0], 1,
                                  max_unpacked_bytes=32 * 1024**2)
            self.assertFalse(any('-sign' in call.args for call in openssl.call_args_list),
                             'public contract must be checked before manifest signing')
        self.assertFalse(output.exists(), 'invalid public package cannot reach signing/output completion')
        with self.assertRaisesRegex(ValueError, message):
            publisher.prepared(legacy, self.public, max_unpacked_bytes=32 * 1024**2)
        store = Store()
        with self.assertRaisesRegex(ValueError, message):
            self.upload(store, path=legacy)
        self.assertEqual(store.writes, [], 'invalid updater cannot upload any remote object')

    def test_public_gate_requires_every_updater_file_and_rejects_unconfigured_legacy_packages(self):
        for i, name in enumerate(sorted(publisher.UPDATER_FILES)):
            with self.subTest(missing=name):
                self.rejected_public_package(f'missing-{i}', 'five updater files', missing=(name,))
        self.rejected_public_package('unconfigured', 'five updater files',
                                     missing=sorted(publisher.UPDATER_FILES))

    def test_public_gate_rejects_wrong_feed_channel_architecture_and_key_path(self):
        variants = ({'base_url': 'https://fork.example/updates/'},
                    {'base_url': public_fixture.FEED.rstrip('/')},
                    {'channel': 'local'}, {'architecture': 'aarch64'}, {'public_key': '../key.pem'},
                    {'format': True}, {'unsupported': 'field'})
        for i, config in enumerate(variants):
            with self.subTest(config=config):
                self.rejected_public_package(f'config-{i}', 'configuration|fields', config_changes=config)

    def test_public_gate_rejects_minimum_sequence_above_manifest_or_non_integer_floor(self):
        for i, sequence in enumerate((2, 0, True, '1')):
            with self.subTest(sequence=sequence):
                self.rejected_public_package(f'sequence-{i}', 'Invalid integer',
                                             config_changes={'sequence': sequence})

    def test_public_gate_rejects_embedded_key_different_from_authentic_manifest_signer(self):
        other = self.case / 'other-test-private.pem'
        release.openssl('genpkey', '-algorithm', 'ED25519', '-out', other)
        other_public = release.openssl('pkey', '-in', other, '-pubout')
        self.rejected_public_package('wrong-key', 'approved manifest signer', key_data=other_public)

    def test_public_gate_rejects_non_ed25519_public_keys(self):
        other = self.case / 'other-test-x25519.pem'
        release.openssl('genpkey', '-algorithm', 'X25519', '-out', other)
        self.rejected_public_package('wrong-algorithm', 'Ed25519 signing key',
                                     key_data=release.openssl('pkey', '-in', other, '-pubout'))

    def test_public_gate_accepts_lower_sequence_floor_and_equivalent_public_pem_encoding(self):
        dist = self.invalid_package('renewal', key_data=b'\n' + self.public.read_bytes().replace(b'\n', b'\r\n'))
        output = self.case / 'renewed-release'
        publisher.prepare(dist, output, self.key, self.sources[0], self.source_infos[0], 2,
                          max_unpacked_bytes=32 * 1024**2)
        catalog, _ = publisher.prepared(output, self.public, max_unpacked_bytes=32 * 1024**2)
        self.assertEqual(catalog['sequence'], 2)

    def test_public_gate_rejects_private_and_combined_pem_even_when_openssl_accepts_public_prefix(self):
        public, private = self.public.read_bytes(), self.key.read_bytes()
        combined = self.case / 'test-only-combined.pem'
        combined.write_bytes(public + private)
        self.assertEqual(release.public_der(combined), release.public_der(self.public),
                         'regression requires OpenSSL to accept this combined bundle')
        for i, data in enumerate((private, public + private, private + public, public + public)):
            with self.subTest(bundle=i):
                self.rejected_public_package(f'bundle-{i}', 'public-only PEM', key_data=data)

    def test_public_configuration_rejects_duplicate_fields(self):
        config = (self.distributions[0] / release.APP / 'updater/config.json').read_bytes()
        duplicate = config.replace(b'"format": 1,', b'"format": 1, "format": 1,')
        self.rejected_public_package('duplicate', 'Duplicate JSON key', config_raw=duplicate)

    def test_public_gate_reuses_the_single_validated_unpack(self):
        for operation in ('prepare', 'prepared'):
            with self.subTest(operation=operation), patch.object(release, 'unpack', wraps=release.unpack) as unpack:
                if operation == 'prepare':
                    publisher.prepare(self.distributions[0], self.case / 'one-unpack', self.key,
                                      self.sources[0], self.source_infos[0], 1, max_unpacked_bytes=32 * 1024**2)
                else:
                    publisher.prepared(self.path, self.public, max_unpacked_bytes=32 * 1024**2)
                self.assertEqual(unpack.call_count, 1)

    def test_package_inspection_checks_available_space_and_avoids_symlink_temporary_parents(self):
        with patch.object(release.shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(100, 100, 0)):
            with self.assertRaisesRegex(ValueError, 'free disk space'):
                publisher.prepared(self.path, self.public, max_unpacked_bytes=32 * 1024**2)
            output = self.case / 'no-space'
            with self.assertRaisesRegex(ValueError, 'free disk space'):
                publisher.prepare(self.distributions[0], output, self.key, self.sources[0], self.source_infos[0], 1,
                                  max_unpacked_bytes=32 * 1024**2)
            self.assertFalse(output.exists())
        link = self.case / 'linked-parent'
        link.symlink_to(self.case, target_is_directory=True)
        output = link / 'unsafe-output'
        with self.assertRaisesRegex(ValueError, 'inspection parent'):
            publisher.prepare(self.distributions[0], output, self.key, self.sources[0], self.source_infos[0], 1,
                              max_unpacked_bytes=32 * 1024**2)
        self.assertFalse(output.exists())

    def test_inspection_bounds_raw_tar_by_available_space_and_keeps_declared_expansion_cap(self):
        manifest = release.parse((self.path / 'manifest.json').read_bytes())
        dist = release.parse((self.path / 'distribution.json').read_bytes())
        payload = release.validate_distribution(dist, manifest, 32 * 1024**2)
        payload_bytes = sum(row.get('size', 0) for row in payload.values())
        minimum_tar = 1024 + sum(512 + ((row.get('size', 0) + 511) // 512) * 512
                                 for row in payload.values())
        raw_size = len(lzma.decompress((self.path / manifest['artifact']['filename']).read_bytes()))
        self.assertGreater(raw_size, minimum_tar, 'fixture needs real PAX headers or tar padding')
        free = payload_bytes + release.INSPECTION_RESERVE + minimum_tar
        with patch.object(release.shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(free, 0, free)):
            with self.assertRaisesRegex(ValueError, 'free disk space'):
                publisher.prepared(self.path, self.public, max_unpacked_bytes=32 * 1024**2)
        with self.assertRaisesRegex(ValueError, 'Expanded archive exceeds limit'):
            publisher.prepared(self.path, self.public, max_unpacked_bytes=minimum_tar)

    def test_real_http_requests_identify_publisher_to_cloudflare(self):
        seen = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                agent = self.headers.get('User-Agent', '')
                seen.append(agent)
                body = b'{}' if agent.startswith('NoventraPublisher/') else b'error code: 1010'
                self.send_response(200 if agent.startswith('NoventraPublisher/') else 403)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(thread.join)
        self.addCleanup(server.shutdown)
        # Observe actual wire headers on owned loopback HTTP. Production keeps
        # its fixed HTTPS origin, certificate checks and redirect restrictions.
        def connection(host, *, timeout, context):
            self.assertIn(host, ('api.cloudflare.com', 'noventra-upload.example'))
            self.assertTrue(context.check_hostname)
            return http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=timeout)

        with patch.object(publisher.http.client, 'HTTPSConnection', side_effect=connection):
            api = publisher.RemoteStore('a' * 32, 'noventra-browser-releases', 'fixture-not-a-live-token')
            worker = publisher.SignedWorkerStore('https://noventra-upload.example', self.key, self.public)
            for store, action in ((api, 'GET'), (worker, 'inspect')):
                with store.request(action, publisher.CATALOG_KEY) as (response, _):
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read(), b'{}')
        self.assertEqual(len(seen), 2)

    def test_prepared_release_has_real_signature_source_binding_and_complete_inventory(self):
        catalog, objects = publisher.prepared(self.path, self.public, max_unpacked_bytes=32 * 1024**2)
        self.assertEqual(catalog['sequence'], 1)
        self.assertEqual(len(objects), 8)
        self.assertTrue(any(f"/{catalog['artifact']['sha256']}/" in r['key'] for r in objects.values()))
        self.assertEqual(set(p.name for p in self.path.iterdir()), set(objects) | {'publication.json', 'channel.json'})
        self.assertNotIn(self.key.name, [p.name for p in self.path.iterdir()])

    def test_public_sequence_boundary_matches_javascript_catalog(self):
        catalog = release.parse((self.path / 'channel.json').read_bytes())
        for sequence, accepted in ((2**53 - 1, True), (2**53, False)):
            value = copy.deepcopy(catalog)
            value['sequence'] = sequence
            value['release_id'] = f"{sequence:020d}-{value['manifest_sha256']}"
            if accepted:
                self.assertEqual(publisher.validate_catalog(value), value)
            else:
                with self.assertRaises(ValueError):
                    publisher.validate_catalog(value)

    def test_upload_verifies_every_immutable_file_then_publishes_channel_last(self):
        store = Store()
        result = self.upload(store)
        self.assertTrue(result['pointer_written_last'])
        self.assertEqual(store.writes[-1], publisher.CATALOG_KEY)
        self.assertEqual(len(store.writes), 9)
        self.assertEqual(release.parse(store.objects[publisher.CATALOG_KEY])['sequence'], 1)
        self.assertEqual(self.upload(store, expected=1)['uploaded'], [])
        self.assertEqual(len(store.writes), 9, 'idempotent publication cannot rewrite immutable objects')

    def test_next_release_keeps_all_old_objects_and_advances_sequence(self):
        store = Store()
        self.upload(store)
        old = dict(store.objects)
        self.upload(store, expected=1, path=self.releases[1])
        for key, value in old.items():
            if key != publisher.CATALOG_KEY:
                self.assertEqual(store.objects[key], value)
        self.assertEqual(release.parse(store.objects[publisher.CATALOG_KEY])['sequence'], 2)
        with self.assertRaisesRegex(ValueError, 'rollback'):
            self.upload(store, expected=2)

    def test_existing_pointer_requires_explicit_sequence_and_first_release_requires_absence(self):
        store = Store()
        self.upload(store)
        before = copy.deepcopy(store.objects)
        with self.assertRaisesRegex(ValueError, 'expected sequence'):
            self.upload(store, expected=0)
        self.assertEqual(store.objects, before)

    def test_immutable_conflict_is_never_overwritten_and_pointer_remains_absent(self):
        store = Store()
        _, objects = publisher.prepared(self.path, self.public, max_unpacked_bytes=32 * 1024**2)
        conflict = objects['manifest.sig']['key']
        store.objects[conflict] = b'changed remote bytes'
        with self.assertRaisesRegex(ValueError, 'different bytes'):
            self.upload(store)
        self.assertEqual(store.objects[conflict], b'changed remote bytes')
        self.assertNotIn(publisher.CATALOG_KEY, store.objects)

    def test_concurrent_pointer_change_refuses_final_commit(self):
        store = Store()
        store.change_before_commit = True
        with self.assertRaisesRegex(ValueError, 'changed during upload'):
            self.upload(store)
        self.assertEqual(store.objects[publisher.CATALOG_KEY], b'concurrent publication')
        self.assertNotIn(publisher.CATALOG_KEY, store.writes)

    def test_signature_inventory_source_tampering_and_unexpected_files_fail_before_upload(self):
        for filename in ('manifest.sig', 'artifact-index.json', 'source-info.json'):
            original = (self.path / filename).read_bytes()
            (self.path / filename).write_bytes(b'bad')
            store = Store()
            with self.assertRaises((ValueError, OSError)):
                self.upload(store)
            self.assertEqual(store.writes, [])
            (self.path / filename).write_bytes(original)
        (self.path / 'private.pem').write_text('not a real key')
        with self.assertRaisesRegex(ValueError, 'Unexpected files'):
            self.upload(Store())

    def test_source_identity_and_unsafe_catalog_rejected(self):
        catalog = release.parse((self.path / 'channel.json').read_bytes())
        for filename in ('../archive.tar.xz', 'https:archive.tar.xz', 'archive.exe'):
            modified = copy.deepcopy(catalog)
            modified['artifact']['filename'] = filename
            with self.assertRaises(ValueError):
                publisher.validate_catalog(modified)
        info = release.parse((self.path / 'source-info.json').read_bytes())
        source = self.path / info['source_archive']['filename']
        info['snapshot_sha256'] = '0' * 64
        broken = self.case / 'bad-source-info.json'
        broken.write_bytes(release.encode(info))
        with self.assertRaisesRegex(ValueError, 'identify this binary'):
            publisher.source_info(broken, source, release.parse((self.path / 'manifest.json').read_bytes()))

    def test_token_files_and_publication_lock_are_private_and_no_follow(self):
        token = self.case / 'test-token'
        token.write_text('disposable_test_token_not_a_credential')
        token.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'private'):
            publisher.token_value(token)
        token.chmod(0o600)
        self.assertEqual(publisher.token_value(token), token.read_text())
        lock = self.case / 'publisher.lock'
        with publisher.publication_lock(lock):
            with self.assertRaises(BlockingIOError):
                with publisher.publication_lock(lock):
                    self.fail('Concurrent lock must fail')
        self.assertEqual(lock.stat().st_mode & 0o777, 0o600)
        link = self.case / 'lock-link'
        link.symlink_to(lock)
        with self.assertRaises(OSError):
            with publisher.publication_lock(link):
                self.fail('Symlink lock must fail')

    def test_python_permit_is_accepted_by_actual_javascript_authorizer(self):
        store = publisher.SignedWorkerStore('https://owned-upload.example.workers.dev', self.key, self.public)
        value, headers = store.permit('inspect', publisher.CATALOG_KEY)
        der = release.public_der(self.public)
        code = """
import {authorize} from './noventra/release-site/upload-worker.mjs';
let input=''; for await (const c of process.stdin) input+=c;
const x=JSON.parse(input);const r=new Request(x.origin+x.value.path,{method:x.value.method,headers:x.headers});
const out=await authorize(r,x.public);process.stdout.write(JSON.stringify(out));
"""
        result = subprocess.run(['node', '--input-type=module', '-e', code], cwd=ROOT,
            input=json.dumps({'origin': store.origin, 'value': value, 'headers': headers,
                              'public': base64.b64encode(der).decode()}),
            text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), value)
        with self.assertRaises(ValueError):
            publisher.SignedWorkerStore('http://attacker.example', self.key, self.public)

    def test_signed_multipart_transport_uses_parts_and_abort_on_failure(self):
        store = publisher.SignedWorkerStore('https://owned-upload.example.workers.dev', self.key, self.public)
        path = self.case / 'large.tar.xz'
        path.write_bytes(b'x' * (publisher.PART_SIZE + 7))
        record = publisher.file_record(path)
        key = f"browser/artifacts/linux-x86_64/{record['sha256']}/{record['filename']}"
        operations = []
        def operation(action, target, data=b'', **extra):
            operations.append((action, len(data), extra))
            if action == 'begin':
                return {'upload_id': 'test-upload-id'}
            if action == 'part':
                return {'partNumber': extra['part_number'], 'etag': release.sha(data)}
            return {}
        with patch.object(store, 'operation', side_effect=operation):
            store.put(key, path, record)
        self.assertEqual([op[0] for op in operations], ['begin', 'part', 'part', 'complete'])
        self.assertEqual([op[1] for op in operations if op[0] == 'part'], [publisher.PART_SIZE, 7])
        operations.clear()
        def fail_part(action, target, data=b'', **extra):
            if action == 'part':
                raise ValueError('part failed')
            return operation(action, target, data, **extra)
        with patch.object(store, 'operation', side_effect=fail_part), patch.object(store, 'verify', return_value=False):
            with self.assertRaisesRegex(ValueError, 'part failed'):
                store.put(key, path, record)
        self.assertEqual([op[0] for op in operations], ['begin', 'abort'])


if __name__ == '__main__':
    unittest.main()
