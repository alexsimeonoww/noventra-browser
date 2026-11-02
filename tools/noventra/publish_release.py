#!/usr/bin/env python3
"""Prepare a signed public Linux release, then explicitly upload it to owned R2.

No build, installation, resource provisioning, browser launch or private-key
generation occurs here. Upload accepts a private token file or environment value,
never a command-line credential. The public Worker has no publication endpoint.
"""

import argparse
import base64
import contextlib
import fcntl
import hashlib
import http.client
import json
import lzma
import os
from pathlib import Path
import re
import ssl
import stat
import subprocess
import tarfile
import tempfile
import time
import urllib.parse

import release_manifest as release

CATALOG_LIMIT = 16 * 1024
SOURCE_INFO_LIMIT = 1024 * 1024
API_LIMIT = 300 * 1024**2
PART_SIZE = 8 * 1024**2
TARGET = 'linux-x86_64'
CHANNEL = 'stable'
PUBLIC_FEED = 'https://downloads.noventrastudio.com/browser/updates/stable/linux-x86_64/'
UPDATER_FILES = {'desktop_updater.py', 'release_manifest.py', 'update_delivery.py',
                 'config.json', 'release-public.pem'}
BUCKET = 'noventra-browser-releases'
ACCOUNT = 'ce800c24f51aa6f29ca58de1504205ef'
HOST = 'api.cloudflare.com'
USER_AGENT = 'NoventraPublisher/1 (+https://noventrastudio.com)'
CATALOG_KEY = f'browser/channels/{CHANNEL}/{TARGET}.json'


def file_hash(path):
    with release.open_regular(path) as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def file_record(path):
    path = release.regular(path)
    return {'filename': release.filename(path.name), 'size': path.stat().st_size,
            'sha256': file_hash(path)}


def source_info(path, source, manifest):
    raw = release.read(path, SOURCE_INFO_LIMIT)
    value = release.parse(raw)
    release.keys(value, ('format', 'product', 'version_label', 'snapshot_sha256', 'source_archive'))
    release.require(value['format'] == 1 and value['product'] == release.PRODUCT
                    and value['version_label'] == manifest['version_label']
                    and value['snapshot_sha256'] == manifest['snapshot_sha256'],
                    'Source information does not identify this binary snapshot')
    release.require(value['source_archive'] == file_record(source), 'Source archive hash/size mismatch')
    release.require(source.name.endswith(('.tar.xz', '.zip')), 'Expected source tar.xz or zip')
    return raw, value


def validate_catalog(value):
    release.keys(value, ('format', 'product', 'channel', 'target', 'sequence', 'release_id',
                         'version_label', 'manifest_sha256', 'artifact', 'source'))
    release.require(value['format'] == 1 and value['product'] == release.PRODUCT
                    and value['channel'] == CHANNEL and value['target'] == TARGET,
                    'Wrong public release catalog identity')
    sequence = release.integer(value['sequence'], 1, 2**53 - 1)
    release.filename(value['version_label'])
    digest = release.digest(value['manifest_sha256'])
    release.require(value['release_id'] == f'{sequence:020d}-{digest}', 'Invalid immutable release ID')
    for field in ('artifact', 'source'):
        record = value[field]
        release.keys(record, ('filename', 'size', 'sha256'))
        release.filename(record['filename'])
        release.integer(record['size'], 1, API_LIMIT)
        release.digest(record['sha256'])
    release.require(value['artifact']['filename'].endswith('.tar.xz')
                    and value['source']['filename'].endswith(('.tar.xz', '.zip')),
                    'Invalid public artifact type')
    return value


def plan_for(manifest, source, raw_manifest):
    catalog = validate_catalog({
        'format': 1, 'product': release.PRODUCT, 'channel': CHANNEL, 'target': TARGET,
        'sequence': manifest['sequence'], 'version_label': manifest['version_label'],
        'manifest_sha256': release.sha(raw_manifest),
        'release_id': f"{manifest['sequence']:020d}-{release.sha(raw_manifest)}",
        'artifact': manifest['artifact'], 'source': source,
    })
    prefix = f"browser/releases/{CHANNEL}/{TARGET}/{catalog['release_id']}"
    plan = {
        'manifest.json': f'{prefix}/manifest.json',
        'manifest.sig': f'{prefix}/manifest.sig',
        'distribution.json': f'{prefix}/distribution.json',
        'source-info.json': f'{prefix}/source-info.json',
        manifest['artifact']['filename']: f"browser/artifacts/{TARGET}/{manifest['artifact']['sha256']}/{manifest['artifact']['filename']}",
        source['filename']: f"browser/sources/{source['sha256']}/{source['filename']}",
        'artifact-index.json': f"browser/artifact-index/{TARGET}/{manifest['artifact']['filename']}.json",
        'source-index.json': f"browser/source-index/{source['filename']}.json",
    }
    release.require(len(plan) == 8, 'Public filenames collide')
    return catalog, plan


