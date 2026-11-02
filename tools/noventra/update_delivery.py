#!/usr/bin/env python3
"""Manual Linux AppDir delivery into an explicit private root; never touches profiles."""

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import http.client
import ipaddress
import io
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
import urllib.error
import urllib.parse
import urllib.request

import release_manifest as release

require = release.require
DOWNLOAD_TIMEOUT = 300
USER_AGENT = 'NoventraUpdater/1 (+https://noventrastudio.com)'


def origin(url):
    parsed = urllib.parse.urlsplit(url)
    require(parsed.scheme in ('https', 'http') and parsed.hostname
            and parsed.username is None and parsed.password is None
            and not parsed.query and not parsed.fragment, 'Invalid delivery URL')
    port = parsed.port or (443 if parsed.scheme == 'https' else 80)
    if parsed.scheme == 'http':
        try:
            local = ipaddress.ip_address(parsed.hostname).is_loopback
        except ValueError:
            local = False
        require(local, 'HTTP is restricted to literal loopback addresses')
    require('%' not in parsed.path and '\\' not in parsed.path
            and all(p not in ('.', '..') for p in parsed.path.split('/')), 'Unsafe URL path')
    return parsed.scheme, parsed.hostname.lower(), port


def config(path):
    value = release.parse(release.read(path, release.MANIFEST_LIMIT))
    release.keys(value, ('format', 'base_url', 'public_key', 'install_root', 'channel',
                         'architecture', 'max_artifact_bytes', 'max_unpacked_bytes'))
    require(type(value['format']) is int and value['format'] == 1, 'Unsupported client config')
    for name in ('public_key', 'install_root'):
        require(isinstance(value[name], str) and Path(value[name]).is_absolute(), 'Paths must be absolute')
    require(isinstance(value['base_url'], str) and value['base_url'].endswith('/'),
            'base_url must end in /')
    origin(value['base_url'])
    require(value['architecture'] in ('x86_64', 'aarch64'), 'Unsupported architecture')
    require(isinstance(value['channel'], str)
            and re.fullmatch('[a-z][a-z0-9-]{0,31}', value['channel']), 'Invalid channel')
    release.integer(value['max_artifact_bytes'], 1, 16 * 1024**3)
    release.integer(value['max_unpacked_bytes'], 10240, 64 * 1024**3)
    value['signer_id'] = release.sha(release.ed25519(release.public_der(value['public_key'])))
    return value


def binding(cfg):
    return {key: cfg[key] for key in ('base_url', 'channel', 'architecture', 'signer_id')}


def directory(path):
    path = Path(path).absolute()
    for part in reversed((path, *path.parents)):
        require(stat.S_ISDIR(part.lstat().st_mode), 'Symlink/non-directory in install path')
    info = path.stat()
    require(info.st_uid == os.getuid() and not info.st_mode & 0o022, 'Install root must be private and owned')
    return path


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_state(root, state):
    fd, name = tempfile.mkstemp(prefix='.state-', dir=root)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(release.encode(state))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, root / 'state.json')
        sync_directory(root)
    finally:
        if os.path.lexists(name):
            os.unlink(name)


@contextmanager
def locked(root, *, create=False):
    root = directory(root)
    flags = os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK
    if create:
        flags |= os.O_CREAT | os.O_EXCL
    fd = os.open(root / '.lock', flags, 0o600)
    try:
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                and info.st_nlink == 1 and not info.st_mode & 0o077, 'Unsafe install lock')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield root
    finally:
        os.close(fd)


def initialize(cfg):
    root = Path(cfg['install_root'])
    if not os.path.lexists(root):
        root.mkdir(mode=0o700)
    directory(root)
    require(not any(root.iterdir()), 'Initialization requires an empty owned root')
    with locked(root, create=True) as root:
        require(set(p.name for p in root.iterdir()) == {'.lock'}, 'Initialization requires an empty owned root')
        (root / 'releases').mkdir(mode=0o700)
        (root / '.staging').mkdir(mode=0o700)
        atomic_state(root, {'format': 1, 'binding': binding(cfg), 'highest_sequence': 0,
                            'highest_manifest': None, 'active': None, 'previous': None, 'staged': None})
    return {'status': 'initialized', 'install_root': str(root)}


