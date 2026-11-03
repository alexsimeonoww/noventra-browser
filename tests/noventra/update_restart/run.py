"""Real About restart through signed loopback packages in an owned HOME/display.

Both package versions deliberately use the same qualified candidate native ELF.
Only their explicit local labels and sequence floors differ. This proves native
restart, authenticated delivery, atomic replacement, cancellation and retained
Space data; it does not claim to test an older native implementation.
"""
import argparse
import base64
from contextlib import contextmanager
import ctypes
import hashlib
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import threading
import time
import traceback
from types import SimpleNamespace
import urllib.parse

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tests/noventra'))
sys.path.insert(0, str(ROOT / 'tools/noventra'))
from benchmark import parse_stat
from extension_smoke import request
from layout_test_support import LayoutBrowser
from portable_transfer_smoke import isolate
from workspace_management_smoke import protocol
from dock_rail_regressions_smoke import Input
from interface_polish_smoke import native_key
from incognito_space_smoke import owned_windows, private_window_manager
import package_distribution as package
import release_manifest as release
import local_delivery_server

DEEP = '''function deep(root,name){for(const e of root.querySelectorAll('*')){
 if(e.localName===name)return e;if(e.shadowRoot){const found=deep(e.shadowRoot,name);if(found)return found;}}return null;}
 const about=deep(document,'settings-about-page');'''
STATUS = '(()=>{' + DEEP + '''const button=about?.shadowRoot?.querySelector('#relaunch');
 return {status:about?.currentUpdateStatusEvent_?.status,
 text:about?.shadowRoot?.querySelector('#updateStatusMessage')?.innerText,
 buttonText:button?.innerText,buttonVisible:!!button&&!button.hidden&&!!button.getClientRects().length,
 buttonDisabled:button?.disabled};})()'''
BINDINGS = [HERE / 'run.py', HERE / 'dialog.mjs',
 ROOT / 'noventra/package/desktop_updater.py', ROOT / 'noventra/package/installer.py',
 ROOT / 'noventra/package/AppRun', ROOT / 'tools/noventra/package_distribution.py',
 ROOT / 'tools/noventra/release_manifest.py', ROOT / 'tools/noventra/update_delivery.py',
 ROOT / 'tools/noventra/local_delivery_server.py', ROOT / 'tests/noventra/layout_test_support.py',
 ROOT / 'tests/noventra/portable_transfer_smoke.py', ROOT / 'tests/noventra/layout_editor_input.mjs',
 ROOT / 'tests/noventra/incognito_space_smoke.py']


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def check(result, name, condition):
    result['checks'].append({'name': name, 'passed': bool(condition)})
    assert condition, name


