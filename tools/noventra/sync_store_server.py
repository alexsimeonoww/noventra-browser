#!/usr/bin/env python3
"""Serve an authenticated opaque object store for Noventra workspace sync.

The browser encrypts and authenticates all object bodies. This server knows only
vault IDs, opaque object IDs, ciphertext, and compare-and-swap revisions. It does
not receive a recovery key or interpret the inherited vault wire format.
"""

import argparse
import base64
import binascii
from contextlib import closing
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import os
from pathlib import Path
import re
import socket
import sqlite3
import ssl
import stat
import threading
import uuid


MAX_OBJECT_BYTES = 16 * 1024 * 1024
MAX_OBJECT_ID_BYTES = 4096
DEFAULT_QUOTA_BYTES = 1024 * 1024 * 1024
MAX_OBJECTS = 100_000
MAX_VAULTS = 1000
ETAG = re.compile(r'"[0-9a-f]{32}"\Z')
OBJECT_PATH = re.compile(r'/v1/vaults/([0-9a-f-]{36})/objects/([A-Za-z0-9_-]+)\Z')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_token(token):
    require(isinstance(token, str) and 32 <= len(token) <= 1024
            and all(33 <= ord(char) <= 126 for char in token),
            'Use a random token of 32 to 1024 printable ASCII characters without spaces')
    return token.encode('ascii')


def read_token(path):
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
    with os.fdopen(os.open(path, flags), 'rb') as source:
        info = os.fstat(source.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid(),
                'The token must be an owned regular file')
        require(info.st_mode & 0o077 == 0, 'The token file must not be accessible to other users')
        raw = source.read(1026)
    require(len(raw) <= 1025, 'Token file is too large')
    return validate_token(raw.rstrip(b'\n').decode('ascii'))


def object_path(path):
    require(len(path) <= 5600, 'Object path is too long')
    match = OBJECT_PATH.fullmatch(path)
    require(match is not None, 'Unsupported object path')
    vault, encoded = match.groups()
    require(str(uuid.UUID(vault)) == vault and uuid.UUID(vault).int != 0,
            'Invalid vault ID')
    try:
        raw = base64.urlsafe_b64decode(encoded + '=' * (-len(encoded) % 4))
        value = raw.decode('utf-8')
    except (binascii.Error, UnicodeDecodeError, ValueError) as error:
        raise ValueError('Invalid object ID') from error
    require(0 < len(raw) <= MAX_OBJECT_ID_BYTES
            and base64.urlsafe_b64encode(raw).rstrip(b'=').decode('ascii') == encoded,
            'Invalid object ID')
    return vault, value


class Store:
    """One SQLite transaction covers each condition check and object write."""

    def __init__(self, directory, quota_bytes=DEFAULT_QUOTA_BYTES):
        self.directory = Path(directory).absolute()
        require(quota_bytes >= MAX_OBJECT_BYTES, 'Quota must allow at least one maximum-size object')
        self.quota_bytes = quota_bytes
        if not self.directory.exists():
            self.directory.mkdir(mode=0o700, parents=True)
        info = self.directory.lstat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
                and info.st_mode & 0o077 == 0,
                'Store directory must be owned, private (0700), and not a symlink')
        self.database = self.directory / 'objects.sqlite3'
        fd = os.open(self.database, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            info = os.fstat(fd)
            require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                    and info.st_mode & 0o077 == 0, 'Database must be an owned private regular file')
        finally:
            os.close(fd)
        with closing(self.connect()) as db:
            db.execute('CREATE TABLE IF NOT EXISTS objects ('
                       'vault TEXT NOT NULL, object_id TEXT NOT NULL, '
                       'revision TEXT NOT NULL, data BLOB NOT NULL, '
                       'PRIMARY KEY(vault, object_id))')

    def connect(self):
        db = sqlite3.connect(self.database, timeout=5, isolation_level=None)
        db.execute('PRAGMA synchronous=FULL')
        db.execute('PRAGMA busy_timeout=5000')
        return db

    def read(self, vault, object_id):
        with closing(self.connect()) as db:
            return db.execute('SELECT revision, data FROM objects WHERE vault=? AND object_id=?',
                              (vault, object_id)).fetchone()

    def write(self, vault, object_id, expected, data):
        require(0 < len(data) <= MAX_OBJECT_BYTES, 'Invalid ciphertext size')
        with closing(self.connect()) as db:
            try:
                db.execute('BEGIN IMMEDIATE')
                old = db.execute('SELECT revision,length(data) FROM objects '
                                 'WHERE vault=? AND object_id=?', (vault, object_id)).fetchone()
                if (old is not None if expected is None else old is None or old[0] != expected):
                    db.rollback()
                    return 412, None
                count, total = db.execute('SELECT count(*),coalesce(sum(length(data)),0) FROM objects').fetchone()
                if total - (old[1] if old else 0) + len(data) > self.quota_bytes:
                    db.rollback()
                    return 413, None
                if old is None and count >= MAX_OBJECTS:
                    db.rollback()
                    return 413, None
                if old is None and not db.execute('SELECT 1 FROM objects WHERE vault=? LIMIT 1', (vault,)).fetchone():
                    vaults = db.execute('SELECT count(DISTINCT vault) FROM objects').fetchone()[0]
                    if vaults >= MAX_VAULTS:
                        db.rollback()
                        return 413, None
                revision = '"' + uuid.uuid4().hex + '"'
                db.execute('INSERT INTO objects(vault,object_id,revision,data) VALUES(?,?,?,?) '
                           'ON CONFLICT(vault,object_id) DO UPDATE SET revision=excluded.revision,data=excluded.data',
                           (vault, object_id, revision, data))
                db.commit()
                return (200 if old else 201), revision
            except BaseException:
                db.rollback()
                raise