def read_state(root, cfg):
    value = release.parse(release.read(root / 'state.json', release.MANIFEST_LIMIT))
    release.keys(value, ('format', 'binding', 'highest_sequence', 'highest_manifest',
                         'active', 'previous', 'staged'))
    require(value['format'] == 1 and value['binding'] == binding(cfg), 'Install trust/config binding mismatch')
    release.integer(value['highest_sequence'])
    if value['highest_sequence']:
        release.digest(value['highest_manifest'])
    else:
        require(value['highest_manifest'] is None, 'Invalid initial state')
    for field in ('active', 'previous', 'staged'):
        if value[field] is not None:
            release.filename(value[field])
            require(value[field].startswith('r-'), 'Invalid release selection')
    directory(root / 'releases')
    directory(root / '.staging')
    return value


class ConstrainedRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self, base):
        self.base = base
        self.deadline = time.monotonic() + DOWNLOAD_TIMEOUT
        super().__init__()

    def timeout(self):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, 'Download deadline exceeded')
        return min(15, remaining)

    def http_error_302(self, req, fp, code, msg, headers):
        # urllib drains redirect bodies with an unbounded fp.read(). Close the
        # network response first and give it an empty body instead, retaining
        # its URL normalization, redirect loop limits and redirect_request checks.
        fp.close()
        req.timeout = min(req.timeout, self.timeout())
        with io.BytesIO() as empty:
            return super().http_error_302(req, empty, code, msg, headers)

    http_error_301 = http_error_303 = http_error_307 = http_error_308 = http_error_302

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        try:
            require(origin(newurl) == origin(self.base), 'Cross-origin or scheme redirect rejected')
            require(urllib.parse.urlsplit(newurl).path.startswith(urllib.parse.urlsplit(self.base).path),
                    'Redirect left configured release path')
        except ValueError:
            if fp is not None:
                fp.close()
            raise
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(cfg, name, destination, limit, expected_size=None, expected_hash=None):
    release.filename(name)
    url = cfg['base_url'] + name
    require(origin(url) == origin(cfg['base_url']), 'Invalid download URL')
    redirects = ConstrainedRedirect(cfg['base_url'])
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), redirects)
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT,
                                                  'Accept-Encoding': 'identity'})
    hasher = hashlib.sha256()
    size = 0
    try:
        response = opener.open(request, timeout=redirects.timeout())
    except urllib.error.HTTPError as error:
        code = error.code
        error.close()
        raise ValueError(f'Delivery HTTP status {code}') from None
    with response, Path(destination).open('xb') as output:
        require(response.status == 200, 'Unexpected HTTP status')
        require(response.headers.get('Content-Encoding', 'identity') == 'identity', 'Encoded HTTP payload rejected')
        lengths = response.headers.get_all('Content-Length') or []
        require(len(lengths) <= 1, 'Ambiguous HTTP length')
        declared = None
        if lengths:
            require(lengths[0].isdigit(), 'Invalid HTTP length')
            declared = release.integer(int(lengths[0]), 0, limit)
            require(expected_size is None or declared == expected_size, 'HTTP size mismatch')
        while True:
            redirects.timeout()
            chunk = response.read1(min(1024 * 1024, limit - size + 1))
            redirects.timeout()
            if not chunk:
                break
            size += len(chunk)
            require(size <= limit, 'Download limit exceeded')
            output.write(chunk)
            hasher.update(chunk)
        output.flush()
        os.fsync(output.fileno())
    require(declared is None or size == declared, 'Truncated HTTP response')
    require(expected_size is None or size == expected_size, 'Download size mismatch')
    require(expected_hash is None or hasher.hexdigest() == expected_hash, 'Download hash mismatch')


def authenticated(cfg, raw, signature, *, allow_expired=False):
    signer = release.verify_signature(raw, signature, cfg['public_key'])
    require(signer == cfg['signer_id'], 'Configured public key changed')
    manifest = release.validate_manifest(release.parse(raw), allow_expired=allow_expired)
    require(manifest['signer_id'] == signer and manifest['channel'] == cfg['channel']
            and manifest['architecture'] == cfg['architecture'], 'Release does not match client trust/target')
    require(manifest['artifact']['size'] <= cfg['max_artifact_bytes'], 'Artifact exceeds client limit')
    return manifest


