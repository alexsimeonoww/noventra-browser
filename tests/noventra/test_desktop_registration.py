"""Desktop registration and launch checks in disposable XDG directories."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


installer = module('desktop_installer', 'noventra/package/installer.py')
development = module('desktop_development', 'tools/noventra/install_desktop.py')
TEMPLATE = ROOT / 'noventra/package/noventra-browser.desktop'


class DesktopRegistrationTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('xdg-settings') and shutil.which('desktop-file-validate'),
                         'XDG desktop integration unavailable')
    def test_selecting_default_preserves_registered_desktop_file(self):
        with tempfile.TemporaryDirectory(prefix='noventra-default-registration-') as temporary:
            root = Path(temporary)
            launcher = root / 'AppRun'
            launcher.write_text('#!/bin/sh\nexit 0\n')
            launcher.chmod(0o755)
            for name, content in (
                    ('package', installer.desktop_contents(TEMPLATE, launcher, root / 'icon.png')),
                    ('development', development.desktop_contents(launcher, root / 'icon.png'))):
                with self.subTest(registration=name):
                    data = root / name / 'data'
                    config = root / name / 'config'
                    desktop = data / 'applications/noventra-browser.desktop'
                    desktop.parent.mkdir(parents=True)
                    config.mkdir()
                    desktop.write_bytes(content)
                    before = installer.describe(desktop)
                    env = dict(os.environ, XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(config),
                               XDG_CONFIG_DIRS=str(root / 'empty-config'),
                               XDG_CURRENT_DESKTOP='', DESKTOP_SESSION='', DE='')
                    env.pop('BROWSER', None)
                    for _ in range(2):
                        selected = subprocess.run(
                            ['xdg-settings', 'set', 'default-web-browser', desktop.name],
                            env=env, text=True, capture_output=True)
                        self.assertEqual(selected.returncode, 0, selected.stderr)
                        self.assertEqual(installer.describe(desktop), before,
                                         'Default selection changed the installer-owned desktop file')
                        checked = subprocess.run(
                            ['xdg-settings', 'check', 'default-web-browser', desktop.name],
                            env=env, text=True, capture_output=True, check=True)
                        self.assertEqual(checked.stdout.strip(), 'yes')
                    subprocess.run(['desktop-file-validate', str(desktop)], check=True)

    def test_simple_install_paths_work_with_omarchy_command_discovery(self):
        with tempfile.TemporaryDirectory(prefix='noventra-desktop-discovery-') as temporary:
            root = Path(temporary)
            launcher = root / 'AppRun'
            launcher.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            launcher.chmod(0o755)
            for name, content in (
                    ('package', installer.desktop_contents(TEMPLATE, launcher, root / 'icon.png')),
                    ('development', development.desktop_contents(launcher, root / 'icon.png'))):
                with self.subTest(registration=name):
                    desktop = root / (name + '.desktop')
                    desktop.write_bytes(content)
                    result = subprocess.run(
                        ['bash', '-c', '''command=$(sed -n 's/^Exec=\\([^ ]*\\).*/\\1/p' "$1" | head -1)
command -v "$command" >/dev/null || exit 3
"$command" "$2"
''', 'desktop-check', str(desktop), 'https://example.invalid/a%20b?x=$value'],
                        text=True, capture_output=True, check=False)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, 'https://example.invalid/a%20b?x=$value\n')

    @unittest.skipUnless(shutil.which('gio'), 'GIO desktop launcher unavailable')
    def test_reserved_install_paths_keep_desktop_quoting(self):
        with tempfile.TemporaryDirectory(prefix='noventra-desktop-escaping-') as temporary:
            root = Path(temporary)
            directory = root / 'space $literal `literal` %value ; quotation"and\''
            directory.mkdir()
            launcher = directory / 'AppRun'
            record = root / 'arguments'
            launcher.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$NOVENTRA_DESKTOP_TEST_RECORD"\n')
            launcher.chmod(0o755)
            url = 'https://example.invalid/a%20b?x=$value;keep'
            for name, content in (
                    ('package', installer.desktop_contents(TEMPLATE, launcher, root / 'icon.png')),
                    ('development', development.desktop_contents(launcher, root / 'icon.png'))):
                with self.subTest(registration=name):
                    desktop = root / (name + '.desktop')
                    desktop.write_bytes(content)
                    record.unlink(missing_ok=True)
                    result = subprocess.run(
                        ['gio', 'launch', str(desktop), url], text=True,
                        capture_output=True, check=False,
                        env=dict(os.environ, NOVENTRA_DESKTOP_TEST_RECORD=str(record)))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    deadline = time.monotonic() + 5
                    while not record.exists() and time.monotonic() < deadline:
                        time.sleep(.02)
                    self.assertEqual(record.read_text(), url + '\n')


if __name__ == '__main__':
    unittest.main()
