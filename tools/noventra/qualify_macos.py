#!/usr/bin/env python3
"""Bounded local headless version and JavaScript DOM checks with mock Keychain."""

import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import signal
import subprocess
import time

HTML = ('<!doctype html><html><body><p id="native-smoke">PENDING</p>'
        '<script>document.getElementById("native-smoke").textContent='
        '"NOVENTRA_NATIVE_JS_"+(40+2);'
        'document.documentElement.dataset.noventraSmoke="passed";'
        '</script></body></html>')


def process_table():
    result = subprocess.run(['/bin/ps', '-axo', 'pid=,ppid=,command='],
                            capture_output=True, text=True, check=True, timeout=5)
    table = {}
    for line in result.stdout.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) == 3:
            table[int(fields[0])] = (int(fields[1]), fields[2])
    return table


def owned_processes(root, table):
    owned = {root}
    while more := {pid for pid, (parent, _) in table.items() if parent in owned} - owned:
        owned |= more
    return {pid: table[pid] for pid in owned if pid in table}


def terminate_owned(process, tracked):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    for sig in (signal.SIGTERM, signal.SIGKILL):
        table = process_table()
        for pid, (_, command) in tracked.items():
            if pid != process.pid and pid in table and table[pid][1] == command:
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    pass
        time.sleep(.1)


def run_probe(executable, output, mode, timeout, version):
    output.mkdir()
    profile = output / 'profile'
    html = output / 'smoke.html'
    html.write_text(HTML)
    argv = [str(executable), '--headless', '--incognito', '--no-first-run',
            '--no-default-browser-check', '--use-mock-keychain',
            '--disable-background-networking', '--disable-sync', f'--user-data-dir={profile}']
    argv += ['--version'] if mode == 'version' else ['--virtual-time-budget=1500', '--dump-dom', html.as_uri()]
    result = {'argv': argv, 'timeout_seconds': timeout, 'passed': False,
              'timed_out': False, 'exit': None, 'profile_owned': True}
    started = time.monotonic()
    tracked = {}
    with (output / 'stdout.log').open('wb') as out, (output / 'stderr.log').open('wb') as err:
        process = subprocess.Popen(argv, stdout=out, stderr=err, start_new_session=True)
        try:
            while process.poll() is None and time.monotonic() - started < timeout:
                tracked.update(owned_processes(process.pid, process_table()))
                time.sleep(.1)
            result['timed_out'] = process.poll() is None
        finally:
            try:
                tracked.update(owned_processes(process.pid, process_table()))
            finally:
                terminate_owned(process, tracked)
        result['exit'] = process.returncode
    text = (output / 'stdout.log').read_text(errors='replace')
    result['dom_marker'] = '<p id="native-smoke">NOVENTRA_NATIVE_JS_42</p>' in text and 'data-noventra-smoke="passed"' in text
    result['version_marker'] = f'Noventra {version} (Chromium ' in text
    result['elapsed_seconds'] = round(time.monotonic() - started, 3)
    table = process_table()
    result['owned_processes_remaining'] = [pid for pid, (_, command) in tracked.items()
                                           if pid in table and table[pid][1] == command]
    result['passed'] = (not result['timed_out'] and result['exit'] == 0
                        and not result['owned_processes_remaining']
                        and result['version_marker' if mode == 'version' else 'dom_marker'])
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def qualify(app, output, timeout=30):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise ValueError('Native Apple-silicon macOS is required for runtime qualification')
    if not 1 <= timeout <= 120:
        raise ValueError('Runtime timeout must be between 1 and 120 seconds')
    app, output = Path(app).absolute(), Path(output).absolute()
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    if (app.name != 'Noventra.app' or info.get('CFBundleIdentifier') != 'local.noventra.browser'
            or info.get('CFBundleExecutable') != 'Noventra'):
        raise ValueError('Expected the native Noventra application identity')
    if (output.exists() or output.is_symlink() or output.resolve().is_relative_to(app.resolve())
            or app.resolve().is_relative_to(output.resolve())):
        raise ValueError('Runtime output must be a new directory outside the application')
    output.mkdir(parents=True, exist_ok=False)
    executable = app / 'Contents/MacOS/Noventra'
    version = run_probe(executable, output / 'version', 'version', timeout, info['CFBundleShortVersionString'])
    dom = (run_probe(executable, output / 'dom', 'dom', timeout, info['CFBundleShortVersionString'])
           if version['passed'] else {'passed': False, 'skipped': 'Version launch failed'})
    result = {'format': 1, 'kind': 'noventra-macos-runtime', 'app': str(app),
              'passed': version['passed'] and dom['passed'], 'version': version, 'dom': dom,
              'native_ui_qualified': False, 'real_keychain_qualified': False,
              'scope': 'local headless JavaScript DOM; fresh incognito profile; mock Keychain'}
    (output / 'runtime-receipt.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=30)
    args = parser.parse_args()
    try:
        result = qualify(args.app, args.output, args.timeout)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Runtime qualification failed: {error}\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
