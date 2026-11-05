#!/usr/bin/env python3
"""Register a local Noventra launcher and its approved icons for this user."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def exec_argument(path):
    # Desktop-entry strings and Exec quoting each consume one escape level.
    value = str(path)
    if any(character in value for character in ('\n', '\r', '\0')):
        raise ValueError('Launcher paths cannot contain line breaks or NUL')
    for character in ('\\', '"', '`', '$'):
        value = value.replace(character, '\\' + character)
    return (value if re.fullmatch(r'[/A-Za-z0-9._:+-]+', value) else
            '"' + value.replace('\\', '\\\\').replace('%', '%%') + '"')


def desktop_contents(launcher, icon):
    template = (ROOT / 'noventra/package/noventra-browser.desktop').read_text()
    command = exec_argument(launcher)
    if '%' in str(launcher):
        # GIO validates the executable before expanding literal %% field codes.
        command = '/usr/bin/env ' + command
    lines = []
    count = 0
    for line in template.splitlines():
        if line.startswith('Exec='):
            if not line.startswith('Exec=noventra-browser'):
                raise ValueError('Unexpected portable launcher command')
            line = line.replace('noventra-browser', command, 1)
            count += 1
        elif line == 'Icon=noventra-browser':
            line = 'Icon=' + str(icon).replace('\\', '\\\\')
        lines.append(line)
    if count != 3:
        raise ValueError('Expected the main launcher and two desktop actions')
    return ('\n'.join(lines) + '\n').encode()


def install(launcher, data_home, record, appearance='dark'):
    launcher = launcher.expanduser().resolve(strict=True)
    if not launcher.is_file() or not os.access(launcher, os.X_OK):
        raise ValueError('The launcher must be an executable file')
    data_home = data_home.expanduser().absolute()
    record = record.expanduser().absolute()
    if record.exists():
        raise ValueError('Use a new installation record path')
    generated = ROOT / 'noventra/resources/generated'
    manifest = json.loads((generated / 'manifest.json').read_text())
    if manifest.get('desktop_source') != 'approved/noventra-symbol.png':
        raise ValueError('The generated icons must come from the approved logo')
    files = {}
    for relative, expected in manifest['files'].items():
        if relative.startswith('linux/hicolor/') and relative.endswith('/apps/noventra-browser.png'):
            data = (generated / relative).read_bytes()
            if digest(data) != expected:
                raise ValueError('Approved icon hash mismatch: ' + relative)
            files[Path('icons') / Path(relative).relative_to('linux')] = data
    if len(files) != 9:
        raise ValueError('Expected nine approved desktop icon sizes')
    panel_source = 'logo-dark-64.png' if appearance == 'dark' else 'logo-light-64.png'
    panel_data = (generated / panel_source).read_bytes()
    if digest(panel_data) != manifest['files'][panel_source]:
        raise ValueError('Approved panel icon hash mismatch')
    panel_icon = Path('icons/hicolor/64x64/apps/noventra-browser-panel.png')
    files[panel_icon] = panel_data
    icon = data_home / panel_icon
    desktop = Path('applications/noventra-browser.desktop')
    files[desktop] = desktop_contents(launcher, icon)
    for relative in files:
        if (data_home / relative).is_symlink():
            raise ValueError('Refusing to replace a symlink: ' + str(relative))
    with tempfile.TemporaryDirectory(prefix='noventra-desktop-') as temporary:
        candidate = Path(temporary) / 'noventra-browser.desktop'
        candidate.write_bytes(files[desktop])
        subprocess.run(['desktop-file-validate', str(candidate)], check=True)
    record.parent.mkdir(parents=True, exist_ok=True)
    result = {'format': 1, 'launcher': str(launcher), 'data_home': str(data_home),
              'desktop_id': 'noventra-browser.desktop', 'icon': str(icon),
              'approved_source': manifest['desktop_source'], 'panel_appearance': appearance,
              'panel_icon_source': panel_source, 'files': {}, 'cache_updates': []}
    try:
        for relative, data in files.items():
            target = data_home / relative
            previous = None
            if target.exists():
                before = record.parent / (record.stem + '-before') / relative
                before.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(target, before)
                previous = {'path': str(before), 'sha256': digest(before.read_bytes())}
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
            temporary.chmod(0o644)
            os.replace(temporary, target)
            result['files'][str(target)] = {'sha256': digest(data), 'before': previous}
        for command in (['update-desktop-database', str(data_home / 'applications')],
                        ['gtk-update-icon-cache', '--force', '--ignore-theme-index', str(data_home / 'icons/hicolor')]):
            completed = subprocess.run(command, capture_output=True, text=True)
            result['cache_updates'].append({'command': command, 'exit_code': completed.returncode,
                                            'stdout': completed.stdout, 'stderr': completed.stderr})
            completed.check_returncode()
        result['installed'] = True
        return result
    finally:
        record.write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('--appearance', choices=('dark', 'light'), default='dark',
                        help='Panel appearance; selects the existing contrasting browser mark')
    parser.add_argument('--data-home', type=Path,
                        default=Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')))
    arguments = parser.parse_args()
    result = install(arguments.launcher, arguments.data_home, arguments.record,
                     arguments.appearance)
    print(json.dumps({'installed': result['installed'], 'desktop_id': result['desktop_id'],
                      'icon': result['icon'], 'record': str(arguments.record)}, indent=2))


if __name__ == '__main__':
    main()
