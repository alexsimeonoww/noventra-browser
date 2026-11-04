#!/usr/bin/env python3
"""Verify, draft and explicitly publish an already-qualified signed release.

This never creates repositories/tags, changes remotes, builds Chromium, signs
artifacts, reads private keys or overwrites release assets. GitHub CLI supplies
its own authentication. Cloudflare remains the browser's signed update origin.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import urllib.parse
import zipfile

import publish_release as public
import release_manifest as release

MAX_REPLY = 32 * 1024**2
MAX_KIT = 512 * 1024**2
MAX_FILE = 64 * 1024**2
KIT_ROOT = 'Noventra-linux-source-build-kit/'
IDENTITY = 'source-identity.json'
PROVENANCE = 'noventra-release-provenance.json'
QUALIFICATION = 'noventra-qualification.json'
CHECKSUMS = 'SHA256SUMS'


def repository(value):
    release.require(isinstance(value, str) and re.fullmatch(
        r'[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9][A-Za-z0-9_.-]{0,99}', value),
        'Use an explicit OWNER/REPOSITORY on github.com')
    return value


def commit_sha(value):
    release.require(isinstance(value, str) and re.fullmatch('[a-f0-9]{40}', value),
                    'An explicit full source commit SHA is required')
    return value


def tag_name(value):
    release.require(isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._/-]{0,150}', value)
                    and '..' not in value and '//' not in value
                    and not value.endswith(('/', '.', '.lock')),
                    'Unsafe or ambiguous release tag')
    return value


def git_blob(data):
    return hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()


def mapped(name, prepared_prefix, build_prefix):
    if name.startswith('prepared-source/'):
        tail, prefix = name.removeprefix('prepared-source/'), prepared_prefix
    elif name.startswith('build-inputs/'):
        tail, prefix = name.removeprefix('build-inputs/'), build_prefix
    else:
        raise ValueError('Source inventory contains an unsupported path')
    release.relative(tail)
    if prefix:
        release.relative(prefix)
    result = '/'.join(p for p in (prefix, tail) if p)
    release.relative(result)
    return result


def source_tree(path, catalog, manifest, public_key, *, prepared_prefix='prepared-source',
                build_prefix='', identity_path='noventra/source-identity.json'):
    """Bind every ZIP inventory byte to its expected committed Git blob/mode."""
    release.relative(identity_path)
    expected = {}
    key_digest = public.file_hash(public_key)
    public_key_paths = []
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        release.require(2 <= len(entries) <= 20002
                        and sum(i.file_size for i in entries) <= MAX_KIT,
                        'Source kit exceeds inspection limits')
        by_name = {}
        for item in entries:
            release.require(item.filename.startswith(KIT_ROOT) and not item.is_dir(),
                            'Unexpected source ZIP root or directory entry')
            name = item.filename.removeprefix(KIT_ROOT)
            release.relative(name)
            mode = item.external_attr >> 16
            release.require(name not in by_name and stat.S_IFMT(mode) in (0, stat.S_IFREG)
                            and 0 <= item.file_size <= MAX_FILE, 'Unsafe or duplicate source ZIP entry')
            by_name[name] = item
        release.require(IDENTITY in by_name and 'README.md' in by_name, 'Missing generated source kit metadata')
        raw_identity = archive.read(by_name[IDENTITY])
        identity = release.parse(raw_identity)
        release.keys(identity, ('format', 'product', 'version_label', 'snapshot_sha256', 'engine',
                                'source_freeze_sha256', 'layer_manifest_sha256', 'files'))
        release.require(identity['format'] == 1 and identity['product'] == release.PRODUCT
                        and identity['version_label'] == manifest['version_label']
                        and identity['snapshot_sha256'] == manifest['snapshot_sha256'],
                        'Source kit identity differs from the signed binary snapshot')
        release.digest(identity['source_freeze_sha256'])
        release.digest(identity['layer_manifest_sha256'])
        release.require(isinstance(identity['engine'], dict) and identity['engine'], 'Missing upstream source pins')
        files = identity['files']
        release.require(isinstance(files, dict) and 0 < len(files) <= 20000
                        and set(by_name) == set(files) | {IDENTITY, 'README.md'},
                        'Unlisted or missing source kit entries')
        for name, record in files.items():
            release.keys(record, ('sha256', 'bytes', 'mode'))
            release.digest(record['sha256'])
            release.integer(record['bytes'], 0, MAX_FILE)
            release.integer(record['mode'], 0, 0o777)
            item = by_name[name]
            release.require(item.file_size == record['bytes']
                            and stat.S_IMODE(item.external_attr >> 16) == record['mode'],
                            'Source kit file size/mode differs from inventory')
            data = archive.read(item)
            release.require(release.sha(data) == record['sha256'], 'Source kit file hash differs from inventory')
            target = mapped(name, prepared_prefix, build_prefix)
            release.require(target not in expected, 'Source paths map to the same repository path')
            expected[target] = {'sha': git_blob(data), 'type': 'blob',
                                'mode': '100755' if record['mode'] & 0o111 else '100644'}
            if record['sha256'] == key_digest:
                public_key_paths.append(target)
        release.require('LICENSE' in expected, 'Commit the inventoried inherited LICENSE at repository root')
        release.require(public_key_paths, 'The committed source kit must include the exact approved public PEM')
        release.require(identity_path not in expected, 'Source identity path collides with inventoried source')
        expected[identity_path] = {'sha': git_blob(raw_identity), 'type': 'blob', 'mode': '100644'}
    release.require(public.file_record(path) == catalog['source'], 'Source kit changed during inspection')
    return identity, expected, {'sha256': release.sha(raw_identity),
                                'path': identity_path, 'files': len(files),
                                'public_key_paths': sorted(public_key_paths)}


def qualification(path, manifest, distribution, identity):
    raw = release.read(path, release.DISTRIBUTION_LIMIT)
    value = release.parse(raw)
    binary = distribution['files'][release.RUNTIME + 'noventra-browser']['sha256']
    resources = distribution['files'][release.RUNTIME + 'resources.pak']['sha256']
    release.require(value.get('passed') is True and value.get('binary_sha256') == binary
                    and value.get('resources_sha256') == resources
                    and value.get('source_freeze_sha256') == identity['source_freeze_sha256'],
                    'Qualification does not identify this source and binary/resources')
    functional = release.integer(value.get('functional_checks'), 1)
    optical = release.integer(value.get('optical_checks', 0))
    return {'format': 1, 'passed': True, 'platform': 'linux', 'architecture': 'x86_64',
            'version_label': manifest['version_label'], 'snapshot_sha256': manifest['snapshot_sha256'],
            'binary_sha256': binary, 'resources_sha256': resources,
            'source_freeze_sha256': identity['source_freeze_sha256'],
            'functional_checks': functional, 'optical_checks': optical,
            'original_receipt_sha256': release.sha(raw)}


class GitHub:
    """Small fixed-host gh API adapter; no shell or token access."""

    def request(self, method, endpoint, *, body=None, file=None, missing=False):
        release.require(endpoint.startswith('/repos/') or endpoint.startswith('https://uploads.github.com/repos/'),
                        'Unsupported GitHub endpoint')
        command = ['gh', 'api', '--hostname', 'github.com', '--include', '--method', method,
                   '-H', 'X-GitHub-Api-Version: 2022-11-28', endpoint]
        data = None
        if file is not None:
            release.regular(file)
            command += ['-H', 'Content-Type: application/octet-stream', '--input', str(file)]
        else:
            command += ['-H', 'Accept: application/vnd.github+json']
            if body is not None:
                command += ['--input', '-']
                data = release.encode(body)
        # gh owns authentication. Response/error streams stay in private unnamed
        # files, and errors never echo remote text or credentials into receipts.
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            process = subprocess.run(command, input=data, stdout=out, stderr=err, timeout=300)
            release.require(out.tell() <= MAX_REPLY, 'GitHub response exceeds inspection limit')
            out.seek(0)
            raw = out.read()
        split = re.split(b'\r?\n\r?\n', raw, maxsplit=1)
        release.require(len(split) == 2 and split[0].startswith(b'HTTP/'), 'Invalid GitHub CLI API response')
        first = split[0].splitlines()[0].split()
        release.require(len(first) >= 2 and first[1].isdigit(), 'Invalid GitHub response status')
        status = int(first[1])
        if status == 404 and missing:
            return None
        release.require(process.returncode == 0 and 200 <= status < 300,
                        f'GitHub {method} request failed (HTTP {status}); inspect remote state before retrying')
        try:
            return json.loads(split[1])
        except (ValueError, UnicodeError) as error:
            raise ValueError('Invalid GitHub JSON response') from error

    def assets(self, repo, release_id):
        values = []
        for page in range(1, 21):
            result = self.request('GET', f'/repos/{repo}/releases/{release_id}/assets?per_page=100&page={page}')
            release.require(isinstance(result, list), 'Invalid GitHub release asset list')
            values.extend(result)
            if len(result) < 100:
                return values
        raise ValueError('GitHub release asset list exceeds limit')


def tag_commit(api, repo, tag):
    encoded = urllib.parse.quote(tag, safe='')
    ref = api.request('GET', f'/repos/{repo}/git/ref/tags/{encoded}')
    release.require(isinstance(ref, dict) and ref.get('ref') == 'refs/tags/' + tag, 'Unexpected tag reference')
    obj = ref.get('object', {})
    for _ in range(8):
        digest = commit_sha(obj.get('sha'))
        if obj.get('type') == 'commit':
            return digest
        release.require(obj.get('type') == 'tag', 'Tag does not resolve to a commit')
        value = api.request('GET', f'/repos/{repo}/git/tags/{digest}')
        obj = value.get('object', {})
    raise ValueError('Excess nested annotated tags')


def remote_source(api, repo, commit, tag, expected):
    meta = api.request('GET', f'/repos/{repo}')
    release.require(isinstance(meta, dict) and meta.get('full_name', '').lower() == repo.lower()
                    and meta.get('private') is False and not meta.get('archived') and not meta.get('disabled'),
                    'The explicit public source repository must exist and be active')
    release.require(tag_commit(api, repo, tag) == commit, 'Existing release tag differs from explicit source commit')
    value = api.request('GET', f'/repos/{repo}/git/commits/{commit}')
    release.require(value.get('sha') == commit, 'GitHub returned a different source commit')
    tree_sha = commit_sha(value.get('tree', {}).get('sha'))
    tree = api.request('GET', f'/repos/{repo}/git/trees/{tree_sha}?recursive=1')
    release.require(tree.get('sha') == tree_sha and tree.get('truncated') is False
                    and isinstance(tree.get('tree'), list), 'Complete immutable source tree is required')
    actual = {}
    for item in tree['tree']:
        path = item.get('path')
        release.require(isinstance(path, str) and path not in actual, 'Duplicate/invalid GitHub tree path')
        actual[path] = item
    for path, wanted in expected.items():
        item = actual.get(path, {})
        release.require(all(item.get(k) == v for k, v in wanted.items()),
                        f'Committed source differs from source kit: {path}')
    return tree_sha


def asset_record(name, data):
    release.filename(name)
    return {'filename': name, 'size': len(data), 'sha256': release.sha(data)}


def inspect(path, key, qualified, api, repo, commit, tag, *, prepared_prefix='prepared-source',
            build_prefix='', identity_path='noventra/source-identity.json'):
    path = Path(path).absolute()
    repo, commit, tag = repository(repo), commit_sha(commit), tag_name(tag)
    catalog, _ = public.prepared(path, key)
    manifest = release.parse(release.read(path / 'manifest.json', release.MANIFEST_LIMIT))
    distribution = release.parse(release.read(path / 'distribution.json', release.DISTRIBUTION_LIMIT))
    identity, expected, source = source_tree(path / catalog['source']['filename'], catalog, manifest, key,
        prepared_prefix=prepared_prefix, build_prefix=build_prefix, identity_path=identity_path)
    quality = qualification(qualified, manifest, distribution, identity)
    tree = remote_source(api, repo, commit, tag, expected)
    provenance = {'format': 1, 'product': release.PRODUCT, 'repository': repo,
                  'source_commit': commit, 'source_tree': tree, 'tag': tag,
                  'version_label': manifest['version_label'], 'sequence': manifest['sequence'],
                  'manifest_sha256': catalog['manifest_sha256'],
                  'snapshot_sha256': manifest['snapshot_sha256'],
                  'source_archive': catalog['source'], 'source_identity': source,
                  'source_freeze_sha256': identity['source_freeze_sha256'],
                  'qualification_receipt_sha256': quality['original_receipt_sha256']}
    assets = {p.name: {'path': p, **public.file_record(p)} for p in path.iterdir()}
    generated = {PROVENANCE: release.encode(provenance), QUALIFICATION: release.encode(quality)}
    for name, raw in generated.items():
        release.require(name not in assets, 'Generated release asset filename collision')
        assets[name] = asset_record(name, raw)
    checksums = ''.join(f"{assets[n]['sha256']}  {n}\n" for n in sorted(assets)).encode('ascii')
    generated[CHECKSUMS] = checksums
    assets[CHECKSUMS] = asset_record(CHECKSUMS, checksums)
    notes = f'''Noventra {manifest['version_label']} — Linux x86_64.

Source commit: https://github.com/{repo}/commit/{commit}
License and upstream notices: https://github.com/{repo}/blob/{commit}/LICENSE

Download the Linux archive or the exact source & build kit from this release.
SHA256SUMS covers all other attached files. The source kit inventories the exact
Noventra changes/build inputs and pinned upstream sources; it is not a bundled
Chromium tree or a reproducible-build guarantee.

The signed update manifest identifies binary snapshot `{manifest['snapshot_sha256']}`.
The provenance and sanitized qualification summaries bind it to this source
commit and the recorded Linux checks ({quality['functional_checks']} functional,
{quality['optical_checks']} optical). Native macOS/Windows packages are not included.

The installed browser checks its pinned signed feed at
https://downloads.noventrastudio.com/browser/updates/stable/linux-x86_64/ .
This GitHub Release is the source/release record and artifact mirror. Ordinary
code commits do not ship browser updates. This Linux AppDir is unsigned at the
OS level; Ed25519 signatures protect update delivery.
'''
    return {'repo': repo, 'commit': commit, 'tag': tag, 'tree': tree,
            'catalog': catalog, 'provenance': provenance, 'assets': assets,
            'generated': generated, 'notes': notes,
            'title': f"Noventra {manifest['version_label']} — Linux"}


def verify_assets(api, plan, value, *, complete):
    release.require(value.get('tag_name') == plan['tag'] and value.get('target_commitish') == plan['commit']
                    and value.get('name') == plan['title'] and value.get('body') == plan['notes']
                    and value.get('prerelease') is True, 'Existing GitHub release metadata differs; nothing was overwritten')
    release_id = release.integer(value.get('id'), 1)
    records = api.assets(plan['repo'], release_id)
    names = {}
    for record in records:
        name = record.get('name')
        release.require(name in plan['assets'] and name not in names,
                        'Unexpected or duplicate existing GitHub release asset')
        wanted = plan['assets'][name]
        release.require(record.get('state') == 'uploaded' and record.get('size') == wanted['size']
                        and record.get('digest') == 'sha256:' + wanted['sha256'],
                        'Existing GitHub asset differs or has no verified SHA256; nothing was overwritten')
        names[name] = record
    if complete:
        release.require(set(names) == set(plan['assets']), 'GitHub release is missing required verified assets')
    return names


def existing_release(api, repo, tag):
    endpoint = f"/repos/{repo}/releases/tags/{urllib.parse.quote(tag, safe='')}"
    current = api.request('GET', endpoint, missing=True)
    if current is not None:
        release.require(isinstance(current, dict), 'Invalid GitHub release response')
        return current
    # The tag endpoint can hide unpublished drafts even from their owner.
    # Inspect the authenticated release list before creating or publishing one.
    matches = []
    for page in range(1, 21):
        values = api.request('GET', f'/repos/{repo}/releases?per_page=100&page={page}')
        release.require(isinstance(values, list) and len(values) <= 100
                        and all(isinstance(value, dict) for value in values),
                        'Invalid GitHub release list')
        matches.extend(value for value in values if value.get('tag_name') == tag)
        release.require(len(matches) <= 1, 'Multiple GitHub releases have the requested tag')
        if len(values) < 100:
            return matches[0] if matches else None
    raise ValueError('GitHub release list exceeds inspection limit')


def operate(plan, api, operation):
    release.require(operation in ('check', 'draft', 'publish'), 'Unsupported GitHub release operation')
    repo, tag = plan['repo'], plan['tag']
    current = existing_release(api, repo, tag)
    present = verify_assets(api, plan, current, complete=not current.get('draft')) if current else {}
    if operation == 'check':
        return {'status': 'verified_read_only', 'repository': repo, 'tag': tag,
                'source_commit': plan['commit'], 'source_tree': plan['tree'],
                'existing_release': current.get('html_url') if current else None,
                'existing_draft': current.get('draft') if current else None,
                'verified_existing_assets': len(present),
                'missing_assets': sorted(set(plan['assets']) - set(present)),
                'assets': [{k: a[k] for k in ('filename', 'size', 'sha256')} for a in plan['assets'].values()]}
    if operation == 'publish':
        release.require(current is not None, 'Create and verify the draft first; no release was created')
        verify_assets(api, plan, current, complete=True)
    if not current:
        current = api.request('POST', f'/repos/{repo}/releases', body={
            'tag_name': tag, 'target_commitish': plan['commit'], 'name': plan['title'],
            'body': plan['notes'], 'draft': True, 'prerelease': True, 'make_latest': 'false'})
        present = verify_assets(api, plan, current, complete=False)
    if current.get('draft'):
        if operation == 'draft':
            with tempfile.TemporaryDirectory(prefix='noventra-gh-assets-') as temporary:
                folder = Path(temporary)
                for name, raw in plan['generated'].items():
                    (folder / name).write_bytes(raw)
                for name, asset in plan['assets'].items():
                    if name in present:
                        continue
                    source = asset.get('path') or folder / name
                    release.require(public.file_hash(source) == asset['sha256'], 'Local release asset changed')
                    upload = f"https://uploads.github.com/repos/{repo}/releases/{current['id']}/assets?name={urllib.parse.quote(name, safe='')}"
                    result = api.request('POST', upload, file=source)
                    release.require(result.get('name') == name and result.get('state') == 'uploaded'
                                    and result.get('size') == asset['size']
                                    and result.get('digest') == 'sha256:' + asset['sha256'],
                                    'GitHub upload did not confirm exact bytes; draft retained for inspection')
                    release.require(public.file_hash(source) == asset['sha256'], 'Local release asset changed during upload')
        verify_assets(api, plan, current, complete=True)
        release.require(tag_commit(api, repo, tag) == plan['commit'], 'Tag changed; draft was not published')
        if operation == 'publish':
            current = api.request('PATCH', f"/repos/{repo}/releases/{current['id']}",
                                  body={'draft': False, 'make_latest': 'false'})
            release.require(current.get('draft') is False, 'Publication response did not confirm public visibility')
            verify_assets(api, plan, current, complete=True)
            release.require(tag_commit(api, repo, tag) == plan['commit'], 'Tag changed during publication; inspect GitHub release')
    return {'status': 'draft_verified' if current.get('draft') else 'published_verified',
            'url': current.get('html_url'), 'repository': repo, 'tag': tag,
            'source_commit': plan['commit'], 'source_tree': plan['tree'],
            'assets': len(plan['assets']), 'immutable': current.get('immutable') is True,
            'cloudflare_published_by_this_command': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('check', 'draft', 'publish'),
                        help='check is read-only; draft uploads; publish explicitly makes the verified draft public')
    parser.add_argument('--release', type=Path, required=True)
    parser.add_argument('--public-key', type=Path, required=True)
    parser.add_argument('--qualification', type=Path, required=True)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--tag', required=True, help='An existing remote tag; this tool never creates or moves it')
    parser.add_argument('--prepared-prefix', default='prepared-source')
    parser.add_argument('--build-prefix', default='')
    parser.add_argument('--source-identity-path', default='noventra/source-identity.json')
    args = parser.parse_args()
    try:
        api = GitHub()
        plan = inspect(args.release, args.public_key, args.qualification, api,
                       args.repo, args.commit, args.tag, prepared_prefix=args.prepared_prefix,
                       build_prefix=args.build_prefix, identity_path=args.source_identity_path)
        result = operate(plan, api, args.operation)
    except (ValueError, OSError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f'GitHub release: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
