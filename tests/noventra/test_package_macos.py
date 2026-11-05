"""Linux-safe synthetic validation units; never build, sign, mount or launch an app."""

import copy
import hashlib
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
import package_macos as package


def thin(cpu=0x100000c, filetype=2):
    # Header-only fixture: valid architecture shape, never an executable browser.
    return struct.pack('<8I', 0xfeedfacf, cpu, 0, filetype, 0, 0, 0, 0)


def universal():
    left, right = thin(0x1000007), thin()
    return (struct.pack('>2I', 0xcafebabe, 2)
            + struct.pack('>5I', 0x1000007, 0, 48, len(left), 0)
            + struct.pack('>5I', 0x100000c, 0, 80, len(right), 0) + left + right)


class ValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nv-mac-package-unit-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'Noventra.app'
        self.signing = self.root / 'Noventra Packaging'
        self.receipt = self.root / 'build-receipt.json'
        self.bundle(self.app, 'Noventra', package.BUNDLE_ID)
        self.put(self.app / 'Contents/Resources/Assets.car', b'Synthetic asset fixture, not actool output')
        framework = self.app / 'Contents/Frameworks/Noventra Framework.framework'
        version = framework / 'Versions/A'
        self.plist(version / 'Resources/Info.plist', {
            'CFBundleIdentifier': package.BUNDLE_ID + '.framework',
            'CFBundleExecutable': 'Noventra Framework'})
        self.put(version / 'Noventra Framework', thin(filetype=6), executable=True)
        (framework / 'Versions/Current').symlink_to('A', target_is_directory=True)
        (framework / 'Resources').symlink_to('Versions/Current/Resources', target_is_directory=True)
        (framework / 'Noventra Framework').symlink_to('Versions/Current/Noventra Framework')
        (framework / 'Helpers').symlink_to('Versions/Current/Helpers', target_is_directory=True)
        (framework / 'Libraries').symlink_to('Versions/Current/Libraries', target_is_directory=True)
        for suffix, identifier in (('', '.helper'), (' (Renderer)', '.helper.renderer'), (' (GPU)', '.helper')):
            self.bundle(version / 'Helpers' / ('Noventra Helper' + suffix + '.app'),
                        'Noventra Helper' + suffix, package.BUNDLE_ID + identifier)
        self.put(version / 'Helpers/chrome_crashpad_handler', thin(), executable=True)
        self.put(version / 'Libraries/libEGL.dylib', thin(filetype=6), executable=True)
        for name in package.ENTITLEMENTS:
            self.plist(self.signing / name, {'com.apple.security.cs.allow-jit': True}
                       if name != 'app-entitlements.plist' else {'com.apple.security.device.audio-input': True})
        for name in ('parts.py', 'build_props_config.py', 'chromium_config.py'):
            self.put(self.signing / 'signing' / name, b'# synthetic unit fixture; never executed\n')
        self.make_receipt()

    def put(self, path, value, executable=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)
        path.chmod(0o755 if executable else 0o644)

    def plist(self, path, value):
        self.put(path, plistlib.dumps(value))

    def bundle(self, path, executable, identifier):
        self.plist(path / 'Contents/Info.plist', {'CFBundleIdentifier': identifier,
            'CFBundleName': executable, 'CFBundleExecutable': executable,
            'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '156.0.1.2'})
        self.put(path / 'Contents/MacOS' / executable, thin(), executable=True)

    def edit_plist(self, path, **changes):
        value = plistlib.loads(path.read_bytes())
        value.update(changes)
        self.plist(path, value)

    def make_receipt(self):
        evidence = {}
        for gate in package.GATES:
            path = self.root / 'evidence' / (gate + '.json')
            self.put(path, json.dumps({'format': 1, 'gate': gate, 'status': 'passed',
                                      'unit_fixture_only': True}).encode())
            evidence[gate] = {'path': path.relative_to(self.root).as_posix(), 'sha256': package.sha(path)}
        self.data = {'format': 1, 'kind': 'noventra-macos-build',
            'target': {'os': 'mac', 'cpu': 'arm64', 'component_build': False},
            'app': {'name': 'Noventra.app', 'bundle_id': package.BUNDLE_ID,
                    'tree_sha256': package.inventory_digest(self.app)},
            'source': {'manifest_sha256': 'a' * 64},
            'gates': dict.fromkeys(package.GATES, True), 'evidence': evidence,
            'signing_inputs': {'path': 'Noventra Packaging',
                              'tree_sha256': package.inventory_digest(self.signing)}}
        self.save_receipt()

    def save_receipt(self):
        self.receipt.write_text(json.dumps(self.data))

    def plan(self):
        base = 'Noventra.app/Contents/Frameworks/Noventra Framework.framework'
        values = [
            ('app', 'Noventra.app', 'app-entitlements.plist', 'kill,library,restrict,runtime'),
            ('framework', base, None, ''),
            ('helper-app', base + '/Helpers/Noventra Helper.app', None, 'kill,library,restrict,runtime'),
            ('helper-renderer-app', base + '/Helpers/Noventra Helper (Renderer).app',
             'helper-renderer-entitlements.plist', 'kill,restrict,runtime'),
            ('helper-gpu-app', base + '/Helpers/Noventra Helper (GPU).app',
             'helper-gpu-entitlements.plist', 'kill,restrict,runtime'),
            ('crashpad', base + '/Helpers/chrome_crashpad_handler', None, 'kill,library,restrict,runtime'),
            ('libEGL.dylib', base + '/Libraries/libEGL.dylib', None, ''),
        ]
        return [{'name': n, 'path': p, 'entitlements': e, 'options': o,
                 'identifier': package.BUNDLE_ID, 'sign_with_identifier': False}
                for n, p, e, o in values]

    def test_valid_bundle_identity_framework_links_and_arm64_inventory(self):
        validated = package.validate_bundle(self.app)
        self.assertEqual(validated['version'], '156.0.1.2')
        self.assertEqual(len(validated['code']), 7)
        self.assertTrue(all(arches == ['arm64'] for arches in validated['code'].values()))
        self.assertEqual(package.validate_receipt(self.receipt, self.app)[1], self.signing)

    def test_canonical_inventory_hash_and_independent_root(self):
        entries = package.inventory(self.app)
        expected = hashlib.sha256(json.dumps(entries, sort_keys=True,
                                  separators=(',', ':')).encode('utf-8')).hexdigest()
        self.assertEqual(package.inventory_digest(self.app), expected)
        self.assertEqual(entries['Contents/Frameworks/Noventra Framework.framework/Versions/Current'],
                         {'type': 'symlink', 'target': 'A'})
        old = package.inventory_digest(self.app)
        (self.app / 'Contents/MacOS/Noventra').chmod(0o700)
        self.assertNotEqual(package.inventory_digest(self.app), old)

    def test_rejects_renamed_helium_id_executable_and_helpers(self):
        info = self.app / 'Contents/Info.plist'
        for changes in ({'CFBundleIdentifier': 'net.imput.helium'},
                        {'CFBundleName': 'Helium'}, {'CFBundleExecutable': '../Noventra'}):
            original = info.read_bytes()
            with self.subTest(changes=changes):
                self.edit_plist(info, **changes)
                with self.assertRaises((ValueError, OSError)):
                    package.validate_bundle(self.app)
                info.write_bytes(original)
        helper = self.app / 'Contents/Frameworks/Noventra Framework.framework/Versions/A/Helpers/Noventra Helper.app/Contents/Info.plist'
        self.edit_plist(helper, CFBundleIdentifier='net.imput.helium.helper')
        with self.assertRaisesRegex(ValueError, 'helper bundle identity'):
            package.validate_bundle(self.app)

    def test_x64_only_linux_and_truncated_browser_rejected(self):
        path = self.app / 'Contents/MacOS/Noventra'
        for data in (thin(0x1000007), b'\x7fELF' + b'linux fixture', thin()[:12]):
            with self.subTest(data=data):
                self.put(path, data, executable=True)
                with self.assertRaises(ValueError):
                    package.validate_bundle(self.app)

    def test_universal_arm64_allowed_but_cpu_mismatch_overlap_and_bounds_rejected(self):
        path = self.root / 'universal'
        path.write_bytes(universal())
        self.assertEqual(package.macho_architectures(path), {'arm64', 'x86_64'})
        for offset, value in ((8, 0x100000c), (20, 0xffffffff), (36, 49)):
            data = bytearray(universal())
            struct.pack_into('>I', data, offset, value)
            path.write_bytes(data)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                package.macho_architectures(path)

    def test_nested_x64_library_rejected(self):
        path = self.app / 'Contents/Frameworks/Noventra Framework.framework/Versions/A/Libraries/libEGL.dylib'
        self.put(path, thin(0x1000007, 6), executable=True)
        with self.assertRaisesRegex(ValueError, 'ARM64 slice required'):
            package.validate_bundle(self.app)

    def test_missing_assets_or_executable_permission_rejected(self):
        asset = self.app / 'Contents/Resources/Assets.car'
        asset.unlink()
        with self.assertRaisesRegex(ValueError, 'Assets.car'):
            package.validate_bundle(self.app)
        self.put(asset, b'fixture')
        (self.app / 'Contents/MacOS/Noventra').chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'Missing executable'):
            package.validate_bundle(self.app)

    def test_sparkle_framework_or_active_feed_rejected(self):
        sparkle = self.app / 'Contents/Frameworks/Sparkle.framework'
        sparkle.mkdir()
        with self.assertRaisesRegex(ValueError, 'Sparkle'):
            package.validate_bundle(self.app)
        sparkle.rmdir()
        self.edit_plist(self.app / 'Contents/Info.plist', SUFeedURL='https://fixture.invalid/update')
        with self.assertRaisesRegex(ValueError, 'Sparkle update'):
            package.validate_bundle(self.app)

    def test_link_escapes_absolute_broken_and_root_symlink_rejected(self):
        link = self.app / 'Contents/escape'
        outside = self.root / 'outside'
        outside.write_text('fixture')
        for target in ('../../outside', str(outside), 'missing'):
            link.symlink_to(target)
            with self.subTest(target=target), self.assertRaises((ValueError, OSError)):
                package.inventory(self.app)
            link.unlink()
        alias = self.root / 'Alias.app'
        alias.symlink_to(self.app, target_is_directory=True)
        with self.assertRaises(ValueError):
            package.inventory(alias)

    def test_fifo_and_setid_file_rejected(self):
        extra = self.app / 'Contents/fifo'
        os.mkfifo(extra)
        with self.assertRaisesRegex(ValueError, 'Special file'):
            package.inventory(self.app)
        extra.unlink()
        self.put(extra, b'fixture')
        extra.chmod(0o4755)
        with self.assertRaisesRegex(ValueError, 'Set-id'):
            package.inventory(self.app)

    def test_receipt_requires_all_real_gate_reports_and_hashes(self):
        for gate in package.GATES:
            original = copy.deepcopy(self.data)
            self.data['gates'][gate] = False
            self.save_receipt()
            with self.subTest(gate=gate), self.assertRaisesRegex(ValueError, 'gate'):
                package.validate_receipt(self.receipt, self.app)
            self.data = original
        self.data['evidence'][package.GATES[0]]['sha256'] = '0' * 64
        self.save_receipt()
        with self.assertRaisesRegex(ValueError, 'evidence hash mismatch'):
            package.validate_receipt(self.receipt, self.app)

    def test_failed_report_cannot_pass_with_success_boolean_and_matching_hash(self):
        gate = package.GATES[0]
        report = self.root / self.data['evidence'][gate]['path']
        report.write_text(json.dumps({'format': 1, 'gate': gate, 'status': 'failed'}))
        self.data['evidence'][gate]['sha256'] = package.sha(report)
        self.save_receipt()
        with self.assertRaisesRegex(ValueError, 'did not pass'):
            package.validate_receipt(self.receipt, self.app)

    def test_receipt_must_bind_exact_app_signing_inputs_and_native_target(self):
        original = copy.deepcopy(self.data)
        for change in ('app', 'signing_inputs', 'target'):
            self.data = copy.deepcopy(original)
            if change == 'target':
                self.data['target']['os'] = 'linux'
            else:
                self.data[change]['tree_sha256'] = '0' * 64
            self.save_receipt()
            with self.subTest(change=change), self.assertRaises(ValueError):
                package.validate_receipt(self.receipt, self.app)

    def test_receipt_evidence_paths_and_duplicate_json_keys_rejected(self):
        for path in ('../escape', '/tmp/escape', 'evidence//report', 'evidence/./report'):
            self.data['evidence'][package.GATES[0]]['path'] = path
            self.save_receipt()
            with self.subTest(path=path), self.assertRaises(ValueError):
                package.validate_receipt(self.receipt, self.app)
        self.receipt.write_text('{"format":1,"format":1}')
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON'):
            package.json_object(self.receipt)

    def test_plan_covers_each_macho_once_and_orders_inside_out(self):
        code = package.validate_bundle(self.app)['code']
        plan = package.validate_plan(self.app, self.signing, self.plan(), code)
        self.assertEqual([part['name'] for part in plan][-2:], ['framework', 'app'])
        self.assertEqual(next(part for part in plan if part['name'] == 'helper-renderer-app')['options'],
                         'kill,restrict,runtime')

    def test_unknown_code_missing_parts_escape_and_entitlements_rejected(self):
        code = package.validate_bundle(self.app)['code']
        plans = [self.plan()[:-1], self.plan() + [self.plan()[0]], self.plan(), self.plan()]
        plans[2][0]['path'] = '../Other.app'
        plans[3][0]['entitlements'] = '../other.plist'
        for plan in plans:
            with self.subTest(plan=plan), self.assertRaises((ValueError, OSError)):
                package.validate_plan(self.app, self.signing, plan, code)
        extra = self.app / 'Contents/extra.dylib'
        self.put(extra, thin(filetype=6), executable=True)
        code = package.validate_bundle(self.app)['code']
        with self.assertRaisesRegex(ValueError, 'Unplanned Mach-O'):
            package.validate_plan(self.app, self.signing, self.plan(), code)

    def test_jit_helpers_keep_real_entitlement_and_compatible_signing_flags(self):
        code = package.validate_bundle(self.app)['code']
        for change in ('missing', 'library', 'unknown'):
            plan = self.plan()
            renderer = next(part for part in plan if part['name'] == 'helper-renderer-app')
            if change == 'missing':
                renderer['entitlements'] = None
            else:
                renderer['options'] += ',library' if change == 'library' else ',invented'
            with self.subTest(change=change), self.assertRaises(ValueError):
                package.validate_plan(self.app, self.signing, plan, code)
        self.plist(self.signing / 'helper-renderer-entitlements.plist', {})
        with self.assertRaisesRegex(ValueError, 'JIT helper'):
            package.validate_plan(self.app, self.signing, self.plan(), code)

    def test_unplanned_executable_script_rejected(self):
        self.put(self.app / 'Contents/launch.sh', b'#!/bin/sh\nexit 0\n', executable=True)
        with self.assertRaisesRegex(ValueError, 'non-Mach-O executable'):
            package.validate_bundle(self.app)

    def test_pinned_chromium_install_resource_accepted(self):
        script = (Path(__file__).parent / 'fixtures/macos-install-156.sh').read_bytes()
        resource = self.app / ('Contents/Frameworks/Noventra Framework.framework/'
                               'Versions/A/Resources/install.sh')
        self.put(resource, script, executable=True)
        validated = package.validate_bundle(self.app)
        self.assertNotIn(resource.relative_to(self.app).as_posix(), validated['code'])

    def test_modified_chromium_install_resource_rejected(self):
        script = (Path(__file__).parent / 'fixtures/macos-install-156.sh').read_bytes()
        resource = self.app / ('Contents/Frameworks/Noventra Framework.framework/'
                               'Versions/A/Resources/install.sh')
        self.put(resource, script + b'\n# changed\n', executable=True)
        with self.assertRaises(ValueError):
            package.validate_bundle(self.app)

    def test_misplaced_chromium_install_resource_rejected(self):
        script = (Path(__file__).parent / 'fixtures/macos-install-156.sh').read_bytes()
        self.put(self.app / 'Contents/install.sh', script, executable=True)
        with self.assertRaisesRegex(ValueError, 'non-Mach-O executable'):
            package.validate_bundle(self.app)

    def test_receipt_shapes_and_component_boolean_are_strict(self):
        for key, value in (('evidence', []), ('source', None), ('format', True)):
            original = copy.deepcopy(self.data)
            self.data[key] = value
            self.save_receipt()
            with self.subTest(key=key), self.assertRaises(ValueError):
                package.validate_receipt(self.receipt, self.app)
            self.data = original
        self.data['target']['component_build'] = 0
        self.save_receipt()
        with self.assertRaisesRegex(ValueError, 'Component-build'):
            package.validate_receipt(self.receipt, self.app)

    def test_linux_package_refuses_before_any_subprocess_or_output(self):
        with patch.object(package.sys, 'platform', 'linux'), patch.object(package.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'native macOS'):
                package.package(self.app, self.receipt, self.root / 'output')
            run.assert_not_called()
        self.assertFalse((self.root / 'output').exists())

    def test_ad_hoc_package_refused_before_mutation(self):
        with patch.object(package.sys, 'platform', 'darwin'), patch.object(package.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'Apple-issued'):
                package.package(self.app, self.receipt, self.root / 'output')
            run.assert_not_called()
        self.assertFalse((self.root / 'output').exists())

    def test_identity_selector_must_be_exact_available_apple_identity(self):
        fingerprint = 'A' * 40
        for name in ('Self Issued Local', 'Apple Development: Fixture', 'Developer ID Application: Fixture'):
            response = subprocess.CompletedProcess([], 0, f'  1) {fingerprint} "{name}"\n'.encode(), b'')
            with self.subTest(name=name), patch.object(package.subprocess, 'run', return_value=response):
                if name == 'Self Issued Local':
                    with self.assertRaisesRegex(ValueError, 'Apple-issued'):
                        package.select_signing_identity(fingerprint)
                else:
                    self.assertEqual(package.select_signing_identity(fingerprint)['sha1'], fingerprint)
                    with self.assertRaisesRegex(ValueError, 'not available'):
                        package.select_signing_identity('B' * 40)
        with patch.object(package.subprocess, 'run') as run:
            for selector in ('-', 'Fixture', 'A' * 39):
                with self.subTest(selector=selector), self.assertRaises(ValueError):
                    package.select_signing_identity(selector)
            run.assert_not_called()

    def test_signed_part_requires_same_team_flags_and_generated_entitlements(self):
        identity = {'sha1': 'A' * 40, 'type': 'Apple Development', 'team_identifier': 'TESTTEAM01'}
        part = self.plan()[0]
        flags = '0x12a00(kill,restrict,library-validation,runtime)'
        expected = plistlib.loads((self.signing / part['entitlements']).read_bytes())

        def invoke(team='TESTTEAM01', actual_flags=flags, entitlements=expected):
            def run(argv, **kwargs):
                if '--entitlements' in argv:
                    return plistlib.dumps(entitlements)
                if '--display' in argv:
                    return f'CodeDirectory v=20500 flags={actual_flags}\nTeamIdentifier={team}\n'.encode()
                return b''
            return package.verify_signed_part(self.app, part, self.signing, identity, run)

        invoke()
        for kwargs in ({'team': 'OTHERTEAM1'}, {'actual_flags': '0x0(none)'},
                       {'entitlements': {**expected, 'com.apple.security.cs.disable-library-validation': True}}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                invoke(**kwargs)

    def test_every_signing_command_uses_selected_identity_and_generated_settings(self):
        for part in self.plan():
            argv = package.signing_command(self.app.parent / part['path'], part, self.signing, 'A' * 40)
            self.assertEqual(argv[argv.index('--sign') + 1], 'A' * 40)
            if part['options']:
                self.assertEqual(argv[argv.index('--options') + 1], part['options'])
            if part['entitlements']:
                self.assertEqual(argv[argv.index('--entitlements') + 1], self.signing / part['entitlements'])

    def test_identity_preflight_rejects_fake_apple_named_certificate(self):
        identity = {'sha1': 'A' * 40, 'type': 'Apple Development'}
        def run(argv):
            if '--verify' in argv:
                raise ValueError('Apple anchor verification failed')
            return b''
        with self.assertRaisesRegex(ValueError, 'Apple anchor'):
            package.preflight_signing_identity(identity, self.app / 'Contents/MacOS/Noventra', self.root, run)
        self.assertFalse(any(p.name.startswith('.noventra-signing-') for p in self.root.iterdir()))

    def test_plan_rejects_security_exception_even_in_generated_entitlements(self):
        path = self.signing / 'app-entitlements.plist'
        self.plist(path, {'com.apple.security.cs.disable-library-validation': True})
        with self.assertRaisesRegex(ValueError, 'protection'):
            package.validate_plan(self.app, self.signing, self.plan(), package.validate_bundle(self.app)['code'])

    def test_plan_cannot_drop_hardened_runtime_or_library_validation(self):
        for flag in ('runtime', 'library'):
            plan = self.plan()
            plan[0]['options'] = ','.join(v for v in plan[0]['options'].split(',') if v != flag)
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, 'protection'):
                package.validate_plan(self.app, self.signing, plan, package.validate_bundle(self.app)['code'])


if __name__ == '__main__':
    unittest.main()
