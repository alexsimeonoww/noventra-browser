#!/usr/bin/env python3
"""Package an explicit preserved snapshot as an unsigned local Linux AppDir."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tarfile

from snapshot import REQUIRED, crashpad_handler, managed_schema_files
import release_manifest as release
import update_delivery as delivery

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'noventra/package'
ICONS = ROOT / 'noventra/resources/generated'
BINARY = 'noventra-browser'
REQUIRED_FILES = (set(REQUIRED) - {'chromedriver'}) | {
    BINARY, 'args.gn', 'provenance.json', 'LICENSE-Helium', 'LICENSE-Chromium',
    'initial_preferences', 'resources.pak.info', 'locales/en-US.pak',
}
OPTIONAL_FILES = {
    'chromedriver', 'xdg-mime', 'xdg-settings', 'locale-branding.json',
    'validation.json', 'noventra_crashpad_handler', 'helium_crashpad_handler',
    'headless_command_resources.pak',
}
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def relative(name):
    require(isinstance(name, str) and name and not name.startswith('/')
            and '\\' not in name and ':' not in name
            and not any(ord(c) < 32 or ord(c) == 127 for c in name)
            and all(part not in ('', '.', '..') for part in name.split('/')),
            f'Unsafe manifest path: {name!r}')
    return Path(name)


def digest(value):
    require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value),
            f'Invalid SHA-256: {value!r}')
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'Duplicate JSON key: {key!r}')
        result[key] = value
    return result


def json_object(data):
    def invalid_constant(value):
        raise ValueError(f'Invalid JSON constant: {value}')

    value = json.loads(data, object_pairs_hook=unique_object, parse_constant=invalid_constant)
    require(isinstance(value, dict), 'Expected a JSON object')
    return value


def regular(root, name):
    """Disallow symlinks in every component, not just at the final filename."""
    path = root
    parts = relative(name).parts
    for index, part in enumerate(parts):
        path = path / part
        mode = path.lstat().st_mode
        require(not stat.S_ISLNK(mode), f'Symlinks are not snapshot inputs: {path}')
        require(stat.S_ISREG(mode) if index == len(parts) - 1 else stat.S_ISDIR(mode),
                f'Not a regular file/directory: {path}')
    return path


def stamp(path):
    info = path.stat()
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns,
            info.st_ctime_ns, info.st_mode)


def transfer(root, name, target=None, expected=None):
    """Hash a regular input, optionally copying it to a new independent file."""
    path = regular(root, name)
    before = stamp(path)
    require(not before[-1] & (stat.S_ISUID | stat.S_ISGID),
            f'Privileged input permissions are unsupported: {name}')
    output = None
    hasher = hashlib.sha256()
    # O_NONBLOCK prevents a concurrent replacement with a FIFO from hanging.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as source:
        info = os.fstat(source.fileno())
        require(stat.S_ISREG(info.st_mode) and (info.st_dev, info.st_ino) == before[:2],
                f'Input changed while opening: {name}')
        try:
            if target is not None:
                target.parent.mkdir(parents=True, exist_ok=True)
                output = target.open('xb')
            while chunk := source.read(1024 * 1024):
                hasher.update(chunk)
                if output is not None:
                    output.write(chunk)
        finally:
            if output is not None:
                output.close()
    require(stamp(regular(root, name)) == before, f'Input changed while reading: {name}')
    actual = hasher.hexdigest()
    if expected is not None:
        require(actual == digest(expected), f'Snapshot hash mismatch: {name}')
    mode = 0o755 if before[-1] & 0o111 else 0o644
    if target is not None:
        target.chmod(mode)
    return {'sha256': actual, 'size': before[2], 'mode': oct(mode)}, before


class Inputs:
    def __init__(self):
        self.files = {}

    def add(self, root, name, expected=None):
        record, before = transfer(root, name, expected=expected)
        self.files[(root, name)] = (record, before)
        return record

    def read(self, root, name):
        record = self.add(root, name)
        data = regular(root, name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == record['sha256'],
                f'Input changed while parsing: {name}')
        return data

    def verify(self):
        for (root, name), (_, before) in self.files.items():
            require(stamp(regular(root, name)) == before,
                    f'Input changed before completion: {name}')


def reject_links(root):
    # Even an unlisted link is rejected. Never follow it or hash unlisted files.
    def failed_walk(error):
        raise error

    for directory, dirs, files in os.walk(root, followlinks=False, onerror=failed_walk):
        for name in dirs + files:
            path = Path(directory) / name
            mode = path.lstat().st_mode
            require(stat.S_ISREG(mode) or stat.S_ISDIR(mode),
                    f'Snapshot contains a symlink or special file: {path}')


def runtime_name(name):
    return (name in REQUIRED_FILES | OPTIONAL_FILES
            or re.fullmatch(r'(?:LICENSE|COPYING|NOTICE)(?:[-.][A-Za-z0-9_.-]+)?', name)
            or re.fullmatch(r'locales/[^/]+\.pak', name)
            or name.startswith('resources/'))


def architecture(args):
    assignments = {}
    for line in args.splitlines():
        line = line.split('#', 1)[0].strip()
        if re.match(r'target_(cpu|os)\s*=', line):
            match = re.fullmatch(r'target_(cpu|os)\s*=\s*"([a-z0-9]+)"', line)
            require(match is not None, 'args.gn requires literal target_cpu/target_os')
            key, value = match.groups()
            require(key not in assignments, f'Ambiguous target_{key} in args.gn')
            assignments[key] = value
    require(assignments.get('os', 'linux') == 'linux', 'Only Linux snapshots are supported')
    require(assignments.get('cpu') in ('x64', 'arm64'),
            'args.gn must explicitly declare target_cpu = "x64" or "arm64"')
    return {'x64': 'x86_64', 'arm64': 'aarch64'}[assignments['cpu']]


def validate_snapshot(source, inputs):
    reject_links(source)
    raw = inputs.read(source, 'snapshot.json')
    manifest = json_object(raw)
    require(manifest.get('binary') == BINARY, 'Expected binary=noventra-browser')
    files = manifest.get('files')
    require(isinstance(files, dict) and files, 'Snapshot requires a nonempty files object')
    for name, expected in files.items():
        relative(name)
        digest(expected)
        require(runtime_name(name), f'Not a supported runtime/license file: {name}')
    missing = REQUIRED_FILES - files.keys()
    require(not missing, f'Incomplete snapshot; missing declared files: {sorted(missing)}')
    for name, expected in files.items():
        inputs.add(source, name, expected)
    require(regular(source, BINARY).stat().st_mode & 0o111, 'Browser must be executable')
    helper = crashpad_handler(source, BINARY).name
    require(helper in files, f'Required crashpad helper is not declared: {helper}')
    # Validate the loose managed schema against the packaged resource, never
    # against today's mutable Chromium output or third-party source tree.
    for name, content in managed_schema_files(source).items():
        require(name in files and hashlib.sha256(content).hexdigest() == files[name],
                f'Missing or inconsistent declared managed resource: {name}')
    provenance = json_object(regular(source, 'provenance.json').read_bytes())
    require(provenance, 'Snapshot provenance must be nonempty')
    json_object(regular(source, 'initial_preferences').read_bytes())
    for name in ('LICENSE-Helium', 'LICENSE-Chromium'):
        require(regular(source, name).stat().st_size, f'Empty license: {name}')
    arch = architecture(regular(source, 'args.gn').read_text())
    return manifest, raw, arch, helper


def package(source, output, version, *, archive=False, update_config=None):
    require(isinstance(version, str)
            and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,95}', version),
            'Supply a safe explicit local version label (letters, digits, . _ + -)')
    source = Path(source).resolve(strict=True)
    require(source.is_dir(), 'Source must be a preserved snapshot directory')
    requested = Path(output).absolute()
    require(not os.path.lexists(requested), f'Output already exists: {requested}')
    output = requested.parent.resolve(strict=True) / requested.name
    for protected in (source, ASSETS, ICONS):
        protected = protected.resolve()
        require(not output.is_relative_to(protected) and not protected.is_relative_to(output),
                'Output must be separate from the snapshot and packaging inputs')

    inputs = Inputs()
    manifest, raw, arch, helper = validate_snapshot(source, inputs)
    asset_names = ('AppRun', 'install.sh', 'uninstall.sh', 'installer.py',
                   'noventra-wrapper.sh', 'video-buffer-compatibility.sh', 'noventra-browser.desktop',
                   'local.noventra.browser.metainfo.xml', 'apparmor.cfg', 'CREDITS.md', 'README.md')
    for name in asset_names:
        inputs.add(ASSETS, name)
    updates = None
    if update_config is not None:
        config_path = Path(update_config).resolve(strict=True)
        value = json_object(inputs.read(config_path.parent, config_path.name))
        release.keys(value, ('format', 'base_url', 'channel', 'sequence', 'public_key'))
        require(type(value['format']) is int and value['format'] == 1,
                'Unsupported public update configuration')
        release.integer(value['sequence'], 1)
        require(isinstance(value['public_key'], str), 'Supply a public key path')
        key = Path(value['public_key'])
        require(key.is_absolute(), 'Supply an absolute independently trusted public key path')
        key_data = inputs.read(key.parent, key.name)
        require(re.fullmatch(rb'\s*-----BEGIN PUBLIC KEY-----\s+[A-Za-z0-9+/=\r\n]+-----END PUBLIC KEY-----\s*',
                             key_data) is not None,
                'Updates require a public-only PEM key; private material is never a packaging input')
        client = {key: value[key] for key in ('format', 'base_url', 'channel', 'public_key')}
        client.update(architecture=arch, install_root='/unused',
                      max_artifact_bytes=2 * 1024**3, max_unpacked_bytes=4 * 1024**3)
        require(isinstance(client['base_url'], str) and client['base_url'].endswith('/')
                and isinstance(client['channel'], str)
                and re.fullmatch('[a-z][a-z0-9-]{0,31}', client['channel']),
                'Invalid update feed/channel')
        delivery.origin(client['base_url'])
        public = release.openssl('pkey', '-pubin', '-in', key, '-pubout')
        signer = release.sha(release.ed25519(release.public_der(key)))
        updates = {'format': 1, 'base_url': value['base_url'], 'channel': value['channel'],
                   'architecture': arch, 'sequence': value['sequence'],
                   'public_key': 'release-public.pem'}
        inputs.add(ASSETS, 'desktop_updater.py')
        for name in ('release_manifest.py', 'update_delivery.py'):
            inputs.add(ROOT / 'tools/noventra', name)
    icon_manifest = json_object(inputs.read(ICONS, 'manifest.json'))
    require(icon_manifest.get('desktop_source') == 'approved/noventra-symbol.png',
            'Expected the approved Noventra desktop icon source')
    symbol_hash = digest(icon_manifest['sources']['noventra-symbol.png']['sha256'])
    inputs.add(ICONS.parent / 'approved', 'noventra-symbol.png', symbol_hash)
    for size in ICON_SIZES:
        name = f'linux/hicolor/{size}x{size}/apps/noventra-browser.png'
        inputs.add(ICONS, name, icon_manifest['files'][name])
    inputs.verify()

    # Exclusive output creation: a failure leaves an incomplete directory for
    # inspection, with no distribution.json completion marker. Never overwrite.
    output.mkdir()
    appdir = output / 'Noventra.AppDir'
    runtime = appdir / 'opt/noventra'
    inventory = {}

    def copy(root, name, target, mode=None):
        record, _ = transfer(root, name, target, inputs.files[(root, name)][0]['sha256'])
        if mode is not None:
            target.chmod(mode)
            record['mode'] = oct(mode)
        inventory[target.relative_to(output).as_posix()] = record

    def write(target, data):
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as stream:
            stream.write(data)
        target.chmod(0o644)
        inventory[target.relative_to(output).as_posix()] = {
            'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data), 'mode': '0o644',
        }

    for name in sorted(manifest['files']):
        copy(source, name, runtime / name)
    copy(source, 'snapshot.json', runtime / 'snapshot.json')
    copy(ASSETS, 'noventra-wrapper.sh', runtime / 'noventra-wrapper', 0o755)
    copy(ASSETS, 'video-buffer-compatibility.sh', runtime / 'video-buffer-compatibility.sh', 0o644)
    copy(ASSETS, 'AppRun', appdir / 'AppRun', 0o755)
    for name in ('install.sh', 'uninstall.sh'):
        copy(ASSETS, name, appdir / name, 0o755)
    copy(ASSETS, 'installer.py', appdir / 'installer.py', 0o644)
    if updates is not None:
        copy(ASSETS, 'desktop_updater.py', appdir / 'updater/desktop_updater.py', 0o644)
        for name in ('release_manifest.py', 'update_delivery.py'):
            copy(ROOT / 'tools/noventra', name, appdir / 'updater' / name, 0o644)
        write(appdir / 'updater/config.json', release.encode(updates))
        write(appdir / 'updater/release-public.pem', public)
    # Native DIR_EXTERNAL_EXTENSIONS queries create this directory if absent.
    # An inventoried non-JSON marker preserves it through the file-only archive
    # and delivery verifier without installing or disabling any extension.
    extensions = runtime / 'extensions'
    extensions.mkdir(mode=0o755)
    extensions.chmod(0o755)  # Keep package permissions independent of caller umask.
    write(extensions / 'README.noventra.txt',
          b'This directory is reserved for the native external-extension path.\n'
          b'No extension manifests or extension payloads are shipped here.\n'
          b'This non-JSON file keeps the directory in the package inventory and archive.\n')
    for directory in ('', 'usr/share/applications'):
        copy(ASSETS, 'noventra-browser.desktop', appdir / directory / 'noventra-browser.desktop')
    copy(ASSETS, 'local.noventra.browser.metainfo.xml',
         appdir / 'usr/share/metainfo/local.noventra.browser.metainfo.xml')
    for name in ('CREDITS.md', 'README.md', 'apparmor.cfg'):
        copy(ASSETS, name, appdir / 'usr/share/doc/noventra' / name)
    copy(ICONS, 'manifest.json', appdir / 'usr/share/doc/noventra/branding-manifest.json')
    for size in ICON_SIZES:
        name = f'linux/hicolor/{size}x{size}/apps/noventra-browser.png'
        copy(ICONS, name, appdir / 'usr/share/icons' / name.removeprefix('linux/'))
    for name in ('noventra-browser.png', '.DirIcon'):
        copy(ICONS, 'linux/hicolor/256x256/apps/noventra-browser.png', appdir / name)
    launcher = appdir / 'usr/bin/noventra-browser'
    launcher.parent.mkdir(parents=True)
    launcher.symlink_to('../../opt/noventra/noventra-wrapper')
    inventory[launcher.relative_to(output).as_posix()] = {
        'symlink': '../../opt/noventra/noventra-wrapper',
    }
    metadata = {
        'format': 1, 'id': 'local.noventra.browser', 'name': 'Noventra',
        'version_label': version, 'version_origin': 'explicit local packaging label',
        'architecture': arch, 'platform': 'linux', 'unsigned': True,
        'snapshot_sha256': hashlib.sha256(raw).hexdigest(),
        'crashpad_helper': helper, 'approved_symbol_sha256': symbol_hash,
        'source_snapshot': str(source),
    }
    if updates is not None:
        metadata['updates'] = {'base_url': updates['base_url'], 'channel': updates['channel'],
                               'minimum_sequence': updates['sequence'], 'signer_id': signer}
    write(appdir / 'noventra-package.json', (json.dumps(metadata, indent=2) + '\n').encode())
    # This inner inventory travels inside the archive and lets the standalone
    # per-user installer validate every AppDir input without this checkout.
    install_manifest = {'format': 1, 'id': metadata['id'], 'files': {
        str(Path(name).relative_to('Noventra.AppDir')): record
        for name, record in inventory.items()
    }}
    write(appdir / 'install-manifest.json',
          (json.dumps(install_manifest, indent=2, sort_keys=True) + '\n').encode())
    if archive:
        tarpath = output / f'noventra-{version}-{arch}_linux.tar.xz'
        # Add an explicit inventory only; tarfile must not recurse over an
        # unexpected file or dereference even our generated internal symlink.
        with tarfile.open(tarpath, 'x:xz', dereference=False) as bundle:
            for name in sorted(inventory):
                bundle.add(output / name, arcname=name, recursive=False)
        tarpath.chmod(0o644)
        inventory[tarpath.name], _ = transfer(output, tarpath.name)
    inputs.verify()
    result = {**metadata, 'complete': True, 'files': inventory}
    temporary = output / '.distribution.json.tmp'
    with temporary.open('x') as stream:
        stream.write(json.dumps(result, indent=2, sort_keys=True) + '\n')
    temporary.chmod(0o644)
    # The final filesystem operation marks the entire AppDir/archive complete.
    temporary.replace(output / 'distribution.json')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Explicit preserved snapshot')
    parser.add_argument('--output', type=Path, required=True, help='New directory; parent must exist')
    parser.add_argument('--version', required=True, help='Known local label; never inferred from a tree')
    parser.add_argument('--tar', action='store_true', help='Also archive the complete AppDir as tar.xz')
    parser.add_argument('--update-config', type=Path,
                        help='Explicit public HTTPS feed/channel/sequence and trusted public-key JSON')
    args = parser.parse_args()
    try:
        result = package(args.source, args.output, args.version, archive=args.tar,
                         update_config=args.update_config)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'Packaging failed: {error}\n')
    print(json.dumps({'output': str(args.output.resolve()),
                      'completion': 'distribution.json',
                      'version_label': result['version_label'],
                      'architecture': result['architecture']}, indent=2))


if __name__ == '__main__':
    main()