def validate_public_package(inspection, manifest, payload, approved_der):
    """Require automatic updates pinned to this public release's signer/feed."""
    prefix = release.APP + 'updater/'
    release.require({prefix + name for name in UPDATER_FILES} <= payload.keys(),
                    'Public release requires all five updater files')
    folder = inspection / prefix
    value = release.parse(release.read(folder / 'config.json', release.MANIFEST_LIMIT))
    release.keys(value, ('format', 'base_url', 'channel', 'architecture', 'sequence', 'public_key'))
    release.require(type(value['format']) is int and value['format'] == 1
                    and value['base_url'] == PUBLIC_FEED and value['channel'] == CHANNEL
                    and value['architecture'] == 'x86_64'
                    and value['public_key'] == 'release-public.pem',
                    'Public updater configuration must use the stable Linux x86_64 feed and pinned key path')
    release.integer(value['sequence'], 1, manifest['sequence'])
    key_data = release.read(folder / 'release-public.pem', 4096)
    release.require(re.fullmatch(rb'\s*-----BEGIN PUBLIC KEY-----\s+'
                                rb'[A-Za-z0-9+/=\r\n]+-----END PUBLIC KEY-----\s*',
                                key_data) is not None,
                    'Public updater requires a public-only PEM key')
    der = release.ed25519(release.public_der(folder / 'release-public.pem'))
    release.require(der == release.ed25519(approved_der)
                    and release.sha(der) == manifest['signer_id'],
                    'Public updater key differs from the approved manifest signer')


def prepare(distribution, output, private_key, source, info, sequence, *, lifetime=30 * 86400,
            max_unpacked_bytes=4 * 1024**3):
    distribution, output, source, info = map(lambda p: Path(p).absolute(), (distribution, output, source, info))
    release.require(not os.path.lexists(output), 'Output already exists')
    # Inspect source identity before the expensive signed archive verification.
    dist = release.parse(release.read(distribution / 'distribution.json', release.DISTRIBUTION_LIMIT))
    release.require(dist.get('platform') == 'linux' and dist.get('architecture') == 'x86_64',
                    'Public release target is Linux x86_64 only')
    source_raw, source_value = source_info(info, source, dist)
    release.integer(source_value['source_archive']['size'], 1, API_LIMIT)
    result = release.publish(distribution, output, private_key, sequence, channel=CHANNEL,
                             lifetime=lifetime, max_artifact_bytes=API_LIMIT,
                             max_unpacked_bytes=max_unpacked_bytes,
                             package_validator=validate_public_package)
    raw_manifest = release.read(output / 'manifest.json', release.MANIFEST_LIMIT)
    manifest = release.validate_manifest(release.parse(raw_manifest))
    release.require(manifest['architecture'] == 'x86_64', 'Public Linux target is x86_64 only')
    catalog, plan = plan_for(manifest, source_value['source_archive'], raw_manifest)
    release.copy_checked(source, output / source.name, source_value['source_archive']['size'],
                         source_value['source_archive']['sha256'])
    (output / 'source-info.json').write_bytes(source_raw)
    (output / 'artifact-index.json').write_bytes(release.encode(manifest['artifact']))
    (output / 'source-index.json').write_bytes(release.encode(source_value['source_archive']))
    (output / 'channel.json').write_bytes(release.encode(catalog))
    records = {name: {'key': key, **file_record(output / name)} for name, key in plan.items()}
    publication = {'format': 1, 'catalog': catalog, 'objects': records,
                   'catalog_sha256': file_hash(output / 'channel.json')}
    # Completion marker is last; a failed preparation is never uploadable.
    (output / 'publication.json').write_bytes(release.encode(publication))
    return {**result, 'status': 'prepared_public_release', 'source_sha256': catalog['source']['sha256'],
            'publication_sha256': file_hash(output / 'publication.json')}


