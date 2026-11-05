#!/usr/bin/env python3
"""Install, update or remove this unsigned local Linux package for the current user.

Only inventoried package files are copied. Uninstall removes exact recorded,
unchanged files and restores unchanged backups; browser profiles are never used.
"""
# Copyright 2026 Noventra contributors
# SPDX-License-Identifier: GPL-3.0-only
import argparse
import copy
from contextlib import contextmanager, nullcontext
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ID = 'local.noventra.browser'
DIRECTORY = 'noventra-browser'
MANIFEST = 'install-manifest.json'
RECORD = 'installation.json'
UPDATE = '.update'
SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
EXTERNAL = ('applications/noventra-browser.desktop',) + tuple(
    f'icons/hicolor/{n}x{n}/apps/noventra-browser.png' for n in SIZES)
LINK = ('usr/bin/noventra-browser', '../../opt/noventra/noventra-wrapper')


def require(value, message):
    if not value:
        raise ValueError(message)


def relative(name):
    require(isinstance(name, str) and name and not name.startswith('/')
            and '\\' not in name and ':' not in name
            and not any(ord(c) < 32 or ord(c) == 127 for c in name)
            and all(x not in ('', '.', '..') for x in name.split('/')),
            f'Unsafe inventory path: {name!r}')
    return Path(name)


def entry(root, name):
    require(stat.S_ISDIR(root.lstat().st_mode), f'Unsafe root directory: {root}')
    parts = relative(name).parts
    current = root
    for part in parts[:-1]:
        current /= part
        require(stat.S_ISDIR(current.lstat().st_mode), f'Unsafe directory: {current}')
    return current / parts[-1]


def identity(path):
    info = path.lstat()
    return [info.st_dev, info.st_ino]


def describe(path):
    before = path.lstat()
    if stat.S_ISLNK(before.st_mode):
        return {'symlink': os.readlink(path), 'identity': identity(path)}
    require(stat.S_ISREG(before.st_mode), f'Expected a regular file: {path}')
    require(not before.st_mode & (stat.S_ISUID | stat.S_ISGID), f'Privileged file: {path}')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        opened = os.fstat(stream.fileno())
        require((opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino),
                f'File changed while opening: {path}')
        hasher = hashlib.sha256()
        while chunk := stream.read(1024 * 1024):
            hasher.update(chunk)
        digest = hasher.hexdigest()
    after = path.lstat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_mode)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_mode),
            f'File changed while reading: {path}')
    return {'sha256': digest, 'size': after.st_size, 'mode': oct(stat.S_IMODE(after.st_mode)),
            'identity': [after.st_dev, after.st_ino]}


def unchanged(root, name, expected):
    try:
        return describe(entry(root, name)) == expected
    except (OSError, ValueError):
        return False


def json_file(path):
    require(stat.S_ISREG(path.lstat().st_mode) and path.stat().st_size <= 16 * 1024**2,
            f'Invalid inventory/record: {path}')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f'Duplicate JSON key: {key}')
            result[key] = value
        return result
    value = json.loads(path.read_bytes(), object_pairs_hook=unique,
                       parse_constant=lambda x: require(False, f'Invalid JSON value: {x}'))
    require(isinstance(value, dict), f'Expected JSON object: {path}')
    return value


