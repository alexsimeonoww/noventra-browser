"""Two independent profiles exercising native encrypted workspace sync.

Run only after the parent supplies a compiled runtime, from this checkout:
  work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    work/ram-investigation/with-xvfb.py \
    python3 -B work/browser-gaps-2026-10-25/sync-keyring/with-private-keyring.py \
    --output work/browser-gaps-2026-10-25/sync-keyring/candidate-r1 -- \
    work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    tests/noventra/workspace_sync_smoke.py --browser /absolute/runtime \
    --output work/browser-gaps-2026-10-25/sync-test/candidate-r1

Create, join, recovery acknowledgement, Sync now, conflict choices and disconnect
use real XTest input on an authenticated, private Xvfb. Metadata/layout fixture
edits and credential/storage negative probes use the native WebUI API. The
production opaque HTTP/SQLite server runs on two owned loopback origins sharing
one store; the test-only handler counts requests and can take either client
offline. It never decrypts or changes the encrypted object protocol. Corruption
and rollback replace only the known head row in the owned store, preserving the
original ciphertext and database snapshots. No extension or backup API is used.

Work: 900s; evidence/owned cleanup: 60s. Credentials travel to CDP over stdin, not
argv, and are redacted from evidence. Profiles and encrypted SQLite snapshots
remain in a private output directory for review. Positive profiles require the
owned private libsecret keyring wrapper; Basic storage must reject setup. Two
profiles sharing a private test keyring do not qualify separate devices. Live
draft retention is measured before restart; restart checks URLs/cookies/storage,
mapping and sync configuration, not restoration of an in-memory document token.
"""

import argparse
import base64
from contextlib import closing, contextmanager
from copy import deepcopy
import ctypes as C
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import select
import signal
import sqlite3
import subprocess
import sys
import threading
import time
import traceback
import uuid

import compact_chrome_smoke as n
from interface_polish_smoke import DEEP
from layout_test_support import LayoutBrowser
from workspace_tools_smoke import native_text

sys.path.insert(0, str(n.ROOT / 'tools/noventra'))
import sync_store_server as store_server


OWNED_ROOT = n.ROOT / 'work/browser-gaps-2026-10-25/sync-test'
A_WORK = '00000000-0000-4000-8000-000000000041'
A_ARCHIVE = '00000000-0000-4000-8000-000000000042'
B_LOCAL = '00000000-0000-4000-8000-000000000043'
FLAGS = (*n.FLAGS, '--no-proxy-server',
         '--disable-background-networking')
ORDER = (*n.TOP_ORDER, 'spaces')
SECRET_SELECTORS = ('#noventraSyncToken', '#noventraSyncJoinKey',
                    '#noventraSyncRecoveryKey')
STATUS_FIELDS = {'configured', 'syncing', 'conflict', 'dirty', 'state', 'error',
                 'last_success', 'generation', 'origin', 'vault_id', 'scope',
                 'local_only_spaces'}
WITNESS = '''({token:window.syncDocumentToken,timeOrigin:performance.timeOrigin,
  url:location.href,draft:document.querySelector('#draft').value,
  password:document.querySelector('#password').value,cookie:document.cookie,
  storage:localStorage.getItem('syncLocalWitness')})'''
PAGE = '''<!doctype html><meta charset="utf-8"><title>Owned sync document</title>
<style>body{margin:24px;background:#edf2fa;font:16px sans-serif}
input{display:block;margin:14px 0;padding:8px;width:270px}</style>
<h1>Local account document</h1><label>Unsaved draft<input id="draft"></label>
<label>Local password field<input id="password" type="password"></label>
<script>window.syncDocumentToken=crypto.randomUUID();</script>'''


def digest(value):
    if not isinstance(value, bytes):
        value = json.dumps(value, sort_keys=True, ensure_ascii=False,
                           default=lambda data: {'bytes_sha256': digest(data)}
                           if isinstance(data, bytes) else None).encode()
    return hashlib.sha256(value).hexdigest()


def file_digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