def prepared(path, public_key, *, max_unpacked_bytes=4 * 1024**3):
    path = Path(path).absolute()
    publication = release.parse(release.read(path / 'publication.json', SOURCE_INFO_LIMIT))
    release.keys(publication, ('format', 'catalog', 'objects', 'catalog_sha256'))
    release.require(publication['format'] == 1, 'Invalid publication marker')
    raw_manifest = release.read(path / 'manifest.json', release.MANIFEST_LIMIT)
    signer = release.verify_signature(raw_manifest, release.read(path / 'manifest.sig', 64), public_key)
    manifest = release.validate_manifest(release.parse(raw_manifest))
    release.require(manifest['channel'] == CHANNEL and manifest['architecture'] == 'x86_64'
                    and manifest['signer_id'] == signer, 'Wrong public release target or signer')
    source = validate_catalog(publication['catalog'])['source']
    _, info = source_info(path / 'source-info.json', path / source['filename'], manifest)
    catalog, plan = plan_for(manifest, info['source_archive'], raw_manifest)
    release.require(publication['catalog'] == catalog, 'Publication catalog differs from signed release')
    release.require(release.parse(release.read(path / 'artifact-index.json', CATALOG_LIMIT)) == manifest['artifact']
                    and release.parse(release.read(path / 'source-index.json', CATALOG_LIMIT)) == info['source_archive'],
                    'Immutable filename index mismatch')
    release.require(release.parse(release.read(path / 'channel.json', CATALOG_LIMIT)) == catalog
                    and file_hash(path / 'channel.json') == publication['catalog_sha256'],
                    'Catalog completion hash mismatch')
    expected = {name: {'key': key, **file_record(path / name)} for name, key in plan.items()}
    release.require(publication['objects'] == expected, 'Public file inventory mismatch')
    release.require(set(p.name for p in path.iterdir()) == set(plan) | {'channel.json', 'publication.json'},
                    'Unexpected files in public release directory')
    raw_dist = release.read(path / 'distribution.json', release.DISTRIBUTION_LIMIT)
    release.require(len(raw_dist) == manifest['distribution']['size']
                    and release.sha(raw_dist) == manifest['distribution']['sha256'],
                    'Signed distribution hash mismatch')
    release.require(file_record(path / manifest['artifact']['filename']) == manifest['artifact'],
                    'Signed archive hash mismatch')
    payload = release.validate_distribution(release.parse(raw_dist), manifest, max_unpacked_bytes)
    release.inspection_parent(path.parent)
    approved_der = release.ed25519(release.public_der(public_key))
    release.require(release.sha(approved_der) == signer, 'Approved public key changed during verification')
    with tempfile.TemporaryDirectory(prefix='.verify-public-release-', dir=path.parent) as temporary:
        inspection = Path(temporary) / 'inspection'
        release.inspect_package(path / manifest['artifact']['filename'], inspection,
                                manifest, payload, max_unpacked_bytes)
        validate_public_package(inspection, manifest, payload, approved_der)
    return catalog, expected


def token_value(path=None):
    if path is not None:
        path = release.regular(path)
        mode = path.stat()
        release.require(mode.st_uid == os.getuid() and not mode.st_mode & 0o077,
                        'Token file must be owned and private (0600)')
        token = release.read(path, 16384).decode().strip()
    else:
        token = os.environ.get('CLOUDFLARE_API_TOKEN', '')
    release.require(20 <= len(token) <= 16384 and all(33 <= ord(c) <= 126 for c in token),
                    'A private Cloudflare API token is required')
    return token