def check_sequence(state, manifest, raw):
    sequence = manifest['sequence']
    require(sequence >= state['highest_sequence'], 'Downgrade/replay rejected')
    if sequence == state['highest_sequence']:
        require(release.sha(raw) == state['highest_manifest'], 'Sequence reused for different manifest')
    state['highest_sequence'] = sequence
    state['highest_manifest'] = release.sha(raw)


def identity(manifest, raw):
    return f"r-{manifest['sequence']}-{release.sha(raw)}"


def verify_installed(root, selected, cfg, *, allow_expired=False):
    release.filename(selected)
    path = directory(root / 'releases' / selected)
    require(set(p.name for p in path.iterdir()) == {'manifest.json', 'manifest.sig', 'distribution.json',
                                                  'Noventra.AppDir'}, 'Unexpected installed entries')
    raw = release.read(path / 'manifest.json', release.MANIFEST_LIMIT)
    manifest = authenticated(cfg, raw, release.read(path / 'manifest.sig', 64), allow_expired=allow_expired)
    require(identity(manifest, raw) == selected, 'Installed release identity changed')
    dist = release.read(path / 'distribution.json', release.DISTRIBUTION_LIMIT)
    require(len(dist) == manifest['distribution']['size'] and release.sha(dist) == manifest['distribution']['sha256'],
            'Installed inventory changed')
    payload = release.validate_distribution(release.parse(dist), manifest, cfg['max_unpacked_bytes'])
    expected_dirs = {str(p) for name in payload for p in Path(name).parents if str(p) != '.'}
    seen = set()
    appdir = path / 'Noventra.AppDir'
    require(stat.S_ISDIR(appdir.lstat().st_mode), 'Invalid installed AppDir')
    for parent, dirs, files in os.walk(appdir, followlinks=False):
        for name in dirs + files:
            member = Path(parent) / name
            relative = member.relative_to(path).as_posix()
            mode = member.lstat().st_mode
            if stat.S_ISDIR(mode):
                require(relative in expected_dirs and not mode & 0o022, 'Unexpected/writable installed directory')
                continue
            require(relative in payload, 'Unexpected installed file')
            seen.add(relative)
            record = payload[relative]
            if relative == release.LINK:
                require(stat.S_ISLNK(mode) and os.readlink(member) == release.LINK_TARGET, 'Installed link changed')
            else:
                require(stat.S_ISREG(mode) and stat.S_IMODE(mode) == int(record['mode'], 8)
                        and member.lstat().st_nlink == 1, 'Installed file mode/link changed')
                count = 0
                hasher = hashlib.sha256()
                with release.open_regular(member) as stream:
                    while chunk := stream.read(1024 * 1024):
                        count += len(chunk)
                        require(count <= record['size'], 'Installed file grew')
                        hasher.update(chunk)
                require(count == record['size'] and hasher.hexdigest() == record['sha256'], 'Installed file changed')
    require(seen == set(payload), 'Installed payload incomplete')
    release.verify_package(path, manifest, payload)
    return manifest, raw


