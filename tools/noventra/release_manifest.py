#!/usr/bin/env python3
"""Sign explicit completed AppDir distributions; no builds, keys or publication."""

import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time

PRODUCT = 'local.noventra.browser'
FORMAT = 'noventra-delivery-v1'
MANIFEST_LIMIT = 64 * 1024
DISTRIBUTION_LIMIT = 16 * 1024 * 1024
MAX_FILES = 20000
MAX_LIFETIME = 31 * 24 * 3600
INSPECTION_RESERVE = 64 * 1024**2
LINK = 'Noventra.AppDir/usr/bin/noventra-browser'
LINK_TARGET = '../../opt/noventra/noventra-wrapper'
APP = 'Noventra.AppDir/'
RUNTIME = APP + 'opt/noventra/'
REQUIRED = {APP + 'AppRun', APP + 'noventra-package.json',
            RUNTIME + 'noventra-browser', RUNTIME + 'noventra-wrapper',
            RUNTIME + 'snapshot.json', RUNTIME + 'provenance.json',
            RUNTIME + 'LICENSE-Helium', RUNTIME + 'LICENSE-Chromium', LINK}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def integer(value, minimum=0, maximum=2**63 - 1):
    require(type(value) is int and minimum <= value <= maximum, 'Invalid integer')
    return value


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(value):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value),
            'Invalid SHA-256')
    return value


def keys(value, expected):
    require(isinstance(value, dict) and set(value) == set(expected), 'Unexpected object fields')


def parse(data):
    def pairs(items):
        out = {}
        for key, value in items:
            require(key not in out, 'Duplicate JSON key')
            out[key] = value
        return out

    def constant(value):
        raise ValueError('Invalid JSON number')

    try:
        result = json.loads(data, object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeError, RecursionError) as error:
        raise ValueError('Invalid JSON') from error
    require(isinstance(result, dict), 'Expected JSON object')
    return result


def encode(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode()


def relative(name):
    require(isinstance(name, str) and 0 < len(name.encode()) <= 4096
            and '\\' not in name and ':' not in name
            and all(32 <= ord(c) != 127 for c in name)
            and all(p not in ('', '.', '..') for p in name.split('/')), 'Unsafe path')
    return Path(name)


def filename(name):
    relative(name)
    require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,150}', name), 'Unsafe filename')
    return name


def regular(path):
    path = Path(path).absolute()
    for parent in reversed(path.parents):
        require(stat.S_ISDIR(parent.lstat().st_mode), 'Symlink/non-directory parent')
    require(stat.S_ISREG(path.lstat().st_mode), 'Expected regular file, without symlinks')
    return path


def inspection_parent(path):
    path = Path(path).absolute()
    for parent in reversed((path, *path.parents)):
        require(stat.S_ISDIR(parent.lstat().st_mode), 'Symlink/non-directory inspection parent')
    return path


def open_regular(path):
    path = regular(path)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    stream = os.fdopen(fd, 'rb')
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        stream.close()
        raise ValueError('Input is not regular')
    return stream


def read(path, limit):
    with open_regular(path) as stream:
        require(os.fstat(stream.fileno()).st_size <= limit, 'File exceeds size limit')
        data = stream.read(limit + 1)
    require(len(data) <= limit, 'File exceeds size limit')
    return data


def copy_checked(source, target, size, expected):
    integer(size)
    hasher = hashlib.sha256()
    count = 0
    with open_regular(source) as src, Path(target).open('xb') as dst:
        while chunk := src.read(min(1024 * 1024, size - count + 1)):
            count += len(chunk)
            require(count <= size, 'Input exceeds declared size')
            dst.write(chunk)
            hasher.update(chunk)
        dst.flush()
        os.fsync(dst.fileno())
    require(count == size and hasher.hexdigest() == digest(expected), 'Input hash/size mismatch')


def openssl(*args, data=None):
    # Fixed argv only. Neither the manifest nor server supplies commands.
    result = subprocess.run(['openssl', *map(str, args)], input=b'' if data is None else data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=30, check=False)
    require(result.returncode == 0, 'OpenSSL operation failed')
    return result.stdout


def public_der(path, private=False):
    regular(path)
    args = ['pkey'] + ([] if private else ['-pubin'])
    return openssl(*args, '-in', path, '-pubout', '-outform', 'DER')


def ed25519(der):
    # RFC 8410 SubjectPublicKeyInfo for a raw 32-byte Ed25519 public key.
    require(len(der) == 44 and der[:12] == bytes.fromhex('302a300506032b6570032100'),
            'An Ed25519 signing key is required')
    return der


