#!/usr/bin/env python3
"""Transactionally update a same-engine Mac workspace, retaining its native objects.

This only prepares source. Run build_macos.py build afterwards in the same workspace.
An interrupted update must be rolled back with --rollback-update HISTORY_DIRECTORY.
"""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import uuid

sys.dont_write_bytecode = True
import build_macos as build

ROOT = build.ROOT
LAYER_NAME = 'noventra/patches/chromium-156-macos'
KIND = 'noventra-macos-source-update-v1'
METADATA = {'workspace.json', 'build-receipt.json', 'failure.json',
            'evidence/source-pristine.json', 'evidence/source_verified.json',
            'evidence/keychain_identity_verified.json', 'evidence/sparkle_disabled.json'}
CATALOG = 'chrome/app/theme/chromium/mac/Assets.xcassets/'
ASSETS = 'chrome/app/theme/chromium/mac/Assets.car'
UNSET = object()


def real_path(path):
    path = Path(os.path.abspath(path))
    build.child(Path(path.anchor), path.relative_to(path.anchor).as_posix())
    return path


def describe(path):
    if not os.path.lexists(path):
        return None
    info = path.lstat()
    build.require(stat.S_ISREG(info.st_mode), f'Expected regular file: {path}')
    mode = stat.S_IMODE(info.st_mode)
    build.require(mode & ~0o777 == 0, f'Unsafe special file mode: {path}')
    return {'sha256': build.sha(path), 'mode': mode,
            'mtime_ns': info.st_mtime_ns, 'atime_ns': info.st_atime_ns}


def matches(actual, expected):
    return actual is None and expected is None or (
        actual is not None and expected is not None
        and all(actual[key] == expected[key] for key in ['sha256', 'mode']))


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def durable_mkdir(path):
    missing = []
    current = path
    while not current.exists():
        build.require(not current.is_symlink(), f'Linked update directory: {current}')
        missing.append(current)
        current = current.parent
    build.require(current.is_dir() and not current.is_symlink(), f'Expected real directory: {current}')
    for directory in reversed(missing):
        directory.mkdir()
        sync_directory(directory)
        sync_directory(directory.parent)


def durable_json(path, value):
    temporary = build.child(path.parent, path.name + '.new')
    if os.path.lexists(temporary):
        describe(temporary)
        temporary.unlink()  # A killed journal write never replaces the authoritative journal.
    with temporary.open('x') as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)
    sync_directory(path.parent)


def copy_durable(source, target):
    durable_mkdir(target.parent)
    shutil.copy2(source, target)
    with target.open('rb') as stream:
        os.fsync(stream.fileno())
    sync_directory(target.parent)