def space_required(cfg, manifest, payload, reserve):
    """Conservative staging estimate for this publisher's bounded tar format.

    The strict decoder still enforces max_unpacked_bytes while streaming. Space
    can change concurrently or be quota-limited; an I/O failure remains an error.
    """
    release.integer(reserve)
    expanded = sum(row.get('size', 0) for row in payload.values())
    blocks = sum(512 + ((row.get('size', 0) + 511) // 512) * 512
                 for row in payload.values())
    # The decoder permits one <=64KiB PAX header per entry plus end padding.
    tar_bound = min(cfg['max_unpacked_bytes'], blocks + (len(payload) + 2) * (65536 + 512) + 10240)
    return manifest['artifact']['size'] + expanded + tar_bound + reserve


def inspect_remote(cfg, root, state, stage, *, space_reserve=None):
    with tempfile.TemporaryDirectory(prefix='download-', dir=root / '.staging') as folder:
        temp = Path(folder)
        download(cfg, 'manifest.json', temp / 'manifest.json', release.MANIFEST_LIMIT)
        download(cfg, 'manifest.sig', temp / 'manifest.sig', 64, expected_size=64)
        raw = release.read(temp / 'manifest.json', release.MANIFEST_LIMIT)
        signature = release.read(temp / 'manifest.sig', 64)
        manifest = authenticated(cfg, raw, signature)
        check_sequence(state, manifest, raw)
        # Retain the highest authentic observation even if later downloads fail.
        atomic_state(root, state)
        selected = identity(manifest, raw)
        if not stage:
            return {'status': 'up_to_date' if selected == state['active'] else 'available',
                    'sequence': manifest['sequence'], 'version_label': manifest['version_label']}
        if (root / 'releases' / selected).exists():
            verify_installed(root, selected, cfg)
        else:
            item = manifest['distribution']
            download(cfg, item['filename'], temp / item['filename'], release.DISTRIBUTION_LIMIT,
                     expected_size=item['size'], expected_hash=item['sha256'])
            raw_dist = release.read(temp / 'distribution.json', release.DISTRIBUTION_LIMIT)
            payload = release.validate_distribution(release.parse(raw_dist), manifest, cfg['max_unpacked_bytes'])
            if space_reserve is not None:
                require(shutil.disk_usage(root).free >= space_required(cfg, manifest, payload, space_reserve),
                        'Not enough free disk space to safely download and prepare this update')
            item = manifest['artifact']
            download(cfg, item['filename'], temp / item['filename'], cfg['max_artifact_bytes'],
                     expected_size=item['size'], expected_hash=item['sha256'])
            prepared = temp / 'prepared'
            release.unpack(temp / manifest['artifact']['filename'], prepared, payload, cfg['max_unpacked_bytes'])
            release.verify_package(prepared, manifest, payload)
            for name, contents in (('manifest.json', raw), ('manifest.sig', signature), ('distribution.json', raw_dist)):
                with (prepared / name).open('xb') as stream:
                    stream.write(contents)
                    stream.flush()
                    os.fsync(stream.fileno())
            # Flush directory entries before making the completed release reachable.
            for parent, _, _ in os.walk(prepared, topdown=False, followlinks=False):
                sync_directory(parent)
            prepared.rename(root / 'releases' / selected)
        # A visible orphan can follow a successful rename and failed fsync.
        # Reusing it must redo this barrier before reporting staging success.
        sync_directory(root / 'releases')
        if selected == state['active']:
            return {'status': 'up_to_date', 'sequence': manifest['sequence']}
        state['staged'] = selected
        atomic_state(root, state)
        return {'status': 'staged', 'release': selected, 'sequence': manifest['sequence']}


def operate(cfg, operation):
    if operation == 'init':
        return initialize(cfg)
    with locked(cfg['install_root']) as root:
        state = read_state(root, cfg)
        if operation in ('check', 'stage'):
            return inspect_remote(cfg, root, state, operation == 'stage')
        if operation == 'activate':
            require(state['staged'] is not None, 'No staged release')
            manifest, raw = verify_installed(root, state['staged'], cfg)
            require(manifest['sequence'] == state['highest_sequence']
                    and release.sha(raw) == state['highest_manifest'], 'Staged release superseded')
            previous = state['active']
            state['active'] = state['staged']
            state['previous'] = previous
            state['staged'] = None
            atomic_state(root, state)
        elif operation == 'rollback':
            require(state['previous'] is not None, 'No previous release')
            # Explicit rollback may choose an expired, formerly trusted release;
            # its signature, complete installed inventory and key must still verify.
            verify_installed(root, state['previous'], cfg, allow_expired=True)
            state['active'], state['previous'] = state['previous'], state['active']
            state['staged'] = None
            atomic_state(root, state)
        elif operation == 'active-path':
            require(state['active'] is not None, 'No active release')
            verify_installed(root, state['active'], cfg, allow_expired=True)
        elif operation != 'status':
            raise ValueError('Unknown operation')
        result = {'status': operation, **state}
        if state['active']:
            result['active_path'] = str(root / 'releases' / state['active'] / 'Noventra.AppDir/AppRun')
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('operation', choices=('init', 'status', 'check', 'stage', 'activate', 'rollback', 'active-path'))
    args = parser.parse_args()
    try:
        result = operate(config(args.config), args.operation)
    except (ValueError, OSError, subprocess.SubprocessError, tarfile.TarError,
            lzma.LZMAError, http.client.HTTPException) as error:
        parser.exit(1, f'Local delivery: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