def verify_signature(raw, signature, public_key):
    require(len(raw) <= MANIFEST_LIMIT and len(signature) == 64, 'Invalid signature envelope size')
    der = ed25519(public_der(public_key))
    with tempfile.TemporaryDirectory(prefix='noventra-signature-') as folder:
        p = Path(folder)
        (p / 'manifest').write_bytes(raw)
        (p / 'signature').write_bytes(signature)
        # Verify against the already-read key, not a second mutable path lookup.
        (p / 'public.der').write_bytes(der)
        openssl('pkeyutl', '-verify', '-pubin', '-inkey', p / 'public.der',
                '-keyform', 'DER', '-rawin', '-in', p / 'manifest',
                '-sigfile', p / 'signature')
    return sha(der)


def validate_manifest(value, *, now=None, allow_expired=False):
    keys(value, ('format', 'product', 'channel', 'platform', 'architecture', 'sequence',
                 'version_label', 'created_at', 'expires_at', 'signer_id',
                 'snapshot_sha256', 'artifact', 'distribution'))
    require(value['format'] == FORMAT and value['product'] == PRODUCT
            and value['platform'] == 'linux'
            and value['architecture'] in ('x86_64', 'aarch64'), 'Wrong release identity')
    require(isinstance(value['channel'], str)
            and re.fullmatch('[a-z][a-z0-9-]{0,31}', value['channel']), 'Invalid channel')
    filename(value['version_label'])
    integer(value['sequence'], 1)
    created = integer(value['created_at'], 1)
    expires = integer(value['expires_at'], created + 1, created + MAX_LIFETIME)
    now = int(time.time()) if now is None else now
    require(created <= now + 300 and (allow_expired or now < expires), 'Manifest expired or from the future')
    digest(value['signer_id'])
    digest(value['snapshot_sha256'])
    for field in ('artifact', 'distribution'):
        record = value[field]
        keys(record, ('filename', 'size', 'sha256'))
        filename(record['filename'])
        integer(record['size'], 1)
        digest(record['sha256'])
    require(value['artifact']['filename'].endswith('.tar.xz'), 'Expected AppDir tar.xz')
    require(value['distribution']['filename'] == 'distribution.json', 'Expected distribution inventory')
    require(value['distribution']['size'] <= DISTRIBUTION_LIMIT, 'Inventory too large')
    return value


def validate_distribution(value, manifest, expanded_limit):
    require(type(value.get('format')) is int and value.get('format') == 1 and value.get('complete') is True
            and value.get('unsigned') is True and value.get('id') == PRODUCT,
            'Expected completed unsigned Noventra distribution')
    for field in ('platform', 'architecture', 'version_label', 'snapshot_sha256'):
        require(value.get(field) == manifest[field], 'Distribution identity mismatch')
    files = value.get('files')
    require(isinstance(files, dict) and 0 < len(files) <= MAX_FILES, 'Invalid inventory')
    artifact = manifest['artifact']
    require(artifact['filename'] in files, 'Archive missing from inventory')
    archive_record = files[artifact['filename']]
    require(archive_record == {'sha256': artifact['sha256'], 'size': artifact['size'],
                               'mode': '0o644'}, 'Archive inventory mismatch')
    payload = {name: record for name, record in files.items() if name != artifact['filename']}
    require(REQUIRED <= payload.keys(), 'Required AppDir entries missing')
    total = 0
    for name, record in payload.items():
        relative(name)
        require(name.startswith(APP), 'Non-AppDir payload')
        require(isinstance(record, dict), 'Invalid inventory record')
        if name == LINK:
            require(record == {'symlink': LINK_TARGET}, 'Invalid launcher link')
        else:
            keys(record, ('sha256', 'size', 'mode'))
            digest(record['sha256'])
            total += integer(record['size'])
            require(record['mode'] in ('0o644', '0o755'), 'Unsafe file mode')
        for parent in Path(name).parents:
            require(str(parent) not in payload, 'Inventory path prefix collision')
    require(total <= expanded_limit, 'Expanded size exceeds limit')
    for executable in (APP + 'AppRun', RUNTIME + 'noventra-browser', RUNTIME + 'noventra-wrapper'):
        require(payload[executable]['mode'] == '0o755', 'Entry point is not executable')
    require(payload[RUNTIME + 'snapshot.json']['sha256'] == manifest['snapshot_sha256'],
            'Snapshot identity mismatch')
    return payload