class Redactor:
    def __init__(self):
        self.values = set()
        self.byte_values = set()

    def add(self, value):
        assert isinstance(value, str) and value
        self.values.add(value)
        self.values.add(base64.b64encode(value.encode()).decode())
        self.values.add(base64.urlsafe_b64encode(value.encode()).decode().rstrip('='))
        self.byte_values.add(value.encode())
        if re.fullmatch(r'[A-Za-z0-9_-]{32,}', value):
            try:
                raw = base64.urlsafe_b64decode(value + '=' * (-len(value) % 4))
                self.byte_values.add(raw)
                self.values.add(raw.hex())
            except ValueError:
                pass
        return value

    def text(self, value):
        for secret in sorted(self.values, key=len, reverse=True):
            value = value.replace(secret, '[REDACTED]')
        return value

    def safe(self, value):
        if isinstance(value, dict):
            return {key: '[REDACTED]' if key in ('token', 'recovery_key', 'password')
                    else self.safe(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [self.safe(item) for item in value]
        return self.text(value) if isinstance(value, str) else value

    def contains(self, value):
        encoded = json.dumps(value, ensure_ascii=False)
        return any(secret in encoded for secret in self.values)


def write_json(path, value, redactor):
    path.write_text(json.dumps(redactor.safe(value), ensure_ascii=False,
                               indent=2, sort_keys=True) + '\n')


def check(result, phase, label, actual, expected=True):
    entry = {'phase': phase, 'check': label, 'passed': actual == expected,
             'actual': actual, 'expected': expected}
    result['checks'].append(entry)
    if not entry['passed']:
        raise AssertionError(phase + ': ' + label)


def wait(predicate, timeout=40, description='condition'):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.25)
    raise TimeoutError('Timed out: ' + description)


class Budget:
    def __init__(self):
        self.started = time.monotonic()
        signal.signal(signal.SIGALRM, self.expired)
        signal.setitimer(signal.ITIMER_REAL, 900)

    @staticmethod
    def expired(*_):
        raise TimeoutError('Workspace sync fixture exhausted its bounded budget')

    def cleanup(self):
        signal.setitimer(signal.ITIMER_REAL, 60)

    def finish(self):
        signal.setitimer(signal.ITIMER_REAL, 0)
        return round(time.monotonic() - self.started, 3)


def process_identity(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        if fields[0] in ('Z', 'X'):
            return None  # A zombie has stopped executing and cannot be killed.
        return {'pid': pid, 'session': int(fields[3]), 'starttime': int(fields[19])}
    except (OSError, ValueError, IndexError):
        return None


def attest_private_keyring():
    assert os.environ.get('NOVENTRA_TEST_KEYRING') == '1', 'Owned private keyring wrapper required'
    pid = int(os.environ['NOVENTRA_TEST_KEYRING_PID'])
    identity = process_identity(pid)
    assert identity and Path(f'/proc/{pid}').stat().st_uid == os.getuid(), 'Owned Secret Service is not live'
    address = os.environ.get('DBUS_SESSION_BUS_ADDRESS', '')
    assert address.startswith('unix:path=/tmp/nv-sync-keyring-'), 'Refusing a host or unexpected session bus'
    socket = Path(address.removeprefix('unix:path='))
    assert socket.name == 'bus' and socket.is_socket()
    assert socket.parent.stat().st_uid == os.getuid() and socket.parent.stat().st_mode & 0o077 == 0
    assert os.environ.get('DBUS_SYSTEM_BUS_ADDRESS') == 'unix:path=' + str(socket.parent / 'no-system-bus')
    assert not (socket.parent / 'no-system-bus').exists()
    argv = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
    assert Path(os.fsdecode(argv[0])).name == 'gnome-keyring-daemon'
    assert ('--control-directory=' + str(socket.parent / 'control')).encode() in argv
    data = Path(os.environ['NOVENTRA_TEST_KEYRING_DATA']).resolve(strict=True)
    assert data.is_relative_to((n.ROOT / 'work/browser-gaps-2026-10-25/sync-keyring').resolve())
    assert data.stat().st_uid == os.getuid() and data.stat().st_mode & 0o077 == 0
    owner = subprocess.check_output(['gdbus', 'call', '--address', address,
        '--dest', 'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
        '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID', 'org.freedesktop.secrets'],
        text=True, stderr=subprocess.DEVNULL, timeout=3)
    match = re.search(r'uint32 (\d+)', owner)
    assert match and int(match[1]) == pid, 'Secret Service owner is not the attested child'
    return {'service_identity': identity, 'data': str(data), 'address': address,
            'host_bus_used': False, 'host_keyring_used': False}


class OwnedBrowser(LayoutBrowser):
    """Reuse the frozen launcher, with stdin CDP and owned-session cleanup."""
    def __init__(self, browser, profile, output, redactor, label, restore=False,
                 password_store='gnome-libsecret'):
        super().__init__(browser, profile, output,
                         (*FLAGS, '--password-store=' + password_store,
                          *(('--restore-last-session',) if restore else ())))
        self.redactor, self.label = redactor, label
        self.calls = []
        self.identity = None
        self.tree = None
        self.closed = False
        self.members = {}
        self.cdp_timeout = 24

    def __enter__(self):
        super().__enter__()
        self.identity = process_identity(self.process.pid)
        assert self.identity and self.identity['session'] == self.process.pid
        self.tree = n.NativeUITree(self)
        return self

    def ev(self, js, target=None):
        completed = subprocess.run(
            ['node', str(n.ROOT / 'tests/noventra/cdp_eval.mjs'),
             (target or self.settings)['webSocketDebuggerUrl']], input=js,
            capture_output=True, text=True, timeout=self.cdp_timeout)
        if completed.returncode:
            raise RuntimeError('Owned CDP evaluation failed: ' +
                               self.redactor.text(completed.stderr[-1800:]))
        try:
            return json.loads(completed.stdout)
        except json.JSONDecodeError:
            raise RuntimeError('Owned CDP returned invalid JSON') from None

    def rpc(self, message, *args):
        self.calls.append({'message': message, 'action': args[0] if args and
                           isinstance(args[0], str) else '', 'at': time.monotonic()})
        return self.ev('''(async()=>{
          const {sendWithPromise}=await import('chrome://resources/js/cr.js');
          return await Promise.race([
            sendWithPromise(...ARGS).then(value=>({status:'resolved',value}),
              error=>({status:'rejected',error:String(error)})),
            new Promise(r=>setTimeout(()=>r({status:'timeout'}),18000))]);
        })()'''.replace('ARGS', json.dumps([message, *args])))

    def call(self, message, *args):
        reply = self.rpc(message, *args)
        if reply['status'] != 'resolved':
            raise RuntimeError('Native callback did not resolve: ' +
                               self.redactor.text(json.dumps(reply)))
        return reply['value']

    def protocol(self, method, params=None, target=None):
        # This helper is only for non-secret browser commands and captures.
        command = [{'method': method, 'params': params or {}, 'returnValue': True}]
        completed = subprocess.run(
            ['node', str(n.ROOT / 'tests/noventra/layout_editor_input.mjs'),
             (target or self.settings)['webSocketDebuggerUrl'], json.dumps(command)],
            capture_output=True, text=True, timeout=self.cdp_timeout)
        if completed.returncode:
            raise RuntimeError('Owned CDP command failed: ' +
                               self.redactor.text(completed.stderr[-1000:]))
        return json.loads(completed.stdout)

    def normal_quit(self):
        self.remember_members()
        endpoint = n.request(self.port, '/json/version')['webSocketDebuggerUrl']
        script = '''const s=new WebSocket(process.argv[1]);
          s.onopen=()=>s.send(JSON.stringify({id:1,method:'Browser.close'}));
          s.onclose=()=>process.exit(0);setTimeout(()=>process.exit(1),10000);'''
        subprocess.run(['node', '-e', script, endpoint], check=True,
                       capture_output=True, timeout=12)
        code = self.process.wait(timeout=12)
        assert code == 0, 'Normal browser quit failed'
        return code

    def remember_members(self):
        if not self.process:
            return
        root_identity = process_identity(self.process.pid)
        if self.identity is None and self.process.poll() is None:
            self.identity = root_identity
        session_owned = root_identity is None or root_identity == self.identity
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            identity = process_identity(int(entry.name))
            if not identity:
                continue
            owned = session_owned and identity['session'] == self.process.pid
            if not owned:
                # Crashpad may detach from the browser session. Only its exact
                # disposable config path authorizes cleanup outside the group.
                try:
                    argv = (entry / 'cmdline').read_bytes().split(b'\0')
                    prefix = ('--database=' + str(self.profile / 'test-config') + '/').encode()
                    owned = any(arg.startswith(prefix) for arg in argv)
                except (OSError, ProcessLookupError):
                    continue
            if owned:
                self.members[(identity['pid'], identity['starttime'])] = identity

    def __exit__(self, *_):
        if self.closed:
            return
        self.remember_members()
        if self.process and self.process.poll() is None:
            current = process_identity(self.process.pid)
            if self.identity is not None and current != self.identity:
                raise RuntimeError('Owned process identity changed; refusing cleanup')
            if current and current['session'] == self.process.pid:
                os.killpg(self.process.pid, signal.SIGTERM)
                try:
                    self.process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.process.wait(timeout=3)
        # Handle detached Crashpad or children surviving a normal Browser.close.
        survivors = [i for i in self.members.values() if process_identity(i['pid']) == i]
        for identity in survivors:
            try:
                os.kill(identity['pid'], signal.SIGTERM)
            except ProcessLookupError:
                pass
        if survivors:
            until = time.monotonic() + 3
            while time.monotonic() < until and any(process_identity(i['pid']) == i for i in survivors):
                time.sleep(.1)
            for identity in survivors:
                if process_identity(identity['pid']) == identity:
                    try:
                        os.kill(identity['pid'], signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        if hasattr(self, 'log'):
            self.log.close()
        log = self.output / 'browser.log'
        if log.exists():
            raw = log.read_text(errors='replace')
            log.write_text(self.redactor.text(raw))
        self.closed = True


class OwnedSeat:
    """Focus only a window belonging to the selected owned browser session."""
    def __init__(self):
        self.input = n.Input('x11')
        p = self.input.pointer
        self.x, self.d = p.x, p.d
        self.x.XDefaultRootWindow.argtypes = [C.c_void_p]
        self.x.XDefaultRootWindow.restype = C.c_ulong
        self.x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
            C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
        self.x.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
        self.x.XInternAtom.restype = C.c_ulong
        self.x.XGetWindowProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong,
            C.c_long, C.c_long, C.c_int, C.c_ulong, C.POINTER(C.c_ulong),
            C.POINTER(C.c_int), C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
            C.POINTER(C.POINTER(C.c_ubyte))]
        self.x.XFree.argtypes = [C.c_void_p]
        self.x.XRaiseWindow.argtypes = [C.c_void_p, C.c_ulong]
        self.x.XSetInputFocus.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong]
        self.x.XGetInputFocus.argtypes = [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)]
        self.x.XGetGeometry.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
            C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_uint),
            C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
        self.pid_atom = self.x.XInternAtom(self.d, b'_NET_WM_PID', 0)

    def children(self, window):
        root, parent, count = C.c_ulong(), C.c_ulong(), C.c_uint()
        children = C.POINTER(C.c_ulong)()
        assert self.x.XQueryTree(self.d, window, C.byref(root), C.byref(parent),
                                C.byref(children), C.byref(count))
        try:
            return [children[i] for i in range(count.value)], parent.value
        finally:
            if children:
                self.x.XFree(children)

    def pid(self, window):
        kind, count, remaining = C.c_ulong(), C.c_ulong(), C.c_ulong()
        fmt, data = C.c_int(), C.POINTER(C.c_ubyte)()
        error = self.x.XGetWindowProperty(self.d, window, self.pid_atom, 0, 1,
            0, 0, C.byref(kind), C.byref(fmt), C.byref(count), C.byref(remaining), C.byref(data))
        try:
            return C.cast(data, C.POINTER(C.c_ulong))[0] if not error and count.value and fmt.value == 32 else 0
        finally:
            if data:
                self.x.XFree(data)

    def window(self, browser):
        children, _ = self.children(self.x.XDefaultRootWindow(self.d))
        candidates = []
        for window in children:
            pid = self.pid(window)
            identity = process_identity(pid) if pid else None
            root, left, top = C.c_ulong(), C.c_int(), C.c_int()
            width, height, border, depth = C.c_uint(), C.c_uint(), C.c_uint(), C.c_uint()
            large = self.x.XGetGeometry(self.d, window, C.byref(root), C.byref(left),
                C.byref(top), C.byref(width), C.byref(height), C.byref(border), C.byref(depth)) \
                and width.value >= 500 and height.value >= 400
            if identity and identity['session'] == browser.process.pid and large:
                candidates.append(window)
        assert len(candidates) == 1, 'Expected one owned native browser window'
        return candidates[0]

    def focus(self, browser, target=None):
        target = target or browser.settings
        n.request(browser.port, '/json/activate/' + target['id'])
        window = self.raise_window(browser)
        wait(lambda: browser.ev('document.visibilityState==="visible"', target),
             timeout=5, description='owned target visible')
        return window

    def raise_window(self, browser):
        window = self.window(browser)
        self.x.XRaiseWindow(self.d, window)
        self.x.XSetInputFocus(self.d, window, 2, 0)
        self.x.XFlush(self.d)
        return window

    def focused(self, browser):
        focus, revert = C.c_ulong(), C.c_int()
        self.x.XGetInputFocus(self.d, C.byref(focus), C.byref(revert))
        expected, current = self.window(browser), focus.value
        for _ in range(12):
            if current == expected:
                return True
            if current <= 1 or current == self.x.XDefaultRootWindow(self.d):
                return False
            _, current = self.children(current)
        return False

    def close(self):
        self.input.close()