class SyncHandler(BaseHTTPRequestHandler):
    # Closing each request makes rejected/malformed bodies unambiguous. Browser
    # clients keep their own bounded operation deadlines; the server bounds reads.
    protocol_version = 'HTTP/1.1'

    def setup(self):
        self.request.settimeout(30)
        if isinstance(self.request, ssl.SSLSocket):
            # Handshake inside the bounded request worker, never in accept().
            self.request.do_handshake()
        super().setup()

    def log_message(self, *args):
        pass  # Never log authorization, object paths, or payloads.

    def respond(self, status, data=b'', revision=None):
        self.close_connection = True
        self.send_response(status)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Connection', 'close')
        if revision is not None:
            self.send_header('ETag', revision)
        self.end_headers()
        if data:
            self.wfile.write(data)

    def authenticated_path(self):
        supplied = self.headers.get_all('Authorization', [])
        expected = b'Bearer ' + self.server.token
        if len(supplied) != 1 or not hmac.compare_digest(supplied[0].encode('latin-1'), expected):
            self.respond(401)
            return None
        try:
            return object_path(self.path)
        except ValueError:
            self.respond(400)
            return None

    def do_GET(self):
        address = self.authenticated_path()
        if address is None:
            return
        try:
            value = self.server.store.read(*address)
            if value is None:
                self.respond(404)
            elif not ETAG.fullmatch(value[0]) or len(value[1]) > MAX_OBJECT_BYTES:
                self.respond(500)
            else:
                self.respond(200, value[1], value[0])
        except sqlite3.Error:
            self.respond(503)

    def do_PUT(self):
        address = self.authenticated_path()
        if address is None:
            return
        absent = self.headers.get_all('If-None-Match', [])
        match = self.headers.get_all('If-Match', [])
        if absent == ['*'] and not match:
            expected = None
        elif not absent and len(match) == 1 and ETAG.fullmatch(match[0]):
            expected = match[0]
        else:
            self.respond(400)
            return
        lengths = self.headers.get_all('Content-Length', [])
        if (len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit()
                or self.headers.get_all('Transfer-Encoding')
                or self.headers.get_all('Content-Type') != ['application/octet-stream']):
            self.respond(400)
            return
        length = int(lengths[0])
        if not 0 < length <= MAX_OBJECT_BYTES:
            self.respond(413)
            return
        try:
            data = self.rfile.read(length)
            if len(data) != length:
                self.respond(400)
                return
            status, revision = self.server.store.write(*address, expected, data)
            self.respond(status, revision=revision)
        except sqlite3.Error:
            self.respond(503)
        except (TimeoutError, ConnectionError):
            self.close_connection = True

    def do_OPTIONS(self):
        self.respond(405)  # No browser-page CORS or unauthenticated preflight API.


class SyncServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, *args, **kwargs):
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(*args, **kwargs)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):
        pass  # No request details, paths or credentials in uncaught socket logs.


def make_server(directory, token, host='127.0.0.1', port=0,
                quota_bytes=DEFAULT_QUOTA_BYTES, tls_cert=None, tls_key=None):
    token = validate_token(token) if isinstance(token, str) else validate_token(token.decode('ascii'))
    address = '127.0.0.1' if host == 'localhost' else host
    ip = ipaddress.ip_address(address)
    require(bool(tls_cert) == bool(tls_key), 'Provide both TLS certificate and private key')
    require(ip.is_loopback or tls_cert, 'Non-loopback binding requires TLS')
    store = Store(directory, quota_bytes)
    server_class = SyncServer
    if ip.version == 6:
        class IPv6SyncServer(SyncServer):
            address_family = socket.AF_INET6
        server_class = IPv6SyncServer
    server = server_class((address, port), SyncHandler)
    try:
        server.store = store
        server.token = token
        if tls_cert:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(tls_cert, tls_key)
            server.socket = context.wrap_socket(server.socket, server_side=True,
                                                do_handshake_on_connect=False)
        return server
    except BaseException:
        server.server_close()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--store', type=Path, required=True)
    parser.add_argument('--token-file', type=Path, required=True)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--quota-bytes', type=int, default=DEFAULT_QUOTA_BYTES)
    parser.add_argument('--tls-cert', type=Path)
    parser.add_argument('--tls-key', type=Path)
    args = parser.parse_args()
    try:
        token = read_token(args.token_file)
        with make_server(args.store, token, args.host, args.port, args.quota_bytes,
                         args.tls_cert, args.tls_key) as server:
            address, port = server.server_address[:2]
            authority = f'[{address}]:{port}' if ':' in address else f'{address}:{port}'
            print(f'Workspace sync origin: {"https" if args.tls_cert else "http"}://{authority}', flush=True)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
    except (OSError, UnicodeError, ValueError, sqlite3.Error) as error:
        parser.exit(1, f'Workspace sync server: {error}\n')


if __name__ == '__main__':
    main()
