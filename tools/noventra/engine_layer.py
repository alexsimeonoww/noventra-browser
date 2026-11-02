"""Verify and apply the separate, qualified Chromium156 Noventra layer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ENGINE = {
    'platform': '83027a19267e526f08e25a3af1a30b45ee7e3e2e',
    'helium': '2f81b9cafa38a95071e20de3f9cd6b0aca6d3c65',
    'chromium': 'a21ce95c24ce3964416f09ae1862f53f319eb4d0',
    'version': '156.0.8078.79',
    'target_cpu': 'x64',
}
LAYER = 'noventra/patches/chromium-156'
TOOLING = (
    'scripts/build.sh', 'scripts/shared.sh', 'scripts/docker-build.sh', 'scripts/dev.sh',
    '.github/scripts/build.sh', 'docker/build.Dockerfile', 'downloads.ini',
    'tools/noventra/engine_layer.py', 'tools/noventra/build_storage.py',
)
OVERLAYS = set(TOOLING) - {'tools/noventra/engine_layer.py', 'tools/noventra/build_storage.py'}
RESOURCE_METADATA = {
    'initial_preferences.json', 'generated/manifest.json',
    'approved/noventra-favicon-dark.png', 'approved/noventra-favicon-light.png',
    'approved/noventra-symbol.png',
}
MANIFEST_KEYS = {
    'schema', 'engine', 'builder_image_id', 'tooling', 'series_sha256', 'patches',
    'replacements_sha256', 'resource_inputs', 'pristine_files', 'final_files',
    'gn_args_sha256',
}
RECEIPT = 'noventra-engine156-receipt.json'
PENDING = 'noventra-engine156-applied.json'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def relative(value):
    require(isinstance(value, str) and value and '\\' not in value,
            f'Invalid relative path: {value!r}')
    parts = value.split('/')
    require(not any(p in ('', '.', '..', '.git') for p in parts),
            f'Unsafe relative path: {value!r}')
    return Path(*parts)


def safe(root, name):
    path = root
    for part in relative(name).parts:
        path = path / part
        require(not path.is_symlink(), f'Symlink is not a declared input: {path}')
    return path


def digest(value):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value),
            'Expected SHA256 hex digest')
    return value


def verify_files(root, files, *, missing_allowed=False):
    for name, expected in files.items():
        path = safe(root, name)
        if expected is None and missing_allowed:
            require(not path.exists(), f'Expected absent pristine file: {path}')
        else:
            require(path.is_file() and sha(path) == digest(expected),
                    f'File hash mismatch: {path}')


def version(source):
    path = safe(source, 'chrome/VERSION')
    require(path.is_file(), 'Source chrome/VERSION is missing')
    values = dict(line.split('=', 1) for line in path.read_text().splitlines())
    return '.'.join(values[key] for key in ('MAJOR', 'MINOR', 'BUILD', 'PATCH'))


def git(repo, *args):
    return subprocess.check_output(
        ['git', '--no-optional-locks', '-C', str(repo), *args], text=True).strip()


def pins(workspace):
    for repo, expected, allowed in (
            (workspace, ENGINE['platform'], OVERLAYS),
            (workspace / 'helium-chromium', ENGINE['helium'], set())):
        require(Path(git(repo, 'rev-parse', '--show-toplevel')).resolve() == repo,
                f'Expected an independent pinned checkout: {repo}')
        require(git(repo, 'rev-parse', 'HEAD') == expected, f'Wrong checkout: {repo}')
        changed = set(git(repo, 'diff', '--name-only', 'HEAD', '--').splitlines())
        require(changed <= allowed, f'Unreviewed upstream changes: {sorted(changed - allowed)}')
    require((workspace / 'helium-chromium/chromium_version.txt').read_text().strip()
            == ENGINE['version'], 'Pinned base has the wrong Chromium version')


def validate_patch(path, declared):
    # Ignore hunk data when identifying -p1 file headers: source lines can
    # themselves begin with "--- " or "+++ ". GNU patch validates the diff.
    old_left = new_left = headers = 0
    for line in path.read_text().splitlines():
        if old_left or new_left:
            if line.startswith('\\'):
                continue
            require(line and line[0] in ' +-', 'Invalid patch hunk')
            old_left -= line[0] in ' -'
            new_left -= line[0] in ' +'
            require(old_left >= 0 and new_left >= 0, 'Invalid patch hunk length')
            continue
        hunk = re.match(r'^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@', line)
        if hunk:
            old_left, new_left = (int(count) if count is not None else 1
                                  for count in hunk.groups())
            continue
        if not line.startswith(('--- ', '+++ ')):
            continue
        name = line[4:].split('\t', 1)[0].split(' ', 1)[0]
        if name == '/dev/null':
            require(line.startswith('--- '), 'Layer deletions are not declared')
            continue
        require(not name.startswith('/') and '/' in name, 'Invalid patch header')
        name = name.split('/', 1)[1]
        relative(name)
        require(name in declared, f'Undeclared patch path: {name}')
        headers += 1
    require(headers > 0 and old_left == new_left == 0,
            'Patch has missing file headers or an incomplete hunk')


class Layer:
    def __init__(self, project, workspace):
        self.project = project.resolve(strict=True)
        self.workspace = workspace.resolve(strict=True)
        self.source = safe(self.workspace, 'build/src')
        self.build = safe(self.workspace, 'build')
        self.directory = safe(self.project, LAYER)
        self.manifest_path = safe(self.directory, 'manifest.json')
        require(self.manifest_path.is_file(),
                'Final qualified Chromium156 manifest is absent; preparation is disabled')
        self.manifest = json.loads(self.manifest_path.read_text())
        self.identity = sha(self.manifest_path)
        m = self.manifest
        require(isinstance(m, dict) and set(m) == MANIFEST_KEYS,
                'Invalid manifest fields')
        require(m['schema'] == 1 and m['engine'] == ENGINE, 'Wrong engine manifest')
        require(isinstance(m['builder_image_id'], str) and
                re.fullmatch('sha256:[0-9a-f]{64}', m['builder_image_id']),
                'Builder must have an immutable image ID')
        for key in ('tooling', 'resource_inputs', 'pristine_files', 'final_files'):
            require(isinstance(m[key], dict) and m[key], f'Invalid {key} inventory')
        require(isinstance(m['patches'], list) and m['patches'], 'Empty patch declaration')
        for row in m['patches']:
            require(isinstance(row, dict) and set(row) == {'file', 'sha256'},
                    'Invalid patch declaration')
            relative(row['file'])
            digest(row['sha256'])
        require(set(m['tooling']) == set(TOOLING), 'Incomplete tooling declaration')
        verify_files(self.project, m['tooling'])
        require(m['final_files'] and set(m['pristine_files']) == set(m['final_files']),
                'Pristine and final path inventories must match')
        for name, value in m['final_files'].items():
            require(relative(name).parts[0] != 'out', 'Layer cannot change compiler outputs')
            digest(value)
        for name, value in m['pristine_files'].items():
            relative(name)
            if value is not None:
                digest(value)
        digest(m['gn_args_sha256'])
        require(sha(safe(self.directory, 'series')) == digest(m['series_sha256']),
                'Series changed')
        self.series = [line.strip() for line in (self.directory / 'series').read_text().splitlines()
                       if line.strip() and not line.lstrip().startswith('#')]
        require(self.series and len(self.series) == len(set(self.series)), 'Empty/duplicate series')
        require(self.series == [row['file'] for row in m['patches']], 'Patch order changed')
        for row in m['patches']:
            patch = safe(self.directory, row['file'])
            require(patch.is_file() and sha(patch) == digest(row['sha256']), 'Patch changed')
            validate_patch(patch, m['final_files'])
        self.resources = safe(self.project, 'noventra/resources')
        require(sha(safe(self.resources, 'replacements.txt')) == digest(m['replacements_sha256']),
                'Resource replacement list changed')
        require(RESOURCE_METADATA <= set(m['resource_inputs']),
                'Resource inputs must include approved artwork, generation manifest and defaults')
        verify_files(self.resources, m['resource_inputs'])
        self.replacements = []
        for line in (self.resources / 'replacements.txt').read_text().splitlines():
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            pair = line.split()
            require(len(pair) == 2, 'Invalid resource replacement')
            src, dst = pair
            relative(src)
            relative(dst)
            require(src in m['resource_inputs'] and dst in m['final_files'],
                    'Resource replacement must be declared')
            require(m['resource_inputs'][src] == m['final_files'][dst],
                    'Resource and final destination hashes differ')
            self.replacements.append((src, dst))
        require(self.replacements and
                len({dst for _, dst in self.replacements}) == len(self.replacements),
                'Empty or duplicate resource destinations')
        pins(self.workspace)
        require(os.environ.get('ARCH', 'x64') in ('x64', 'x86_64'),
                'Only the qualified Linux x64 configuration is supported')

    def source_version(self):
        require(self.source.is_dir() and version(self.source) == ENGINE['version'],
                'Protected/stale source version; expected Chromium156')

    def admit(self, mode):
        if mode == 'prepare':
            if self.source.exists():
                if (self.source / 'chrome/VERSION').exists():
                    self.source_version()
                require(self.source.is_dir() and not any(self.source.iterdir()),
                        'Preparation requires a fresh empty src; no existing source adoption')
            for marker in (RECEIPT, PENDING, 'noventra-layer-applied'):
                require(not (self.build / marker).exists(), f'Stale preparation marker: {marker}')
        else:
            self.verify_prepared()
        return {'mode': mode, 'manifest_sha256': self.identity,
                'builder_image_id': self.manifest['builder_image_id']}

    def verify_final(self):
        self.source_version()
        verify_files(self.source, self.manifest['final_files'])

    def verify_args(self):
        args = safe(self.source, 'out/Default/args.gn')
        require(args.is_file() and sha(args) == self.manifest['gn_args_sha256'],
                'GN arguments differ from the qualified configuration')
        text = args.read_text()
        pgo = re.findall(r'^chrome_pgo_phase\s*=\s*(\d+)\s*$', text, re.M)
        cpu = re.findall(r'^target_cpu\s*=\s*"([^"]+)"\s*$', text, re.M)
        require(pgo and pgo[-1] == '2' and cpu and cpu[-1] == 'x64',
                'Expected phase2 PGO and x64 arguments')
        ninja = safe(self.source, 'out/Default/build.ninja')
        require(ninja.is_file() and ninja.stat().st_size > 0, 'GN generation is incomplete')

    def marker(self, state):
        return {'schema': 1, 'state': state, 'engine': ENGINE,
                'manifest_sha256': self.identity,
                'gn_args_sha256': self.manifest['gn_args_sha256'],
                'declared_files': len(self.manifest['final_files'])}

    def write_marker(self, name, state):
        destination = safe(self.build, name)
        require(not destination.exists(), f'Refusing to overwrite marker: {destination}')
        fd, temporary = tempfile.mkstemp(prefix='.engine156-', dir=self.build)
        try:
            with os.fdopen(fd, 'w') as stream:
                json.dump(self.marker(state), stream, indent=2)
                stream.write('\n')
                stream.flush()
                os.fsync(stream.fileno())
            os.link(temporary, destination)  # Exclusive publication; never replace a marker.
        finally:
            Path(temporary).unlink(missing_ok=True)

    def check_marker(self, name, state):
        path = safe(self.build, name)
        require(path.is_file() and json.loads(path.read_text()) == self.marker(state),
                f'Missing or stale {state} receipt')

    def apply(self):
        self.source_version()
        require(not (self.build / RECEIPT).exists() and not (self.build / PENDING).exists(),
                'Layer already applied; use a verified resume')
        verify_files(self.source, self.manifest['pristine_files'], missing_allowed=True)
        utility = safe(self.workspace, 'helium-chromium/utils/patches.py')
        subprocess.run([sys.executable, str(utility), 'apply', '--no-fuzz',
                        str(self.source), str(self.directory)], check=True)
        for src, dst in self.replacements:
            target = safe(self.source, dst)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(safe(self.resources, src), target)
        self.verify_final()
        self.write_marker(PENDING, 'applied')

    def seal(self):
        self.check_marker(PENDING, 'applied')
        self.verify_final()
        self.verify_args()
        self.write_marker(RECEIPT, 'prepared')  # Written only after successful GN generation.

    def verify_prepared(self):
        self.check_marker(RECEIPT, 'prepared')
        self.verify_final()
        self.verify_args()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('admit', 'apply', 'seal'))
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--mode', choices=('prepare', 'resume'), default='prepare')
    args = parser.parse_args()
    try:
        layer = Layer(args.project, args.workspace)
        if args.action == 'admit':
            print(json.dumps(layer.admit(args.mode)))
        else:
            getattr(layer, args.action)()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Engine156 preflight failed: {error}\n')


if __name__ == '__main__':
    main()