class Gate:
    def __init__(self, label):
        self.label, self.paused, self.active = label, False, 0
        self.lock = threading.Condition()
        self.events = []

    def pause(self, paused=True):
        with self.lock:
            self.paused = paused
            if paused and not self.lock.wait_for(lambda: not self.active, timeout=5):
                raise TimeoutError('Owned server requests did not drain')

    def count(self, method=None):
        with self.lock:
            return sum(method is None or e['method'] == method for e in self.events)


class FaultHandler(store_server.SyncHandler):
    """Only a network gate and metadata ledger; production parsing is reused."""
    def respond(self, status, data=b'', revision=None):
        gate = self.server.gate
        with gate.lock:
            gate.events.append({'method': self.command, 'status': status,
                                'bytes': len(data), 'at': time.monotonic()})
        super().respond(status, data, revision)

    def dispatch(self, method):
        gate = self.server.gate
        with gate.lock:
            paused = gate.paused
            if not paused:
                gate.active += 1
        if paused:
            self.respond(503)
            return
        try:
            method(self)
        finally:
            with gate.lock:
                gate.active -= 1
                gate.lock.notify_all()

    def do_GET(self):
        self.dispatch(store_server.SyncHandler.do_GET)

    def do_PUT(self):
        self.dispatch(store_server.SyncHandler.do_PUT)


class OpaqueServers:
    def __init__(self, directory, token):
        self.directory = directory
        self.servers, self.workers, self.gates = {}, {}, {}
        try:
            for label in ('A', 'B'):
                server = store_server.make_server(directory, token)
                server.RequestHandlerClass = FaultHandler
                server.gate = Gate(label)
                self.servers[label], self.gates[label] = server, server.gate
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                self.workers[label] = thread
        except BaseException:
            self.close()
            raise

    def origin(self, label):
        return f'http://127.0.0.1:{self.servers[label].server_port}'

    @property
    def store(self):
        return self.servers['A'].store

    def rows(self):
        with closing(self.store.connect()) as db:
            return db.execute('SELECT vault,object_id,revision,data FROM objects '
                              'ORDER BY vault,object_id').fetchall()

    def snapshot(self, path):
        with closing(self.store.connect()) as db, closing(sqlite3.connect(path)) as copy:
            db.backup(copy)

    def head(self, vault):
        # Test-only identification from sync_vault_store.cc. The server remains
        # opaque; WorkspaceSyncObjectURL base64url-encodes this UTF-8 object ID.
        key = f'helium/v1/{vault}/head'
        value = self.store.read(vault, key)
        assert value is not None, 'Native encrypted head missing'
        return {'vault': vault, 'object_id': key, 'revision': value[0], 'data': value[1]}

    def replace_head(self, head):
        assert all(g.paused and not g.active for g in self.gates.values())
        with closing(self.store.connect()) as db:
            db.execute('BEGIN IMMEDIATE')
            cursor = db.execute('UPDATE objects SET revision=?,data=? '
                                'WHERE vault=? AND object_id=?',
                                (head['revision'], head['data'], head['vault'], head['object_id']))
            assert cursor.rowcount == 1
            db.commit()

    def pause_all(self, paused=True):
        for gate in self.gates.values():
            gate.pause(paused)

    def close(self):
        for label, server in self.servers.items():
            server.shutdown()
            server.server_close()
            if label in self.workers:
                self.workers[label].join(timeout=2)


@contextmanager
def local_documents():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            data = PAGE.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}'
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)


def ui(browser, body):
    return browser.ev('(async()=>{' + DEEP + '''
      const page=deep(document,'settings-appearance-page');
      if(!page)throw Error('Appearance page missing');const root=page.shadowRoot;
    ''' + body + '})()')


def click_ui(browser, seat, selector, activate=True):
    if activate:
        seat.focus(browser)
    else:
        assert seat.focused(browser), 'Measured section navigation must keep native focus'
    wait(lambda: ui(browser, 'const el=root.querySelector(SELECTOR);return el&&!el.disabled;'
                    .replace('SELECTOR', json.dumps(selector))),
         timeout=8, description='native sync control enabled')
    point = ui(browser, '''const el=root.querySelector(SELECTOR);
      if(!el||el.disabled)throw Error('Unavailable sync control');
      el.scrollIntoView({block:'center',behavior:'instant'});
      const r=el.getBoundingClientRect();if(!r.width||!r.height)throw Error('Hidden sync control');
      return {x:r.x+r.width/2,y:r.y+r.height/2};'''.replace('SELECTOR', json.dumps(selector)))
    content = browser.tree.rect(n.one(browser.tree.read(), r'class_name=MultiContentsView '))
    seat.input.pointer.click(content['x'] + point['x'], content['y'] + point['y'])


def show_sync(browser, seat):
    seat.focus(browser)
    if ui(browser, "return root.querySelector('#customizeSpaces').hidden;"):
        click_ui(browser, seat, '[data-section="customizeSpaces"]')
    wait(lambda: ui(browser, "return !!root.querySelector('#noventraWorkspaceSync');"),
         timeout=10, description='native sync section present')
    if not ui(browser, "return root.querySelector('#noventraWorkspaceSync').open;"):
        click_ui(browser, seat, '#noventraWorkspaceSync > summary')
    wait(lambda: ui(browser, 'return page.noventraSyncAvailable_;'), timeout=10,
         description='first native sync status after opening details')


def fill_ui(browser, seat, selector, value):
    click_ui(browser, seat, selector)
    n.native_key(seat.input, 'a', ('Control_L',))
    native_text(seat.input, value)
    assert ui(browser, 'return root.querySelector(SELECTOR).value===VALUE;'
              .replace('SELECTOR', json.dumps(selector)).replace('VALUE', json.dumps(value))), \
        'Native sync field entry differs'


def focus_check(browser, seat, result, phase, expected):
    wait(lambda: ui(browser, 'return !page.noventraSyncBusy_ && root.activeElement?.id===EXPECTED;'
                    .replace('EXPECTED', json.dumps(expected))),
         timeout=8, description='sync operation keyboard focus')
    actual = ui(browser, 'return root.activeElement?.id;')
    check(result, phase, 'keyboard focus after operation', actual, expected)
    check(result, phase, 'focus remains in the selected owned native window', seat.focused(browser))


def spaces(browser, action='list', space='', value=''):
    return browser.call('noventraSpaces', action, space, value)


def model(browser):
    return {'spaces': [{k: s[k] for k in ('id', 'name', 'archived')} for s in spaces(browser)],
            'layout': browser.state()['layout'],
            'routes': browser.call('noventraWorkspaceTools', 'list', {})['routes']}


def portable(layout):
    value = deepcopy(layout)
    value['revision'] = 0
    value['items'].sort(key=lambda i: (i['edge'], i['order'], i['id']))
    return value


def status(browser):
    value = browser.call('noventraWorkspaceSync', 'status', {})
    assert isinstance(value, dict) and STATUS_FIELDS == value.keys(), 'Sync status contract differs'
    assert not browser.redactor.contains(value), 'Secret appeared in status'
    assert not ({'token', 'recovery_key', 'key', 'credentials'} & value.keys()), 'Status exposes secret field'
    assert isinstance(value['generation'], int) and not isinstance(value['generation'], bool)
    assert all(isinstance(value[key], bool) for key in ('configured', 'syncing', 'conflict', 'dirty'))
    assert value['scope'] == 'workspace-settings-v1', 'Unexpected sync scope'
    if value['last_success']:
        assert re.match(r'^\d{4}-\d{2}-\d{2}T', value['last_success']), 'Expected ISO last_success'
    return value


def healthy(browser):
    value = status(browser)
    return value if value['configured'] and not value['syncing'] and not value['dirty'] and not value['error'] \
        and not value['conflict'] and value['last_success'] else None


def targets(browser):
    return [t for t in n.request(browser.port, '/json/list') if t.get('type') == 'page']


