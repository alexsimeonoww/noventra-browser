"""Disposable, explicitly configured packages and legacy publication witnesses."""

from pathlib import Path
import tarfile

import publish_release as public
import release_manifest as release

FEED = 'https://downloads.noventrastudio.com/browser/updates/stable/linux-x86_64/'


def update_configuration(path, key, sequence=1):
    path.write_bytes(release.encode({'format': 1, 'base_url': FEED, 'channel': 'stable',
                                    'sequence': sequence, 'public_key': str(key)}))
    return path


def rewrite_updater(distribution, *, missing=(), config_changes=None, config_raw=None, key_data=None):
    """Produce a correctly inventoried archive with deliberately bad update data."""
    distribution = Path(distribution)
    folder = distribution / release.APP / 'updater'
    if config_changes is not None:
        config = release.parse((folder / 'config.json').read_bytes())
        (folder / 'config.json').write_bytes(release.encode({**config, **config_changes}))
    if config_raw is not None:
        (folder / 'config.json').write_bytes(config_raw)
    if key_data is not None:
        (folder / 'release-public.pem').write_bytes(key_data)
    for name in missing:
        (folder / name).unlink()
    dist = release.parse((distribution / 'distribution.json').read_bytes())
    archive = next(distribution.glob('*.tar.xz'))
    archive.unlink()
    files = {}
    for path in sorted((distribution / release.APP).rglob('*')):
        if path.is_symlink():
            files[path.relative_to(distribution).as_posix()] = {'symlink': release.LINK_TARGET}
        elif path.is_file():
            files[path.relative_to(distribution).as_posix()] = {
                'sha256': public.file_hash(path), 'size': path.stat().st_size,
                'mode': oct(path.stat().st_mode & 0o777)}
    inner = distribution / release.APP / 'install-manifest.json'
    inner.write_bytes(release.encode({'format': 1, 'id': release.PRODUCT, 'files': {
        str(Path(name).relative_to(release.APP)): record for name, record in files.items()
        if name != release.APP + 'install-manifest.json'}}))
    files[release.APP + 'install-manifest.json'] = {
        'sha256': public.file_hash(inner), 'size': inner.stat().st_size, 'mode': '0o644'}
    with tarfile.open(archive, 'x:xz', dereference=False) as bundle:
        for name in sorted(files):
            bundle.add(distribution / name, arcname=name, recursive=False)
    archive.chmod(0o644)
    files[archive.name] = {'sha256': public.file_hash(archive), 'size': archive.stat().st_size, 'mode': '0o644'}
    (distribution / 'distribution.json').write_bytes(release.encode({**dist, 'files': files}))


def legacy_prepared(distribution, output, key, source, info, sequence=1):
    """Emulate a previously prepared signed release, without the new public gate.

    This uses only disposable test keys and proves revalidation rejects actual
    well-signed legacy files rather than failing an unrelated hash check first.
    """
    release.publish(distribution, output, key, sequence, channel='stable', max_unpacked_bytes=32 * 1024**2)
    raw = (output / 'manifest.json').read_bytes()
    manifest = release.parse(raw)
    source_raw, value = public.source_info(info, source, manifest)
    catalog, plan = public.plan_for(manifest, value['source_archive'], raw)
    release.copy_checked(source, output / source.name, value['source_archive']['size'],
                         value['source_archive']['sha256'])
    (output / 'source-info.json').write_bytes(source_raw)
    (output / 'artifact-index.json').write_bytes(release.encode(manifest['artifact']))
    (output / 'source-index.json').write_bytes(release.encode(value['source_archive']))
    (output / 'channel.json').write_bytes(release.encode(catalog))
    (output / 'publication.json').write_bytes(release.encode({
        'format': 1, 'catalog': catalog, 'catalog_sha256': public.file_hash(output / 'channel.json'),
        'objects': {name: {'key': target, **public.file_record(output / name)} for name, target in plan.items()}}))