class RemoteStore:
    """Fixed-origin Cloudflare REST transport, streaming archives both ways."""

    def __init__(self, account, bucket, token):
        release.require(re.fullmatch('[a-f0-9]{32}', account) is not None, 'Invalid account ID')
        release.require(re.fullmatch('[a-z0-9][a-z0-9-]{1,62}[a-z0-9]', bucket) is not None,
                        'Invalid R2 bucket')
        self.prefix = f'/client/v4/accounts/{account}/r2/buckets/{bucket}/objects/'
        self.token = token
        self.context = ssl.create_default_context()

    @contextlib.contextmanager
    def request(self, method, key, *, source=None, size=0):
        release.relative(key)
        path = self.prefix + urllib.parse.quote(key, safe='/')
        deadline = time.monotonic() + 300
        connection = http.client.HTTPSConnection(HOST, timeout=15, context=self.context)
        try:
            connection.putrequest(method, path)
            connection.putheader('Authorization', 'Bearer ' + self.token)
            connection.putheader('Accept-Encoding', 'identity')
            connection.putheader('User-Agent', USER_AGENT)
            if source is not None:
                connection.putheader('Content-Type', 'application/octet-stream')
                connection.putheader('Content-Length', str(size))
            connection.endheaders()
            if source is not None:
                sent = 0
                with release.open_regular(source) as stream:
                    while chunk := stream.read(1024 * 1024):
                        release.require(time.monotonic() < deadline, 'R2 upload deadline exceeded')
                        sent += len(chunk)
                        release.require(sent <= size, 'Upload source grew')
                        connection.send(chunk)
                release.require(sent == size, 'Upload source changed size')
            result = connection.getresponse()
            # No redirects: the token never leaves the fixed Cloudflare API host.
            yield result, deadline
        finally:
            connection.close()

    @staticmethod
    def bounded(response, deadline, limit):
        chunks = []
        count = 0
        while True:
            release.require(time.monotonic() < deadline, 'R2 response deadline exceeded')
            block = response.read(min(1024 * 1024, limit - count + 1))
            if not block:
                break
            count += len(block)
            release.require(count <= limit, 'R2 response exceeds limit')
            chunks.append(block)
        return b''.join(chunks)

    def read(self, key, limit):
        with self.request('GET', key) as (response, deadline):
            if response.status == 404:
                return None
            release.require(response.status == 200, f'R2 read failed (HTTP {response.status})')
            return self.bounded(response, deadline, limit)

    def verify(self, key, record):
        with self.request('GET', key) as (response, deadline):
            if response.status == 404:
                return False
            release.require(response.status == 200, f'R2 verification failed (HTTP {response.status})')
            count = 0
            digest = hashlib.sha256()
            while True:
                release.require(time.monotonic() < deadline, 'R2 verification deadline exceeded')
                block = response.read(min(1024 * 1024, record['size'] - count + 1))
                if not block:
                    break
                count += len(block)
                release.require(count <= record['size'], 'Existing remote object has different bytes')
                digest.update(block)
            release.require(count == record['size'] and digest.hexdigest() == record['sha256'],
                            'Existing remote object has different bytes')
            return True

    def put(self, key, path, record):
        release.require(record['size'] <= API_LIMIT, 'Artifact exceeds R2 REST upload limit')
        release.require(file_hash(path) == record['sha256'], 'Upload source changed')
        with self.request('PUT', key, source=path, size=record['size']) as (response, deadline):
            release.require(response.status == 200, f'R2 upload failed (HTTP {response.status})')
            value = release.parse(self.bounded(response, deadline, SOURCE_INFO_LIMIT))
            release.require(value.get('success') is True, 'R2 rejected the upload')
        release.require(file_hash(path) == record['sha256'], 'Upload source changed during request')