def native_open(browser, seat, url, space):
    seat.focus(browser)
    spaces(browser, 'switch', space)
    # Do not reactivate the Settings tab here: it belongs to Personal and would
    # undo the account switch before the measured native new-tab navigation.
    seat.raise_window(browser)
    before = {t['id'] for t in targets(browser)}
    n.native_key(seat.input, 'l', ('Control_L',))
    native_text(seat.input, url)
    n.native_key(seat.input, 'Return', ('Alt_L',))
    target = wait(lambda: next((t for t in targets(browser) if t['id'] not in before
                               and t.get('url') == url), None),
                  timeout=10, description='native new-tab document')
    wait(lambda: browser.ev('document.readyState==="complete" && !!window.syncDocumentToken', target),
         timeout=6, description='owned document loaded')
    return target


def seed_document(browser, seat, base, label, space, redactor, sentinels):
    target = native_open(browser, seat, base + '/' + label, space)
    account = label + '-account-' + secrets.token_hex(8)
    password = redactor.add(label + '-password-' + secrets.token_hex(16))
    draft = label + '-unsaved-' + secrets.token_hex(8)
    sentinels.extend([account, password, draft])
    browser.ev('''(()=>{document.cookie='syncLocalWitness='+ACCOUNT+'; Path=/; Max-Age=3600; SameSite=Lax';
      localStorage.setItem('syncLocalWitness',ACCOUNT);
      document.querySelector('#password').value=PASSWORD;return true;})()'''
      .replace('ACCOUNT', json.dumps(account)).replace('PASSWORD', json.dumps(password)), target)
    seat.focus(browser, target)
    point = browser.ev('(()=>{const r=document.querySelector("#draft").getBoundingClientRect();'
                       'return {x:r.x+r.width/2,y:r.y+r.height/2};})()', target)
    content = browser.tree.rect(n.one(browser.tree.read(), r'class_name=MultiContentsView '))
    seat.input.pointer.click(content['x'] + point['x'], content['y'] + point['y'])
    native_text(seat.input, draft)
    before = browser.ev(WITNESS, target)
    assert before['draft'] == draft and before['storage'] == account, 'Witness preparation failed'
    return {'target': target, 'before': before, 'space': space}


def retained(browser, witnesses, result, phase):
    for label, witness in witnesses.items():
        actual = browser.ev(WITNESS, witness['target'])
        check(result, phase, browser.label + ' retains document/draft/account ' + label,
              digest(actual), digest(witness['before']))


def hidden_settings(browser, seat, witness):
    if ui(browser, "return root.querySelector('#noventraWorkspaceSync').open;"):
        click_ui(browser, seat, '#noventraWorkspaceSync > summary')
    seat.focus(browser, witness['target'])
    assert browser.ev('document.visibilityState==="hidden"'), 'Settings must be hidden for automatic checks'


def mirror(a, b, mapping):
    left = [{**s, 'id': mapping[s['id']]} for s in a['spaces'] if s['id'] in mapping]
    right = [s for s in b['spaces'] if s['id'] in mapping.values()]
    return left == right and portable(a['layout']) == portable(b['layout'])


def await_mirror(a, b, mapping, result, phase):
    started = time.monotonic()
    def matched():
        first, second = model(a), model(b)
        return (first, second) if mirror(first, second, mapping) and healthy(a) and healthy(b) else None
    values = wait(matched, timeout=60, description=phase)
    check(result, phase, 'names/order/archive and saved dock layout converge', True)
    for label, value, expected in (('A', values[0], set(mapping)),
                                    ('B', values[1], set(mapping.values()) | {B_LOCAL})):
        observed = [s['id'] for s in value['spaces']]
        check(result, phase, label + ' has exactly the expected stable partitions',
              sorted(observed), sorted(expected))
    result.setdefault('convergence_seconds', {})[phase] = round(time.monotonic() - started, 3)
    return values


def save_head(head, output, label, redactor):
    (output / (label + '.bin')).write_bytes(head['data'])
    write_json(output / (label + '.json'), {k: v for k, v in head.items() if k != 'data'} |
               {'sha256': digest(head['data']), 'bytes': len(head['data'])}, redactor)


def capture(browser, output, label):
    """Mask password controls before pixels; redact native text before writing."""
    for value in ui(browser, 'return SELECTORS.map(s=>root.querySelector(s)?.value||"");'
                    .replace('SELECTORS', json.dumps(SECRET_SELECTORS))):
        if value:
            browser.redactor.add(value)
    ui(browser, '''for(const selector of SELECTORS){const e=root.querySelector(selector);
      if(e){e.dataset.syncOldVisibility=e.style.visibility;e.style.visibility='hidden';}}
      return true;'''.replace('SELECTORS', json.dumps(SECRET_SELECTORS)))
    try:
        shot = browser.protocol('Page.captureScreenshot', {'format': 'png'})
        (output / (label + '.png')).write_bytes(base64.b64decode(shot['data']))
        native = browser.tree.read().text
        (output / (label + '.native.txt')).write_text(browser.redactor.text(native))
    finally:
        ui(browser, '''for(const selector of SELECTORS){const e=root.querySelector(selector);
          if(e){e.style.visibility=e.dataset.syncOldVisibility||'';delete e.dataset.syncOldVisibility;}}
          return true;'''.replace('SELECTORS', json.dumps(SECRET_SELECTORS)))