def until(predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise TimeoutError('Native update fixture condition exceeded its budget')


def process(pid):
    root = Path('/proc') / str(pid)
    before = parse_stat((root / 'stat').read_text())
    record = {**before, 'uid': root.stat().st_uid, 'exe': os.readlink(root / 'exe')}
    after = parse_stat((root / 'stat').read_text())
    assert before['starttime'] == after['starttime'] and before['ppid'] == after['ppid']
    return record


def live(row):
    try:
        current = process(row['pid'])
        return all(current[key] == row[key] for key in ('starttime', 'uid', 'exe'))
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return False


def descendants():
    rows = {}
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            rows[int(path.name)] = process(int(path.name))
        except (OSError, ValueError, IndexError):
            pass
    owned = {os.getpid()}
    changed = True
    while changed:
        changed = False
        for pid, row in rows.items():
            if row['ppid'] in owned and pid not in owned:
                owned.add(pid)
                changed = True
    return [rows[pid] for pid in owned - {os.getpid()} if pid in rows]


def serial(row):
    return {key: row[key] for key in ('pid', 'ppid', 'session', 'starttime', 'uid', 'exe')}


class RootMonitor:
    def __init__(self, row):
        self.row = row
        self.pid = row['pid']

    def poll(self):
        return None if live(self.row) else 0


class Browser(LayoutBrowser):
    def start(self):
        self.output.mkdir()
        self.profile.mkdir()
        self.active = self.profile / 'DevToolsActivePort'
        self.log = (self.output / 'browser.log').open('x')
        env = dict(os.environ, NOVENTRA_CONFIG_HOME=str(self.profile / 'test-config'))
        argv = [str(self.browser), '--user-data-dir=' + str(self.profile),
                '--ozone-platform=x11', '--disable-gpu', '--window-size=1200,760',
                '--window-position=0,0', '--no-first-run', '--no-default-browser-check',
                '--password-store=basic', '--ui-toolkit=gtk',
                '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
                'chrome://settings/appearance']
        write(self.output / 'launch.json', {'argv': argv})
        self.coordinator = subprocess.Popen(argv, env=env, stdout=self.log,
                                            stderr=self.log, start_new_session=True)
        self.process = self.coordinator
        self.attach()
        return self

    def attach(self, previous=None, *, document_ready=True):
        def ready():
            if not self.active.exists():
                return False
            lines = self.active.read_text().splitlines()
            if len(lines) < 2 or not lines[0].isdigit() or (previous and lines == previous):
                return False
            try:
                pages = request(int(lines[0]), '/json/list')
            except (OSError, ValueError):
                return False
            return (int(lines[0]), lines, pages) if any(p['type'] == 'page' for p in pages) else False
        self.port, self.active_lines, pages = until(ready, timeout=60)
        self.settings = None if previous else next((p for p in pages if p['type'] == 'page' and p['url'].startswith('chrome://settings/')), None)
        if self.settings is None:
            self.settings = request(self.port, '/json/new?chrome://settings/appearance', 'PUT')
        if document_ready:
            self.ready_settings()

    def ready_settings(self):
        activate(self, self.settings)
        until(lambda: self.ev('document.readyState==="complete"&&!!document.querySelector("settings-ui")?.shadowRoot'))

    def native(self, app):
        path = str(app / 'opt/noventra/noventra-browser')
        found = []
        for row in descendants():
            if row['exe'] != path:
                continue
            row['arguments'] = (Path('/proc') / str(row['pid']) / 'cmdline').read_bytes().split(b'\0')
            if not re.search(rb'(?<!\S)--type=', b' '.join(row['arguments'])):
                found.append(row)
        assert len(found) == 1, [serial(row) for row in found]
        row = found[0]
        assert row['uid'] == os.getuid()
        arguments = [arg.decode(errors='strict') for arg in row['arguments'] if arg]
        expected = '--user-data-dir=' + str(self.profile)
        if len(arguments) == 1:
            assert not re.search(r'\s--|[\r\n\x00]', str(self.profile)), 'Ambiguous owned profile path'
            assert len(re.findall(r'(?<!\S)--user-data-dir(?:=|\s|$)', arguments[0])) == 1
            following = ('ozone-platform', 'window-position', 'window-size', 'restore-last-session',
                         'no-first-run', 'no-default-browser-check', 'remote-debugging-address',
                         'remote-debugging-port', 'password-store', 'ui-toolkit', 'disable-gpu')
            boundary = r'(?= --(?:' + '|'.join(following) + r')(?:=|\s|$)|$)'
            assert re.search(r'(?<!\S)' + re.escape(expected) + boundary, arguments[0]), arguments
        else:
            assert [a for a in arguments if a.startswith('--user-data-dir')] == [expected], arguments
        assert not re.search(r'(?<!\S)--(?:no-sandbox|single-process|disable-setuid-sandbox)(?=\s|=|$)',
                             ' '.join(arguments))
        return row


def target(browser, url):
    page = request(browser.port, '/json/new?' + url, 'PUT')
    until(lambda: browser.ev('document.readyState==="complete"&&location.href===' + json.dumps(url), page))
    return page


def activate(browser, page):
    request(browser.port, '/json/activate/' + page['id'])
    until(lambda: browser.ev('document.visibilityState==="visible"', page))


def click(browser, page, selector):
    activate(browser, page)
    point = browser.ev('(()=>{' + DEEP + 'const e=' + selector + ''';
      if(!e||e.hidden||e.disabled||!e.getClientRects().length)return null;
      e.scrollIntoView({block:'center'});const r=e.getBoundingClientRect();
      return {x:r.x+r.width/2,y:r.y+r.height/2};})()''', page)
    assert point, selector
    steps = [{'method': 'Input.dispatchMouseEvent', 'params': {'type': kind,
              'button': 'left' if kind != 'mouseMoved' else 'none', 'clickCount': 1,
              'x': point['x'], 'y': point['y']}} for kind in ('mouseMoved', 'mousePressed', 'mouseReleased')]
    subprocess.run(['/usr/bin/node', str(ROOT / 'tests/noventra/layout_editor_input.mjs'),
                    page['webSocketDebuggerUrl'], json.dumps(steps)], check=True, timeout=25)


def capture(browser, page, path):
    data = protocol(browser, 'Page.captureScreenshot', {'format': 'png'}, page)
    path.write_bytes(base64.b64decode(data['data']))


def status(browser, page):
    return browser.ev(STATUS, page)


def dialog_node(browser, page):
    return browser.ev('(()=>{' + DEEP + '''const d=about?.shadowRoot?.querySelector('relaunch-confirmation-dialog');
      const description=d?.relaunchConfirmationDialogDesc;
      return d?.shadowRoot?.querySelector('#dialog')?.open&&typeof description==='string'&&description.trim()
        ? {text:d.innerText||d.shadowRoot.textContent,description}:null;})()''', page)


@contextmanager
def pages():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            role = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get('role', ['owned'])[0]
            data = ('<!doctype html><meta charset="utf-8"><title>Owned update ' + role + '</title>'
                    '<style>body{margin:32px;font:16px sans-serif}</style>'
                    '<h1>Owned update ' + role + '</h1><input id="draft">'
                    '<script>window.ownedToken=crypto.randomUUID()</script>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)
    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{http.server_port}'
    finally:
        http.shutdown(); http.server_close(); thread.join(timeout=3)


def observe_processes(result):
    observations = result.setdefault('process_observations', {})
    for row in descendants():
        try:
            argv = (Path('/proc') / str(row['pid']) / 'cmdline').read_bytes().split(b'\0')
        except OSError:
            continue
        text = b' '.join(argv).decode(errors='replace')
        process_type = re.search(r'(?<!\S)--type=([^\s]+)', text)
        role = process_type[1] if process_type else Path(row['exe']).name
        if 'desktop_updater.py' in text:
            role = next((value.decode() for value in argv if value in
                         (b'launch', b'relaunch', b'background', b'check', b'status')), 'updater')
        observations[str(row['pid']) + ':' + str(row['starttime'])] = {
            **serial(row), 'role': role, 'arguments': [a.decode(errors='replace') for a in argv if a]}


def coordinator_proof(browser, native, *, operation, previous_native=None):
    parent = process(native['ppid'])
    assert parent['ppid'] == os.getpid() and parent['uid'] == os.getuid(), serial(parent)
    assert parent['exe'] == str(Path('/usr/bin/python3').resolve()), serial(parent)
    argv = [a.decode() for a in (Path('/proc') / str(parent['pid']) / 'cmdline').read_bytes().split(b'\0') if a]
    prefix = ['/usr/bin/python3', '-I', '-B', str(browser.browser.parent / 'updater/desktop_updater.py')]
    assert argv[:4] == prefix, argv
    if operation == 'launch':
        assert argv[4:6] == ['launch', '--'], argv
    else:
        assert argv[4:8] == ['--owner-pid', str(previous_native['pid']), 'relaunch', '--'], argv
    after = process(parent['pid'])
    assert all(parent[k] == after[k] for k in ('starttime', 'ppid', 'exe', 'uid'))
    return {**serial(parent), 'operation': operation, 'native': serial(native), 'arguments': argv,
            'native_exit_status_source': 'trusted coordinator returns its exact native child wait status'}


def wait_coordinator(browser, result):
    proof = browser.restart_coordinator
    def exited():
        return os.waitid(os.P_PIDFD, browser.restart_pidfd,
                         os.WEXITED | os.WNOWAIT | os.WNOHANG)
    observed = until(exited, timeout=45)
    assert observed.si_pid == proof['pid'] and observed.si_code == os.CLD_EXITED
    pid, status = os.waitpid(proof['pid'], 0)
    os.close(browser.restart_pidfd)
    browser.restart_pidfd = None
    code = os.waitstatus_to_exitcode(status)
    result['normal_root_shutdowns'].append({**proof, 'coordinator_exit_code': code,
          'native_exit_code': code, 'native_identity_gone': not live(proof['native']),
          'pidfd_wait_identity': observed.si_pid})
    assert pid == proof['pid'] and code == observed.si_status == 0
    assert not live(proof['native'])


def reaped_children(result):
    while True:
        try:
            pid, code = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if not pid:
            return
        matches = [row for row in result.get('process_observations', {}).values() if row['pid'] == pid]
        result['reaped_children'].append({'pid': pid, 'exit_code': os.waitstatus_to_exitcode(code),
              'observed_identity': matches[-1] if matches else None,
              'role': matches[-1]['role'] if matches else 'unobserved ancillary child'})


def close_browser(browser, result):
    try:
        def endpoint_ready():
            try:
                browser.native(browser.browser.parent)
                lines = (browser.profile / 'DevToolsActivePort').read_text().splitlines()
                if len(lines) < 2 or not lines[0].isdigit():
                    return False
                port = int(lines[0])
                endpoint = request(port, '/json/version')['webSocketDebuggerUrl']
                browser.port = port
                return endpoint
            except (OSError, ValueError, AssertionError, IndexError):
                return False
        endpoint = until(endpoint_ready, timeout=60)
        observe_processes(result)
        script = '''const s=new WebSocket(process.argv[1]);let sent=false;
          const t=setTimeout(()=>process.exit(1),15000);s.onopen=()=>{sent=true;s.send(JSON.stringify({id:1,method:'Browser.close'}))};
          s.onclose=()=>{clearTimeout(t);process.exit(sent?0:1)};'''
        subprocess.run(['/usr/bin/node', '-e', script, endpoint], check=True, timeout=20)
        if getattr(browser, 'restart_pidfd', None) is not None:
            wait_coordinator(browser, result)
        if browser.coordinator.poll() is None:
            browser.coordinator.wait(timeout=15)
        assert browser.coordinator.returncode == 0
        until(lambda: not descendants(), timeout=40)
        reaped_children(result)
        result['normal_shutdown'] = True
    except BaseException as error:
        result['normal_shutdown'] = False
        result['shutdown_error'] = repr(error)
        raise


def prepare(runtime, output, feed):
    key = output / 'disposable-test-only.pem'
    release.openssl('genpkey', '-algorithm', 'ED25519', '-out', key)
    key.chmod(0o600)
    public = output / 'disposable-test-only-public.pem'
    public.write_bytes(release.openssl('pkey', '-in', key, '-pubout'))
    package_dirs, releases = {}, {}
    for version in (1, 2):
        config = output / f'update-config-{version}.json'
        config.write_bytes(release.encode({'format': 1, 'base_url': feed, 'channel': 'local',
                                          'sequence': version, 'public_key': str(public)}))
        dist = output / f'package-{version}'
        package.package(runtime, dist, f'update-button-native-{version}', archive=True, update_config=config)
        published = output / f'feed-{version}'
        release.publish(dist, published, key, version, channel='local')
        package_dirs[version] = dist / 'Noventra.AppDir'
        releases[version] = published
    return package_dirs, releases, public



def reused_packages(source, runtime, result):
    source = source.resolve(strict=True)
    assert source.is_relative_to(Path('/var/tmp/noventra-update-button-2026-11-03'))
    prior = read(source / 'result.json')
    assert prior['binary_sha256'] == result['binary_sha256']
    assert prior['resources_sha256'] == result['resources_sha256']
    public = source / 'disposable-test-only-public.pem'
    signer = release.sha(release.public_der(public))
    spec = importlib.util.spec_from_file_location('native_reuse_installer', ROOT / 'noventra/package/installer.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    packages, feeds = {}, {}
    inputs = {str(source / 'result.json'): sha(source / 'result.json'), str(public): sha(public)}
    records = {}
    for version in (1, 2):
        packages[version] = source / f'package-{version}/Noventra.AppDir'
        feeds[version] = source / f'feed-{version}'
        app = packages[version]
        feed = feeds[version]
        raw = (feed / 'manifest.json').read_bytes()
        assert release.verify_signature(raw, (feed / 'manifest.sig').read_bytes(), public) == signer
        manifest = release.validate_manifest(release.parse(raw))
        assert manifest['signer_id'] == signer and manifest['sequence'] == version
        assert manifest['channel'] == 'local' and manifest['version_label'] == f'update-button-native-{version}'
        assert manifest['snapshot_sha256'] == sha(runtime / 'snapshot.json')
        distribution = feed / 'distribution.json'
        assert sha(distribution) == manifest['distribution']['sha256']
        assert distribution.stat().st_size == manifest['distribution']['size']
        payload = release.validate_distribution(release.parse(distribution.read_bytes()), manifest, 4 * 1024**3)
        release.verify_package(app.parent, manifest, payload)
        inventory, fingerprint = installer.validate_package(app)
        assert fingerprint == payload['Noventra.AppDir/install-manifest.json']['sha256']
        assert set(payload) == {'Noventra.AppDir/' + name for name in inventory['files']} | {'Noventra.AppDir/install-manifest.json'}
        for name, row in inventory['files'].items():
            assert row == payload['Noventra.AppDir/' + name]
            if 'sha256' in row:
                inputs[str(app / name)] = row['sha256']
        assert sha(app / 'opt/noventra/noventra-browser') == result['binary_sha256']
        assert sha(app / 'opt/noventra/resources.pak') == result['resources_sha256']
        for installed, trusted in [('updater/desktop_updater.py', ROOT / 'noventra/package/desktop_updater.py'),
                                   ('AppRun', ROOT / 'noventra/package/AppRun'),
                                   ('installer.py', ROOT / 'noventra/package/installer.py'),
                                   ('updater/release_manifest.py', ROOT / 'tools/noventra/release_manifest.py'),
                                   ('updater/update_delivery.py', ROOT / 'tools/noventra/update_delivery.py')]:
            assert sha(app / installed) == sha(trusted)
        archive = feed / manifest['artifact']['filename']
        assert archive.stat().st_size == manifest['artifact']['size']
        assert sha(archive) == manifest['artifact']['sha256']
        for path in [feed / 'manifest.json', feed / 'manifest.sig', distribution, archive, app / 'install-manifest.json']:
            inputs[str(path)] = sha(path)
        records[str(version)] = {'manifest': manifest, 'install_manifest_sha256': fingerprint,
                                'all_app_files_reverified': len(inventory['files'])}
    result['reused_source_inputs'] = inputs
    result['reused_packages'] = {'source': str(source), 'versions': records,
                                'source_read_only_in_namespace': not source.is_relative_to(Path(os.environ['NOVENTRA_TRANSFER_OWNED_HOME']))}
    return packages, feeds, public


def fixture(args):
    args.browser = args.browser.resolve(strict=True)
    if args.packages_from is not None:
        assert args.case == 'update-restart'
        args.packages_from = args.packages_from.resolve(strict=True)
        args.case = json.dumps({'format': 1, 'packages_from': str(args.packages_from)})
    elif args.case != 'update-restart':
        value = json.loads(args.case)
        assert set(value) == {'format', 'packages_from'} and value['format'] == 1
        args.packages_from = Path(value['packages_from']).resolve(strict=True)
    args.expected_binary = args.expected_binary or sha(args.browser)
    args.expected_resources = args.expected_resources or sha(args.browser.parent / 'resources.pak')
    args.output = args.output.absolute()
    isolate(args, HERE / 'run.py')
    args.output.mkdir()
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0, 'Could not become owned child subreaper'
    output = args.output
    data = output / 'data'
    data.mkdir(mode=0o700)
    os.environ.update(XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(Path.home() / '.config'),
                      XDG_CACHE_HOME=str(Path.home() / '.cache'))
    os.environ.pop('APPIMAGE', None)
    bindings = {str(path): sha(path) for path in BINDINGS}
    result = {'passed': False, 'checks': [], 'reaped_children': [], 'owned_roots': [],
              'normal_root_shutdowns': [],
              'source_inputs': bindings, 'binary_sha256': args.expected_binary,
              'resources_sha256': args.expected_resources,
              'snapshot_sha256': sha(args.browser.parent / 'snapshot.json'),
              'runtime_provenance_sha256': sha(args.browser.parent / 'provenance.json'),
              'home_isolated': True, 'network_isolated_no_egress': True,
              'personal_install_touched': False, 'forced_cleanup': False,
              'scope': 'Same candidate ELF repackaged as two honest local versions; real signed loopback delivery/restart'}
    write(output / 'source-binding.json', result)
    feed_port = 0
    if args.packages_from is not None:
        config = read(args.packages_from / 'package-1/Noventra.AppDir/updater/config.json')
        url = urllib.parse.urlsplit(config['base_url'])
        assert url.scheme == 'http' and url.hostname == '127.0.0.1' and url.path == '/'
        assert not url.username and not url.password and not url.query and not url.fragment
        assert type(url.port) is int and 0 < url.port < 65536
        feed_port = url.port
    delivery = ThreadingHTTPServer(('127.0.0.1', feed_port), local_delivery_server.DeliveryHandler)
    delivery.daemon_threads = True
    delivery.release_root = output / 'not-published-yet'
    thread = threading.Thread(target=delivery.serve_forever, daemon=True)
    thread.start()
    browser = None
    try:
        if args.packages_from is None:
            packages, feeds, public = prepare(args.browser.parent, output, f'http://127.0.0.1:{delivery.server_port}/')
        else:
            packages, feeds, public = reused_packages(args.packages_from, args.browser.parent, result)
            for appdir in packages.values():
                config = read(appdir / 'updater/config.json')
                assert config['base_url'] == f'http://127.0.0.1:{delivery.server_port}/'
            write(output / 'reuse-verification.json', {key: result[key] for key in ('reused_source_inputs', 'reused_packages')})
        delivery.release_root = feeds[1]
        result['package_versions'] = {str(v): {'metadata': read(p / 'noventra-package.json'),
              'install_manifest_sha256': sha(p / 'install-manifest.json'),
              'signed_manifest_sha256': sha(feeds[v] / 'manifest.json')} for v, p in packages.items()}
        app = data / 'noventra-browser/app'
        installation = subprocess.run([str(packages[1] / 'install.sh'), '--data-home', str(data)],
                                      capture_output=True, text=True, timeout=120, check=True)
        write(output / 'installation.json', {'returncode': installation.returncode,
                                            'response': json.loads(installation.stdout), 'stderr': installation.stderr})
        check(result, 'The real installer installs only in the owned data home', app.is_dir())
        profile = output / 'retained-profile'
        browser = Browser(app / 'AppRun', profile, output / 'browser')
        browser.start()
        before = browser.native(app)
        first_coordinator = coordinator_proof(browser, before, operation='launch')
        result['owned_roots'].append(serial(before))
        observe_processes(result)
        check(result, 'Managed AppRun owns the real sandboxed native root', before['ppid'] == browser.coordinator.pid)
        marker = profile / 'owned-update-marker'
        marker.write_text('Retain the owned browser profile through update and restart.\n')
        marker_hash = sha(marker)
        old_inode = (app / 'opt/noventra/noventra-browser').stat().st_ino
        with pages() as base:
            regular = target(browser, base + '/page?role=regular')
            browser.ev('document.cookie="owned_update=regular;Path=/;Max-Age=3600;SameSite=Lax";'
                       'localStorage.setItem("owned_update","regular");true', regular)
            spaces = browser.call('noventraSpaces', 'create', '', 'Update retained Work')
            work_id = next(s['id'] for s in spaces if s['name'] == 'Update retained Work')
            previous = {p['id'] for p in request(browser.port, '/json/list')}
            browser.call('noventraSpaces', 'switch', work_id, '')
            work = until(lambda: next((p for p in request(browser.port, '/json/list')
                         if p['type'] == 'page' and p['id'] not in previous), None))
            protocol(browser, 'Page.navigate', {'url': base + '/page?role=work'}, work)
            until(lambda: browser.ev('document.readyState==="complete"&&!!document.querySelector("#draft")', work))
            check(result, 'The separate regular Space begins without the default Space account data',
                  browser.ev('({cookie:document.cookie,local:localStorage.getItem("owned_update")})', work)
                  == {'cookie': '', 'local': None})
            browser.ev('document.cookie="owned_update=work;Path=/;Max-Age=3600;SameSite=Lax";'
                       'localStorage.setItem("owned_update","work");true', work)
            check(result, 'Writing the Work account preserves the separate default Space account',
                  browser.ev('({cookie:document.cookie,local:localStorage.getItem("owned_update")})', regular)
                  == {'cookie': 'owned_update=regular', 'local': 'regular'})
            browser.call('noventraSpaces', 'switch', '', '')
            about = target(browser, 'chrome://settings/help')
            until(lambda: status(browser, about)['status'] == 'updated', timeout=60)
            check(result, 'Current local signed release offers no update restart button', not status(browser, about)['buttonVisible'])
            delivery.release_root = feeds[2]
            browser.ev('chrome.send("refreshUpdateStatus");true', about)
            ready = until(lambda: (s if (s := status(browser, about))['buttonVisible'] else None), timeout=180)
            write(output / 'ready-ui.json', ready)
            check(result, 'Real native About offers an enabled Update and restart button after signature verification',
                  ready['status'] == 'nearly_updated' and not ready['buttonDisabled']
                  and ready['buttonText'].strip() == 'Update and restart')
            capture(browser, about, output / 'ready-button.png')
            button_geometry(browser, about, result, output)
            state_path = data / 'noventra-browser/updates/delivery/state.json'
            staged = read(state_path)['staged']
            check(result, 'Staging retains the old running native inode and old package label',
                  staged == 'r-2-' + sha(feeds[2] / 'manifest.json')
                  and (app / 'opt/noventra/noventra-browser').stat().st_ino == old_inode
                  and read(app / 'noventra-package.json')['version_label'] == 'update-button-native-1' and live(before))
            private_warning(browser, about, result, output)
            beforeunload(browser, regular, about, result, output, accept=False)
            check(result, 'Beforeunload cancellation retains the staged signed update and old app',
                  read(state_path)['staged'] == staged
                  and read(app / 'noventra-package.json')['version_label'] == 'update-button-native-1'
                  and (app / 'opt/noventra/noventra-browser').stat().st_ino == old_inode and live(before))
            old_lines = browser.active_lines
            observe_processes(result)
            beforeunload(browser, regular, about, result, output, accept=True)
            until(lambda: not live(before), timeout=45)
            check(result, 'The original managed coordinator exits normally after native restart',
                  browser.coordinator.wait(timeout=45) == 0)
            result['normal_root_shutdowns'].append({**first_coordinator, 'coordinator_exit_code': 0,
                'native_exit_code': 0, 'native_identity_gone': not live(before)})
            browser.attach(previous=old_lines, document_ready=False)
            after = browser.native(app)
            browser.restart_coordinator = coordinator_proof(browser, after, operation='relaunch', previous_native=before)
            browser.restart_pidfd = os.pidfd_open(browser.restart_coordinator['pid'])
            assert live(browser.restart_coordinator)
            browser.process = RootMonitor(after)
            observe_processes(result)
            result['owned_roots'].append(serial(after))
            browser.ready_settings()
            check(result, 'Restart opens a new owned native process through managed update code',
                  after['pid'] != before['pid'] and after['starttime'] >= before['starttime'])
            check(result, 'Restart atomically replaced the native file and applied exactly signed local version two',
                  (app / 'opt/noventra/noventra-browser').stat().st_ino != old_inode
                  and read(app / 'noventra-package.json')['version_label'] == 'update-button-native-2'
                  and read(app / 'updater/config.json')['sequence'] == 2
                  and sha(app / 'install-manifest.json') == sha(packages[2] / 'install-manifest.json')
                  and sha(app / 'opt/noventra/noventra-browser') == args.expected_binary)
            delivery_state = read(state_path)
            result['delivery_state_after_restart'] = delivery_state
            check(result, 'Restart retains the exact signed high-water state and clears the staged selection',
                  delivery_state['highest_sequence'] == 2
                  and delivery_state['highest_manifest'] == sha(feeds[2] / 'manifest.json')
                  and delivery_state['staged'] is None)
            installer_source = ROOT / 'noventra/package/installer.py'
            check(result, 'The installed verifier matches the trusted local packaging input',
                  sha(app / 'installer.py') == sha(installer_source))
            spec = importlib.util.spec_from_file_location('native_update_trusted_installer', installer_source)
            installer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(installer)
            installed_state = installer.state_for(app.parent, data)
            installer.verify_installation(app.parent, data, installed_state)
            check(result, 'The trusted installer verifies every installed app and desktop inventory member',
                  installed_state['manifest_sha256'] == sha(packages[2] / 'install-manifest.json'))
            tabs = until(lambda: (targets if all(any(p['url'] == base + '/page?role=' + name for p in targets)
                            for name in ('regular', 'work')) else None)
                         if (targets := request(browser.port, '/json/list')) else None, timeout=40)
            for name in ('regular', 'work'):
                browser.call('noventraSpaces', 'switch', work_id if name == 'work' else '', '')
                page = next(p for p in tabs if p['url'] == base + '/page?role=' + name)
                activate(browser, page)
                until(lambda: browser.ev('document.readyState==="complete"', page))
                witness = browser.ev('({cookie:document.cookie,local:localStorage.getItem("owned_update")})', page)
                check(result, name + ' Space restores its own tab and isolated persistent account data',
                      witness == {'cookie': 'owned_update=' + name, 'local': name})
            catalog = browser.call('noventraSpaces', 'list', '', '')
            check(result, 'The regular Space catalog and profile marker survive update and restart',
                  any(s['id'] == work_id and s['name'] == 'Update retained Work' for s in catalog)
                  and sha(marker) == marker_hash)
            new_about = target(browser, 'chrome://settings/help')
            current = until(lambda: (s if (s := status(browser, new_about))['status'] == 'updated' else None), timeout=60)
            capture(browser, new_about, output / 'updated-about.png')
            check(result, 'Updated About reports current and hides its restart action', not current['buttonVisible'])
        close_browser(browser, result)
        browser.log.close()
        browser = None
        check(result, 'Both identified native roots and their exact coordinators finish normally without forced cleanup',
              len(result['normal_root_shutdowns']) == 2
              and all(row['coordinator_exit_code'] == row['native_exit_code'] == 0
                      and row['native_identity_gone'] for row in result['normal_root_shutdowns'])
              and result['forced_cleanup'] is False)
        check(result, 'Source and fixture inputs stayed unchanged throughout qualification',
              bindings == {str(path): sha(path) for path in BINDINGS})
        if result.get('reused_source_inputs'):
            check(result, 'All explicitly reused signed preparation bytes remained unchanged',
                  all(sha(Path(path)) == expected for path, expected in result['reused_source_inputs'].items()))
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        result['traceback'] = traceback.format_exc()
        if browser is not None:
            observe_processes(result)
            try:
                for page in request(browser.port, '/json/list'):
                    if page['id'] not in getattr(browser, 'armed_beforeunload', set()):
                        continue
                    try:
                        activate(browser, page)
                        browser.ev('window.onbeforeunload=null;true', page)
                    except BaseException as disarm_error:
                        result.setdefault('failure_disarm_errors', []).append(repr(disarm_error))
            except BaseException as observation_error:
                result['failure_cleanup_observation_error'] = repr(observation_error)
            try:
                close_browser(browser, result)
            except BaseException as shutdown_error:
                result['failure_cleanup_error'] = repr(shutdown_error)
            if hasattr(browser, 'log'):
                browser.log.close()
        raise
    finally:
        delivery.shutdown(); delivery.server_close(); thread.join(timeout=3)
        result['remaining_owned_processes'] = [serial(row) for row in descendants()]
        write(output / 'result.json', result)



def button_geometry(browser, about, result, output):
    window_id = protocol(browser, 'Browser.getWindowForTarget', {'targetId': about['id']}, about)['windowId']
    bounds = protocol(browser, 'Browser.getWindowBounds', {'windowId': window_id}, about)['bounds']
    measure = '(()=>{' + DEEP + """const e=about.shadowRoot.querySelector('#relaunch');
      const r=e.getBoundingClientRect();return {width:innerWidth,height:innerHeight,
      left:r.left,top:r.top,right:r.right,bottom:r.bottom,
      scrollWidth:e.scrollWidth,clientWidth:e.clientWidth,text:e.innerText};})()"""
    desktop = browser.ev(measure, about)
    check(result, 'The desktop update button fits its content and visible viewport',
          desktop['left'] >= 0 and desktop['right'] <= desktop['width']
          and desktop['bottom'] <= desktop['height'] and desktop['scrollWidth'] <= desktop['clientWidth'])
    try:
        protocol(browser, 'Browser.setWindowBounds', {'windowId': window_id,
                 'bounds': {'width': 360, 'height': 760}}, about)
        def narrower():
            observed_bounds = protocol(browser, 'Browser.getWindowBounds', {'windowId': window_id}, about)['bounds']
            observed_viewport = browser.ev(measure, about)
            if observed_bounds['width'] < bounds['width'] and observed_viewport['width'] < desktop['width']:
                return {'bounds': observed_bounds, 'viewport': observed_viewport}
            return None
        observed = until(narrower, timeout=10)
        narrow = observed['viewport']
        result['button_geometry'] = {'desktop': {'bounds': bounds, 'viewport': desktop},
                                    'narrow_native_request_360': observed}
        check(result, 'The requested narrow window actually shrinks both native bounds and page viewport',
              observed['bounds']['width'] < bounds['width'] and narrow['width'] < desktop['width'])
        capture(browser, about, output / 'ready-button-narrow.png')
        check(result, 'The narrow native window keeps the entire Update and restart action readable',
              narrow['left'] >= 0 and narrow['right'] <= narrow['width']
              and narrow['top'] >= 0 and narrow['bottom'] <= narrow['height']
              and narrow['scrollWidth'] <= narrow['clientWidth'])
    finally:
        protocol(browser, 'Browser.setWindowBounds', {'windowId': window_id,
                 'bounds': {key: bounds[key] for key in ('left', 'top', 'width', 'height')}}, about)
        time.sleep(.3)


def private_warning(browser, about, result, output):
    pointer = Input(browser.platform)
    try:
        activate(browser, about)
        result['private_window_manager_present'] = private_window_manager(pointer)
        check(result, 'Private restart warning runs under the owned real EWMH window manager',
              result['private_window_manager_present'])
        physical = SimpleNamespace(process=SimpleNamespace(pid=browser.native(browser.browser.parent)['pid']))
        original_windows = owned_windows(physical, pointer)
        check(result, 'The private warning starts with exactly one real OS browser window', len(original_windows) == 1)
        previous = {p['id'] for p in request(browser.port, '/json/list')}
        native_key(pointer, 'n', ('Control_L', 'Shift_L'))
        private = until(lambda: next((p for p in request(browser.port, '/json/list')
                        if p['type'] == 'page' and p['id'] not in previous), None))
        old_context = protocol(browser, 'Target.getTargetInfo', {'targetId': about['id']}, about)['targetInfo']['browserContextId']
        new_context = protocol(browser, 'Target.getTargetInfo', {'targetId': private['id']}, private)['targetInfo']['browserContextId']
        check(result, 'The warning fixture creates a genuine isolated Incognito Space', old_context != new_context)
        check(result, 'The Incognito Space shares the same physical browser window',
              owned_windows(physical, pointer) == original_windows)
        browser.call('noventraSpaces', 'switch', '', '')
        activate(browser, about)
        click(browser, about, 'about.shadowRoot.querySelector("#relaunch")')
        warning = until(lambda: dialog_node(browser, about))
        write(output / 'private-restart-warning.json', warning)
        check(result, 'Update and restart warns that the real Incognito Space will close',
              'incognito' in (warning['description'] or '').lower())
        capture(browser, about, output / 'private-restart-warning.png')
        click(browser, about, 'about.shadowRoot.querySelector("relaunch-confirmation-dialog").shadowRoot.querySelector("#cancel")')
        until(lambda: dialog_node(browser, about) is None)
        check(result, 'Canceling the private warning keeps the Incognito tab and current app',
              any(p['id'] == private['id'] for p in request(browser.port, '/json/list'))
              and status(browser, about)['buttonVisible'])
        protocol(browser, 'Target.closeTarget', {'targetId': private['id']}, about)
        until(lambda: all(p['id'] != private['id'] for p in request(browser.port, '/json/list')))
    finally:
        pointer.close()


def beforeunload(browser, page, about, result, output, *, accept):
    label = 'accept' if accept else 'cancel'
    folder = output / ('beforeunload-' + label)
    folder.mkdir()
    activate(browser, page)
    protocol(browser, 'Input.dispatchMouseEvent', {'type': 'mousePressed', 'x': 100, 'y': 160,
              'button': 'left', 'clickCount': 1}, page)
    protocol(browser, 'Input.dispatchMouseEvent', {'type': 'mouseReleased', 'x': 100, 'y': 160,
              'button': 'left', 'clickCount': 1}, page)
    browser.ev('document.querySelector("#draft").value="owned unsaved draft";'
               'window.onbeforeunload=e=>{e.preventDefault();e.returnValue=""};true', page)
    check(result, label + ': the page has trusted user activation before its beforeunload handler',
          browser.ev('navigator.userActivation.hasBeenActive', page))
    browser.armed_beforeunload = getattr(browser, 'armed_beforeunload', set()) | {page['id']}
    retained = browser.ev('({token:window.ownedToken,timeOrigin:performance.timeOrigin,draft:document.querySelector("#draft").value})', page)
    with (folder / 'dialog.log').open('x') as log:
        listener = subprocess.Popen(['/usr/bin/node', str(HERE / 'dialog.mjs'),
                                    page['webSocketDebuggerUrl'], str(folder)], stdout=log, stderr=log)
        try:
            until(lambda: (folder / 'ready.json').exists())
            click(browser, about, 'about.shadowRoot.querySelector("#relaunch")')
            until(lambda: (folder / 'opened.json').exists())
            opening = read(folder / 'opened.json')
            check(result, label + ': Update and restart opens a real native beforeunload dialog', opening['type'] == 'beforeunload')
            pointer = Input(browser.platform)
            try:
                pointer.capture(folder / 'native-dialog.png').close()
            finally:
                pointer.close()
            write(folder / 'decision.json', {'accept': accept})
            assert listener.wait(timeout=15) == 0
            if not accept:
                check(result, 'Rejecting the real dialog preserves the unsaved document and draft',
                      read(folder / 'handled.json') == {'acknowledged': True, 'accept': False}
                      and browser.ev('({token:window.ownedToken,timeOrigin:performance.timeOrigin,draft:document.querySelector("#draft").value})', page) == retained)
        finally:
            if listener.poll() is None:
                write(folder / 'decision.json', {'accept': False})
                write(folder / 'stop.json', {'fixture_failed': True})
                listener.wait(timeout=15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', '--browser', dest='browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--packages-from', type=Path, help='Reuse and fully verify this owned prepared fixture; fresh HOME/data/profile')
    parser.add_argument('--expected-binary', default='')
    parser.add_argument('--expected-resources', default='')
    parser.add_argument('--case', default='update-restart')
    parser.add_argument('--inside-owned-home', action='store_true')
    parser.add_argument('--rtc-policy-diagnostic', action='store_true')
    args = parser.parse_args()
    fixture(args)


if __name__ == '__main__':
    main()