class SignedWorkerStore:
    """Short-lived, domain-separated signed permits; no Cloudflare token.

    The temporary Worker receives only a public key. An 8 MiB part fits below
    Cloudflare's inbound body limit, and the completed object is read back and
    hashed by the publisher before any channel pointer is committed.
    """

    def __init__(self, url, private_key, public_key):
        parsed = urllib.parse.urlsplit(url)
        release.require(parsed.scheme == 'https' and parsed.hostname and not parsed.username
                        and not parsed.password and parsed.port in (None, 443)
                        and parsed.path in ('', '/') and not parsed.query and not parsed.fragment,
                        'Upload URL must be an explicit HTTPS origin')
        release.require(re.fullmatch(r'[a-zA-Z0-9.-]+', parsed.hostname) is not None,
                        'Invalid uploader hostname')
        self.host = parsed.hostname
        self.origin = 'https://' + self.host
        self.private_key = release.regular(private_key)
        release.require(release.ed25519(release.public_der(private_key, private=True))
                        == release.ed25519(release.public_der(public_key)),
                        'Upload signer differs from the pinned release signer')
        self.context = ssl.create_default_context()
        self.etags = {}

    def permit(self, action, key, data=b'', **extra):
        methods = {'inspect': 'GET', 'read': 'GET', 'put': 'PUT', 'begin': 'POST',
                   'part': 'PUT', 'complete': 'POST', 'abort': 'POST'}
        release.relative(key)
        release.require(action in methods and len(data) <= PART_SIZE, 'Invalid uploader operation')
        value = {'format': 'noventra-r2-upload-v1', 'origin': self.origin,
                 'method': methods[action], 'path': '/v1/' + action, 'action': action,
                 'key': key, 'expires': int(time.time()) + 240,
                 'size': len(data), 'sha256': release.sha(data), **extra}
        raw = json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('ascii')
        with tempfile.TemporaryDirectory(prefix='noventra-upload-permit-') as folder:
            message = Path(folder) / 'message'
            message.write_bytes(b'noventra-r2-upload-v1\n' + raw)
            signature = release.openssl('pkeyutl', '-sign', '-inkey', self.private_key,
                                        '-rawin', '-in', message)
        release.require(len(signature) == 64, 'Invalid permit signature')
        encode = lambda b: base64.urlsafe_b64encode(b).rstrip(b'=').decode('ascii')
        return value, {'X-Noventra-Permit': encode(raw), 'X-Noventra-Signature': encode(signature)}

    @contextlib.contextmanager
    def request(self, action, key, data=b'', **extra):
        value, headers = self.permit(action, key, data, **extra)
        headers.update({'Content-Length': str(len(data)), 'Accept-Encoding': 'identity',
                        'Content-Type': 'application/octet-stream',
                        'User-Agent': USER_AGENT})
        connection = http.client.HTTPSConnection(self.host, timeout=15, context=self.context)
        deadline = time.monotonic() + 300
        try:
            connection.request(value['method'], value['path'], body=data, headers=headers)
            yield connection.getresponse(), deadline
        finally:
            connection.close()

    def operation(self, action, key, data=b'', **extra):
        with self.request(action, key, data, **extra) as (response, deadline):
            release.require(response.status == 200,
                            f'Signed upload {action} failed (HTTP {response.status}); inspect before retrying')
            return release.parse(RemoteStore.bounded(response, deadline, SOURCE_INFO_LIMIT))

    def read(self, key, limit):
        with self.request('read', key) as (response, deadline):
            if response.status == 404:
                self.etags[key] = None
                return None
            release.require(response.status == 200, f'Signed object read failed (HTTP {response.status})')
            etag = response.getheader('ETag', '')
            release.require(re.fullmatch(r'"[A-Za-z0-9+/=_-]{1,256}"', etag) is not None,
                            'Missing conditional-publication ETag')
            self.etags[key] = etag[1:-1]
            return RemoteStore.bounded(response, deadline, limit)

    def verify(self, key, record):
        with self.request('read', key) as (response, deadline):
            if response.status == 404:
                return False
            release.require(response.status == 200, f'Signed object verification failed (HTTP {response.status})')
            count = 0
            digest = hashlib.sha256()
            while True:
                release.require(time.monotonic() < deadline, 'Signed object verification deadline exceeded')
                block = response.read(min(1024 * 1024, record['size'] - count + 1))
                if not block:
                    break
                count += len(block)
                release.require(count <= record['size'], 'Existing remote object has different bytes')
                digest.update(block)
            release.require(count == record['size'] and digest.hexdigest() == record['sha256'],
                            'Existing remote object has different bytes')
            return True

    def put(self, key, path, record):
        release.require(record['size'] <= API_LIMIT, 'Artifact exceeds publication size limit')
        release.require(file_hash(path) == record['sha256'], 'Upload source changed')
        if record['size'] <= PART_SIZE:
            data = release.read(path, PART_SIZE)
            extra = {'expected_etag': self.etags.get(key)} if key == CATALOG_KEY else {}
            self.operation('put', key, data, **extra)
        else:
            begun = self.operation('begin', key, total_size=record['size'], total_sha256=record['sha256'])
            upload_id = begun.get('upload_id')
            release.require(isinstance(upload_id, str) and 0 < len(upload_id) <= 2048,
                            'Uploader did not return a multipart identity')
            parts = []
            try:
                with release.open_regular(path) as stream:
                    while data := stream.read(PART_SIZE):
                        part = self.operation('part', key, data, upload_id=upload_id,
                                              part_number=len(parts) + 1)
                        release.keys(part, ('partNumber', 'etag'))
                        release.require(part['partNumber'] == len(parts) + 1
                                        and isinstance(part['etag'], str), 'Invalid uploaded part receipt')
                        parts.append(part)
                self.operation('complete', key, upload_id=upload_id, parts=parts,
                               total_size=record['size'], total_sha256=record['sha256'])
            except BaseException:
                # If the complete response was lost, a complete correct object
                # is safe to reuse. Otherwise abort this owned upload when known.
                complete = False
                try:
                    complete = self.verify(key, record)
                except (ValueError, OSError, http.client.HTTPException):
                    pass
                if not complete:
                    try:
                        self.operation('abort', key, upload_id=upload_id)
                    except (ValueError, OSError, http.client.HTTPException):
                        pass  # The primary error remains visible; R2 expires orphan parts.
                    raise
        release.require(file_hash(path) == record['sha256'], 'Upload source changed during request')


