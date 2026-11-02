"""Preserve a completed Linux build for local comparisons, without hard links."""

import argparse
import gzip
import hashlib
import json
import mmap
from pathlib import Path
import shutil
import struct
import subprocess

REQUIRED = (
    'chrome_100_percent.pak', 'chrome_200_percent.pak',
    'chromedriver', 'icudtl.dat',
    'libEGL.so', 'libGLESv2.so', 'libqt5_shim.so', 'libqt6_shim.so',
    'libvk_swiftshader.so', 'libvulkan.so.1', 'product_logo_256.png',
    'resources.pak', 'v8_context_snapshot.bin', 'vk_swiftshader_icd.json',
)

CRASHPAD_HANDLERS = ('noventra_crashpad_handler', 'helium_crashpad_handler')


def crashpad_handler(source, binary):
    """Find the Linux helper required by this executable, including old builds.

    A renamed browser may still use the Helium helper; mutable build outputs
    may contain both names. Inspect the embedded, NUL-terminated lookup rather
    than guessing from the browser filename or choosing whichever file exists.
    mmap avoids loading the entire browser into Python memory.
    """
    with (source / binary).open('rb') as stream:
        if not stream.seek(0, 2):
            raise ValueError('Empty browser executable')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
            names = [name for name in CRASHPAD_HANDLERS
                     if image.find(name.encode('ascii') + b'\0') != -1]
    if len(names) != 1:
        raise ValueError(f'Expected exactly one embedded crashpad helper name: {names}')
    helper = source / names[0]
    if not helper.is_file():
        raise FileNotFoundError(f'Missing required crashpad helper: {helper}')
    metadata = helper.stat()
    if not metadata.st_size or not (metadata.st_mode & 0o111):
        raise ValueError(f'Crashpad helper must be a nonempty executable: {helper}')
    return helper


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def _pack_resources(data):
    """Read GRIT v4/v5 packs without requiring a prepared Chromium Python tree.

    Layout: tools/grit/grit/format/data_pack.py. Keep aliases: a schema can
    share its payload with another resource after GRIT deduplication.
    """
    try:
        version, = struct.unpack_from('<I', data)
        if version == 4:
            count, encoding = struct.unpack_from('<IB', data, 4)
            header_size, alias_count = 9, 0
        elif version == 5:
            encoding, count, alias_count = struct.unpack_from('<BxxxHH', data, 4)
            header_size = 12
        else:
            raise ValueError(f'Unsupported resource pack version: {version}')
        table_end = header_size + (count + 1) * 6 + alias_count * 4
        if encoding not in (0, 1, 2) or table_end > len(data):
            raise ValueError('Invalid resource pack header')
        entries = [struct.unpack_from('<HI', data, header_size + i * 6)
                   for i in range(count + 1)]
        ids = [entry[0] for entry in entries[:-1]]
        offsets = [entry[1] for entry in entries]
        if (ids != sorted(set(ids)) or 0 in ids or entries[-1][0] != 0
                or offsets != sorted(offsets) or offsets[0] != table_end
                or offsets[-1] != len(data)):
            raise ValueError('Invalid resource pack index')
        resources = {resource_id: data[start:entries[i + 1][1]]
                     for i, (resource_id, start) in enumerate(entries[:-1])}
        for i in range(alias_count):
            resource_id, index = struct.unpack_from(
                '<HH', data, header_size + (count + 1) * 6 + i * 4)
            if not resource_id or resource_id in resources or index >= count:
                raise ValueError('Invalid resource pack alias')
            resources[resource_id] = resources[entries[index][0]]
        return resources
    except struct.error as error:
        raise ValueError('Truncated resource pack') from error


def _validate_managed_schema(schema):
    """Validate uBlock's policy-schema subset; fail closed on new vocabulary.

    Chromium's policy::Schema requires object roots, typed property schemas
    and one schema for array items. Do not silently accept constraints that
    this packager cannot validate; extend this subset with tests when needed.
    """
    def validate(node):
        if not isinstance(node, dict):
            raise ValueError('Managed schema nodes must be objects')
        kind = node.get('type')
        if kind not in ('object', 'array', 'string', 'boolean', 'integer', 'number'):
            raise ValueError('Invalid or unsupported managed schema type')
        allowed = {'type', 'title', 'description', '$schema'}
        for key in ('title', 'description', '$schema'):
            if key in node and not isinstance(node[key], str):
                raise ValueError(f'Invalid managed schema annotation: {key}')
        if kind == 'object':
            allowed.add('properties')
            properties = node.get('properties', {})
            if not isinstance(properties, dict):
                raise ValueError('Managed schema properties must be an object')
            for child in properties.values():
                validate(child)
        elif kind == 'array':
            allowed.add('items')
            validate(node.get('items'))
        if node.keys() - allowed:
            raise ValueError(f'Unsupported managed schema attributes: {sorted(node.keys() - allowed)}')

    validate(schema)
    if schema['type'] != 'object':
        raise ValueError('Managed schema root must have type object')


