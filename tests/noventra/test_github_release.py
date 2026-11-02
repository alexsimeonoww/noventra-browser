"""GitHub release tests: disposable keys/fake AppDirs, mocked GitHub boundary."""

import copy
import hashlib
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import urllib.parse
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
sys.path.insert(0, str(ROOT / 'tests/noventra'))
import github_release as github
import publish_release as public
import release_manifest as release
import test_package_distribution as package_fixture
import public_release_fixture as public_fixture

REPO = 'acme/noventra-browser'
COMMIT = '1' * 40
TREE = '2' * 40
TAG = 'v1-fixture-r1'


class FakeGitHub:
    def __init__(self, tree):
        self.tree = tree
        self.release = None
        self.asset_values = []
        self.writes = []
        self.tag = COMMIT
        self.fail_upload = None
        self.release_pages = None

    def assets(self, repo, release_id):
        return copy.deepcopy(self.asset_values)

    def request(self, method, endpoint, *, body=None, file=None, missing=False):
        if method != 'GET':
            self.writes.append((method, endpoint))
        if endpoint == f'/repos/{REPO}' and method == 'GET':
            return {'full_name': REPO, 'private': False, 'archived': False, 'disabled': False}
        if endpoint.startswith(f'/repos/{REPO}/git/ref/tags/'):
            return {'ref': 'refs/tags/' + TAG, 'object': {'type': 'commit', 'sha': self.tag}}
        if endpoint == f'/repos/{REPO}/git/commits/{COMMIT}':
            return {'sha': COMMIT, 'tree': {'sha': TREE}}
        if endpoint == f'/repos/{REPO}/git/trees/{TREE}?recursive=1':
            return {'sha': TREE, 'truncated': False, 'tree': copy.deepcopy(self.tree)}
        if endpoint.startswith(f'/repos/{REPO}/releases/tags/'):
            if self.release is None and not missing:
                raise ValueError('Missing release')
            if self.release is not None and self.release.get('draft'):
                return None
            return copy.deepcopy(self.release)
        if method == 'GET' and endpoint.startswith(f'/repos/{REPO}/releases?'):
            page = int(urllib.parse.parse_qs(urllib.parse.urlsplit(endpoint).query)['page'][0])
            if self.release_pages is not None:
                return copy.deepcopy(self.release_pages.get(page, []))
            return [copy.deepcopy(self.release)] if self.release is not None and page == 1 else []
        if method == 'POST' and endpoint == f'/repos/{REPO}/releases':
            self.release = {**body, 'id': 7, 'html_url': f'https://github.com/{REPO}/releases/tag/{TAG}', 'immutable': False}
            return copy.deepcopy(self.release)
        if method == 'POST' and endpoint.startswith(f'https://uploads.github.com/repos/{REPO}/releases/7/assets?name='):
            name = urllib.parse.parse_qs(urllib.parse.urlsplit(endpoint).query)['name'][0]
            if name == self.fail_upload:
                raise ValueError('Injected upload failure')
            if any(a['name'] == name for a in self.asset_values):
                raise ValueError('No clobber permitted')
            data = file.read_bytes()
            value = {'id': len(self.asset_values) + 1, 'name': name, 'state': 'uploaded',
                     'size': len(data), 'digest': 'sha256:' + release.sha(data)}
            self.asset_values.append(value)
            return copy.deepcopy(value)
        if method == 'PATCH' and endpoint == f'/repos/{REPO}/releases/7':
            self.release.update(body)
            return copy.deepcopy(self.release)
        raise AssertionError((method, endpoint))


class GitHubReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='noventra-github-release-tests-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.key = cls.base / 'test-only-private.pem'
        release.openssl('genpkey', '-algorithm', 'ED25519', '-out', cls.key)
        cls.key.chmod(0o600)
        cls.public = cls.base / 'test-public.pem'
        cls.public.write_bytes(release.openssl('pkey', '-in', cls.key, '-pubout'))
        fixture = package_fixture.PackageTests('test_hash_mismatch')
        fixture.setUp()
        cls.addClassCleanup(fixture.doCleanups)
        dist = cls.base / 'distribution'
        config = public_fixture.update_configuration(cls.base / 'update-config.json', cls.public)
        package_fixture.package.package(fixture.source, dist, '1-fixture', archive=True, update_config=config)
        cls.distribution = dist
        inventory = release.parse((dist / 'distribution.json').read_bytes())
        cls.source_files = {
            'prepared-source/chrome/example.cc': b'// fake source fixture\n',
            'build-inputs/LICENSE': (ROOT / 'LICENSE').read_bytes(),
            'build-inputs/noventra/update/stable-public.pem': cls.public.read_bytes(),
        }
        cls.identity = {'format': 1, 'product': release.PRODUCT, 'version_label': '1-fixture',
                        'snapshot_sha256': inventory['snapshot_sha256'],
                        'engine': {'chromium': 'fake-pinned-fixture'},
                        'source_freeze_sha256': 'a' * 64, 'layer_manifest_sha256': 'b' * 64,
                        'files': {n: {'sha256': release.sha(v), 'bytes': len(v), 'mode': 0o644}
                                  for n, v in cls.source_files.items()}}
        raw_identity = release.encode(cls.identity)
        cls.source = cls.base / 'noventra-1-fixture-source.zip'
        cls.zip_files = {**cls.source_files, 'source-identity.json': raw_identity, 'README.md': b'Generated fixture source kit\n'}
        cls.write_zip(cls.source, cls.zip_files)
        info = cls.base / 'source-info.json'
        info.write_bytes(release.encode({'format': 1, 'product': release.PRODUCT,
            'version_label': inventory['version_label'], 'snapshot_sha256': inventory['snapshot_sha256'],
            'source_archive': public.file_record(cls.source)}))
        cls.source_info = info
        cls.prepared = cls.base / 'prepared'
        public.prepare(dist, cls.prepared, cls.key, cls.source, info, 1, max_unpacked_bytes=32 * 1024**2)
        cls.qual = cls.base / 'qualification.json'
        cls.qual.write_bytes(release.encode({'passed': True, 'functional_checks': 7, 'optical_checks': 0,
            'source_freeze_sha256': 'a' * 64,
            'binary_sha256': inventory['files'][release.RUNTIME + 'noventra-browser']['sha256'],
            'resources_sha256': inventory['files'][release.RUNTIME + 'resources.pak']['sha256'],
            'local_private_path': '/private/profile/DO_NOT_PUBLISH'}))
        cls.remote_tree = []
        for name, raw in cls.source_files.items():
            path = name.removeprefix('build-inputs/')
            oid = hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest()
            cls.remote_tree.append({'path': path, 'sha': oid, 'type': 'blob', 'mode': '100644'})
        cls.remote_tree.append({'path': 'noventra/source-identity.json', 'sha': github.git_blob(raw_identity), 'type': 'blob', 'mode': '100644'})

    @staticmethod
    def write_zip(path, files, *, symlink=None):
        with zipfile.ZipFile(path, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, raw in files.items():
                item = zipfile.ZipInfo(github.KIT_ROOT + name)
                item.external_attr = ((stat.S_IFLNK if name == symlink else stat.S_IFREG) | 0o644) << 16
                archive.writestr(item, raw)

    def setUp(self):
        self.api = FakeGitHub(copy.deepcopy(self.remote_tree))
        self.temporary_case = tempfile.TemporaryDirectory(prefix='case-', dir=self.base)
        self.addCleanup(self.temporary_case.cleanup)
        self.case = Path(self.temporary_case.name)

    def inspect(self, qualification=None):
        return github.inspect(self.prepared, self.public, qualification or self.qual,
                              self.api, REPO, COMMIT, TAG)

    def test_read_only_check_binds_exact_remote_source_and_sanitizes_qualification(self):
        plan = self.inspect()
        result = github.operate(plan, self.api, 'check')
        self.assertEqual(result['status'], 'verified_read_only')
        self.assertEqual(self.api.writes, [])
        self.assertEqual(result['source_commit'], COMMIT)
        self.assertEqual(len(plan['assets']), 13)
        self.assertNotIn(b'DO_NOT_PUBLISH', b''.join(plan['generated'].values()))
        self.assertEqual(json.loads(plan['generated'][github.PROVENANCE])['source_tree'], TREE)

    def test_draft_uploads_exact_assets_without_publishing_and_retry_reuses_all(self):
        plan = self.inspect()
        result = github.operate(plan, self.api, 'draft')
        self.assertEqual(result['status'], 'draft_verified')
        self.assertTrue(self.api.release['draft'])
        self.assertEqual(len(self.api.asset_values), 13)
        before = len(self.api.writes)
        self.assertEqual(github.operate(self.inspect(), self.api, 'draft')['status'], 'draft_verified')
        self.assertEqual(len(self.api.writes), before)
        self.assertFalse(any(method == 'PATCH' for method, _ in self.api.writes))

    def test_publish_requires_complete_draft_then_is_idempotent(self):
        plan = self.inspect()
        with self.assertRaisesRegex(ValueError, 'draft first'):
            github.operate(plan, self.api, 'publish')
        self.assertEqual(self.api.writes, [])
        github.operate(plan, self.api, 'draft')
        self.assertEqual(github.operate(plan, self.api, 'publish')['status'], 'published_verified')
        before = list(self.api.writes)
        self.assertEqual(github.operate(self.inspect(), self.api, 'publish')['status'], 'published_verified')
        self.assertEqual(self.api.writes, before)

    def test_hidden_draft_is_found_on_a_later_release_list_page(self):
        plan = self.inspect()
        github.operate(plan, self.api, 'draft')
        self.api.release_pages = {
            1: [{'id': 100 + i, 'tag_name': 'other-' + str(i), 'draft': True} for i in range(100)],
            2: [copy.deepcopy(self.api.release)],
        }
        before = list(self.api.writes)
        result = github.operate(plan, self.api, 'check')
        self.assertTrue(result['existing_draft'])
        self.assertEqual(result['verified_existing_assets'], 13)
        self.assertEqual(self.api.writes, before)

    def test_duplicate_hidden_drafts_are_refused_before_mutation(self):
        plan = self.inspect()
        github.operate(plan, self.api, 'draft')
        self.api.release_pages = {1: [copy.deepcopy(self.api.release),
                                      {**copy.deepcopy(self.api.release), 'id': 8}]}
        before = list(self.api.writes)
        with self.assertRaisesRegex(ValueError, 'Multiple GitHub releases'):
            github.operate(plan, self.api, 'publish')
        self.assertEqual(self.api.writes, before)

    def test_asset_conflict_unknown_asset_and_metadata_conflict_never_clobber(self):
        plan = self.inspect()
        github.operate(plan, self.api, 'draft')
        first = copy.deepcopy(self.api.asset_values[0])
        for change in ({'digest': 'sha256:' + '0' * 64}, {'digest': None}, {'name': 'unexpected-private-file'}):
            self.api.asset_values[0] = {**first, **change}
            before = list(self.api.writes)
            with self.assertRaises(ValueError):
                github.operate(plan, self.api, 'publish')
            self.assertEqual(self.api.writes, before)
        self.api.asset_values[0] = first
        self.api.release['body'] += 'changed'
        with self.assertRaisesRegex(ValueError, 'metadata differs'):
            github.operate(plan, self.api, 'draft')

    def test_partial_failed_upload_retains_draft_and_identical_retry_resumes(self):
        plan = self.inspect()
        self.api.fail_upload = github.PROVENANCE
        with self.assertRaisesRegex(ValueError, 'Injected upload failure'):
            github.operate(plan, self.api, 'draft')
        self.assertTrue(self.api.release['draft'])
        prior = {a['name'] for a in self.api.asset_values}
        self.api.fail_upload = None
        github.operate(self.inspect(), self.api, 'draft')
        self.assertEqual(len(self.api.asset_values), len(plan['assets']))
        self.assertEqual(len(prior), len([a for a in self.api.asset_values if a['name'] in prior]))

    def test_wrong_committed_source_or_moved_tag_refused_before_release_mutation(self):
        self.api.tree[0]['sha'] = '0' * 40
        with self.assertRaisesRegex(ValueError, 'Committed source differs'):
            self.inspect()
        self.assertEqual(self.api.writes, [])
        self.api.tree = copy.deepcopy(self.remote_tree)
        self.api.tag = '3' * 40
        with self.assertRaisesRegex(ValueError, 'tag differs'):
            self.inspect()

    def test_tag_change_before_publication_keeps_release_draft(self):
        plan = self.inspect()
        github.operate(plan, self.api, 'draft')
        self.api.tag = '3' * 40
        with self.assertRaisesRegex(ValueError, 'Tag changed'):
            github.operate(plan, self.api, 'publish')
        self.assertTrue(self.api.release['draft'])

    def test_qualification_wrong_binary_or_source_is_refused(self):
        value = release.parse(self.qual.read_bytes())
        for field in ('binary_sha256', 'resources_sha256', 'source_freeze_sha256'):
            broken = self.case / (field + '.json')
            broken.write_bytes(release.encode({**value, field: '0' * 64}))
            with self.assertRaisesRegex(ValueError, 'Qualification does not identify'):
                self.inspect(broken)
        self.assertEqual(self.api.writes, [])

    def test_github_mirror_revalidates_embedded_public_update_contract_before_remote_access(self):
        dist = self.case / 'legacy-distribution'
        shutil.copytree(self.distribution, dist, symlinks=True)
        public_fixture.rewrite_updater(dist, config_changes={'base_url': 'https://fork.example/updates/'})
        legacy = self.case / 'legacy-release'
        public_fixture.legacy_prepared(dist, legacy, self.key, self.source, self.source_info)
        with patch.object(self.api, 'request', wraps=self.api.request) as request:
            with self.assertRaisesRegex(ValueError, 'Public updater configuration'):
                github.inspect(legacy, self.public, self.qual, self.api, REPO, COMMIT, TAG)
            request.assert_not_called()
        self.assertEqual(self.api.writes, [])

    def test_source_zip_rejects_unknown_files_bad_hash_symlink_and_unsafe_names(self):
        catalog = release.parse((self.prepared / 'channel.json').read_bytes())
        manifest = release.parse((self.prepared / 'manifest.json').read_bytes())
        variants = [({**self.zip_files, 'unexpected.txt': b'not inventoried'}, None),
                    ({**self.zip_files, 'prepared-source/chrome/example.cc': b'changed'}, None),
                    (self.zip_files, 'prepared-source/chrome/example.cc'),
                    ({**self.zip_files, '../escape': b'bad'}, None)]
        for i, (files, symlink) in enumerate(variants):
            path = self.case / f'bad-{i}.zip'
            self.write_zip(path, files, symlink=symlink)
            with self.assertRaises(ValueError):
                github.source_tree(path, {**catalog, 'source': public.file_record(path)}, manifest, self.public)

    def test_gh_adapter_uses_fixed_host_structured_json_and_redacts_remote_errors(self):
        def run(command, *, input, stdout, stderr, timeout):
            self.assertEqual(command[:4], ['gh', 'api', '--hostname', 'github.com'])
            self.assertIn('--input', command)
            self.assertEqual(json.loads(input), {'draft': True})
            stdout.write(b'HTTP/2.0 201 Created\r\nContent-Type: application/json\r\n\r\n{"id":7}')
            return SimpleNamespace(returncode=0)
        with patch('github_release.subprocess.run', side_effect=run):
            self.assertEqual(github.GitHub().request('POST', f'/repos/{REPO}/releases', body={'draft': True}), {'id': 7})
        def fail(command, *, input, stdout, stderr, timeout):
            stdout.write(b'HTTP/2.0 403 Forbidden\n\n{"message":"DUMMY_SECRET_MUST_NOT_ESCAPE"}')
            return SimpleNamespace(returncode=1)
        with patch('github_release.subprocess.run', side_effect=fail):
            with self.assertRaises(ValueError) as error:
                github.GitHub().request('GET', f'/repos/{REPO}')
            self.assertNotIn('DUMMY_SECRET', str(error.exception))

    def test_explicit_target_validation_and_git_blob_identity(self):
        for value in ('https://github.com/acme/repo', 'owner/repo/other', '../repo'):
            with self.assertRaises(ValueError): github.repository(value)
        for value in ('main', 'HEAD', '1' * 7):
            with self.assertRaises(ValueError): github.commit_sha(value)
        for value in ('../release', 'v1.lock', 'v1 with spaces'):
            with self.assertRaises(ValueError): github.tag_name(value)
        self.assertEqual(github.git_blob(b''), 'e69de29bb2d1d6434b8b29ae775ad8c2e48c5391')


if __name__ == '__main__':
    unittest.main()
