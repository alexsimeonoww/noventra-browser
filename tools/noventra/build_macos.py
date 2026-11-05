#!/usr/bin/env python3
"""Prepare and build a pinned, isolated Apple-silicon Noventra test application.

This tool never launches, installs, signs, notarizes, or publishes the browser.
The current Mac source layer is a candidate until native preimage checks pass.
"""

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
LAYER = ROOT / 'noventra/patches/chromium-156-macos'
PREFERENCES = ROOT / 'noventra/resources/initial_preferences.json'
ENGINE = {
    'chromium': 'a21ce95c24ce3964416f09ae1862f53f319eb4d0',
    'version': '156.0.8078.79',
    'helium': '2f81b9cafa38a95071e20de3f9cd6b0aca6d3c65',
    'platform': '0c7da2ba0b61a2143adf1c6d56ccd0df6a53d0bf',
    'target_os': 'mac', 'target_cpu': 'arm64',
}
WORKSPACE_KIND = 'noventra-macos-workspace'
BUNDLE_ID = 'local.noventra.browser'
APP_NAME = 'Noventra.app'
GN_REQUIRED = {
    'target_os': '"mac"', 'target_cpu': '"arm64"', 'is_component_build': 'false',
    'enable_sparkle': 'false', 'enable_updater': 'false',
    'include_branded_entitlements': 'false', 'use_system_xcode': 'true',
    'use_remoteexec': 'false',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def relative(name):
    require(isinstance(name, str) and name and not name.startswith('/')
            and '\\' not in name and ':' not in name
            and all(p not in ('', '.', '..', '.git') for p in name.split('/'))
            and not any(ord(c) < 32 or ord(c) == 127 for c in name),
            f'Unsafe relative path: {name!r}')
    return Path(name)


def child(root, name):
    path = root
    for part in relative(name).parts:
        path = path / part
        require(not path.is_symlink(), f'Symlink is not a source input: {path}')
    return path


def sha(path):
    require(stat.S_ISREG(path.lstat().st_mode), f'Expected regular file: {path}')
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def json_object(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f'Duplicate JSON key: {key}')
            result[key] = value
        return result
    def invalid(value):
        raise ValueError(f'Invalid JSON constant: {value}')
    result = json.loads(path.read_text(), object_pairs_hook=unique, parse_constant=invalid)
    require(isinstance(result, dict), f'Expected JSON object: {path}')
    return result


def write_json(path, value):
    require(not path.is_symlink(), f'Refusing linked output: {path}')
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.new')
    require(not temporary.is_symlink(), f'Refusing linked temporary output: {temporary}')
    with temporary.open('w') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')
    temporary.replace(path)


def load_layer(directory=LAYER):
    manifest_path = child(directory, 'manifest.json')
    manifest = json_object(manifest_path)
    require(manifest.get('schema') == 'noventra-macos-source-port-v1'
            and manifest.get('engine') == ENGINE, 'Wrong Mac source manifest or engine pins')
    before, after = manifest.get('pristine_files'), manifest.get('final_files')
    require(isinstance(before, dict) and isinstance(after, dict) and before
            and before.keys() == after.keys(), 'Inconsistent source file declarations')
    for table, allow_absence in [(before, True), (after, False)]:
        for name, expected in table.items():
            relative(name)
            require((expected is None and allow_absence)
                    or isinstance(expected, str) and re.fullmatch('[0-9a-f]{64}', expected),
                    f'Invalid source digest: {name}')
    patches = manifest.get('patches')
    require(isinstance(patches, list) and patches, 'No source patches declared')
    names = []
    declared_paths = set()
    for item in patches:
        name = item.get('file')
        relative(name)
        require('/' not in name and name not in names, 'Unsafe or duplicate patch filename')
        path = child(directory, name)
        require(sha(path) == item.get('sha256'), f'Patch hash mismatch: {name}')
        text = path.read_text()
        headers = re.findall(r'(?m)^diff --git a/(\S+) b/(\S+)$', text)
        require(headers, 'Patch has no standard Git headers')
        for old, new in headers:
            relative(old)
            relative(new)
            require(old == new and new in before, f'Undeclared or renamed patch path: {new}')
            declared_paths.add(new)
        require(not re.search(r'(?m)^(?:new|old) mode (?:120000|160000)|^new file mode (?:120000|160000)', text),
                'Source patch cannot create symlinks or submodules')
        names.append(name)
    require(declared_paths == before.keys(), 'Source manifest and patch scopes differ')
    series = child(directory, 'series')
    require(sha(series) == manifest.get('series_sha256'), 'Patch series hash mismatch')
    require(series.read_text().splitlines() == names, 'Patch series order mismatch')
    return manifest, sha(manifest_path)


def verify_files(source, files):
    """Check every declared input, including absent new files, without following links."""
    actual = {}
    mismatches = []
    for name, expected in files.items():
        try:
            path = child(source, name)
            value = None if not os.path.lexists(path) else (
                sha(path) if stat.S_ISREG(path.lstat().st_mode) else {'error': 'not a regular file'})
        except (OSError, ValueError) as error:
            value = {'error': str(error)}
        actual[name] = value
        if value != expected:
            mismatches.append({'path': name, 'expected': expected, 'actual': value})
    if mismatches:
        raise SourceMismatch(mismatches)
    return actual


class SourceMismatch(ValueError):
    def __init__(self, mismatches):
        self.mismatches = mismatches
        first = mismatches[0]
        super().__init__(f'{len(mismatches)} source preimage/postimage mismatch(es); '
                         f'{first["path"]}: expected {first["expected"]}, actual {first["actual"]}')


def literal_assignments(text, keys=None):
    result = {}
    for raw in text.splitlines():
        line = raw.split('#', 1)[0].strip()
        match = re.match(r'([a-zA-Z_][a-zA-Z_0-9]*)\s*=', line)
        if not match or keys is not None and match[1] not in keys:
            continue
        key = match[1]
        require(key not in result, f'Ambiguous GN assignment: {key}')
        full = re.fullmatch(r'[a-zA-Z_][a-zA-Z_0-9]*\s*=\s*("[^"\n]*"|true|false|-?\d+)', line)
        require(full is not None, f'Expected literal GN assignment: {line}')
        result[key] = full[1]
    return result


def render_gn_args(core, mac):
    values = literal_assignments(core)
    values.update(literal_assignments(mac))
    values.update(GN_REQUIRED)
    # This local test uses the pinned core's non-PGO configuration. It avoids
    # making a release-profile download an undocumented prerequisite.
    values['chrome_pgo_phase'] = '0'
    values['is_debug'] = 'false'
    values['cc_wrapper'] = '""'
    return '\n'.join(f'{key} = {value}' for key, value in sorted(values.items())) + '\n'


def verify_gn_gates(text):
    actual = literal_assignments(text, set(GN_REQUIRED))
    require(actual == GN_REQUIRED, f'Unsafe native GN configuration: {actual}')
    return actual


def git(repo, *args):
    return subprocess.check_output(['git', '--no-optional-locks', '-C', str(repo), *args], text=True).strip()


def verify_platform(root):
    for repo, pin in [(root, ENGINE['platform']), (root / 'helium-chromium', ENGINE['helium'])]:
        require(repo.is_dir() and not repo.is_symlink(), f'Expected real platform checkout: {repo}')
        require(Path(git(repo, 'rev-parse', '--show-toplevel')).resolve() == repo.resolve(),
                f'Expected independent checkout: {repo}')
        require(git(repo, 'rev-parse', 'HEAD') == pin, f'Wrong pinned checkout: {repo}')
        require(not git(repo, 'status', '--porcelain', '--untracked-files=normal'),
                f'Platform/core has unreviewed changes: {repo}')
    require((root / 'helium-chromium/chromium_version.txt').read_text().strip() == ENGINE['version'],
            'Core Chromium version does not match the source layer')
    return {'platform': ENGINE['platform'], 'helium': ENGINE['helium'], 'tracked_clean': True}


class Workspace:
    def __init__(self, path, platform_root, identity, *, create=False, project=ROOT,
                 tool_path=None, preferences_path=None):
        requested = Path(path).absolute()
        require(not requested.is_symlink(), 'Workspace cannot be a symlink')
        parent = requested.parent.resolve(strict=True)
        self.root = parent / requested.name
        project = project.resolve(strict=True)
        require(not self.root.is_relative_to(project) and not project.is_relative_to(self.root),
                'Use a separate workspace outside the Noventra checkout')
        self.state_path = self.root / 'workspace.json'
        self.platform = Path(platform_root).absolute()
        require(not self.platform.is_symlink(), 'Platform root cannot be a symlink')
        # A fresh compiler checkout may be inside the not-yet-created workspace.
        self.platform = self.platform.resolve(strict=False)
        self.source = child(self.root, 'src')
        self.output = child(self.root, 'src/out/Default')
        self.bindings = {'format': 1, 'kind': WORKSPACE_KIND, 'workspace': str(self.root),
                         'platform_root': str(self.platform), 'engine': ENGINE,
                         'manifest_sha256': identity, 'tool_sha256': sha(tool_path or Path(__file__)),
                         'initial_preferences_sha256': sha(preferences_path or PREFERENCES)}
        if self.root.exists():
            require(self.state_path.is_file() and not self.state_path.is_symlink(),
                    'Existing workspace has no Noventra ownership receipt; refusing to adopt it')
            self.state = json_object(self.state_path)
            require(all(self.state.get(k) == v for k, v in self.bindings.items()),
                    'Workspace pins, inputs, tool, or paths changed; use a new workspace')
        else:
            require(create, 'Workspace does not exist; run prepare first')
            self.root.mkdir()
            self.state = {**self.bindings, 'phase': 'created', 'in_progress': None}
            self.save()
        for name in ['logs', 'evidence']:
            require(not (self.root / name).is_symlink(), f'Linked workspace directory: {name}')
            (self.root / name).mkdir(exist_ok=True)
        require(not self.source.is_symlink() and not self.output.is_symlink(), 'Linked compiler workspace')

    def save(self):
        write_json(self.state_path, self.state)

    def begin(self, step):
        self.state['in_progress'] = step
        self.save()

    def complete(self, phase):
        self.state.update(phase=phase, in_progress=None)
        self.save()

    def command(self, label, command, *, cwd=None, env=None):
        log = child(self.root, 'logs/' + label + '.log')
        print(f'{label}: {log}', flush=True)
        with log.open('ab') as output:
            output.write(('\n$ ' + ' '.join(map(str, command)) + '\n').encode())
            output.flush()
            result = subprocess.run(list(map(str, command)), cwd=cwd or self.root,
                                    env=env, stdout=output, stderr=subprocess.STDOUT)
        require(result.returncode == 0, f'{label} failed ({result.returncode}); inspect {log}')
        return {'path': str(log.relative_to(self.root)), 'sha256': sha(log)}

    def capture(self, label, command):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = child(self.root, 'logs/' + label + '.log')
        log.write_text(result.stdout)
        require(result.returncode == 0, f'{label} failed; inspect {log}')
        return result.stdout.strip()

    def report(self, gate, **details):
        path = self.root / 'evidence' / (gate + '.json')
        write_json(path, {'format': 1, 'gate': gate, 'status': 'passed', **details})
        return {'path': str(path.relative_to(self.root)), 'sha256': sha(path)}


@contextlib.contextmanager
def exclusive(workspace):
    path = child(workspace.root, '.build.lock')
    with path.open('a') as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('Another process is using this Mac workspace') from error
        yield


def prerequisites(workspace, min_free_gib):
    require(platform.system() == 'Darwin' and platform.machine() == 'arm64',
            'Native Apple-silicon macOS is required; Linux and cross builds are disabled')
    require(sys.version_info >= (3, 13), 'Use Python 3.13 or newer, as required by the pinned platform')
    for tool in ['git', 'bash', 'greadlink', 'xcrun', 'xcodebuild', 'zstd', 'wget', 'patch']:
        require(shutil.which(tool), f'Missing build prerequisite: {tool}')
    free = shutil.disk_usage(workspace.root).free
    require(free >= min_free_gib * 1024**3,
            f'Need at least {min_free_gib} GiB free in the compiler workspace; have {free / 1024**3:.1f}')
    sdk = workspace.capture('sdk-version', ['xcrun', '--sdk', 'macosx', '--show-sdk-version'])
    require(re.fullmatch(r'\d+(?:\.\d+)*', sdk), 'Unrecognized Mac SDK version')
    require(tuple(map(int, sdk.split('.'))) >= (26, 5), 'Pinned Chromium requires Mac SDK 26.5 or newer')
    outputs = {'sdk_version': sdk,
               'xcode': workspace.capture('xcode-version', ['xcodebuild', '-version'])}
    for tool in ['actool', 'clang', 'metal', 'lipo']:
        outputs[tool] = workspace.capture('find-' + tool, ['xcrun', '--find', tool])
    workspace.command('python-dependencies', [sys.executable, '-c', 'import requests; import PIL'])
    write_json(workspace.root / 'prerequisites.json', {
        'format': 1, 'host': {'os': platform.system(), 'machine': platform.machine()},
        'free_bytes': free, 'minimum_free_gib': min_free_gib, 'tools': outputs})
    return outputs


def native_env():
    env = os.environ.copy()
    env.update(PYTHONDONTWRITEBYTECODE='1', DEPOT_TOOLS_UPDATE='0',
               SISO_REAPI_ADDRESS='', SISO_REAPI_INSTANCE='',
               PATH=str(Path(sys.executable).parent) + os.pathsep + env.get('PATH', ''))
    return env


def acquire_platform(workspace, clone):
    if workspace.platform.exists():
        return verify_platform(workspace.platform)
    require(clone and workspace.platform.is_relative_to(workspace.root),
            'Supply a pinned platform checkout, or --clone-platform with a path inside this workspace')
    workspace.command('clone-platform', ['git', 'clone', '--no-checkout',
        'https://github.com/imputnet/helium-macos.git', workspace.platform], env=native_env())
    workspace.command('checkout-platform', ['git', '-C', workspace.platform, 'checkout', '--detach', ENGINE['platform']])
    workspace.command('core-submodule', ['git', '-C', workspace.platform, 'submodule', 'update', '--init', '--recursive'],
                      env=native_env())
    return verify_platform(workspace.platform)


def prepare_upstream(workspace):
    require(not workspace.source.exists(), 'Source already exists; refusing destructive preparation/restart')
    core = workspace.platform / 'helium-chromium'
    resources = workspace.root / 'core-resources'
    require(not resources.exists(), 'Resource staging already exists; do not repeat partial preparation')
    shutil.copytree(core / 'resources', resources)
    script = r'''set -euo pipefail
_root_dir="$1"
source "$_root_dir/devutils/shared.sh"
_download_cache="$2/download_cache"
_src_dir="$2/src"
_out_dir="$_src_dir/out/Default"
fetch_sources true
'''
    workspace.command('fetch-sources', ['bash', '--noprofile', '--norc', '-c', script,
                       'noventra-fetch', workspace.platform, workspace.root], env=native_env())
    require(not (workspace.source / 'out').exists(),
            'Fetched source unexpectedly contains build output; refusing stale native artifacts')
    commands = [
        ['prune_binaries.py', '--keep-build-dependencies', workspace.source, core / 'pruning.list'],
        ['patches.py', 'apply', workspace.source, core / 'patches', workspace.platform / 'patches'],
        ['domain_substitution.py', 'apply', '-r', core / 'domain_regex.list', '-f', core / 'domain_substitution.list', workspace.source],
        ['name_substitution.py', '--sub', '-t', workspace.source],
        ['i18n_apply.py', '-t', workspace.source],
        ['helium_version.py', '--tree', core, '--platform-tree', workspace.platform, '--chromium-tree', workspace.source],
        ['generate_resources.py', core / 'resources/generate_resources.txt', resources],
        ['replace_resources.py', workspace.platform / 'resources/platform_resources.txt', workspace.platform / 'resources', workspace.source],
        ['replace_resources.py', core / 'resources/helium_resources.txt', resources, workspace.source],
    ]
    for number, (script_name, *arguments) in enumerate(commands):
        workspace.command(f'prepare-{number:02d}-{Path(script_name).stem}',
            [sys.executable, core / 'utils' / script_name, *arguments], env=native_env())
    verify_platform(workspace.platform)


def verify_identity(source):
    expected = {
        'chrome/app/theme/chromium/BRANDING': ['PRODUCT_FULLNAME=Noventra', 'PRODUCT_SHORTNAME=Noventra',
                                               'MAC_BUNDLE_ID=' + BUNDLE_ID],
        'chrome/common/chrome_paths_mac.mm': ['product_dir_name = "local.noventra.browser";'],
        'components/os_crypt/common/keychain_password_mac.mm': [
            'kDefaultServiceName[] = "Noventra Storage Key";', 'kDefaultAccountName[] = "Noventra";',
            'KeychainPassword::GetPasswordWithError() const'],
        'chrome/browser/browser_process_impl.cc': ['ShowKeychainStartupError()',
            'base::Process::TerminateCurrentProcessImmediately(EXIT_FAILURE);'],
        'chrome/browser/mac/initial_prefs.mm': ['base::apple::OuterBundlePath()', '.Append("initial_preferences")'],
    }
    records = {}
    for name, needles in expected.items():
        path = child(source, name)
        text = path.read_text()
        require(all(needle in text for needle in needles), f'Noventra Mac identity/protection missing: {name}')
        records[name] = sha(path)
    return records


def verify_chromium(source):
    version = child(source, 'chrome/VERSION')
    values = dict(line.split('=', 1) for line in version.read_text().splitlines() if '=' in line)
    actual = '.'.join(values.get(key, '') for key in ['MAJOR', 'MINOR', 'BUILD', 'PATCH'])
    require(actual == ENGINE['version'], 'Prepared Chromium version does not match the source layer')
    lastchange = child(source, 'build/util/LASTCHANGE')
    match = re.search(r'(?m)^LASTCHANGE=([0-9a-f]{40})(?:-|$)', lastchange.read_text())
    require(match is not None and match[1] == ENGINE['chromium'], 'Prepared Chromium LASTCHANGE pin mismatch')
    return {'version': actual, 'commit': match[1], 'version_file_sha256': sha(version),
            'lastchange_file_sha256': sha(lastchange)}


def compile_catalog(workspace):
    catalog = child(workspace.source, 'chrome/app/theme/chromium/mac/Assets.xcassets')
    require(catalog.is_dir(), 'Missing Noventra AppIcon source catalog')
    output = workspace.root / 'actool-output'
    require(not output.exists(), 'Native asset output already exists; do not repeat partial preparation')
    output.mkdir()
    command = ['xcrun', 'actool', '--compile', str(output), '--platform', 'macosx',
        '--minimum-deployment-target', '13.0', '--app-icon', 'AppIcon',
        '--output-partial-info-plist', str(output / 'partial-info.plist'), str(catalog)]
    log = workspace.command('actool', command)
    generated = child(output, 'Assets.car')
    require(generated.is_file() and generated.stat().st_size > 0, 'actool produced no Assets.car')
    target = child(workspace.source, 'chrome/app/theme/chromium/mac/Assets.car')
    shutil.copyfile(generated, target)
    workspace.state['assets_car_sha256'] = sha(target)
    workspace.state['assets_report'] = workspace.report('assets_car_actool', command=command, log=log,
        source_catalog=str(catalog.relative_to(workspace.root)), assets_car_sha256=sha(target))
    workspace.save()


def prepare(workspace, manifest, clone):
    if workspace.state['phase'] == 'prepared' and not workspace.state.get('in_progress'):
        validate_prepared(workspace, manifest)
        return
    require(workspace.state['phase'] == 'created' and not workspace.state.get('in_progress'),
            'Partial preparation cannot be restarted safely; preserve it and choose a new workspace')
    workspace.begin('platform-acquisition')
    acquire_platform(workspace, clone)
    workspace.begin('upstream-preparation')
    prepare_upstream(workspace)
    workspace.begin('noventra-source-application')
    chromium = verify_chromium(workspace.source)
    before = verify_files(workspace.source, manifest['pristine_files'])
    write_json(workspace.root / 'evidence/source-pristine.json', {
        'format': 1, 'status': 'passed', 'manifest_sha256': workspace.bindings['manifest_sha256'], 'files': before})
    # An independent empty Git directory avoids accidentally using a parent checkout's index.
    git_dir = workspace.root / 'patch-replay.git'
    workspace.command('init-patch-worktree', ['git', 'init', '--bare', git_dir])
    for number, patch in enumerate(manifest['patches']):
        command = ['git', '--git-dir=' + str(git_dir), '--work-tree=' + str(workspace.source), 'apply']
        path = child(LAYER, patch['file'])
        workspace.command(f'layer-{number}-check', command + ['--check', str(path)], cwd=workspace.source)
        workspace.command(f'layer-{number}-apply', command + [str(path)], cwd=workspace.source)
    after = verify_files(workspace.source, manifest['final_files'])
    workspace.state['source_report'] = workspace.report('source_verified',
        manifest_sha256=workspace.bindings['manifest_sha256'], files=after,
        pristine_report_sha256=sha(workspace.root / 'evidence/source-pristine.json'),
        platform_pins=verify_platform(workspace.platform), chromium=chromium, native_host='Darwin/arm64')
    workspace.state['identity_report'] = workspace.report('keychain_identity_verified',
        bundle_id=BUNDLE_ID, profile_directory=BUNDLE_ID, service='Noventra Storage Key', account='Noventra',
        source_files=verify_identity(workspace.source))
    workspace.save()
    workspace.begin('native-assets')
    compile_catalog(workspace)
    workspace.complete('prepared')


def validate_prepared(workspace, manifest):
    verify_platform(workspace.platform)
    verify_chromium(workspace.source)
    verify_files(workspace.source, manifest['final_files'])
    verify_identity(workspace.source)
    catalog = child(workspace.source, 'chrome/app/theme/chromium/mac/Assets.car')
    require(sha(catalog) == workspace.state.get('assets_car_sha256'), 'Generated asset catalog changed')
    for key, gate in [('source_report', 'source_verified'), ('identity_report', 'keychain_identity_verified'),
                      ('assets_report', 'assets_car_actool')]:
        evidence = workspace.state.get(key, {})
        path = child(workspace.root, evidence.get('path'))
        require(sha(path) == evidence.get('sha256'), f'Workspace gate evidence changed: {gate}')
        report = json_object(path)
        require(report.get('gate') == gate and report.get('status') == 'passed', f'Failed saved gate: {gate}')


def build(workspace, manifest, jobs):
    require(isinstance(jobs, int) and not isinstance(jobs, bool) and 1 <= jobs <= 64,
            'Choose an explicit compile job count between 1 and 64')
    require(workspace.state['phase'] in ['prepared', 'built', 'complete']
            and workspace.state.get('in_progress') in [None, 'configure', 'compile', 'finalize'],
            'Only an owned, successfully prepared source tree can be built/resumed')
    validate_prepared(workspace, manifest)
    workspace.begin('configure')
    core = workspace.platform / 'helium-chromium'
    args = render_gn_args((core / 'flags.gn').read_text(), (workspace.platform / 'flags.macos.gn').read_text())
    verify_gn_gates(args)
    workspace.output.mkdir(parents=True, exist_ok=True)
    args_path = child(workspace.output, 'args.gn')
    args_path.write_text(args)
    gn = child(workspace.source, 'buildtools/mac/gn')
    require(gn.is_file() and os.access(gn, os.X_OK), 'Missing native Mac GN tool')
    workspace.command('configure-siso', [sys.executable,
        workspace.source / 'build/config/siso/configure_siso.py',
        '--rbe_instance=projects/rbe-chrome-untrusted/instances/default_instance',
        '--reapi_address=', '--reapi_instance=', '--reapi_backend_config_path='],
        cwd=workspace.source, env=native_env())
    workspace.command('gn-gen', [gn, 'gen', 'out/Default', '--fail-on-unused-args'],
                      cwd=workspace.source, env=native_env())
    # Query evaluated GN values as well as checking our written literal overrides.
    evaluated = subprocess.run([str(gn), 'args', 'out/Default', '--list', '--short'],
        cwd=workspace.source, env=native_env(), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (workspace.root / 'logs/gn-evaluated.log').write_text(evaluated.stdout)
    require(evaluated.returncode == 0, 'GN argument query failed; inspect logs/gn-evaluated.log')
    actual = verify_gn_gates(evaluated.stdout)
    workspace.state['sparkle_report'] = workspace.report('sparkle_disabled', evaluated=actual,
        args_gn_sha256=sha(args_path), gn_output_sha256=sha(workspace.root / 'logs/gn-evaluated.log'))
    workspace.save()
    workspace.begin('compile')
    workspace.state['build_jobs'] = jobs
    workspace.save()
    depot = child(workspace.source, 'third_party/depot_tools/autoninja.py')
    siso = child(workspace.source, 'third_party/siso/cipd/siso')
    env = native_env()
    env['SISO_PATH'] = str(siso)
    workspace.command('autoninja', [sys.executable, depot, '-C', workspace.output,
                      '-j', jobs, 'chrome', 'chrome/installer/mac'], cwd=workspace.source, env=env)
    workspace.complete('built')
    finalize(workspace, manifest)


def finalize(workspace, manifest):
    from package_macos import inventory_digest, validate_bundle
    workspace.begin('finalize')
    validate_prepared(workspace, manifest)
    app = child(workspace.output, APP_NAME)
    info_path = child(app, 'Contents/Info.plist')
    with info_path.open('rb') as stream:
        info = plistlib.load(stream)
    require(info.get('CFBundleIdentifier') == BUNDLE_ID
            and info.get('CFBundleExecutable') == 'Noventra', 'Built app has wrong Noventra identity')
    executable = child(app, 'Contents/MacOS/Noventra')
    arches = workspace.capture('app-architectures', ['xcrun', 'lipo', '-archs', str(executable)]).split()
    require(arches == ['arm64'], f'Expected native arm64 executable; got {arches}')
    require(sha(child(app, 'Contents/Resources/Assets.car')) == workspace.state['assets_car_sha256'],
            'Built app does not contain the native Noventra asset catalog')
    preferences = json_object(PREFERENCES)
    require(not preferences.get('distribution', {}).get('skip_first_run_ui', False),
            'Initial preferences must not bypass onboarding')
    target = child(app, 'Contents/Resources/initial_preferences')
    shutil.copyfile(PREFERENCES, target)
    require(sha(target) == workspace.bindings['initial_preferences_sha256'], 'Initial preferences changed during build')
    validate_bundle(app)
    signing = child(workspace.output, 'Noventra Packaging')
    for name in ['signing/build_props_config.py', 'signing/config.py', 'signing/parts.py',
                 'app-entitlements.plist', 'helper-renderer-entitlements.plist', 'helper-gpu-entitlements.plist']:
        require(child(signing, name).is_file(), f'Missing generated native signing input: {name}')
    evidence = {name: workspace.state[key] for name, key in [
        ('source_verified', 'source_report'), ('assets_car_actool', 'assets_report'),
        ('keychain_identity_verified', 'identity_report'), ('sparkle_disabled', 'sparkle_report')]}
    receipt = {'format': 1, 'kind': 'noventra-macos-build',
        'target': {'os': 'mac', 'cpu': 'arm64', 'component_build': False},
        'app': {'name': APP_NAME, 'bundle_id': BUNDLE_ID, 'tree_sha256': inventory_digest(app)},
        'source': {'manifest_sha256': workspace.bindings['manifest_sha256'], 'engine': ENGINE},
        'gates': {name: True for name in evidence}, 'evidence': evidence,
        'signing_inputs': {'path': str(signing.relative_to(workspace.root)), 'tree_sha256': inventory_digest(signing)},
        'initial_preferences': {'path': 'Contents/Resources/initial_preferences', 'sha256': sha(target)},
        'native_validation': {'compiled': True, 'app_architectures': arches,
                              'compile_jobs': workspace.state['build_jobs'],
                              'launched': False, 'onboarding_import': 'pending', 'peer_transfer': 'pending'},
        'created_at_unix': int(time.time())}
    write_json(workspace.root / 'build-receipt.json', receipt)
    workspace.complete('complete')
    print(f'Compiled app: {app}\nBuild receipt: {workspace.root / "build-receipt.json"}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check-layer', 'prepare', 'build'])
    parser.add_argument('--workspace', type=Path)
    parser.add_argument('--platform-root', type=Path)
    parser.add_argument('--clone-platform', action='store_true', help='Acquire pinned compiler platform into an absent path inside workspace')
    parser.add_argument('--min-free-gib', type=int, default=180, help='Early free-space floor; default 180 GiB, not a build-size guarantee')
    parser.add_argument('--jobs', type=int, default=min(6, os.cpu_count() or 2),
                        help='Native compile concurrency; default min(6, CPU count), range 1..64')
    args = parser.parse_args()
    workspace = None
    try:
        manifest, identity = load_layer()
        if args.action == 'check-layer':
            print(json.dumps({'manifest_sha256': identity, 'files': len(manifest['final_files']),
                              'engine': ENGINE, 'native_build_verified': False}, indent=2))
            return 0
        require(args.workspace is not None and args.platform_root is not None,
                'Supply explicit --workspace and --platform-root paths')
        require(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                'Native Apple-silicon macOS is required; Linux and cross builds are disabled')
        require(args.min_free_gib >= 80, 'Free-space prerequisite floor cannot be less than 80 GiB')
        workspace = Workspace(args.workspace, args.platform_root, identity, create=args.action == 'prepare')
        with exclusive(workspace):
            prerequisites(workspace, args.min_free_gib)
            if args.action == 'prepare':
                prepare(workspace, manifest, args.clone_platform)
            else:
                build(workspace, manifest, args.jobs)
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        if workspace is not None:
            write_json(workspace.root / 'failure.json', {'format': 1, 'status': 'failed',
                'phase': workspace.state.get('phase'), 'in_progress': workspace.state.get('in_progress'),
                'error': str(error), 'source_mismatches': getattr(error, 'mismatches', None),
                'recorded_at_unix': int(time.time())})
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