def managed_schema_files(runtime, resource_build=None):
    """Return validated loose schema bytes and their resource-ID metadata.

    This is read-only, for callers assembling a NEW snapshot. Read the schema
    from the packaged resource itself, never from current third_party sources.
    Normally resources.pak.info lives beside resources.pak. An older snapshot
    missing it may use resource_build's metadata only if both packs match
    byte-for-byte. Persist/hash every returned file before marking a package
    complete. Uncompressed and gzip resources are supported; others fail closed.
    """
    runtime = Path(runtime)
    resource_build = Path(resource_build) if resource_build is not None else runtime
    packed = (runtime / 'resources.pak').read_bytes()
    if (resource_build.resolve() != runtime.resolve()
            and (resource_build / 'resources.pak').read_bytes() != packed):
        raise ValueError('Resource metadata requires an identical resources.pak')
    info = (resource_build / 'resources.pak.info').read_bytes()
    ids = {}
    for line in info.decode('utf-8').splitlines():
        symbol, resource_id, _ = line.split(',', 2)
        if symbol.startswith('IDR_UBLOCK_'):
            if symbol in ids:
                raise ValueError(f'Duplicate resource metadata: {symbol}')
            ids[symbol] = int(resource_id)
    resources = _pack_resources(packed)

    def resource(path):
        # ui/webui/resources/tools/generate_grd.py's filename-to-ID convention.
        symbol = 'IDR_UBLOCK_' + path.upper().replace('/', '_').replace('.', '_') \
            .replace('-', '_').replace('@', '_AT_')
        if symbol not in ids or ids[symbol] not in resources:
            raise ValueError(f'Missing bundled uBlock resource: {path}')
        data = resources[ids[symbol]]
        try:
            if data.startswith(b'\x1f\x8b'):
                data = gzip.decompress(data)
            parsed = json.loads(data.decode('utf-8'))
        except (ValueError, OSError, EOFError) as error:
            raise ValueError(f'Invalid or unsupported bundled uBlock JSON: {path}') from error
        return data, parsed

    _, manifest = resource('manifest.json')
    if (not isinstance(manifest, dict) or manifest.get('name') != 'uBlock Origin'
            or not isinstance(manifest.get('version'), str)
            or manifest.get('manifest_version') not in (2, 3)):
        raise ValueError('Resource metadata does not identify a uBlock manifest')
    storage = manifest.get('storage', {})
    if not isinstance(storage, dict):
        raise ValueError('Invalid uBlock storage declaration')
    files = {'resources.pak.info': info}
    if 'managed_schema' in storage:
        path = storage['managed_schema']
        if (not isinstance(path, str) or not path.isascii() or '\\' in path or '\x00' in path
                or any(part in ('', '.', '..') for part in path.split('/'))):
            raise ValueError('storage.managed_schema must be a safe relative file path')
        content, schema = resource(path)
        _validate_managed_schema(schema)
        files['resources/ublock/' + path] = content
    return files


def snapshot(source, destination, binary, project_root=None, resource_build=None):
    if binary not in ('helium', 'noventra-browser'):
        raise ValueError('Expected a supported Linux browser executable name')
    if destination.exists():
        raise FileExistsError(f'Refusing to replace snapshot: {destination}')
    files = [source / name for name in (*REQUIRED, binary, 'args.gn')]
    helper = crashpad_handler(source, binary)
    files.append(helper)
    locales = sorted((source / 'locales').glob('*.pak'))
    if not locales:
        raise FileNotFoundError('Build has no locale packs')
    files.extend(locales)
    for name in ('xdg-mime', 'xdg-settings', 'initial_preferences'):
        if (source / name).is_file():
            files.append(source / name)
    files.extend(path for path in sorted((source / 'resources').rglob('*')) if path.is_file())
    files = {str(path.relative_to(source)): path for path in files}
    if project_root is not None:
        files['LICENSE-Helium'] = project_root / 'LICENSE'
        files['LICENSE-Chromium'] = project_root / 'build/src/LICENSE'
        if binary == 'noventra-browser':
            # Package the versioned first-run defaults, never a user's profile
            # or a stale file left beside the mutable build output.
            files['initial_preferences'] = (
                project_root / 'noventra/resources/initial_preferences.json')
    for path in files.values():
        if not path.is_file():
            raise FileNotFoundError(f'Incomplete build: {path}')
    schema_files = managed_schema_files(source, resource_build=resource_build)
    destination.mkdir(parents=True, exist_ok=False)
    manifest = {'source': str(source.resolve()), 'binary': binary, 'files': {}}
    # Deliberately copy: relinking or regenerating build outputs must not alter
    # the baseline. snapshot.json is written last, marking a completed copy.
    for relative, path in files.items():
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        manifest['files'][str(relative)] = sha256(target)
    for relative, content in schema_files.items():
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        manifest['files'][relative] = sha256(target)
    if managed_schema_files(destination) != schema_files:
        raise ValueError('Bundled managed schema changed while copying the snapshot')
    if crashpad_handler(destination, binary).name != helper.name:
        raise ValueError('Required crashpad helper changed while copying the snapshot')
    if binary == 'noventra-browser':
        from locale_branding import normalize_locales
        report = normalize_locales(destination)
        for name, changed in report.items():
            if changed:
                relative = 'locales/' + name
                manifest['files'][relative] = sha256(destination / relative)
        manifest['files']['locale-branding.json'] = sha256(destination / 'locale-branding.json')
    if project_root is not None:
        provenance = {}
        for name, path in [('linux', project_root), ('helium', project_root / 'helium-chromium'),
                           ('chromium', project_root / 'build/src')]:
            provenance[name] = subprocess.check_output(
                ['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        provenance_path = destination / 'provenance.json'
        provenance_path.write_text(json.dumps(provenance, indent=2) + '\n')
        manifest['files']['provenance.json'] = sha256(provenance_path)
    (destination / 'snapshot.json').write_text(json.dumps(manifest, indent=2) + '\n')


def main():
    from build_storage import check_storage

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', choices=('helium', 'noventra-browser'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    check_storage(root)
    destination = args.output.resolve()
    if not destination.is_relative_to(root / 'build'):
        parser.error('Snapshots must stay inside the mounted project build volume')
    snapshot(root / 'build/src/out/Default', destination, args.binary, project_root=root)
    print(f'Preserved local build: {destination}')


if __name__ == '__main__':
    main()