def upload(path, public_key, store, *, expected_sequence=0, max_unpacked_bytes=4 * 1024**3):
    path = Path(path).absolute()
    catalog, objects = prepared(path, public_key, max_unpacked_bytes=max_unpacked_bytes)
    release.integer(expected_sequence)
    previous = store.read(CATALOG_KEY, CATALOG_LIMIT)
    old = validate_catalog(release.parse(previous)) if previous is not None else None
    current = old['sequence'] if old else 0
    release.require(current == expected_sequence, 'Remote sequence differs from the explicit expected sequence')
    release.require(current < catalog['sequence'] or old == catalog,
                    'Refusing a rollback or different manifest at the same sequence')
    uploaded, reused = [], []
    for name, record in objects.items():
        key = record['key']
        if store.verify(key, record):
            reused.append(key)
            continue
        store.put(key, path / name, record)
        release.require(store.verify(key, record), 'Uploaded object could not be read back')
        uploaded.append(key)
    # Each immutable object is complete and verified before the single pointer
    # write. Recheck for a concurrent publication; the signed Worker additionally
    # uses R2 conditional PUT against the ETag observed by this final read.
    release.require(store.read(CATALOG_KEY, CATALOG_LIMIT) == previous,
                    'Channel changed during upload; no pointer was replaced')
    raw_catalog = release.read(path / 'channel.json', CATALOG_LIMIT)
    if previous != raw_catalog:
        store.put(CATALOG_KEY, path / 'channel.json', file_record(path / 'channel.json'))
    release.require(store.read(CATALOG_KEY, CATALOG_LIMIT) == raw_catalog,
                    'Channel publication could not be read back; inspect before retrying')
    return {'status': 'uploaded', 'sequence': catalog['sequence'], 'channel': CHANNEL,
            'target': TARGET, 'release_id': catalog['release_id'],
            'manifest_sha256': catalog['manifest_sha256'], 'uploaded': uploaded,
            'reused': reused, 'pointer_written_last': True}


@contextlib.contextmanager
def publication_lock(path):
    path = Path(path).absolute()
    release.require(path.parent.is_dir(), 'Publication lock parent must exist')
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        release.require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                        and not info.st_mode & 0o077, 'Publication lock must be private and owned')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    local = sub.add_parser('prepare', help='Validate and sign an explicit qualified distribution offline')
    for name in ('distribution', 'output', 'private-key', 'source-archive', 'source-info'):
        local.add_argument('--' + name, type=Path, required=True)
    local.add_argument('--sequence', type=int, required=True)
    local.add_argument('--lifetime', type=int, default=30 * 86400)
    remote = sub.add_parser('upload', help='Upload verified immutable files, then replace the channel pointer')
    remote.add_argument('--release', type=Path, required=True)
    remote.add_argument('--public-key', type=Path, required=True)
    remote.add_argument('--token-file', type=Path, help='Private token file; otherwise CLOUDFLARE_API_TOKEN')
    remote.add_argument('--upload-url', help='Temporary signer-authorized HTTPS Worker origin; no API token needed')
    remote.add_argument('--private-key', type=Path, help='Offline signer for short-lived upload permits')
    remote.add_argument('--account-id', default=ACCOUNT)
    remote.add_argument('--bucket', default=BUCKET)
    remote.add_argument('--expected-sequence', type=int, required=True,
                        help='Use 0 only for the first publication; inspect the live pointer before retrying')
    remote.add_argument('--lock', type=Path, required=True,
                        help='Shared private publisher lock outside the release directory')
    args = parser.parse_args()
    try:
        if args.operation == 'prepare':
            result = prepare(args.distribution, args.output, args.private_key, args.source_archive,
                             args.source_info, args.sequence, lifetime=args.lifetime)
        else:
            with publication_lock(args.lock):
                if args.upload_url:
                    release.require(args.private_key is not None and args.token_file is None,
                                    'Signed upload requires --private-key and no token file')
                    store = SignedWorkerStore(args.upload_url, args.private_key, args.public_key)
                else:
                    release.require(args.private_key is None, 'Private key upload permits require --upload-url')
                    store = RemoteStore(args.account_id, args.bucket, token_value(args.token_file))
                result = upload(args.release, args.public_key, store, expected_sequence=args.expected_sequence)
    except (ValueError, OSError, subprocess.SubprocessError, http.client.HTTPException,
            tarfile.TarError, lzma.LZMAError) as error:
        parser.exit(1, f'Public release publisher: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