def validate_package(source):
    manifest = json_file(source / MANIFEST)
    require(manifest.get('format') == 1 and manifest.get('id') == ID,
            'Unsupported Noventra installer inventory')
    files = manifest.get('files')
    require(isinstance(files, dict) and files, 'Empty installer inventory')
    required = {'AppRun', 'installer.py', 'install.sh', 'uninstall.sh', 'noventra-package.json',
                'noventra-browser.desktop', 'opt/noventra/noventra-browser',
                *(f'usr/share/{name}' for name in EXTERNAL[1:])}
    require(required <= files.keys(), 'Incomplete installer inventory')
    for name, expected in files.items():
        path = entry(source, name)
        require(isinstance(expected, dict), f'Invalid inventory entry: {name}')
        if 'symlink' in expected:
            require((name, expected['symlink']) == LINK and set(expected) == {'symlink'},
                    f'Unsupported package link: {name}')
        else:
            require(set(expected) == {'sha256', 'size', 'mode'}
                    and isinstance(expected['sha256'], str)
                    and re.fullmatch('[0-9a-f]{64}', expected['sha256'])
                    and type(expected['size']) is int and expected['size'] >= 0
                    and expected['mode'] in ('0o644', '0o755'), f'Invalid inventory entry: {name}')
        actual = describe(path)
        actual.pop('identity')
        require(actual == expected, f'Package integrity check failed: {name}')
    actual_names = set()
    for directory, dirs, names in os.walk(source, followlinks=False):
        for name in dirs + names:
            path = Path(directory) / name
            mode = path.lstat().st_mode
            if not stat.S_ISDIR(mode):
                actual_names.add(path.relative_to(source).as_posix())
    require(actual_names == files.keys() | {MANIFEST}, 'Package has missing or unexpected files')
    package = json_file(source / 'noventra-package.json')
    require(package.get('id') == ID and package.get('platform') == 'linux', 'Not a Linux Noventra package')
    host = os.uname()
    require(host.sysname == 'Linux' and host.machine in ('x86_64', 'aarch64')
            and package.get('architecture') == host.machine,
            'Package architecture does not match this Linux machine; nothing was replaced')
    return manifest, describe(source / MANIFEST)['sha256']


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def exchange_paths(first, second):
    """Atomically exchange two existing Linux paths, or change neither.

    Do not emulate this with two renames: the installed launcher must always
    exist, including while rolling back an interrupted update.
    """
    libc = ctypes.CDLL(None, use_errno=True)
    try:
        renameat2 = libc.renameat2
    except AttributeError:
        raise OSError(errno.ENOSYS, 'Atomic updates are not supported by this system') from None
    renameat2.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    renameat2.restype = ctypes.c_int
    # AT_FDCWD, RENAME_EXCHANGE. Neither name can disappear between the moves.
    if renameat2(-100, os.fsencode(first), -100, os.fsencode(second), 2) != 0:
        code = ctypes.get_errno()
        raise OSError(code, 'Atomic updates require filesystem support for rename exchange')


def require_atomic_exchange(preparation):
    first, second = preparation / 'exchange-probe-a', preparation / 'exchange-probe-b'
    first.mkdir(); second.mkdir()
    try:
        exchange_paths(first, second)
    finally:
        first.rmdir(); second.rmdir()