def encode(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def replay(stage, old_layer, old, new_layer, new, git_dir):
    """Prove the actual old postimages lead to the exact new postimages."""
    subprocess.run(['git', 'init', '--bare', '--quiet', str(git_dir)], check=True)
    command = ['git', '--git-dir=' + str(git_dir), '--work-tree=' + str(stage), 'apply']
    for layer, manifest, reverse in [(old_layer, old, True), (new_layer, new, False)]:
        if not reverse:
            build.verify_files(stage, new['pristine_files'])
        for patch in reversed(manifest['patches']) if reverse else manifest['patches']:
            args = (['--reverse'] if reverse else []) + [str(build.child(layer, patch['file']))]
            subprocess.run(command + ['--check'] + args, cwd=stage, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run(command + args, cwd=stage, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        build.verify_files(stage, manifest['pristine_files' if reverse else 'final_files'])


def validate_old(workspace, manifest):
    build.require(workspace.state.get('phase') in ['prepared', 'built', 'complete']
                  and workspace.state.get('in_progress') in [None, 'configure', 'compile', 'finalize'],
                  'Only successfully prepared source can be updated; recover partial preparation first')
    build.validate_prepared(workspace, manifest)
    reports = {}
    for key, name in [('source_report', 'source_verified'),
                      ('identity_report', 'keychain_identity_verified'),
                      ('assets_report', 'assets_car_actool')]:
        expected_path = 'evidence/' + name + '.json'
        build.require(workspace.state[key]['path'] == expected_path, 'Unexpected saved gate path')
        reports[key] = build.json_object(build.child(workspace.root, expected_path))
    pristine_path = build.child(workspace.root, 'evidence/source-pristine.json')
    pristine = build.json_object(pristine_path)
    source = reports['source_report']
    build.require(source.get('manifest_sha256') == workspace.bindings['manifest_sha256']
                  and source.get('files') == manifest['final_files']
                  and source.get('pristine_report_sha256') == build.sha(pristine_path)
                  and source.get('platform_pins') == build.verify_platform(workspace.platform)
                  and source.get('chromium') == build.verify_chromium(workspace.source)
                  and source.get('native_host') == 'Darwin/arm64', 'Saved source gate does not bind old source')
    build.require(pristine.get('status') == 'passed'
                  and pristine.get('manifest_sha256') == workspace.bindings['manifest_sha256']
                  and pristine.get('files') == manifest['pristine_files'], 'Saved pristine gate does not bind old layer')
    build.require(reports['identity_report'].get('source_files') == build.verify_identity(workspace.source),
                  'Saved identity gate does not bind old source')
    build.require(reports['assets_report'].get('assets_car_sha256') == workspace.state['assets_car_sha256'],
                  'Saved asset gate does not bind compiled catalog')


def target_path(workspace, entry):
    name = entry['path']
    build.relative(name)
    if entry['target'] == 'source':
        build.require(name.split('/')[0] != 'out', 'Update cannot touch compiled output')
        return build.child(workspace.source, name)
    build.require(entry['target'] == 'workspace' and name in METADATA, 'Unsafe update metadata path')
    return build.child(workspace.root, name)


def record_file(journal_dir, journal, workspace, target, name, after, *, expected_before=UNSET):
    entry = {'target': target, 'path': name, 'after': after}
    path = target_path(workspace, entry)
    entry['before'] = describe(path)
    if expected_before is not UNSET:
        build.require(matches(entry['before'], expected_before), f'Source changed during update validation: {name}')
        entry['before'] = expected_before
    if entry['before'] is not None:
        backup = build.child(journal_dir, 'before/' + target + '/' + name)
        copy_durable(path, backup)
        build.require(matches(describe(backup), entry['before']), f'Backup changed: {name}')
    journal['files'].append(entry)
    return entry


def replace_file(workspace, journal_dir, entry, version):
    path = target_path(workspace, entry)
    image = entry[version]
    if image is None:
        if os.path.lexists(path):
            path.unlink()
            sync_directory(path.parent)
        return
    origin = build.child(journal_dir, version + '/' + entry['target'] + '/' + entry['path'])
    build.require(matches(describe(origin), image), f'Update image changed: {origin}')
    durable_mkdir(path.parent)
    pending = build.child(journal_dir, 'pending')
    if os.path.lexists(pending):
        build.require(stat.S_ISREG(pending.lstat().st_mode), 'Unsafe pending transaction image')
        pending.unlink()
    copy_durable(origin, pending)
    if version == 'before':
        os.utime(pending, ns=(image['atime_ns'], image['mtime_ns']))
    else:
        # Replayed files inherit staging timestamps; force a fresh timestamp so Ninja sees changes.
        os.utime(pending, None)
    with pending.open('rb') as stream:
        os.fsync(stream.fileno())
    pending.replace(path)
    sync_directory(path.parent)


def save_journal(directory, journal):
    durable_json(directory / 'journal.json', journal)


def clean_staging(directory):
    try:
        for name in ['stage', 'replay.git', 'after']:
            path = build.child(directory, name)
            if path.exists():
                shutil.rmtree(path)
        for name in ['marker.json', 'pending']:
            path = build.child(directory, name)
            if path.exists():
                path.unlink()
    except (OSError, ValueError) as error:
        print(f'Temporary update images retained in {directory}: {error}', file=sys.stderr)


def rollback(workspace, directory):
    directory = real_path(directory)
    history = build.child(workspace.root, 'source-updates')
    build.require(directory.parent == history and len(directory.name) == 32
                  and all(c in '0123456789abcdef' for c in directory.name),
                  'Rollback requires an owned source-updates/<transaction-id> directory')
    journal = build.json_object(build.child(directory, 'journal.json'))
    build.require(journal.get('kind') == KIND and journal.get('workspace') == str(workspace.root)
                  and journal.get('id') == directory.name
                  and journal.get('status') in ['staging', 'applying', 'committed', 'rolled-back'],
                  'Wrong or invalid source update journal')
    if journal['status'] == 'rolled-back':
        clean_staging(directory)
        return
    entries = journal.get('files')
    build.require(isinstance(entries, list), 'Missing update file records')
    if journal['status'] == 'committed':
        state_entry = next((e for e in entries if e['target'] == 'workspace' and e['path'] == 'workspace.json'), None)
        build.require(state_entry is not None, 'Committed journal has no prepared workspace binding')
        actual = describe(target_path(workspace, state_entry))
        if matches(actual, state_entry['after']):
            clean_staging(directory)
            return 'committed'
        build.require(any(matches(actual, image) for image in state_entry.get('intermediate', [])),
                      'Completed update cannot be rolled back by recovery')
        # Commit is durable before the final binding becomes usable by ordinary build.
        # A crash in that gap finishes publication, never rolls back committed source.
        for entry in entries:
            if entry is not state_entry:
                path = target_path(workspace, entry)
                build.require(matches(describe(path), entry['after']),
                              f'Recovery conflict; preserve unexpected edit: {path}')
        replace_file(workspace, directory, state_entry, 'after')
        clean_staging(directory)
        return 'committed'
    seen = set()
    for entry in entries:
        path = target_path(workspace, entry)
        build.require(str(path) not in seen, 'Duplicate recovery target')
        seen.add(str(path))
        actual = describe(path)
        candidates = [entry['before'], entry['after'], *entry.get('intermediate', [])]
        build.require(any(matches(actual, image) for image in candidates),
                      f'Recovery conflict; preserve unexpected edit: {path}')
        for image in candidates:
            build.require(image is None or (isinstance(image.get('mode'), int)
                          and 0 <= image['mode'] <= 0o777), 'Unsafe recovery file mode')
        if entry['before'] is not None:
            backup = build.child(directory, 'before/' + entry['target'] + '/' + entry['path'])
            build.require(matches(describe(backup), entry['before']), f'Recovery backup changed: {path}')
    for entry in reversed(entries):
        path = target_path(workspace, entry)
        if not matches(describe(path), entry['before']):
            replace_file(workspace, directory, entry, 'before')
    for name in sorted(journal.get('created_directories', []), key=lambda item: item.count('/'), reverse=True):
        path = build.child(workspace.source, name)
        if path.exists():
            build.require(path.is_dir(), f'Expected update directory: {path}')
            try:
                path.rmdir()
            except OSError:
                pass  # Keep any files created by another process after the interruption.
    journal['status'] = 'rolled-back'
    save_journal(directory, journal)
    clean_staging(directory)
    return 'rolled-back'


def report_bytes(gate, **details):
    return encode({'format': 1, 'gate': gate, 'status': 'passed', **details})


def prepare_metadata(workspace, new, identity, project, directory, journal, platform_pins):
    state = copy.deepcopy(workspace.state)
    state.update(manifest_sha256=identity,
                 tool_sha256=build.sha(build.child(project, 'tools/noventra/build_macos.py')),
                 initial_preferences_sha256=build.sha(build.child(project, 'noventra/resources/initial_preferences.json')),
                 phase='prepared', in_progress=None,
                 source_update={'id': journal['id'], 'history': str(directory.relative_to(workspace.root))})
    for key in ['sparkle_report', 'build_jobs']:
        state.pop(key, None)
    files = {'evidence/source-pristine.json': encode({
        'format': 1, 'status': 'passed', 'manifest_sha256': identity, 'files': new['pristine_files']})}
    pristine_sha = hashlib.sha256(files['evidence/source-pristine.json']).hexdigest()
    files['evidence/source_verified.json'] = report_bytes('source_verified', manifest_sha256=identity,
        files=build.verify_files(workspace.source, new['final_files']), pristine_report_sha256=pristine_sha,
        platform_pins=platform_pins, chromium=build.verify_chromium(workspace.source), native_host='Darwin/arm64',
        source_update={'id': journal['id'], 'previous_manifest_sha256': workspace.bindings['manifest_sha256']})
    files['evidence/keychain_identity_verified.json'] = report_bytes('keychain_identity_verified',
        bundle_id=build.BUNDLE_ID, profile_directory=build.BUNDLE_ID, service='Noventra Storage Key',
        account='Noventra', source_files=build.verify_identity(workspace.source))
    for key, name in [('source_report', 'source_verified'), ('identity_report', 'keychain_identity_verified')]:
        name = 'evidence/' + name + '.json'
        state[key] = {'path': name, 'sha256': hashlib.sha256(files[name]).hexdigest()}
    files['workspace.json'] = encode(state)
    for name in ['build-receipt.json', 'failure.json', 'evidence/sparkle_disabled.json']:
        files[name] = None
    for name, data in files.items():
        image = build.child(directory, 'after/workspace/' + name)
        if data is not None:
            durable_mkdir(image.parent)
            image.write_bytes(data)
            os.chmod(image, 0o644)
            with image.open('rb') as stream:
                os.fsync(stream.fileno())
            sync_directory(image.parent)
        existing = next((e for e in journal['files'] if e['target'] == 'workspace' and e['path'] == name), None)
        if existing:
            existing['after'] = describe(image) if data is not None else None
        else:
            record_file(directory, journal, workspace, 'workspace', name,
                        describe(image) if data is not None else None)
    # Workspace ownership is the last replacement, after all new gate evidence exists.
    journal['files'].sort(key=lambda e: e['target'] == 'workspace' and e['path'] == 'workspace.json')
    return state


def update(path, previous_kit, platform_root=None, *, project=ROOT):
    project, previous_kit = real_path(project), real_path(previous_kit)
    old_layer, new_layer = build.child(previous_kit, LAYER_NAME), build.child(project, LAYER_NAME)
    old, old_identity = build.load_layer(old_layer)
    new, new_identity = build.load_layer(new_layer)
    build.require(old['final_files'].keys() <= new['final_files'].keys(),
                  'Source scope shrank; this updater requires a reviewed fresh preparation')
    root = real_path(path)
    saved = build.json_object(build.child(root, 'workspace.json'))
    selected_platform = real_path(platform_root or saved.get('platform_root', ''))
    workspace = build.Workspace(root, selected_platform, old_identity, project=project,
        tool_path=build.child(previous_kit, 'tools/noventra/build_macos.py'),
        preferences_path=build.child(previous_kit, 'noventra/resources/initial_preferences.json'))
    describe(build.child(root, '.build.lock'))
    with build.exclusive(workspace):
        # Reconstruct under the lock: an earlier builder may have finished before we acquired it.
        workspace = build.Workspace(root, selected_platform, old_identity, project=project,
            tool_path=build.child(previous_kit, 'tools/noventra/build_macos.py'),
            preferences_path=build.child(previous_kit, 'noventra/resources/initial_preferences.json'))
        history = build.child(root, 'source-updates')
        if history.exists():
            build.require(history.is_dir(), 'Source update history is not a directory')
            for item in history.iterdir():
                build.require(not item.is_symlink() and item.is_dir(), 'Unsafe source update history entry')
                receipt = build.json_object(build.child(item, 'journal.json'))
                build.require(receipt.get('status') in ['committed', 'rolled-back'],
                              f'Interrupted source update; run --rollback-update {item}')
        validate_old(workspace, old)
        build.require(workspace.source.stat().st_dev == root.stat().st_dev,
                      'Source and update history must be on the same filesystem for atomic replacement')
        names = sorted(new['final_files'])
        original = {}
        for name in names:
            build.require(name.split('/')[0] != 'out', 'Source layer includes compiled output')
            path = build.child(workspace.source, name)
            original[name] = describe(path)
        token = uuid.uuid4().hex
        directory = build.child(root, 'source-updates/' + token)
        durable_mkdir(directory)
        journal = {'kind': KIND, 'id': token, 'workspace': str(root), 'status': 'staging',
                   'previous_manifest_sha256': old_identity, 'manifest_sha256': new_identity,
                   'updater_sha256': build.sha(Path(__file__)), 'files': [], 'created_directories': []}
        save_journal(directory, journal)
        try:
            stage = directory / 'stage'
            stage.mkdir()
            for name in names:
                source = build.child(workspace.source, name)
                if os.path.lexists(source):
                    destination = build.child(stage, name)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
                    build.require(matches(describe(destination), original[name]),
                                  f'Source changed while staging: {name}')
            build.verify_files(stage, old['final_files'])
            replay(stage, old_layer, old, new_layer, new, directory / 'replay.git')
            for name in names:
                before = original[name]
                build.require(matches(describe(build.child(workspace.source, name)), before),
                              f'Source changed during update validation: {name}')
                after = describe(build.child(stage, name))
                if matches(before, after):
                    continue
                build.require(not name.startswith(CATALOG) and name != ASSETS,
                              'Icon catalog changed; native actool regeneration with backed-up asset evidence is required')
                destination = build.child(directory, 'after/source/' + name)
                copy_durable(build.child(stage, name), destination)
                record_file(directory, journal, workspace, 'source', name, describe(destination),
                            expected_before=before)
                parent = Path(name).parent
                while parent != Path('.'):
                    real_parent = build.child(workspace.source, parent.as_posix())
                    if real_parent.exists():
                        build.require(real_parent.is_dir() and real_parent.stat().st_dev == root.stat().st_dev,
                                      'Source parent must be a directory on the workspace filesystem')
                        break
                    if parent.as_posix() not in journal['created_directories']:
                        journal['created_directories'].append(parent.as_posix())
                    parent = parent.parent
            state_entry = record_file(directory, journal, workspace, 'workspace', 'workspace.json', None)
            marker = {**workspace.state, 'in_progress': 'source-update',
                      'source_update': {'id': token, 'history': str(directory.relative_to(root))}}
            marker_path = directory / 'marker.json'
            marker_path.write_bytes(encode(marker))
            os.chmod(marker_path, 0o644)
            state_entry['intermediate'] = [describe(marker_path)]
            journal['status'] = 'applying'
            save_journal(directory, journal)
            build.require(build.load_layer(old_layer)[1] == old_identity
                          and build.load_layer(new_layer)[1] == new_identity, 'Source kits changed during validation')
            validate_old(workspace, old)
            build.verify_files(workspace.source, old['final_files'])
            for name, before in original.items():
                build.require(matches(describe(build.child(workspace.source, name)), before),
                              f'Source changed during update validation: {name}')
            # The marker prevents ordinary build/resume after an interrupted source mutation.
            copy_durable(marker_path, directory / 'pending')
            (directory / 'pending').replace(workspace.state_path)
            sync_directory(root)
            for entry in journal['files']:
                if entry['target'] == 'source':
                    build.require(matches(describe(target_path(workspace, entry)), entry['before']),
                                  'Source changed during update validation')
                    replace_file(workspace, directory, entry, 'after')
            build.require(build.sha(build.child(workspace.source, ASSETS)) == workspace.state['assets_car_sha256'],
                          'Generated asset catalog changed during update')
            platform_pins = build.verify_platform(workspace.platform)
            state = prepare_metadata(workspace, new, new_identity, project, directory, journal, platform_pins)
            save_journal(directory, journal)
            for entry in journal['files']:
                if entry['target'] == 'workspace' and entry['path'] != 'workspace.json':
                    replace_file(workspace, directory, entry, 'after')
            workspace.state, workspace.bindings = state, {key: state[key] for key in workspace.bindings}
            build.validate_prepared(workspace, new)
            journal['status'] = 'committed'
            save_journal(directory, journal)
            replace_file(workspace, directory, state_entry, 'after')
            clean_staging(directory)
            return directory
        except BaseException:
            # SIGKILL/power loss leave this same journal for explicit safe recovery.
            saved_journal = build.json_object(directory / 'journal.json')
            if saved_journal['status'] == 'committed':
                print(f'Source update committed; finish publication with --workspace {root} '
                      f'--rollback-update {directory}', file=sys.stderr)
            else:
                rollback(workspace, directory)
            raise


def recover(path, directory):
    root = real_path(path)
    saved = build.json_object(build.child(root, 'workspace.json'))
    build.require(saved.get('kind') == build.WORKSPACE_KIND and saved.get('workspace') == str(root)
                  and saved.get('engine') == build.ENGINE, 'Unowned or wrong-engine recovery workspace')
    from types import SimpleNamespace
    workspace = SimpleNamespace(root=root, source=build.child(root, 'src'))
    build.child(root, 'src/out/Default')
    describe(build.child(root, '.build.lock'))
    with build.exclusive(workspace):
        return rollback(workspace, directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--previous-kit', type=Path, help='Exact old source kit; old code is hashed, never executed')
    parser.add_argument('--platform-root', type=Path, help='Same pinned platform root; defaults to the saved path')
    parser.add_argument('--rollback-update', type=Path,
                        help='Roll back an interrupted update, or finish publishing a durably committed update')
    args = parser.parse_args()
    try:
        if args.rollback_update:
            build.require(args.previous_kit is None and args.platform_root is None,
                          'Rollback takes only --workspace and --rollback-update')
            outcome = recover(args.workspace, args.rollback_update)
            print(f'Source update recovery: {outcome or "rolled-back"}; compiled output was retained.')
        else:
            build.require(args.previous_kit is not None, 'Supply the exact --previous-kit')
            build.require(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                          'Native Apple-silicon macOS is required')
            directory = update(args.workspace, args.previous_kit, args.platform_root)
            print(f'Source prepared; native objects retained. No build performed.\nUpdate history: {directory}')
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors='replace'), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