def verify_package(path, manifest, payload):
    metadata = parse(read(path / APP / 'noventra-package.json', MANIFEST_LIMIT))
    require(metadata.get('id') == PRODUCT and metadata.get('unsigned') is True,
            'Invalid packaged identity')
    for field in ('platform', 'architecture', 'version_label', 'snapshot_sha256'):
        require(metadata.get(field) == manifest[field], 'Packaged identity mismatch')
    snapshot = parse(read(path / RUNTIME / 'snapshot.json', DISTRIBUTION_LIMIT))
    require(snapshot.get('binary') == 'noventra-browser' and isinstance(snapshot.get('files'), dict)
            and snapshot['files'], 'Invalid nested snapshot inventory')
    for name, expected in snapshot['files'].items():
        relative(name)
        require(payload.get(RUNTIME + name, {}).get('sha256') == digest(expected),
                'Nested snapshot inventory mismatch')


def unpack(archive, destination, payload, expanded_limit, *, space_limit=None):
    """Never call tarfile.extract: validate bounded headers and copy regular bytes."""
    destination = Path(destination)
    require(not destination.exists(), 'Extraction destination exists')
    destination.mkdir(mode=0o700)
    with tempfile.TemporaryFile(dir=destination.parent) as raw:
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=256 * 1024 * 1024)
        total = 0
        with open_regular(archive) as compressed:
            while not decoder.eof:
                chunk = compressed.read(1024 * 1024) if decoder.needs_input else b''
                require(chunk or not decoder.needs_input, 'Truncated xz archive')
                block = decoder.decompress(chunk, max_length=1024 * 1024)
                total += len(block)
                require(total <= expanded_limit, 'Expanded archive exceeds limit')
                require(space_limit is None or total <= space_limit,
                        'Not enough free disk space to inspect the package')
                raw.write(block)
            require(not decoder.unused_data and not compressed.read(1), 'Trailing/concatenated xz data')
        raw.seek(0)
        # Bound PAX metadata before tarfile can allocate from attacker-controlled sizes.
        header_count = 0
        while True:
            block = raw.read(512)
            require(len(block) == 512, 'Truncated tar header')
            if block == b'\0' * 512:
                require(raw.read(512) == b'\0' * 512, 'Missing tar end marker')
                while tail := raw.read(1024 * 1024):
                    require(not any(tail), 'Nonzero trailing tar data')
                break
            info = tarfile.TarInfo.frombuf(block, 'utf-8', 'strict')
            header_count += 1
            require(header_count <= 2 * len(payload) + 2, 'Excess archive headers')
            require(info.type in (tarfile.REGTYPE, tarfile.AREGTYPE,
                                  tarfile.SYMTYPE, tarfile.XHDTYPE), 'Unsupported tar entry type')
            require(info.size >= 0 and raw.tell() + ((info.size + 511) // 512) * 512 <= total,
                    'Truncated tar data')
            if info.type == tarfile.XHDTYPE:
                require(info.size <= 65536, 'Excess PAX metadata')
            elif info.type == tarfile.SYMTYPE:
                require(info.size == 0, 'Symlink with payload')
            raw.seek(((info.size + 511) // 512) * 512, os.SEEK_CUR)
        raw.seek(0)
        seen = set()
        with tarfile.open(fileobj=raw, mode='r:') as bundle:
            for info in bundle:
                relative(info.name)
                require(info.name in payload and info.name not in seen, 'Unexpected/duplicate archive member')
                seen.add(info.name)
                require(not info.sparse and set(info.pax_headers) <= {'path', 'mtime', 'atime', 'ctime'},
                        'Unsupported archive metadata')
                record = payload[info.name]
                if info.name == LINK:
                    require(info.issym() and info.linkname == LINK_TARGET and info.size == 0
                            and not info.mode & ~0o777, 'Invalid archive launcher link')
                    continue  # Create this single known link only after all regular files.
                require(info.isreg() and info.mode == int(record['mode'], 8)
                        and info.size == record['size'], 'Archive type/mode/size mismatch')
                target = destination / info.name
                target.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
                hasher = hashlib.sha256()
                with bundle.extractfile(info) as src, target.open('xb') as dst:
                    remaining = info.size
                    while remaining:
                        chunk = src.read(min(remaining, 1024 * 1024))
                        require(chunk, 'Truncated archive member')
                        dst.write(chunk)
                        hasher.update(chunk)
                        remaining -= len(chunk)
                    os.fchmod(dst.fileno(), int(record['mode'], 8))
                    dst.flush()
                    os.fsync(dst.fileno())
                require(hasher.hexdigest() == record['sha256'], 'Archive member hash mismatch')
        require(seen == set(payload), 'Incomplete archive')
    link = destination / LINK
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(LINK_TARGET)
    for parent, _, _ in os.walk(destination, followlinks=False):
        Path(parent).chmod(0o700)


def inspect_package(archive, destination, manifest, payload, expanded_limit):
    """Inspect data only, reserving room for both the raw tar and its payload."""
    parent = inspection_parent(Path(destination).parent)
    payload_bytes = sum(record.get('size', 0) for record in payload.values())
    # Every entry needs a header and regular data uses padded 512-byte blocks.
    minimum_tar = 1024 + sum(512 + ((record.get('size', 0) + 511) // 512) * 512
                             for record in payload.values())
    available = shutil.disk_usage(parent).free - payload_bytes - INSPECTION_RESERVE
    require(available >= minimum_tar, 'Not enough free disk space to inspect the package')
    # Extra PAX headers and padding stay subject to the original expansion cap
    # and to actual available space. Never expand the caller's declared limit.
    unpack(archive, destination, payload, expanded_limit, space_limit=available)
    verify_package(destination, manifest, payload)


def publish(distribution, output, private_key, sequence, *, channel='local', lifetime=86400,
            max_artifact_bytes=2 * 1024**3, max_unpacked_bytes=4 * 1024**3,
            package_validator=None):
    distribution = Path(distribution).absolute()
    output = Path(output).absolute()
    require(not os.path.lexists(output), 'Output already exists')
    inspection_parent(output.parent)
    raw_dist = read(distribution / 'distribution.json', DISTRIBUTION_LIMIT)
    dist = parse(raw_dist)
    archives = [n for n in dist.get('files', {}) if n.endswith('.tar.xz') and '/' not in n]
    require(len(archives) == 1, 'Distribution must declare exactly one archive')
    artifact = dist['files'][archives[0]]
    require(isinstance(artifact, dict), 'Invalid archive record')
    integer(artifact.get('size'), 1, max_artifact_bytes)
    key = ed25519(public_der(private_key, private=True))
    now = int(time.time())
    integer(lifetime, 1, MAX_LIFETIME)
    value = validate_manifest({
        'format': FORMAT, 'product': PRODUCT, 'channel': channel, 'platform': 'linux',
        'architecture': dist.get('architecture'), 'sequence': sequence,
        'version_label': dist.get('version_label'), 'created_at': now, 'expires_at': now + lifetime,
        'signer_id': sha(key), 'snapshot_sha256': dist.get('snapshot_sha256'),
        'artifact': {'filename': archives[0], 'size': artifact['size'], 'sha256': artifact['sha256']},
        'distribution': {'filename': 'distribution.json', 'size': len(raw_dist), 'sha256': sha(raw_dist)},
    })
    payload = validate_distribution(dist, value, max_unpacked_bytes)
    # Output is published only after all copied bytes and unpacked members validate.
    with tempfile.TemporaryDirectory(prefix='.noventra-publish-', dir=output.parent) as folder:
        temp = Path(folder)
        release = temp / 'release'
        release.mkdir()
        copy_checked(distribution / archives[0], release / archives[0], artifact['size'], artifact['sha256'])
        inspect_package(release / archives[0], temp / 'inspection', value, payload, max_unpacked_bytes)
        # A trusted local caller can impose a stricter publication contract.
        # This function object never comes from archive or manifest data.
        if package_validator is not None:
            package_validator(temp / 'inspection', value, payload, key)
        (release / 'distribution.json').write_bytes(raw_dist)
        raw = encode(value)
        require(len(raw) <= MANIFEST_LIMIT, 'Manifest too large')
        (release / 'manifest.json').write_bytes(raw)
        openssl('pkeyutl', '-sign', '-inkey', private_key, '-rawin',
                '-in', release / 'manifest.json', '-out', release / 'manifest.sig')
        (temp / 'public.pem').write_bytes(openssl('pkey', '-pubin', '-inform', 'DER',
                                                '-outform', 'PEM', data=key))
        require(verify_signature(raw, read(release / 'manifest.sig', 64), temp / 'public.pem')
                == value['signer_id'], 'Signing key changed')
        require(not os.path.lexists(output), 'Output appeared during publish')
        release.rename(output)
    return {'status': 'published_locally', 'directory': str(output), 'sequence': sequence,
            'signer_id': value['signer_id'], 'manifest_sha256': sha(raw)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--distribution', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--private-key', type=Path, required=True)
    parser.add_argument('--sequence', type=int, required=True)
    parser.add_argument('--channel', default='local')
    parser.add_argument('--lifetime', type=int, default=86400, help='Manifest lifetime in seconds (max 31 days)')
    args = parser.parse_args()
    try:
        result = publish(args.distribution, args.output, args.private_key, args.sequence,
                         channel=args.channel, lifetime=args.lifetime)
    except (ValueError, OSError, subprocess.SubprocessError, tarfile.TarError, lzma.LZMAError) as error:
        parser.exit(1, f'Delivery publisher: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
