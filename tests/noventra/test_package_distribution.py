"""Offline packaging fixtures only: no Chromium build, runtime, or profile."""

import configparser
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
import package_distribution as package


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='noventra-package-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'preserved'
        self.source.mkdir()
        self.output = self.root / 'result'
        self.helper = 'noventra_crashpad_handler'
        self.fixture()

    def put(self, name, data, executable=False):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o755 if executable else 0o644)

    def fixture(self):
        for name in package.REQUIRED_FILES:
            self.put(name, b'fixture\n')
        self.put('args.gn', b'target_cpu = "x64"\n')
        self.put('provenance.json', b'{"fixture": true, "source": "offline test"}\n')
        self.put('initial_preferences', b'{"distribution": {}}\n')
        # The current native build emits this valid, empty pack in addition to
        # the three required root packs. Preserve it with the runtime inventory.
        self.put('headless_command_resources.pak',
                 struct.pack('<IBxxxHHHI', 5, 1, 0, 0, 0, 18))
        self.browser(self.helper)
        self.put(self.helper, b'fixture helper\n', executable=True)
        schema = b'{"type":"object","properties":{"disabled":{"type":"boolean"}}}'
        manifest = json.dumps({'name': 'uBlock Origin', 'version': '1.0',
                               'manifest_version': 2,
                               'storage': {'managed_schema': 'managed_storage.json'}}).encode()
        chunks = (manifest, schema)
        offset = 12 + 3 * 6
        table = bytearray()
        for index, chunk in enumerate(chunks, 1):
            table.extend(struct.pack('<HI', index, offset))
            offset += len(chunk)
        table.extend(struct.pack('<HI', 0, offset))
        self.put('resources.pak', struct.pack('<IBxxxHH', 5, 1, 2, 0) + table + b''.join(chunks))
        self.put('resources.pak.info', b'IDR_UBLOCK_MANIFEST_JSON,1,manifest.json\n'
                 b'IDR_UBLOCK_MANAGED_STORAGE_JSON,2,managed_storage.json\n')
        self.put('resources/ublock/managed_storage.json', schema)
        self.put('resources/accessibility/helper.js', b'// resource fixture\n')
        self.put('NOTICE-test', b'Additional upstream license notice\n')
        self.refresh()

    def browser(self, helper):
        self.put(package.BINARY, b'not a real browser\0' + helper.encode() + b'\0', executable=True)

    def refresh(self):
        self.manifest = {'binary': package.BINARY, 'files': {
            p.relative_to(self.source).as_posix(): sha(p)
            for p in self.source.rglob('*') if p.is_file() and p.name != 'snapshot.json'
        }}
        self.save()

    def save(self):
        (self.source / 'snapshot.json').write_text(json.dumps(self.manifest) + '\n')

    def run_package(self, **kwargs):
        return package.package(self.source, self.output, '1.2.3-local', **kwargs)

    def rejected(self, message):
        with self.assertRaises((ValueError, OSError), msg=message):
            self.run_package()
        self.assertFalse((self.output / 'distribution.json').exists())

    def test_complete_inventory_archive_independent_copies_and_ignored_files(self):
        original = {p: (p.read_bytes(), p.stat().st_ino) for p in self.source.rglob('*') if p.is_file()}
        self.put('unlisted.txt', b'not packaged')
        self.put('Default/Preferences', b'private profile fixture')
        result = self.run_package(archive=True)
        appdir = self.output / 'Noventra.AppDir'
        runtime = appdir / 'opt/noventra'
        self.assertTrue(result['complete'])
        self.assertEqual(result['architecture'], 'x86_64')
        self.assertEqual(result['crashpad_helper'], self.helper)
        self.assertEqual(result['snapshot_sha256'], sha(self.source / 'snapshot.json'))
        self.assertIn('Noventra.AppDir/opt/noventra/headless_command_resources.pak',
                      result['files'])
        for path, (content, inode) in original.items():
            copied = runtime / path.relative_to(self.source)
            self.assertEqual(copied.read_bytes(), content)
            self.assertEqual(path.read_bytes(), content)
            self.assertNotEqual(copied.stat().st_ino, inode)
        self.assertFalse((runtime / 'unlisted.txt').exists())
        self.assertFalse((runtime / 'Default').exists())
        actual = {p.relative_to(self.output).as_posix() for p in self.output.rglob('*')
                  if (p.is_file() or p.is_symlink()) and p.name != 'distribution.json'}
        self.assertEqual(actual, set(result['files']))
        for name, record in result['files'].items():
            path = self.output / name
            if 'symlink' in record:
                self.assertEqual(os.readlink(path), record['symlink'])
                self.assertTrue(path.resolve().is_relative_to(appdir))
            else:
                self.assertFalse(path.is_symlink())
                self.assertEqual(sha(path), record['sha256'])
                self.assertEqual(path.stat().st_size, record['size'])
                self.assertEqual(oct(path.stat().st_mode & 0o777), record['mode'])
        tarpath = next(self.output.glob('*.tar.xz'))
        with tarfile.open(tarpath) as bundle:
            self.assertEqual(set(bundle.getnames()), {n for n in result['files'] if n.startswith('Noventra.AppDir/')})
            for member in bundle.getmembers():
                self.assertFalse(member.islnk())
                if member.isfile():
                    self.assertEqual(hashlib.sha256(bundle.extractfile(member).read()).hexdigest(),
                                     result['files'][member.name]['sha256'])
        (runtime / package.BINARY).write_bytes(b'independent mutation')
        self.assertEqual((self.source / package.BINARY).read_bytes(), original[self.source / package.BINARY][0])

    def test_both_compiled_helper_names_and_stale_other_helper(self):
        for helper in ('noventra_crashpad_handler', 'helium_crashpad_handler'):
            with self.subTest(helper=helper):
                self.browser(helper)
                self.put(helper, b'helper', executable=True)
                self.refresh()
                self.output = self.root / helper
                result = self.run_package()
                self.assertEqual(result['crashpad_helper'], helper)
                self.assertTrue((self.output / 'Noventra.AppDir/opt/noventra' / helper).is_file())

    def test_external_extension_directory_marker_survives_archive_and_private_umask(self):
        previous_umask = os.umask(0o077)
        try:
            result = self.run_package(archive=True)
        finally:
            os.umask(previous_umask)
        name = 'Noventra.AppDir/opt/noventra/extensions/README.noventra.txt'
        marker = self.output / name
        self.assertTrue(marker.is_file())
        self.assertEqual(marker.parent.stat().st_mode & 0o777, 0o755)
        self.assertEqual(marker.stat().st_mode & 0o777, 0o644)
        self.assertEqual(list(marker.parent.iterdir()), [marker])
        self.assertIn('No extension manifests', marker.read_text())
        self.assertEqual(result['files'][name], {'sha256': sha(marker),
                         'size': marker.stat().st_size, 'mode': '0o644'})
        with tarfile.open(next(self.output.glob('*.tar.xz'))) as bundle:
            member = bundle.getmember(name)
            self.assertTrue(member.isreg())
            self.assertEqual(member.mode, 0o644)
            self.assertEqual(bundle.extractfile(member).read(), marker.read_bytes())
            self.assertEqual([n for n in bundle.getnames() if n.startswith(str(Path(name).parent) + '/')], [name])
        self.assertFalse((self.source / 'extensions').exists())

    def test_standalone_installer_inventory_matches_packaged_files(self):
        result = self.run_package(archive=True)
        appdir = self.output / 'Noventra.AppDir'
        manifest = json.loads((appdir / 'install-manifest.json').read_text())
        self.assertEqual(manifest['format'], 1)
        self.assertEqual(manifest['id'], 'local.noventra.browser')
        expected = {str(Path(name).relative_to('Noventra.AppDir')): record
                    for name, record in result['files'].items()
                    if name.startswith('Noventra.AppDir/') and name != 'Noventra.AppDir/install-manifest.json'}
        self.assertEqual(manifest['files'], expected)
        self.assertTrue({'install.sh', 'uninstall.sh', 'installer.py'} <= manifest['files'].keys())
        for name in ('install.sh', 'uninstall.sh'):
            self.assertEqual((appdir / name).stat().st_mode & 0o777, 0o755)
            subprocess.run(['sh', '-n', str(appdir / name)], check=True)

    def test_missing_ambiguous_and_undeclared_helpers(self):
        self.browser('unknown_crashpad_handler')
        self.refresh()
        self.rejected('unknown helper')
        self.browser('noventra_crashpad_handler\0helium_crashpad_handler')
        self.refresh()
        self.rejected('ambiguous helper')
        self.browser(self.helper)
        self.refresh()
        self.manifest['files'].pop(self.helper)
        self.save()
        self.rejected('undeclared helper')
        (self.source / self.helper).unlink()
        self.rejected('missing helper')

    def test_hash_mismatch(self):
        self.put('icudtl.dat', b'changed after snapshot')
        self.rejected('hash mismatch')

    def test_missing_required_files(self):
        for name in package.REQUIRED_FILES:
            with self.subTest(name=name):
                expected = self.manifest['files'].pop(name)
                self.save()
                self.rejected(name)
                self.manifest['files'][name] = expected

    def test_managed_schema_must_be_declared_and_match_pack(self):
        name = 'resources/ublock/managed_storage.json'
        self.manifest['files'].pop(name)
        self.save()
        self.rejected('undeclared managed schema')
        self.put(name, b'{"type":"object"}')
        self.refresh()
        self.rejected('schema differs from bundled resource')

    def test_unsafe_paths_and_unexpected_payload(self):
        for name in ('../escape', '/tmp/escape', 'resources/../../escape', 'resources//x',
                     './x', 'resources/./x', 'resources/x\x00', 'resources/x\n',
                     'C:/escape', 'resources\\escape', 'Default/Preferences', 'snapshot.json'):
            with self.subTest(name=name):
                self.manifest['files'][name] = '0' * 64
                self.save()
                self.rejected(name)
                self.manifest['files'].pop(name)

    def test_malformed_manifests(self):
        for data in ('[]', '{}', '{', '{"binary":"helium","files":{}}',
                     '{"binary":"noventra-browser","files":[]}',
                     '{"binary":"noventra-browser","files":{},"invalid":NaN}',
                     '{"binary":"noventra-browser","binary":"noventra-browser","files":{}}'):
            with self.subTest(data=data):
                (self.source / 'snapshot.json').write_text(data)
                self.rejected(data)
        self.manifest['files']['icudtl.dat'] = 'not a digest'
        self.save()
        self.rejected('invalid digest')

    def test_symlinks_are_rejected_including_unlisted_escapes(self):
        external = self.root / 'external'
        external.write_text('outside input')
        for target in (external, self.source / 'icudtl.dat', self.root / 'absent'):
            with self.subTest(target=target):
                link = self.source / 'unlisted-link'
                link.symlink_to(target)
                self.rejected('unlisted symlink')
                link.unlink()
        path = self.source / 'icudtl.dat'
        path.unlink()
        path.symlink_to(external)
        self.rejected('listed symlink')

    def test_symlinked_parent_and_special_files(self):
        directory = self.source / 'resources/accessibility'
        shutil.move(directory, self.root / 'moved')
        directory.symlink_to(self.root / 'moved', target_is_directory=True)
        self.rejected('directory link')
        directory.unlink()
        shutil.move(self.root / 'moved', directory)
        os.mkfifo(self.source / 'unlisted-fifo')
        self.rejected('FIFO')

    def test_nonexecutable_browser_or_helper_and_privileged_modes(self):
        for name, mode in ((package.BINARY, 0o644), (self.helper, 0o644), (self.helper, 0o4755)):
            with self.subTest(name=name, mode=mode):
                (self.source / name).chmod(mode)
                self.rejected('invalid executable mode')
                (self.source / name).chmod(0o755)

    def test_source_architecture_only(self):
        self.put('args.gn', b'target_os = "linux"\ntarget_cpu = "arm64" # preserved build\n')
        self.refresh()
        self.assertEqual(self.run_package()['architecture'], 'aarch64')
        self.output = self.root / 'invalid'
        for args in ('', 'target_cpu = "mips"', 'target_cpu = host_cpu',
                     'target_cpu = "x64"\ntarget_cpu = "arm64"',
                     'target_cpu = "x64"\ntarget_os = "win"'):
            with self.subTest(args=args):
                self.put('args.gn', args.encode())
                self.refresh()
                self.rejected('missing/ambiguous/unsupported architecture')

    def test_explicit_safe_version_and_fresh_output_required(self):
        for version in ('', '../bad', 'bad/label', 'bad label', None):
            with self.subTest(version=version), self.assertRaises(ValueError):
                package.package(self.source, self.output, version)
        self.output.mkdir()
        sentinel = self.output / 'keep'
        sentinel.write_text('user data')
        self.rejected('existing output')
        self.assertEqual(sentinel.read_text(), 'user data')
        self.output = self.root / 'dangling'
        self.output.symlink_to(self.root / 'absent')
        self.rejected('dangling output')
        self.output = self.source / 'new-package'
        self.rejected('output inside source')

    def test_failure_never_writes_completion(self):
        with patch.object(package.tarfile, 'open', side_effect=OSError('fixture archive failure')):
            with self.assertRaisesRegex(OSError, 'archive failure'):
                self.run_package(archive=True)
        self.assertTrue((self.output / 'Noventra.AppDir').is_dir())
        self.assertFalse((self.output / 'distribution.json').exists())

    def test_changed_inputs_prevent_completion(self):
        original = package.Inputs.verify
        calls = 0

        def mutate(inputs):
            nonlocal calls
            calls += 1
            if calls == 2:
                self.put('initial_preferences', b'{"changed":true}')
            original(inputs)

        with patch.object(package.Inputs, 'verify', mutate):
            with self.assertRaisesRegex(ValueError, 'changed before completion'):
                self.run_package()
        self.assertFalse((self.output / 'distribution.json').exists())

    def test_wrapper_argument_environment_and_profile_preservation(self):
        # This is an argument recorder, never a browser. NUL lookup is in a comment.
        code = ('import json,os,sys; print(json.dumps({"args":sys.argv[1:],'
                '"env":{k:os.environ.get(k) for k in ("CHROME_WRAPPER",'
                '"LD_LIBRARY_PATH","NOVENTRA_CONFIG_HOME","XDG_CONFIG_HOME","HOME")}}))')
        recorder = (f'#!/bin/sh\nexec {shlex.quote(sys.executable)} -c '
                    f'{shlex.quote(code)} "$@"\n# ').encode() + self.helper.encode() + b'\0\n'
        self.put(package.BINARY, recorder, executable=True)
        self.refresh()
        self.output = self.root / 'package with spaces'
        self.run_package(archive=True)
        appdir = self.output / 'Noventra.AppDir'
        runtime = appdir / 'opt/noventra'
        self.assertEqual((runtime / 'video-buffer-compatibility.sh').read_bytes(),
                         (package.ASSETS / 'video-buffer-compatibility.sh').read_bytes())
        env = dict(os.environ, NOVENTRA_CONFIG_HOME='/fixture/noventra',
                   XDG_CONFIG_HOME='/fixture/xdg', HOME='/fixture/home',
                   NOVENTRA_VIDEO_BUFFER_WORKAROUND='off')
        env.pop('APPIMAGE', None)
        args = ['--incognito', '--user-data-dir=/explicit profile', 'https://example.org/a b', '',
                'literal;$(do-not-run)', 'quotation"and\'']
        for entry in (appdir / 'AppRun', runtime / 'noventra-wrapper', appdir / 'usr/bin/noventra-browser'):
            for inherited in (None, '/fixture/existing libs'):
                with self.subTest(entry=entry, inherited=inherited):
                    env.pop('LD_LIBRARY_PATH', None)
                    if inherited is not None:
                        env['LD_LIBRARY_PATH'] = inherited
                    result = json.loads(subprocess.check_output([str(entry), *args], env=env, text=True))
                    self.assertEqual(result['args'], args)
                    self.assertEqual(result['env']['CHROME_WRAPPER'], str(entry.resolve()))
                    expected = f'{runtime}:{runtime}/lib:{runtime}/lib.target'
                    if entry.name == 'AppRun':
                        expected += f':{appdir}/usr/lib'
                    if inherited is not None:
                        expected += ':' + inherited
                    self.assertEqual(result['env']['LD_LIBRARY_PATH'], expected)
                    for key in ('NOVENTRA_CONFIG_HOME', 'XDG_CONFIG_HOME', 'HOME'):
                        self.assertEqual(result['env'][key], env[key])
        env['APPIMAGE'] = '/fixture/a local image.AppImage'
        result = json.loads(subprocess.check_output([str(appdir / 'AppRun')], env=env, text=True))
        self.assertEqual(result['args'], [])
        self.assertEqual(result['env']['CHROME_WRAPPER'], env['APPIMAGE'])
        unpacked = self.root / 'extracted with spaces'
        with tarfile.open(next(self.output.glob('*.tar.xz'))) as bundle:
            bundle.extractall(unpacked, filter='data')
        extracted = unpacked / 'Noventra.AppDir'
        env.pop('APPIMAGE')
        for entry in (extracted / 'AppRun', extracted / 'usr/bin/noventra-browser'):
            result = json.loads(subprocess.check_output([str(entry), *args], env=env, text=True))
            self.assertEqual(result['args'], args)
            self.assertEqual(result['env']['CHROME_WRAPPER'], str(entry.resolve()))

    def test_archive_inventory_under_private_umask(self):
        before = os.umask(0o077)
        try:
            result = self.run_package(archive=True)
        finally:
            os.umask(before)
        tarpath = next(self.output.glob('*.tar.xz'))
        self.assertEqual(oct(tarpath.stat().st_mode & 0o777), result['files'][tarpath.name]['mode'])

    def test_cli_requires_version_and_produces_fixture_package(self):
        command = [sys.executable, '-B', str(ROOT / 'tools/noventra/package_distribution.py'),
                   '--source', str(self.source), '--output', str(self.output)]
        missing = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn('--version', missing.stderr)
        self.assertFalse(self.output.exists())
        completed = subprocess.run(command + ['--version', 'known-fixture', '--tar'],
                                   capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(completed.stdout)['version_label'], 'known-fixture')
        self.assertTrue((self.output / 'distribution.json').is_file())

    def test_desktop_metadata_and_launcher_contract(self):
        desktop = configparser.ConfigParser(interpolation=None, strict=True)
        desktop.optionxform = str
        desktop.read(package.ASSETS / 'noventra-browser.desktop')
        self.assertEqual(desktop['Desktop Entry']['Name'], 'Noventra')
        self.assertEqual(desktop['Desktop Entry']['Icon'], package.BINARY)
        self.assertEqual(desktop['Desktop Entry']['Exec'], 'noventra-browser %U')
        self.assertEqual(desktop['Desktop Action new-window']['Exec'], 'noventra-browser')
        self.assertEqual(desktop['Desktop Action new-private-window']['Exec'], 'noventra-browser --incognito')
        self.assertNotIn('StartupWMClass', desktop['Desktop Entry'])
        xml = ET.parse(package.ASSETS / 'local.noventra.browser.metainfo.xml').getroot()
        self.assertEqual(xml.findtext('id'), 'local.noventra.browser')
        self.assertEqual(xml.findtext('launchable'), 'noventra-browser.desktop')
        self.assertEqual(xml.findtext('project_license'), 'GPL-3.0-only')
        for name in ('url', 'update_contact', 'developer', 'releases', 'screenshots'):
            self.assertIsNone(xml.find(name))
        for name in ('AppRun', 'noventra-wrapper.sh'):
            text = (package.ASSETS / name).read_text()
            self.assertIn('Copyright 2025 The Helium Authors', text)
            for forbidden in ('--no-sandbox', '--user-data-dir', 'sudo', 'pkexec', 'apparmor_parser'):
                self.assertNotIn(forbidden, text)
            subprocess.run(['sh', '-n', str(package.ASSETS / name)], check=True)


if __name__ == '__main__':
    unittest.main()