@contextmanager
def emulated_viewport(browser, width, height):
    # Device emulation is scoped to its DevTools session. The one-shot protocol
    # helper disconnects immediately, which restores the native viewport before
    # another connection can measure it. Keep this owned connection alive.
    script = '''const [endpoint,width,height]=process.argv.slice(1);
      const socket=new WebSocket(endpoint);let sequence=0;const pending=new Map();
      const timer=setTimeout(()=>process.exit(1),30000);
      await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
      socket.onmessage=e=>{const m=JSON.parse(e.data),p=pending.get(m.id);
        if(p){pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result);}};
      function call(method,params={}){const id=++sequence;return new Promise((resolve,reject)=>{
        pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});}
      try{await call('Emulation.setDeviceMetricsOverride',{
        width:Number(width),height:Number(height),deviceScaleFactor:1,mobile:false});
        process.stdout.write('ready\\n');process.stdin.resume();
        await new Promise(resolve=>process.stdin.once('end',resolve));
        await call('Emulation.clearDeviceMetricsOverride');
      }finally{clearTimeout(timer);socket.close();}'''
    process = subprocess.Popen(['node', '--input-type=module', '-e', script,
        browser.settings['webSocketDebuggerUrl'], str(width), str(height)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([process.stdout], [], [], 3)[0], 'Viewport session did not start'
        assert process.stdout.readline().strip() == 'ready', 'Viewport session failed'
        yield
    finally:
        try:
            _, error = process.communicate('', timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)
            raise RuntimeError('Owned viewport session did not close') from None
        assert process.returncode == 0, 'Owned viewport session failed: ' + error[-1000:]


def narrow(browser, seat, output, result, phase, section='#noventraWorkspaceSync'):
    show_sync(browser, seat)
    for width in (480, 340):
        with emulated_viewport(browser, width, 760):
            time.sleep(.2)
            data = ui(browser, '''const block=root.querySelector('#noventraWorkspaceSync');
              root.querySelector(SECTION).scrollIntoView({block:'start',behavior:'instant'});
              const controls=[block,...block.querySelectorAll('input,select,cr-button,p,section')]
                .filter(e=>e.getClientRects().length).map(e=>{const r=e.getBoundingClientRect();
                  return {id:e.id,left:r.left,right:r.right,width:r.width};});
              return {width:innerWidth,overflow:document.documentElement.scrollWidth>innerWidth+1,controls};'''
              .replace('SECTION', json.dumps(section)))
            check(result, phase, f'{width}px measured CSS viewport', data['width'], width)
            check(result, phase, f'{width}px controls contained', bool(data['controls']) and
                  not data['overflow'] and all(c['left'] >= -1 and c['right'] <= width + 1
                                               for c in data['controls']))
            result.setdefault('narrow', []).append({'phase': phase, **data})
            capture(browser, output, phase + '-' + str(width))


def seed_profile(profile, label, names):
    (profile / 'Default').mkdir(parents=True, mode=0o700)
    all_spaces = {A_WORK: names['work'], A_ARCHIVE: names['archive']} if label == 'A' else {B_LOCAL: names['work']}
    data = {'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 220},
            'noventra': {'browser': {'account_spaces': all_spaces,
                'account_space_order': ['', *all_spaces],
                'archived_account_spaces': [A_ARCHIVE] if label == 'A' else [],
                'workspace_routes': [{'host': label.lower() + '-local.invalid',
                                      'space_id': A_WORK if label == 'A' else B_LOCAL}]}}}
    (profile / 'Default/Preferences').write_text(json.dumps(data, ensure_ascii=False))


def native_connect(browser, seat, origin, token, result, mode='create', vault='', key=''):
    show_sync(browser, seat)
    choose_mode(browser, seat, mode)
    check(result, mode, 'native mode selection', ui(browser, "return root.querySelector('#noventraSyncMode').value;"), mode)
    for selector, value in [('#noventraSyncOrigin', origin), ('#noventraSyncToken', token),
                            *([('#noventraSyncVaultId', vault), ('#noventraSyncJoinKey', key)] if mode == 'join' else [])]:
        fill_ui(browser, seat, selector, value)
    click_ui(browser, seat, '#noventraConnectSync')
    wait(lambda: ui(browser, 'return !page.noventraSyncBusy_ && page.noventraSync_.configured;'),
         timeout=40, description='completed native ' + mode)
    complete = status(browser)
    check(result, mode, 'callback is complete, durable and idle', not complete['syncing'] and
          not complete['error'] and bool(complete['last_success']) and complete['generation'] > 0)
    check(result, mode, 'submitted credential inputs cleared', ui(browser,
          "return !root.querySelector('#noventraSyncToken').value && !root.querySelector('#noventraSyncJoinKey').value;"))
    focus_check(browser, seat, result, mode, 'noventraSyncRecoveryKey' if mode == 'create' else 'noventraSyncNow')
    return complete


def choose_mode(browser, seat, mode):
    click_ui(browser, seat, '#noventraSyncMode')
    n.native_key(seat.input, 'Home')
    if mode == 'join':
        n.native_key(seat.input, 'Down')
    n.native_key(seat.input, 'Return')


def failed_join(browser, servers, origin, token, vault, key, result, phase, expected_error):
    before, rows = model(browser), digest(servers.rows())
    response = browser.rpc('noventraWorkspaceSync', 'join',
                           {'origin': origin, 'token': token, 'vault_id': vault, 'recovery_key': key})
    result.setdefault('secondary_api_probes', []).append({'phase': phase, 'reply': response})
    check(result, phase, 'completed callback rejects invalid credentials', response['status'], 'rejected')
    check(result, phase, 'specific authentication error, not generic transport failure', response.get('error'), expected_error)
    check(result, phase, 'not configured after rejected join', status(browser)['configured'], False)
    check(result, phase, 'tentative protected enrollment was durably removed',
          not (browser.profile / 'Default/Noventra Workspace Sync').exists())
    check(result, phase, 'spaces/routes/saved layout unchanged', model(browser), before)
    check(result, phase, 'opaque store unchanged', digest(servers.rows()), rows)


def fault_sequence(browser, seat, servers, old_head, result, output, witnesses, redactor):
    wait(lambda: healthy(browser), timeout=40, description='healthy before ciphertext faults')
    show_sync(browser, seat)
    servers.pause_all()
    wait(lambda: not status(browser)['syncing'], timeout=10, description='idle before head fault')
    latest = servers.head(old_head['vault'])
    save_head(latest, output, 'faults-original', redactor)
    before, generation = model(browser), status(browser)['generation']
    puts = servers.gates['B'].count('PUT')
    damaged = deepcopy(latest)
    damaged['data'] = damaged['data'][:-1] + bytes([damaged['data'][-1] ^ 1])
    damaged['revision'] = '"' + uuid.uuid4().hex + '"'
    first_alert = None
    try:
        for phase, head, expected_error in (
                ('corrupted ciphertext', damaged, 'sync_authentication_failed'),
                ('replayed old head', old_head, 'sync_rollback')):
            servers.pause_all()
            wait(lambda: not status(browser)['syncing'], timeout=10, description='idle before injected head')
            servers.replace_head(head)
            save_head(head, output, phase + '-injected', redactor)
            servers.snapshot(output / (phase + '.sqlite3'))
            servers.gates['B'].pause(False)
            if phase == 'corrupted ciphertext':
                click_ui(browser, seat, '#noventraSyncNow')
                wait(lambda: ui(browser, 'return !page.noventraSyncBusy_;'), timeout=30,
                     description='failed native Sync now callback completes')
                focus_check(browser, seat, result, phase, 'noventraSyncNow')
            # Replay is detected by the service poll, with no second Sync now.
            after_status = wait(lambda: (value := status(browser)) and value['error'] == expected_error
                                and not value['syncing'] and value,
                                timeout=45, description='authenticated ' + phase + ' error')
            check(result, phase, 'authenticated checkpoint generation does not move', after_status['generation'], generation)
            check(result, phase, 'model remains unchanged', model(browser), before)
            check(result, phase, 'failure does not PUT any object', servers.gates['B'].count('PUT'), puts)
            retained(browser, witnesses, result, phase)
            wait(lambda: ui(browser, 'return page.noventraSync_.error===EXPECTED && !root.querySelector("#noventraSyncError").hidden;'
                            .replace('EXPECTED', json.dumps(expected_error))), timeout=6,
                 description='fresh error in Settings status poll')
            alert = ui(browser, "return root.querySelector('#noventraSyncError').innerText;")
            if first_alert is None:
                first_alert = alert
                check(result, phase, 'failed Sync now shows the authentication alert', 'authenticate' in alert.lower())
            else:
                check(result, phase, 'new native error is not masked by previous action alert',
                      alert != first_alert and 'older' in alert.lower() and
                      ('vault' in alert.lower() or 'history' in alert.lower()))
                check(result, phase, 'obsolete operation-local alert cleared', ui(browser, 'return page.noventraSyncError_;'), '')
            result.setdefault('fault_observations', []).append({'phase': phase, 'status': after_status, 'alert': alert,
                'trigger': 'native Sync now' if phase == 'corrupted ciphertext' else 'automatic backend poll'})
            capture(browser, output, phase)
    finally:
        servers.pause_all()
        servers.replace_head(latest)
        servers.pause_all(False)
    wait(lambda: healthy(browser), timeout=45, description='automatic recovery after restoring ciphertext')
    wait(lambda: ui(browser, 'return !page.noventraSyncError_ && !page.noventraSync_.error && root.querySelector("#noventraSyncError").hidden;'),
         timeout=6, description='automatic recovery clears failed Sync now alert')
    check(result, 'automatic error recovery', 'native and operation-local alerts clear without another action', True)


def section_refresh(a, b, seat, mapping, name, result):
    phase = 'section visibility status refresh'
    show_sync(a, seat)
    baseline = status(a)['generation']
    wait(lambda: ui(a, 'return page.noventraSync_.generation===GENERATION;'
                    .replace('GENERATION', str(baseline))), timeout=6)
    ui(a, '''window.syncFocusEvents={focus:0,blur:0,visibility:0};
      window.syncFocusListener=()=>++window.syncFocusEvents.focus;
      window.syncBlurListener=()=>++window.syncFocusEvents.blur;
      window.syncVisibilityListener=()=>++window.syncFocusEvents.visibility;
      window.addEventListener('focus',window.syncFocusListener);
      window.addEventListener('blur',window.syncBlurListener);
      document.addEventListener('visibilitychange',window.syncVisibilityListener);return true;''')
    try:
        click_ui(a, seat, '[data-section="customizeLayout"]', activate=False)
        time.sleep(2.6)
        spaces(b, 'rename', mapping[A_WORK], name)  # Explicit secondary native API, no activation.
        wait(lambda: (value := healthy(a)) and value['generation'] > baseline and
             next(s['name'] for s in spaces(a) if s['id'] == A_WORK) == name,
             timeout=60, description='background sync while A remains in Layout')
        check(result, phase, 'UI status stays paused while its section is hidden',
              ui(a, 'return page.noventraSync_.generation;'), baseline)
        fresh = status(a)
        click_ui(a, seat, '[data-section="customizeSpaces"]', activate=False)
        wait(lambda: ui(a, 'return page.noventraSync_.generation===GENERATION && !page.noventraSync_.error;'
                        .replace('GENERATION', str(fresh['generation']))), timeout=6,
             description='section return restarts status without window activation')
        check(result, phase, 'fresh status and no stale error after native section return',
              ui(a, "return !root.querySelector('#noventraSyncError').hidden;"), False)
        check(result, phase, 'no window focus or document visibility transition used',
              ui(a, 'return window.syncFocusEvents;'), {'focus': 0, 'blur': 0, 'visibility': 0})
        check(result, phase, 'A kept native focus while B was mutated', seat.focused(a))
    finally:
        ui(a, '''window.removeEventListener('focus',window.syncFocusListener);
          window.removeEventListener('blur',window.syncBlurListener);
          document.removeEventListener('visibilitychange',window.syncVisibilityListener);return true;''')
    await_mirror(a, b, mapping, result, phase)


def sync_calls(browser):
    return sum(c['message'] == 'noventraWorkspaceSync' and c['action'] == 'sync' for c in browser.calls)


def protected_config(browser, redactor, result, phase):
    state = browser.profile / 'Default/Noventra Workspace Sync'
    check(result, phase, browser.label + ' durable protected state file exists', state.is_file())
    for path in (state, browser.profile / 'Default/Preferences', browser.profile / 'Local State'):
        raw = path.read_bytes() if path.exists() else b''
        check(result, phase, browser.label + ' ' + path.name + ' contains no key/token/password sentinel',
              bool(raw) and all(secret not in raw for secret in redactor.byte_values))


def saved_layout(browser, order):
    value = n.top_layout(browser.state()['layout'], order)
    response = browser.apply(value)
    assert response['status'] == 'ok', 'Fixture saved layout was rejected'


def prove_store_opaque(servers, sentinels, redactor, result):
    rows = servers.rows()
    forbidden = [value.encode() for value in sentinels] + [value.encode() for value in redactor.values] + list(redactor.byte_values)
    forbidden.extend(json.dumps(value, ensure_ascii=True)[1:-1].encode() for value in sentinels)
    payloads = [row[3] for row in rows]
    check(result, 'opaque store', 'real encrypted native objects exist', len(rows) >= 4)
    check(result, 'opaque store', 'no workspace/document/account/key/token sentinel in object blobs',
          all(needle not in data for needle in forbidden for data in payloads))
    # Search the SQLite bytes too: names may not leak into another column or a
    # freed page. Object IDs, vault IDs and CAS revisions are allowed metadata.
    raw = servers.store.database.read_bytes()
    check(result, 'opaque store', 'no known private sentinel anywhere in SQLite',
          all(needle not in raw for needle in forbidden))
    result['encrypted_objects'] = [{'object_id': row[1], 'bytes': len(row[3]),
                                     'sha256': digest(row[3])} for row in rows]


def basic_negative(args, output, redactor, result, browsers, servers, names):
    profile = output / 'private/profile-basic-negative'
    seed_profile(profile, 'B', names)
    browser = OwnedBrowser(args.browser, profile, output / 'private/basic-negative',
                           redactor, 'Basic-negative', password_store='basic')
    browsers.append(browser)
    try:
        browser.__enter__()
        saved_layout(browser, ORDER)
        before, rows = model(browser), digest(servers.rows())
        requests = sum(g.count() for g in servers.gates.values())
        reply = browser.rpc('noventraWorkspaceSync', 'create',
                            {'origin': servers.origin('A'), 'token': args.test_token})
        result.setdefault('secondary_api_probes', []).append({'phase': 'Basic storage rejection', 'reply': reply})
        check(result, 'Basic storage rejection', 'setup rejected', reply['status'], 'rejected')
        check(result, 'Basic storage rejection', 'unprotected storage rejected explicitly',
              reply.get('error'), 'sync_key_storage_unavailable')
        check(result, 'Basic storage rejection', 'model unchanged', model(browser), before)
        check(result, 'Basic storage rejection', 'no protected enrollment written',
              not (profile / 'Default/Noventra Workspace Sync').exists())
        check(result, 'Basic storage rejection', 'remains unconfigured', status(browser)['configured'], False)
        check(result, 'Basic storage rejection', 'opaque store unchanged', digest(servers.rows()), rows)
        check(result, 'Basic storage rejection', 'no credential setup request sent',
              sum(g.count() for g in servers.gates.values()), requests)
        check(result, 'Basic storage rejection', 'normal quit', browser.normal_quit(), 0)
    finally:
        browser.__exit__()


def run(args, output, redactor, result, browsers, seat, servers, base):
    suffix = secrets.token_hex(6)
    names = {'work': 'Работа 東京 ' + suffix, 'archive': 'Архив Café ' + suffix,
             'a_edit': 'A Автоматично ' + suffix, 'b_edit': 'B 変更 ' + suffix,
             'b_local': 'Local only ' + suffix, 'b_new': 'New B space ' + suffix}
    sentinels = [suffix, *names.values()]
    token = args.test_token  # Runtime-only attribute, never part of argv/evidence.
    basic_negative(args, output, redactor, result, browsers, servers, names)
    profiles = {label: output / 'private' / ('profile-' + label) for label in ('A', 'B')}
    for label in ('A', 'B'):
        seed_profile(profiles[label], label, names)
        browser = OwnedBrowser(args.browser, profiles[label], output / 'private' / ('initial-' + label), redactor, label)
        browsers.append(browser)
        browser.__enter__()
    a, b = browsers[-2:]
    check(result, 'isolation', 'separate browser processes', a.process.pid != b.process.pid)
    check(result, 'isolation', 'separate DevTools ports and profile roots', a.port != b.port and a.profile != b.profile)
    for browser in (a, b):
        order = ORDER if browser.label == 'A' else ('new-tab', *[i for i in ORDER if i != 'new-tab'])
        saved_layout(browser, order)
        show_sync(browser, seat)
        check(result, 'initial', browser.label + ' starts disconnected', status(browser)['configured'], False)
    witnesses = {'A': {}, 'B': {}}
    for browser, label, space in ((a, 'A-work', A_WORK), (a, 'A-personal', ''),
                                  (b, 'B-local', B_LOCAL), (b, 'B-personal', '')):
        witnesses[browser.label][label] = seed_document(browser, seat, base, label, space, redactor, sentinels)
    initial = {'A': model(a), 'B': model(b)}
    check(result, 'join setup', 'saved layouts differ before join',
          portable(initial['A']['layout']) != portable(initial['B']['layout']))
    created = native_connect(a, seat, servers.origin('A'), token, result)
    first_key = redactor.add(ui(a, "return root.querySelector('#noventraSyncRecoveryKey').value;"))
    first_vault = created['vault_id']
    # Keep the first key unpainted before exercising the real Show control.
    # Do not toggle Show back: its true state must be reset by Disconnect/Create.
    ui(a, '''window.syncRecoveryOwner=page;
      root.querySelector('#noventraSyncRecoveryKey').style.opacity='0';return true;''')
    click_ui(a, seat, '#noventraShowSyncRecovery')
    check(result, 'recovery masking reset', 'native Show changes the input type',
          ui(a, "return root.querySelector('#noventraSyncRecoveryKey').type;"), 'text')
    click_ui(a, seat, '#noventraDisconnectSync')
    wait(lambda: not status(a)['configured'], timeout=12, description='disconnect shown first recovery key')
    created = native_connect(a, seat, servers.origin('A'), token, result)
    key = ui(a, "return root.querySelector('#noventraSyncRecoveryKey').value;")
    redactor.add(key)
    vault = created['vault_id']
    check(result, 'recovery masking reset', 'new vault and distinct new key', vault != first_vault and key != first_key)
    check(result, 'recovery masking reset', 'same Settings element defaults new key to password',
          ui(a, "return window.syncRecoveryOwner===page && root.querySelector('#noventraSyncRecoveryKey').type==='password';"))
    ui(a, "root.querySelector('#noventraSyncRecoveryKey').style.opacity='';return true;")
    check(result, 'create', 'recovery key decodes to 32 bytes',
          len(base64.urlsafe_b64decode(key + '=' * (-len(key) % 4))), 32)
    check(result, 'create', 'recovery key uses canonical unpadded base64url',
          bool(re.fullmatch(r'[A-Za-z0-9_-]{43}', key)) and
          base64.urlsafe_b64encode(base64.urlsafe_b64decode(key + '=')).decode().rstrip('=') == key)
    check(result, 'create', 'recovery input is readonly and masked', ui(a,
          "const e=root.querySelector('#noventraSyncRecoveryKey');return e.readOnly&&e.type==='password';"))
    status(a)
    narrow(a, seat, output, result, 'recovery', '#noventraSyncRecovery')
    click_ui(a, seat, '#noventraSyncRecoverySaved')
    wait(lambda: ui(a, "return !root.querySelector('#noventraSyncRecoveryKey').value;"), timeout=5)
    focus_check(a, seat, result, 'recovery acknowledgement', 'noventraSyncNow')
    check(result, 'recovery acknowledgement', 'key no longer rendered',
          ui(a, "return root.querySelector('#noventraSyncRecovery').hidden;"))
    old_head = servers.head(vault)
    save_head(old_head, output, 'early-authenticated-head', redactor)
    servers.snapshot(output / 'after-create.sqlite3')

    wrong_token = redactor.add(secrets.token_urlsafe(32))
    wrong_key = redactor.add(secrets.token_urlsafe(32))
    failed_join(b, servers, servers.origin('B'), wrong_token, vault, key, result, 'wrong auth', 'sync_auth_required')
    failed_join(b, servers, servers.origin('B'), token, vault, wrong_key, result, 'wrong recovery key', 'sync_authentication_failed')
    retained(b, witnesses['B'], result, 'failed joins')
    show_sync(b, seat)
    choose_mode(b, seat, 'join')
    narrow(b, seat, output, result, 'join-form')
    joined = native_connect(b, seat, servers.origin('B'), token, result, 'join', vault, key)
    check(result, 'join', 'pre-existing non-Personal space stays local', joined['local_only_spaces'], 1)
    first_b = model(b)
    added = [s for s in first_b['spaces'] if s['id'] not in {'', B_LOCAL}]
    check(result, 'join', 'two remote spaces create two fresh local partitions', len(added), 2)
    check(result, 'join', 'local partition IDs differ from both original profiles',
          len({s['id'] for s in added} - {A_WORK, A_ARCHIVE, B_LOCAL}) == 2)
    mapping = {'': '', A_WORK: next(s['id'] for s in added if s['name'] == names['work']),
               A_ARCHIVE: next(s['id'] for s in added if s['name'] == names['archive'])}
    check(result, 'join', 'same-name existing space does not merge',
          len([s for s in first_b['spaces'] if s['name'] == names['work']]), 2)
    await_mirror(a, b, mapping, result, 'join metadata')
    for browser in (a, b):
        retained(browser, witnesses[browser.label], result, 'join retained documents')
        check(result, 'join', browser.label + ' local routes unchanged', model(browser)['routes'], initial[browser.label]['routes'])
    check(result, 'join', 'A tab URLs are not copied to B',
          not any(t['url'] in {w['before']['url'] for w in witnesses['A'].values()} for t in targets(b)))
    fresh = native_open(b, seat, base + '/B-new-partition', mapping[A_WORK])
    clean = b.ev(WITNESS, fresh)
    check(result, 'join', 'fresh mapped partition is signed out',
          clean['cookie'] == '' and clean['storage'] is None and clean['draft'] == '' and clean['password'] == '')
    witnesses['B']['B-new-partition'] = seed_document(b, seat, base, 'B-mapped-work', mapping[A_WORK], redactor, sentinels)
    narrow(b, seat, output, result, 'configured')
    section_name = 'Section visibility ' + suffix
    sentinels.append(section_name)
    section_refresh(a, b, seat, mapping, section_name, result)

    # Changes use native model APIs as fixture setup. Observation never requests
    # sync, and neither Settings page is visible during automatic convergence.
    counts = (sync_calls(a), sync_calls(b), servers.gates['B'].count('GET'))
    hidden_settings(a, seat, witnesses['A']['A-work'])
    hidden_settings(b, seat, witnesses['B']['B-local'])
    spaces(a, 'rename', A_WORK, names['a_edit'])
    spaces(a, 'restore', A_ARCHIVE)
    spaces(a, 'move-before', A_ARCHIVE, A_WORK)
    saved_layout(a, ('tabs', *[i for i in ORDER if i != 'tabs']))
    await_mirror(a, b, mapping, result, 'automatic A to B')
    check(result, 'automatic A to B', 'B performed background native GETs', servers.gates['B'].count('GET') > counts[2])
    check(result, 'automatic A to B', 'no Sync now action used', (sync_calls(a), sync_calls(b)), counts[:2])
    for browser in (a, b):
        check(result, 'automatic A to B', browser.label + ' Settings remained hidden', browser.ev('document.visibilityState'), 'hidden')

    before_b_ids = {s['id'] for s in spaces(b)}
    spaces(b, 'rename', mapping[A_WORK], names['b_edit'])
    spaces(b, 'archive', mapping[A_ARCHIVE])
    spaces(b, 'move-before', mapping[A_WORK], mapping[A_ARCHIVE])
    spaces(b, 'rename', B_LOCAL, names['b_local'])
    created_b = spaces(b, 'create', '', names['b_new'])
    new_b = next(s['id'] for s in created_b if s['id'] not in before_b_ids)
    saved_layout(b, ('reload', *[i for i in ORDER if i != 'reload']))
    new_a = wait(lambda: next((s['id'] for s in spaces(a) if s['name'] == names['b_new']), None),
                 timeout=60, description='new B space auto-enrolls on A')
    check(result, 'automatic B to A', 'new space maps to a distinct local UUID', new_a != new_b)
    mapping[new_a] = new_b
    await_mirror(a, b, mapping, result, 'automatic B to A')
    check(result, 'automatic B to A', 'B join exclusion never uploads',
          not any(s['name'] == names['b_local'] or s['id'] == B_LOCAL for s in spaces(a)))
    check(result, 'automatic B to A', 'no Sync now action used', (sync_calls(a), sync_calls(b)), counts[:2])
    stable_ids = {label: [s['id'] for s in spaces(browser)] for label, browser in (('A', a), ('B', b))}
    for browser in (a, b):
        retained(browser, witnesses[browser.label], result, 'automatic sync retained documents')
        check(result, 'automatic sync', browser.label + ' local routes unchanged', model(browser)['routes'], initial[browser.label]['routes'])

    # Corrupt and replay the real encrypted fixed head. The store is paused and
    # drained before each transactional replacement; no producer races it.
    check(result, 'fault setup', 'authenticated history advanced beyond captured early head',
          status(b)['generation'] > created['generation'])
    fault_sequence(b, seat, servers, old_head, result, output, witnesses['B'], redactor)

    for resolution in ('remote', 'local'):
        phase = 'conflict use-' + resolution
        servers.gates['B'].pause()
        local_name = 'Concurrent B ' + resolution + ' ' + suffix
        remote_name = 'Concurrent A ' + resolution + ' ' + suffix
        sentinels.extend([local_name, remote_name])
        previous = status(a)['generation']
        spaces(b, 'rename', mapping[A_WORK], local_name)
        b_local = model(b)
        spaces(a, 'rename', A_WORK, remote_name)
        wait(lambda: (v := healthy(a)) and v['generation'] > previous, timeout=45,
             description='A commits while B is offline')
        remote_head = servers.head(vault)
        servers.gates['B'].pause(False)
        wait(lambda: status(b)['conflict'], timeout=45, description='native conflict visible')
        check(result, phase, 'conflict retains local settings pending explicit choice', model(b), b_local)
        check(result, phase, 'conflict leaves remote encrypted document intact', digest(servers.head(vault)['data']), digest(remote_head['data']))
        show_sync(b, seat)
        wait(lambda: ui(b, "return !root.querySelector('#noventraSyncConflict').hidden;"), timeout=6)
        check(result, phase, 'visible conflict has a user explanation', bool(ui(b,
              "return root.querySelector('#noventraSyncStatus').innerText.trim();")))
        narrow(b, seat, output, result, phase, '#noventraSyncConflict')
        for browser in (a, b):
            retained(browser, witnesses[browser.label], result, phase)
        click_ui(b, seat, '#noventraSyncUseRemote' if resolution == 'remote' else '#noventraSyncUseLocal')
        wait(lambda: healthy(b), timeout=40, description='explicit conflict resolution completes')
        focus_check(b, seat, result, phase, 'noventraSyncNow')
        await_mirror(a, b, mapping, result, phase)
        expected_name = remote_name if resolution == 'remote' else local_name
        check(result, phase, 'explicitly chosen metadata wins on both profiles',
              [next(s['name'] for s in spaces(browser) if s['id'] == sid)
               for browser, sid in ((a, A_WORK), (b, mapping[A_WORK]))], [expected_name] * 2)

    before_restart = {label: model(browser) for label, browser in (('A', a), ('B', b))}
    status_before = {label: status(browser) for label, browser in (('A', a), ('B', b))}
    for browser in (a, b):
        retained(browser, witnesses[browser.label], result, 'before normal restart')
        check(result, 'stable mapping', browser.label + ' space IDs persist without duplicates',
              sorted(s['id'] for s in spaces(browser)), sorted(stable_ids[browser.label]))
        check(result, 'normal restart', browser.label + ' session-saving quit', browser.normal_quit(), 0)
        browser.__exit__()
        protected_config(browser, redactor, result, 'normal restart protected storage')
    for label in ('A', 'B'):
        browser = OwnedBrowser(args.browser, profiles[label], output / 'private' / ('restart-' + label), redactor, label, restore=True)
        browsers.append(browser)
        browser.__enter__()
    a, b = browsers[-2:]
    check(result, 'normal restart', 'same private keyring survives both browser restarts',
          attest_private_keyring(), result['private_keyring'])
    for browser in (a, b):
        value = wait(lambda: healthy(browser), timeout=45, description='restored encrypted sync config')
        check(result, 'normal restart', browser.label + ' remains configured with same vault', value['vault_id'], vault)
        check(result, 'normal restart', browser.label + ' checkpoint retained', value['generation'] >= status_before[browser.label]['generation'])
        check(result, 'normal restart', browser.label + ' spaces/routes/layout persist', model(browser), before_restart[browser.label])
        check(result, 'normal restart', browser.label + ' recovery key is not shown again',
              ui(browser, "return !root.querySelector('#noventraSyncRecoveryKey').value;"))
        check(result, 'normal restart', browser.label + ' origin retained', value['origin'].rstrip('/'), servers.origin(browser.label))
        for label, witness in witnesses[browser.label].items():
            target = wait(lambda: next((t for t in targets(browser) if t.get('url') == witness['before']['url']), None),
                          timeout=12, description='restored owned tab URL')
            # Activate to load a legitimately discarded restored tab, then read.
            seat.focus(browser, target)
            wait(lambda: browser.ev('!!document.querySelector("#draft")', target), timeout=8)
            actual = browser.ev(WITNESS, target)
            check(result, 'normal restart', browser.label + ' local cookie/storage ' + label,
                  {k: actual[k] for k in ('url', 'cookie', 'storage')},
                  {k: witness['before'][k] for k in ('url', 'cookie', 'storage')})
    after_name = 'Restart 自動 ' + suffix
    sentinels.append(after_name)
    spaces(a, 'rename', A_WORK, after_name)
    for browser in (a, b):
        check(result, 'automatic sync after restart', browser.label + ' Settings hidden',
              browser.ev('document.visibilityState'), 'hidden')
    await_mirror(a, b, mapping, result, 'automatic sync after restart')
    check(result, 'automatic sync after restart', 'no credentials re-entered and no Sync now action', sync_calls(a) + sync_calls(b), 0)

    show_sync(b, seat)
    before_disconnect = model(b)
    click_ui(b, seat, '#noventraDisconnectSync')
    wait(lambda: not status(b)['configured'], timeout=12, description='disconnect completed')
    focus_check(b, seat, result, 'disconnect', 'noventraSyncOrigin')
    check(result, 'disconnect', 'all local spaces/routes/layout retained', model(b), before_disconnect)
    check(result, 'disconnect', 'protected configuration is durably removed',
          not (b.profile / 'Default/Noventra Workspace Sync').exists())
    # Drain an already in-flight request before measuring > one 15s poll period.
    wait(lambda: not servers.gates['B'].active, timeout=5, description='disconnect drains requests')
    count = servers.gates['B'].count()
    disconnected_name = 'Disconnected A ' + suffix
    sentinels.append(disconnected_name)
    spaces(a, 'rename', A_WORK, disconnected_name)
    stop = time.monotonic() + 18
    while time.monotonic() < stop:
        assert not status(b)['configured'], 'Disconnected client reconfigured itself'
        time.sleep(.5)
    check(result, 'disconnect', 'B makes no network requests after disconnect', servers.gates['B'].count(), count)
    check(result, 'disconnect', 'A changes no longer apply to B', model(b), before_disconnect)
    narrow(b, seat, output, result, 'disconnected')
    prove_store_opaque(servers, sentinels, redactor, result)
    servers.snapshot(output / 'final-encrypted-store.sqlite3')
    result['mapping'] = mapping
    result['final_status'] = {'A': status(a), 'B': status(b)}
    result['scope_observed'] = 'names/order/archive and saved dock layout only; routes, tabs and account documents stayed local'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe-only', action='store_true')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    output = args.output.resolve()
    assert output.is_relative_to(OWNED_ROOT.resolve()) and output != OWNED_ROOT.resolve(), 'Use a new owned sync-test directory'
    assert str(n.ROOT / 'work/ram-investigation/xvfb-') in os.environ.get('XAUTHORITY', '')
    assert os.environ.get('NOVENTRA_BENCH_XVFB_PID', '').isdigit(), 'Authenticated private Xvfb required'
    old_umask = os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    (output / 'private').mkdir(mode=0o700)
    redactor, browsers, seat, servers = Redactor(), [], None, None
    result = {'passed': False, 'qualified': False, 'checks': [], 'runtime_executed': True,
              'browser': str(args.browser), 'work_budget_seconds': 900, 'cleanup_budget_seconds': 60,
              'owned_xvfb_pid': int(os.environ['NOVENTRA_BENCH_XVFB_PID']),
              'qualification': 'Two local independent profiles sharing an isolated libsecret test keyring; no physical-device or host-keystore claim',
              'limitations': ['Loopback HTTP fixture; HTTPS certificate/deployment operations are outside this test.',
                  'Metadata/layout edits and credential/storage negatives use native WebUI APIs; create/join/Sync now/resolution/disconnect use XTest.',
                  'Password-field sentinels are local documents; existing saved-password-manager entries are not seeded.',
                  'Across restart, live document tokens/drafts are not expected to survive; URLs/cookies/storage are checked.']}
    budget = Budget()
    try:
        os.environ['GDK_BACKEND'] = 'x11'
        result['private_keyring'] = attest_private_keyring()
        args.test_token = redactor.add(secrets.token_urlsafe(48))
        (output / 'executed-workspace_sync_smoke.py').write_bytes(Path(__file__).read_bytes())
        result['executed_source_sha256'] = digest(Path(__file__).read_bytes())
        result['browser_sha256'] = file_digest(args.browser)
        seat = OwnedSeat()
        servers = OpaqueServers(output / 'private' / 'server', args.test_token)
        with local_documents() as base:
            run(args, output, redactor, result, browsers, seat, servers, base)
        result['passed'] = bool(result['checks']) and all(c['passed'] for c in result['checks'])
        result['qualified'] = result['passed'] and not args.observe_only
    except BaseException as error:
        result['error'] = redactor.text(str(error))
        result['traceback'] = redactor.text(traceback.format_exc())
    finally:
        budget.cleanup()
        if result.get('error'):
            for browser in browsers[-2:]:
                if browser.process and browser.process.poll() is None:
                    try:
                        browser.cdp_timeout = 3
                        # Reading/masking only; never repair a failed operation.
                        if seat:
                            seat.focus(browser)
                        capture(browser, output, 'failure-' + browser.label)
                        result.setdefault('failure_status', {})[browser.label] = status(browser)
                    except BaseException as error:
                        result.setdefault('failure_capture_errors', []).append(redactor.text(str(error)))
            if servers:
                try:
                    servers.snapshot(output / 'failure-encrypted-store.sqlite3')
                except BaseException as error:
                    result.setdefault('failure_capture_errors', []).append(redactor.text(str(error)))
        result['owned_processes'] = [browser.identity for browser in browsers if browser.identity]
        result['native_api_calls'] = {str(index) + '-' + browser.label: browser.calls
                                      for index, browser in enumerate(browsers)}
        cleanup_errors = []
        for browser in reversed(browsers):
            try:
                browser.__exit__()
            except BaseException as error:
                cleanup_errors.append(redactor.text(str(error)))
        if servers:
            result['server_request_ledger'] = {label: gate.events for label, gate in servers.gates.items()}
            try:
                servers.close()
            except BaseException as error:
                cleanup_errors.append(redactor.text(str(error)))
        if seat:
            seat.close()
        result['cleanup_errors'] = cleanup_errors
        result['owned_roots_gone'] = all(process_identity(i['pid']) != i for i in result['owned_processes'])
        result['owned_members'] = [i for browser in browsers for i in browser.members.values()]
        result['owned_members_stopped'] = all(process_identity(i['pid']) != i for i in result['owned_members'])
        if cleanup_errors or not result['owned_roots_gone'] or not result['owned_members_stopped']:
            result['passed'] = result['qualified'] = False
        result['elapsed_seconds'] = budget.finish()
        write_json(output / 'result.json', result, redactor)
        os.umask(old_umask)
    print(json.dumps({'passed': result['passed'], 'qualified': result['qualified'],
                      'checks': len(result['checks']), 'result': str(output / 'result.json')}))
    raise SystemExit(0 if result['passed'] or args.observe_only else 1)


if __name__ == '__main__':
    main()