def save(root, value, name=RECORD):
    with tempfile.NamedTemporaryFile(dir=root, prefix='.installation-', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write((json.dumps(value, indent=2, sort_keys=True) + '\n').encode())
        stream.flush()
        os.fsync(stream.fileno())
    try:
        temporary.chmod(0o600)
        os.replace(temporary, root / name)
        sync_directory(root)
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def lock(root, *, blocking=False):
    fd = os.open(root / '.install-lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        require(stat.S_ISREG(os.fstat(fd).st_mode), 'Invalid installation lock')
        fcntl.flock(fd, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        yield
    finally:
        os.close(fd)


def state_for(root, data_home):
    state = json_file(root / RECORD)
    return validate_state(state, root, data_home)


def validate_state(state, root, data_home):
    require(state.get('format') == 1 and state.get('id') == ID
            and state.get('data_home') == str(data_home)
            and state.get('root_identity') == identity(root), 'Installation record does not match this location')
    require(isinstance(state.get('app_files'), dict) and isinstance(state.get('external'), dict)
            and isinstance(state.get('directories'), dict), 'Invalid installation record')
    def valid_record(record):
        require(isinstance(record, dict) and isinstance(record.get('identity'), list)
                and len(record['identity']) == 2
                and all(type(x) is int and x >= 0 for x in record['identity']),
                'Invalid file ownership record')
        if 'symlink' in record:
            require(set(record) == {'symlink', 'identity'} and record['symlink'] == LINK[1],
                    'Invalid link ownership record')
        else:
            require(set(record) == {'sha256', 'size', 'mode', 'identity'}
                    and isinstance(record['sha256'], str)
                    and re.fullmatch('[0-9a-f]{64}', record['sha256'])
                    and type(record['size']) is int and record['size'] >= 0
                    and isinstance(record['mode'], str)
                    and re.fullmatch(r'0o[0-7]{1,3}', record['mode']), 'Invalid file ownership record')
    for name in state['app_files']:
        relative(name)
        valid_record(state['app_files'][name])
        if 'symlink' in state['app_files'][name]:
            require(name == LINK[0], 'Unexpected link ownership path')
    require(state['external'].keys() <= set(EXTERNAL), 'Unexpected desktop/icon ownership record')
    for operation in state['external'].values():
        require(isinstance(operation, dict) and set(operation) == {'installed', 'backup'},
                'Invalid desktop/icon ownership record')
        valid_record(operation['installed'])
        require('symlink' not in operation['installed'], 'Invalid desktop/icon link record')
        backup = operation.get('backup')
        if backup:
            require(isinstance(backup, dict) and set(backup) == {'path', 'record'}
                    and isinstance(backup['path'], str)
                    and re.fullmatch(r'backups/[0-9]+', backup['path']), 'Invalid backup path')
            valid_record(backup['record'])
            require('symlink' not in backup['record'], 'Invalid backup link record')
    allowed_dirs = {DIRECTORY}
    for name in [*(f'{DIRECTORY}/app/{x}' for x in state['app_files']),
                 *EXTERNAL, f'{DIRECTORY}/backups/0']:
        allowed_dirs.update(str(x) for x in relative(name).parents if str(x) != '.')
    for name in state['directories']:
        relative(name)
        require(name in allowed_dirs or name == f'{DIRECTORY}/app'
                or name.startswith(f'{DIRECTORY}/app/'), 'Unexpected directory ownership record')
    return state


def ensure_parents(data_home, target, root, state):
    current = data_home
    for part in target.relative_to(data_home).parts[:-1]:
        current /= part
        if not os.path.lexists(current):
            current.mkdir(mode=0o755)
            state['directories'][current.relative_to(data_home).as_posix()] = identity(current)
            save(root, state)
        require(stat.S_ISDIR(current.lstat().st_mode), f'Unsafe directory: {current}')


def copy_file(source, target, mode=None):
    expected = describe(source)
    require('sha256' in expected, f'Expected regular source: {source}')
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as inp, target.open('xb') as output:
        opened = os.fstat(inp.fileno())
        require([opened.st_dev, opened.st_ino] == expected['identity'],
                f'Source changed while opening: {source}')
        shutil.copyfileobj(inp, output, length=1024 * 1024)
        target.chmod(int(expected['mode'], 8) if mode is None else mode)
        output.flush()
        os.fsync(output.fileno())
    require(describe(source) == expected, f'Source changed during copy: {source}')
    actual = describe(target)
    require(actual['sha256'] == expected['sha256'], f'Copy failed: {target}')
    return actual


def desktop_contents(source, launcher, icon):
    # Desktop-entry strings and Exec quoting each consume one escape level.
    value = str(launcher)
    require(not any(c in value for c in ('\n', '\r', '\0')), 'Invalid installation path')
    for character in ('\\', '"', '`', '$'):
        value = value.replace(character, '\\' + character)
    command = (value if re.fullmatch(r'[/A-Za-z0-9._:+-]+', value) else
               '"' + value.replace('\\', '\\\\').replace('%', '%%') + '"')
    if '%' in str(launcher):
        command = '/usr/bin/env ' + command
    lines, count = [], 0
    for line in source.read_text().splitlines():
        if line.startswith('Exec='):
            require(line.startswith('Exec=noventra-browser'), 'Unexpected desktop launcher')
            line = line.replace('noventra-browser', command, 1)
            count += 1
        elif line == 'Icon=noventra-browser':
            line = 'Icon=' + str(icon).replace('\\', '\\\\')
        lines.append(line)
    require(count == 3, 'Expected three desktop launch actions')
    return ('\n'.join(lines) + '\n').encode()


def caches(data_home):
    warnings = []
    for command in (['update-desktop-database', str(data_home / 'applications')],
                    ['gtk-update-icon-cache', '--force', '--ignore-theme-index', str(data_home / 'icons/hicolor')]):
        directory = Path(command[-1])
        try:
            current = data_home
            for part in directory.relative_to(data_home).parts:
                current /= part
                require(stat.S_ISDIR(current.lstat().st_mode), 'Changed cache directory')
        except FileNotFoundError:
            continue  # A complete uninstall can leave no directory to refresh.
        except (OSError, ValueError):
            warnings.append(f'{command[0]} skipped because its directory is missing or changed')
            continue
        if shutil.which(command[0]):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                warnings.append(f'{command[0]} exited {result.returncode}; the desktop may need to refresh')
    return warnings


def app_directories(state):
    prefix = f'{DIRECTORY}/app'
    return {name[len(prefix):].lstrip('/'): value
            for name, value in state['directories'].items()
            if name == prefix or name.startswith(prefix + '/')}


def verify_tree(path, files, directories, *, missing=False):
    """Check every path, including empty directories, before moving/removing it."""
    if not os.path.lexists(path):
        require(missing, f'Missing installed app: {path}')
        return
    actual_files, actual_dirs = set(), {''}
    require(stat.S_ISDIR(path.lstat().st_mode), f'Changed app directory: {path}')
    for directory, dirs, names in os.walk(path, followlinks=False):
        for name in dirs + names:
            target = Path(directory) / name
            relative_name = target.relative_to(path).as_posix()
            (actual_dirs if stat.S_ISDIR(target.lstat().st_mode) else actual_files).add(relative_name)
    require(actual_files <= files.keys() and actual_dirs <= directories.keys(),
            f'Unexpected files or directories in {path}; nothing was replaced')
    require(missing or (actual_files == files.keys() and actual_dirs == directories.keys()),
            f'Missing installed files or directories in {path}')
    for name in actual_dirs:
        require(identity(path / name) == directories[name], f'Changed app directory: {path / name}')
    for name in actual_files:
        require(unchanged(path, name, files[name]), f'Changed installed file: {path / name}')


def remove_tree(path, files, directories):
    # An interrupted cleanup can resume, but unknown or changed paths are retained.
    verify_tree(path, files, directories, missing=True)
    if not os.path.lexists(path):
        return
    for name, expected in files.items():
        if os.path.lexists(path / name):
            require(unchanged(path, name, expected), f'Changed update file: {path / name}')
            entry(path, name).unlink()
    for name in sorted(directories, key=lambda x: len(Path(x).parts), reverse=True):
        target = path / name
        if target.exists():
            require(identity(target) == directories[name], f'Changed update directory: {target}')
            target.rmdir()


def verify_external(root, data_home, state):
    require(state['external'].keys() == set(EXTERNAL), 'Incomplete desktop/icon installation')
    for name, operation in state['external'].items():
        require(unchanged(data_home, name, operation['installed']),
                f'Changed desktop/icon file: {data_home / name}; nothing was replaced')
        backup = operation['backup']
        require(not backup or unchanged(root, backup['path'], backup['record']),
                f'Changed original desktop/icon backup for {name}; nothing was replaced')


def verify_installation(root, data_home, state):
    require(state['status'] == 'installed', 'Partial installation; use uninstall.sh to review it first')
    verify_tree(root / 'app', state['app_files'], app_directories(state))
    manifest, fingerprint = validate_package(root / 'app')
    require(fingerprint == state['manifest_sha256']
            and state['app_files'].keys() == manifest['files'].keys() | {MANIFEST},
            'Installation inventory changed; nothing was replaced')
    verify_external(root, data_home, state)


def require_closed(app):
    """Do not move resources out from under an installed Linux browser."""
    prefix = str(app) + '/'
    try:
        binary_identity = identity(app / 'opt/noventra/noventra-browser')
    except FileNotFoundError:
        binary_identity = None
    for process in Path('/proc').iterdir():
        if not process.name.isdecimal():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            executable = os.readlink(process / 'exe')
            info = (process / 'exe').stat()
            running = executable.startswith(prefix) or [info.st_dev, info.st_ino] == binary_identity
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            # Some same-UID protected services hide exe. Their first argument is
            # still useful without treating an unrelated service as Noventra.
            try:
                command = (process / 'cmdline').read_bytes().split(b'\0', 1)[0]
            except (OSError, ProcessLookupError):
                continue
            running = os.fsdecode(command).startswith(prefix)
        require(not running,
                f'Close Noventra before updating or removing it (process {process.name}).')


def recover_update(root, data_home):
    transaction = root / UPDATE
    if not os.path.lexists(transaction):
        return
    require(stat.S_ISDIR(transaction.lstat().st_mode), 'Changed update directory; nothing was removed')
    if not any(transaction.iterdir()):
        # Cleanup may have stopped between the final journal unlink and rmdir.
        transaction.rmdir()
        sync_directory(root)
        return
    journal = json_file(transaction / 'journal.json')
    require(set(journal) == {'format', 'id', 'identity', 'old', 'new'}
            and journal['format'] == 1 and journal['id'] == ID
            and journal['identity'] == identity(transaction), 'Invalid update recovery journal')
    old = validate_state(journal['old'], root, data_home)
    new = validate_state(journal['new'], root, data_home)
    require(old['status'] == new['status'] == 'installed'
            and old['external'].keys() == new['external'].keys() == set(EXTERNAL)
            and all(old['external'][name]['backup'] == new['external'][name]['backup']
                    for name in EXTERNAL), 'Invalid update recovery state')
    allowed = {'journal.json', 'old-app', 'new-app'} | {
        f'{kind}-{index}' for kind in ('old-external', 'new-external') for index in range(len(EXTERNAL))}
    require({x.name for x in transaction.iterdir()} <= allowed,
            'Unexpected update files; recovery retained them for review')
    state = state_for(root, data_home)
    require(state == old or state == new, 'Installation changed during update; recovery retained all files')
    require(app_directories(old)[''] != app_directories(new)[''], 'Invalid app exchange identities')

    def role(path):
        if not os.path.lexists(path):
            return None
        require(stat.S_ISDIR(path.lstat().st_mode), 'Changed app recovery path; retained for review')
        current = identity(path)
        if current == app_directories(old)['']:
            return 'old'
        if current == app_directories(new)['']:
            return 'new'
        raise ValueError('Changed app recovery identity; retained for review')

    slots = [transaction / 'old-app', transaction / 'new-app']
    roles = [role(path) for path in slots]
    require(roles.count('old') <= 1 and roles.count('new') <= 1,
            'Duplicate recovery app identities; retained for review')
    if state == new:
        verify_installation(root, data_home, new)
        for path in slots:
            selected = role(path)
            require(selected in (None, 'old'), 'Conflicting committed app recovery tree')
            if selected:
                require_closed(path)
                remove_tree(path, old['app_files'], app_directories(old))
    else:
        require_closed(root / 'app')
        for path in slots:
            if os.path.lexists(path):
                require_closed(path)
        app = root / 'app'
        current = role(app)
        if current in (None, 'new'):
            old_paths = [path for path in slots if role(path) == 'old']
            require(len(old_paths) == 1, 'Missing original app during recovery; retained for review')
            original = old_paths[0]
            verify_tree(original, old['app_files'], app_directories(old))
        if current == 'new':
            verify_tree(app, new['app_files'], app_directories(new))
            exchange_paths(app, original)
            sync_directory(root)
            sync_directory(transaction)
        elif current is None:
            # Legacy format1 two-rename journals can already have a missing
            # app. Restore that old layout without creating a new absence.
            original.rename(app)
            sync_directory(root)
            sync_directory(transaction)
        verify_tree(app, old['app_files'], app_directories(old))
        for index, name in enumerate(EXTERNAL):
            expected = old['external'][name]['installed']
            if unchanged(data_home, name, expected):
                continue
            target = entry(data_home, name)
            require(not os.path.lexists(target) or unchanged(data_home, name, new['external'][name]['installed']),
                    f'Changed desktop/icon during recovery: {target}; retained for review')
            backup = f'old-external-{index}'
            require(unchanged(transaction, backup, expected), f'Changed update backup: {backup}')
            os.replace(transaction / backup, target)
            sync_directory(target.parent)
        verify_installation(root, data_home, old)
        for path in slots:
            selected = role(path)
            require(selected in (None, 'new'), 'Conflicting rolled-back app recovery tree')
            if selected:
                remove_tree(path, new['app_files'], app_directories(new))
    for index, name in enumerate(EXTERNAL):
        for kind, selected in (('old-external', old), ('new-external', new)):
            relative_name = f'{kind}-{index}'
            path = transaction / relative_name
            if os.path.lexists(path):
                require(unchanged(transaction, relative_name, selected['external'][name]['installed']),
                        f'Changed update file: {path}; retained for review')
                path.unlink()
    # No app tree can be silently ignored, even in a changed recovery journal.
    require({x.name for x in transaction.iterdir()} == {'journal.json'},
            'Incomplete update recovery; files retained for review')
    (transaction / 'journal.json').unlink()
    transaction.rmdir()
    sync_directory(root)


def update(source, data_home, state, manifest, fingerprint):
    root = data_home / DIRECTORY
    verify_installation(root, data_home, state)
    require_closed(root / 'app')
    # Build the entire candidate before touching the active app or desktop files.
    # A kill while preparing can leave this private temporary directory; it is
    # never mistaken for an installation or recursively removed on a later run.
    with tempfile.TemporaryDirectory(dir=root, prefix='.prepare-update-') as temporary:
        preparation = Path(temporary)
        staged = preparation / 'new-app'
        staged.mkdir(mode=0o755)
        next_state = copy.deepcopy(state)
        next_state['manifest_sha256'] = fingerprint
        next_state['app_files'] = {}
        prefix = f'{DIRECTORY}/app'
        next_state['directories'] = {name: value for name, value in state['directories'].items()
                                     if name != prefix and not name.startswith(prefix + '/')}
        for name in [*manifest['files'], MANIFEST]:
            target = staged / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if 'symlink' in manifest['files'].get(name, {}):
                target.symlink_to(manifest['files'][name]['symlink'])
                record = describe(target)
            else:
                record = copy_file(entry(source, name), target)
            next_state['app_files'][name] = record
        for directory, dirs, _ in os.walk(staged):
            path = Path(directory)
            relative_name = path.relative_to(staged).as_posix()
            name = prefix if relative_name == '.' else prefix + '/' + relative_name
            next_state['directories'][name] = identity(path)
            sync_directory(path)
        require(validate_package(staged)[1] == fingerprint, 'Staged package changed')
        # Refuse unsupported kernels/filesystems before publishing a journal or
        # replacing any installed app/desktop entry. No non-atomic fallback.
        require_atomic_exchange(preparation)
        contents = {name: (staged / 'usr/share' / name).read_bytes() for name in EXTERNAL[1:]}
        contents[EXTERNAL[0]] = desktop_contents(
            staged / 'noventra-browser.desktop', root / 'app/AppRun',
            data_home / 'icons/hicolor/256x256/apps/noventra-browser.png')
        for index, name in enumerate(EXTERNAL):
            target = preparation / f'new-external-{index}'
            with target.open('xb') as stream:
                stream.write(contents[name])
                target.chmod(0o644)
                stream.flush()
                os.fsync(stream.fileno())
            next_state['external'][name]['installed'] = describe(target)
            # Keep the prior inode for exact rollback and eventual uninstall.
            os.link(entry(data_home, name), preparation / f'old-external-{index}', follow_symlinks=False)
        journal = {'format': 1, 'id': ID, 'identity': identity(preparation),
                   'old': state, 'new': next_state}
        save(preparation, journal, 'journal.json')
        verify_installation(root, data_home, state)
        require_closed(root / 'app')
        transaction = root / UPDATE
        require(not os.path.lexists(transaction), 'Another update transaction exists')
        preparation.rename(transaction)
        sync_directory(root)
        try:
            exchange_paths(root / 'app', transaction / 'new-app')
            sync_directory(root)
            sync_directory(transaction)
            # Only the backup's name changes here; app/AppRun stays present.
            # Recovery recognizes the old inode in either transaction slot.
            (transaction / 'new-app').rename(transaction / 'old-app')
            sync_directory(transaction)
            for index, name in enumerate(EXTERNAL):
                require(unchanged(data_home, name, state['external'][name]['installed']),
                        f'Desktop/icon changed during update: {data_home / name}')
                target = entry(data_home, name)
                os.replace(transaction / f'new-external-{index}', target)
                sync_directory(target.parent)
            verify_installation(root, data_home, next_state)
            save(root, next_state)
        except Exception:
            # The journal distinguishes a committed state from a failed swap;
            # if rollback is itself interrupted, the next invocation resumes it.
            recover_update(root, data_home)
            raise
        recover_update(root, data_home)
    return {'installed': True, 'updated': True, 'location': str(root / 'app'),
            'uninstall': str(root / 'app/uninstall.sh'),
            'profiles': 'kept separately; never copied or removed', 'warnings': caches(data_home)}


def install(source, data_home, *, lock_held=False):
    manifest, fingerprint = validate_package(source)
    root = data_home / DIRECTORY
    if os.path.lexists(root):
        require(stat.S_ISDIR(root.lstat().st_mode), 'Installation directory is not a regular directory')
        require((root / RECORD).is_file() and not (root / RECORD).is_symlink(),
                f'Unmanaged directory already exists at {root}; nothing was replaced')
        state_for(root, data_home)
        with (nullcontext() if lock_held else lock(root)):
            recover_update(root, data_home)
            if source == root / 'app':
                manifest, fingerprint = validate_package(source)
            state = state_for(root, data_home)
            if state.get('status') == 'uninstalled':
                require(set(state) == {'format', 'id', 'data_home', 'root_identity', 'status',
                                       'manifest_sha256', 'app_files', 'external', 'directories'}
                        and type(state['format']) is int
                        and not state['app_files'] and not state['external']
                        and isinstance(state.get('manifest_sha256'), str)
                        and re.fullmatch('[0-9a-f]{64}', state['manifest_sha256'])
                        and state['directories'].get(DIRECTORY) == identity(root),
                        'Incomplete or changed uninstalled metadata; retained for review')
                require(not os.path.lexists(root / 'app'),
                        'Unexpected app subtree after uninstall; nothing was replaced')
                return fresh_install(source, data_home, manifest, fingerprint,
                                     reuse_root=True, lock_held=True)
            verify_installation(root, data_home, state)
            if state['manifest_sha256'] == fingerprint:
                return {'installed': True, 'already_installed': True, 'location': str(root / 'app')}
            return update(source, data_home, state, manifest, fingerprint)
    return fresh_install(source, data_home, manifest, fingerprint)


def fresh_install(source, data_home, manifest, fingerprint, *, reuse_root=False, lock_held=False):
    """Fresh payload, optionally under an already verified empty managed root."""
    root = data_home / DIRECTORY
    # Refuse unsafe existing desktop/icon paths before creating installation files.
    existing = {}
    for name in EXTERNAL:
        current = data_home
        for part in Path(name).parts:
            current /= part
            if os.path.lexists(current):
                mode = current.lstat().st_mode
                require(stat.S_ISREG(mode) if current == data_home / name else stat.S_ISDIR(mode),
                        f'Refusing to replace a link or special path: {current}')
        target = data_home / name
        if target.exists():
            require(target.stat().st_uid == os.getuid(), f'Existing file belongs to another user: {target}')
            existing[name] = describe(target)
    data_home.mkdir(parents=True, exist_ok=True)
    if reuse_root:
        # Updater cache/high-water and unrelated siblings survive uninstall.
        # Reuse only the absent app namespace; do not overwrite backup files.
        require(not os.path.lexists(root / 'app'), 'Unexpected app subtree; nothing was replaced')
        backups = root / 'backups'
        require(not os.path.lexists(backups) or stat.S_ISDIR(backups.lstat().st_mode),
                'Changed backup directory; reinstall stopped')
        for index, name in enumerate((*EXTERNAL[1:], EXTERNAL[0])):
            if name in existing:
                require(not os.path.lexists(backups / str(index)),
                        'Existing backup retained; reinstall stopped')
    else:
        root.mkdir(mode=0o700)
    state = {'format': 1, 'id': ID, 'data_home': str(data_home), 'root_identity': identity(root),
             'status': 'installing', 'manifest_sha256': fingerprint, 'app_files': {},
             'external': {}, 'directories': {DIRECTORY: identity(root)}}
    with (nullcontext() if lock_held else lock(root)):
        save(root, state)
        for name in [*manifest['files'], MANIFEST]:
            target = root / 'app' / name
            ensure_parents(data_home, target, root, state)
            if 'symlink' in manifest['files'].get(name, {}):
                target.symlink_to(manifest['files'][name]['symlink'])
                result = describe(target)
            else:
                result = copy_file(entry(source, name), target)
            state['app_files'][name] = result
            save(root, state)
        validate_package(root / 'app')
        app = root / 'app'
        contents = {name: (app / 'usr/share' / name).read_bytes() for name in EXTERNAL[1:]}
        contents[EXTERNAL[0]] = desktop_contents(
            app / 'noventra-browser.desktop', app / 'AppRun',
            data_home / 'icons/hicolor/256x256/apps/noventra-browser.png')
        for index, (name, data) in enumerate(contents.items()):
            target = data_home / name
            ensure_parents(data_home, target, root, state)
            backup = None
            if name in existing:
                require(unchanged(data_home, name, existing[name]), f'Existing file changed: {target}')
                backup_path = f'backups/{index}'
                ensure_parents(data_home, root / backup_path, root, state)
                backup = {'path': backup_path, 'record': copy_file(target, root / backup_path)}
            else:
                require(not os.path.lexists(target), f'File appeared during installation: {target}')
            with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.noventra-', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
            try:
                temporary.chmod(0o644)
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
            state['external'][name] = {'installed': describe(target), 'backup': backup}
            save(root, state)
        state['status'] = 'installed'
        save(root, state)
    return {'installed': True, 'location': str(root / 'app'), 'uninstall': str(root / 'app/uninstall.sh'),
            'profiles': 'kept separately; never copied or removed', 'warnings': caches(data_home)}


def uninstall(data_home):
    root = data_home / DIRECTORY
    require(root.is_dir() and not root.is_symlink(), f'No managed local installation at {root}')
    require((root / RECORD).is_file() and not (root / RECORD).is_symlink(),
            f'No managed installation record at {root}; nothing was removed')
    state_for(root, data_home)
    retained = []
    with lock(root):
        recover_update(root, data_home)
        state = state_for(root, data_home)
        require_closed(root / 'app')
        for name, operation in list(state['external'].items()):
            backup = operation['backup']
            if not unchanged(data_home, name, operation['installed']) or (backup and not unchanged(root, backup['path'], backup['record'])):
                retained.append(str(data_home / name))
                continue
            target = entry(data_home, name)
            if backup:
                with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.noventra-', delete=False) as stream:
                    temporary = Path(stream.name)
                temporary.unlink()
                try:
                    copy_file(entry(root, backup['path']), temporary)
                    os.replace(temporary, target)
                finally:
                    temporary.unlink(missing_ok=True)
                entry(root, backup['path']).unlink()
            else:
                target.unlink()
            del state['external'][name]
            save(root, state)
        for name, expected in list(state['app_files'].items()):
            if unchanged(root / 'app', name, expected):
                entry(root / 'app', name).unlink()
                del state['app_files'][name]
                save(root, state)
            elif os.path.lexists(root / 'app' / name):
                retained.append(str(root / 'app' / name))
        for name, expected in sorted(state['directories'].items(), key=lambda x: len(Path(x[0]).parts), reverse=True):
            if name == DIRECTORY:
                continue
            try:
                path = entry(data_home, name)
                if identity(path) == expected and stat.S_ISDIR(path.lstat().st_mode):
                    path.rmdir()
                    del state['directories'][name]
            except (OSError, ValueError):
                pass  # Only empty, unchanged directories created by this installer may go.
        state['status'] = 'uninstalled'
        save(root, state)
        remaining = [path for path in root.iterdir() if path.name not in (RECORD, '.install-lock')]
        if not remaining and not state['external']:
            (root / RECORD).unlink()
            (root / '.install-lock').unlink()
            root.rmdir()
        else:
            retained.extend(str(path) for path in remaining)
    return {'uninstalled': True, 'profiles': 'preserved', 'retained': sorted(set(retained)),
            'warnings': caches(data_home)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('install', 'uninstall'))
    parser.add_argument('--data-home', type=Path,
                        default=Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share'))
    args = parser.parse_args()
    try:
        data_home = args.data_home.expanduser()
        require(data_home.is_absolute(), '--data-home/XDG_DATA_HOME must be an absolute path')
        data_home = data_home.resolve()
        require(os.geteuid() != 0, 'Run this per-user installer without sudo or root')
        result = (install(Path(__file__).resolve().parent, data_home)
                  if args.operation == 'install' else uninstall(data_home))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Noventra {args.operation} failed: {error}\n')
    if args.operation == 'install':
        print('Noventra is installed. Open it from your application menu.', file=sys.stderr)
    else:
        print('Noventra was removed. Your browser profiles were kept.', file=sys.stderr)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
