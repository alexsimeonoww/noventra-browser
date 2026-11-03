"""Real loopback/SQLite checks for the opaque self-hosted workspace sync store."""

import base64
from concurrent.futures import ThreadPoolExecutor
import http.client
from pathlib import Path
import sqlite3
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import uuid

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
import sync_store_server as store


class SyncStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='noventra-sync-store-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.token = 'fixture-only-' + uuid.uuid4().hex
        self.vault = str(uuid.uuid4())
        self.server = store.make_server(self.root / 'store', self.token)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)

    def stop(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=5)
            self.server = None

    def path(self, object_id='opaque/日本語/head', vault=None):
        encoded = base64.urlsafe_b64encode(object_id.encode()).rstrip(b'=').decode()
        return f'/v1/vaults/{vault or self.vault}/objects/{encoded}'

    def request(self, method='GET', path=None, data=None, headers=None, token=None):
        connection = http.client.HTTPConnection(*self.server.server_address[:2], timeout=5)
        fields = {'Authorization': 'Bearer ' + (self.token if token is None else token)}
        if data is not None:
            fields['Content-Type'] = 'application/octet-stream'
        if headers:
            fields.update(headers)
        try:
            connection.request(method, path or self.path(), body=data, headers=fields)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def put(self, data=b'\0ciphertext\xff', revision=None, path=None):
        condition = {'If-None-Match': '*'} if revision is None else {'If-Match': revision}
        return self.request('PUT', path, data, condition)

    def test_authenticated_binary_roundtrip_and_stable_read_revision(self):
        self.assertEqual(self.request()[0], 404)
        self.assertEqual(self.request(token='wrong')[0], 401)
        payload = bytes(range(256)) * 100
        status, headers, body = self.put(payload)
        self.assertEqual((status, body), (201, b''))
        self.assertRegex(headers['ETag'], store.ETAG)
        for _ in range(2):
            status, actual, body = self.request()
            self.assertEqual((status, actual['ETag'], body), (200, headers['ETag'], payload))
            self.assertEqual(actual['Cache-Control'], 'no-store')
            self.assertNotIn('Access-Control-Allow-Origin', actual)

    def test_conditional_writes_never_overwrite_without_exact_revision(self):
        original = self.put(b'first')[1]['ETag']
        self.assertEqual(self.put(b'wrong')[0], 412)
        self.assertEqual(self.put(b'wrong', '"' + '0' * 32 + '"')[0], 412)
        status, headers, _ = self.put(b'second', original)
        self.assertEqual(status, 200)
        self.assertNotEqual(headers['ETag'], original)
        self.assertEqual(self.put(b'stale', original)[0], 412)
        self.assertEqual(self.request()[2], b'second')
        self.assertEqual(self.put(b'missing', original, self.path('missing'))[0], 412)

    def test_concurrent_creation_and_replacement_have_one_winner(self):
        with ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(lambda i: self.put(f'create-{i}'.encode()), range(8)))
        self.assertEqual(sorted(result[0] for result in results), [201] + [412] * 7)
        initial = self.request()[1]['ETag']
        with ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(lambda i: self.put(f'update-{i}'.encode(), initial), range(8)))
        self.assertEqual(sorted(result[0] for result in results), [200] + [412] * 7)
        winner = next(result[1]['ETag'] for result in results if result[0] == 200)
        self.assertEqual(self.request()[1]['ETag'], winner)

    def test_objects_and_vaults_are_separate_and_keys_are_opaque(self):
        paths = [self.path('same'), self.path('same', str(uuid.uuid4())),
                 self.path('../../arbitrary/☃/key'), self.path('helium/v1/unknown-wire-key')]
        for index, path in enumerate(paths):
            self.assertEqual(self.put(str(index).encode(), path=path)[0], 201)
        for index, path in enumerate(paths):
            self.assertEqual(self.request(path=path)[2], str(index).encode())
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['store'])

    def test_restart_preserves_ciphertext_and_cas_revision(self):
        original = self.put(b'restart-ciphertext')[1]['ETag']
        self.stop()
        self.server = store.make_server(self.root / 'store', self.token)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        status, headers, body = self.request()
        self.assertEqual((status, headers['ETag'], body), (200, original, b'restart-ciphertext'))
        self.assertEqual(self.put(b'after-restart', original)[0], 200)

    def test_canonical_paths_and_valid_utf8_are_required(self):
        invalid = [self.path() + '?x=1', self.path() + '#x', self.path() + '=',
                   self.path().replace(self.vault, self.vault.upper()),
                   self.path().replace(self.vault, str(uuid.UUID(int=0))),
                   f'/v1/vaults/{self.vault}/objects/_w',  # non-UTF8
                   f'/v1/vaults/{self.vault}/objects/YR',  # noncanonical padding bits
                   '/v1/vaults/../objects/test', self.path('x' * 4097)]
        for path in invalid:
            with self.subTest(path=path[:120]):
                self.assertEqual(self.request(path=path)[0], 400)
        self.assertEqual(self.request(path=self.path('x' * 4096))[0], 404)

    def test_conditions_are_strict_and_auth_failures_do_not_mutate(self):
        for condition in ({}, {'If-Match': '*'}, {'If-Match': 'W/"' + 'a' * 32 + '"'},
                          {'If-Match': '"' + 'a' * 32 + '","' + 'b' * 32 + '"'},
                          {'If-Match': '"' + 'a' * 32 + '"', 'If-None-Match': '*'}):
            self.assertEqual(self.request('PUT', data=b'x', headers=condition)[0], 400)
        self.assertEqual(self.request('PUT', data=b'x', headers={'If-None-Match': '*'}, token='wrong')[0], 401)
        self.assertEqual(self.request()[0], 404)
        self.assertEqual(self.request('OPTIONS')[0], 405)

    def test_duplicate_auth_length_and_condition_headers_are_rejected(self):
        variants = [ [('Authorization', 'Bearer ' + self.token)],
                     [('Content-Length', '1')], [('If-None-Match', '*')],
                     [('Transfer-Encoding', 'chunked')] ]
        for extra in variants:
            connection = http.client.HTTPConnection(*self.server.server_address[:2], timeout=5)
            try:
                connection.putrequest('PUT', self.path())
                for key, value in [('Authorization', 'Bearer ' + self.token),
                                   ('Content-Length', '1'), ('If-None-Match', '*'),
                                   ('Content-Type', 'application/octet-stream')] + extra:
                    connection.putheader(key, value)
                connection.endheaders(b'x')
                response = connection.getresponse()
                self.assertEqual(response.status, 401 if extra[0][0] == 'Authorization' else 400)
                response.read()
            finally:
                connection.close()
        self.assertEqual(self.request()[0], 404)

    def test_size_and_store_quota_fail_without_changing_existing_object(self):
        original = self.put(b'initial')[1]['ETag']
        with patch.object(store, 'MAX_OBJECT_BYTES', 16):
            self.assertEqual(self.put(b'x' * 17, original)[0], 413)
            self.assertEqual(self.put(b'', original)[0], 413)
        self.server.store.quota_bytes = 10
        self.assertEqual(self.put(b'x' * 11, original)[0], 413)
        self.assertEqual(self.put(b'y' * 4, path=self.path('other'))[0], 413)
        self.assertEqual(self.request()[2], b'initial')
        self.assertEqual(self.put(b'smaller', original)[0], 200)

    def test_store_files_are_private_and_symlinks_refused(self):
        self.assertEqual(self.server.store.directory.stat().st_mode & 0o777, 0o700)
        self.assertEqual(self.server.store.database.stat().st_mode & 0o777, 0o600)
        link = self.root / 'linked'
        link.symlink_to(self.server.store.directory, target_is_directory=True)
        with self.assertRaises(ValueError):
            store.Store(link)
        public = self.root / 'public'
        public.mkdir(mode=0o755)
        with self.assertRaises(ValueError):
            store.Store(public)
        destination = self.root / 'external'
        destination.write_bytes(b'untouched')
        unsafe = self.root / 'unsafe'
        unsafe.mkdir(mode=0o700)
        (unsafe / 'objects.sqlite3').symlink_to(destination)
        with self.assertRaises(OSError):
            store.Store(unsafe)
        self.assertEqual(destination.read_bytes(), b'untouched')

    def test_non_loopback_requires_tls_and_token_file_must_be_private(self):
        with self.assertRaises(ValueError):
            store.make_server(self.root / 'remote', self.token, host='0.0.0.0')
        self.assertFalse((self.root / 'remote').exists())
        token_file = self.root / 'token'
        token_file.write_text(self.token + '\n')
        token_file.chmod(0o600)
        self.assertEqual(store.read_token(token_file), self.token.encode())
        token_file.chmod(0o644)
        with self.assertRaises(ValueError):
            store.read_token(token_file)
        for token in ('short', 'x' * 31 + ' ', 'x' * 1025, 'x' * 32 + '\n'):
            with self.assertRaises(ValueError):
                store.validate_token(token)

    def test_tls_requires_a_trusted_certificate_and_serves_the_same_protocol(self):
        certificate, key = self.root / 'server.crt', self.root / 'server.key'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ed25519', '-nodes',
                        '-keyout', str(key), '-out', str(certificate), '-days', '1',
                        '-subj', '/CN=localhost', '-addext',
                        'subjectAltName=DNS:localhost,IP:127.0.0.1'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        server = store.make_server(self.root / 'tls-store', self.token,
                                   tls_cert=certificate, tls_key=key)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            # A connected peer that never sends a TLS ClientHello must not
            # block the listener from accepting a different valid client.
            stalled = socket.create_connection(server.server_address[:2], timeout=5)
            self.addCleanup(stalled.close)
            untrusted = http.client.HTTPSConnection(*server.server_address[:2], timeout=5)
            try:
                with self.assertRaises(ssl.SSLCertVerificationError):
                    untrusted.request('GET', self.path())
            finally:
                untrusted.close()
            trusted = http.client.HTTPSConnection(*server.server_address[:2], timeout=5,
                       context=ssl.create_default_context(cafile=str(certificate)))
            try:
                trusted.request('GET', self.path(), headers={'Authorization': 'Bearer ' + self.token})
                response = trusted.getresponse()
                self.assertEqual(response.status, 404)
                response.read()
            finally:
                trusted.close()
        finally:
            stalled.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_server_database_contains_no_unrequested_plaintext_metadata(self):
        payload = b'opaque-ciphertext-only'
        self.put(payload)
        with sqlite3.connect(self.server.store.database) as db:
            fields = [entry[1] for entry in db.execute('PRAGMA table_info(objects)')]
            self.assertEqual(fields, ['vault', 'object_id', 'revision', 'data'])
            self.assertEqual(db.execute('SELECT data FROM objects').fetchone()[0], payload)
        self.assertNotIn(self.token.encode(), self.server.store.database.read_bytes())


if __name__ == '__main__':
    unittest.main()
