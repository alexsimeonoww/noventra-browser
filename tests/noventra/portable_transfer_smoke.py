"""Native encrypted-file and direct-peer transfers in two owned profiles.

Run through work/ram-investigation/with-xvfb.py. The fixture additionally hides
the host home and creates a private network namespace with loopback and a dummy
LAN interface only. No
external store, STUN, relay or user profile is accessed. Connection codes and
password plaintext are never written to diagnostic records or screenshots.

--case rtc-preflight checks the existing engine's WebUI RTC support only. It is
not acceptance of the new transfer implementation. Other cases require the new
Settings resources and native handler, exact candidate hashes and a new output.
--case harness-preflight checks only the owned fixture seed/model boundaries.
"""

import argparse
from contextlib import contextmanager
import ctypes as C
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time

from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from interface_polish_smoke import native_key
from layout_editor_smoke import FIND
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree
from onboarding_import_smoke import focus_picker
from sidebar_command_footer_smoke import extension_fixture
from workspace_tools_smoke import native_text


ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / 'tests/noventra').is_dir())
WORK = 'a1000000-0000-4000-8000-000000000001'
ARCHIVE = 'a1000000-0000-4000-8000-000000000002'
RECEIVER_WORK = 'b1000000-0000-4000-8000-000000000001'
PASSWORD = 'Owned Transfer Password 2026!'
SOURCE_PASSWORD = 'synthetic-source-password-only'
RECEIVER_PASSWORD = 'synthetic-existing-password-only'
NEW_PASSWORD = 'synthetic-new-password-only'
BOOKMARK_TITLE = 'Portable bookmark Café 東京'
EXISTING_TITLE = 'Keep receiver bookmark'
PAGE = '''<!doctype html><meta charset="utf-8"><title>Portable witness</title>
<style>body{margin:32px;font:16px sans-serif;background:#eef3f8}</style>
<label>Retained draft <input id="draft"></label>
<script>window.portableToken=crypto.randomUUID();</script>'''


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def check(result, name, passed, actual=None):
    row = {'name': name, 'passed': bool(passed)}
    if actual is not None:
        row['actual'] = actual
    result['checks'].append(row)
    assert passed, name


def evaluate(browser, expression, target=None):
    """Keep private signaling/fixture secrets out of subprocess arguments."""
    run = subprocess.run(
        ['/usr/bin/node', str(ROOT / 'tests/noventra/cdp_eval.mjs'),
         (target or browser.settings)['webSocketDebuggerUrl']],
        input=expression, capture_output=True, text=True, timeout=30)
    if run.returncode:
        # DevTools exception text can contain a caller's input. Do not retain it.
        raise RuntimeError('Owned DevTools evaluation failed; private input omitted')
    return json.loads(run.stdout)


def protocol(browser, method, params, target=None):
    script = '''const [url]=process.argv.slice(1);let body='';
    for await(const block of process.stdin)body+=block;
    const request=JSON.parse(body),socket=new WebSocket(url);
    const result=await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>reject(Error('timeout')),20000);
      socket.onopen=()=>socket.send(JSON.stringify({id:1,...request}));
      socket.onerror=()=>reject(Error('connection'));
      socket.onmessage=event=>{const value=JSON.parse(event.data);if(value.id===1){
        clearTimeout(timer);value.error?reject(Error('protocol')):resolve(value.result)}}});
    socket.close();process.stdout.write(JSON.stringify(result));'''
    run = subprocess.run(
        ['/usr/bin/node', '--input-type=module', '-e', script,
         (target or browser.settings)['webSocketDebuggerUrl']],
        input=json.dumps({'method': method, 'params': params}),
        capture_output=True, text=True, timeout=25)
    if run.returncode:
        raise RuntimeError('Owned DevTools protocol failed; private input omitted')
    return json.loads(run.stdout)


def ui(browser, expression):
    return evaluate(browser, '(()=>{' + FIND + '''
      const index=deep(document,'settings-appearance-page-index');
      const page=index?.shadowRoot.querySelector('settings-appearance-page#dataDevices');
      const component=page?.shadowRoot.querySelector('noventra-transfer');
      const root=component?.shadowRoot;
      if(!root)throw Error('Transfer component missing');
    ''' + expression + '})()')


def call(browser, action, payload=None):
    return evaluate(browser, '''(async()=>{
      const {sendWithPromise}=await import('chrome://resources/js/cr.js');
      return sendWithPromise('noventraTransfer',ACTION,PAYLOAD);
    })()'''.replace('ACTION', json.dumps(action)).replace('PAYLOAD', json.dumps(payload or {})))


def isolate(args, fixture_path=None):
    fixture = Path(fixture_path or __file__).resolve()
    if args.inside_owned_home:
        assert os.environ.get('NOVENTRA_TRANSFER_OWNED_HOME') == str(args.output.parent)
        links = json.loads(subprocess.check_output(['/usr/bin/ip', '-j', 'link', 'show'], text=True))
        assert {link['ifname'] for link in links} == {'lo', 'nv-test0'}
        assert all('UP' in link['flags'] for link in links)
        return
    args.output.mkdir(parents=True, exist_ok=False)
    home = args.output / 'source-home'
    temporary = args.output / 'namespace-tmp'
    runtime = args.output / 'runtime'
    for path in (home, temporary, runtime):
        path.mkdir(mode=0o700)
    assert shutil.which('bwrap')
    argv = ['bwrap', '--die-with-parent', '--ro-bind', '/', '/',
            '--bind', str(args.output), str(args.output), '--bind', str(home),
            str(Path.home()), '--bind', str(temporary), '/tmp', '--ro-bind',
            '/tmp/.X11-unix', '/tmp/.X11-unix', '--dev', '/dev', '--proc', '/proc']
    python_base = Path(sys.base_prefix).resolve().parent
    if python_base.is_relative_to(Path.home()):
        argv += ['--ro-bind', str(python_base), str(python_base)]
    argv += ['--chdir', str(ROOT), '--', sys.executable, '-B', str(fixture),
             '--browser', str(args.browser), '--expected-binary', args.expected_binary,
             '--expected-resources', args.expected_resources, '--output', str(args.output / 'native'),
             '--case', args.case, '--inside-owned-home']
    if args.rtc_policy_diagnostic:
        argv.append('--rtc-policy-diagnostic')
    # unshare retains namespace-local caps just long enough to bring up lo.
    # bwrap then drops capabilities before the non-root browser is executed.
    setup = ('import os,subprocess,sys;'
             'commands=[["link","set","lo","up"],'
             '["link","add","nv-test0","type","dummy"],'
             '["addr","add","10.255.0.1/24","dev","nv-test0"],'
             '["link","set","nv-test0","up"],'
             '["route","add","default","dev","nv-test0"]];'
             '[subprocess.run(["/usr/bin/ip",*command],check=True) for command in commands];'
             'os.execvp("/usr/bin/setpriv",["setpriv",'
             '"--inh-caps=-all","--ambient-caps=-all",*sys.argv[1:]])')
    argv = ['/usr/bin/unshare', '--user', '--map-current-user', '--net', '--keep-caps',
            '/usr/bin/python3', '-c', setup, *argv]
    env = {key: os.environ[key] for key in
           ('DISPLAY', 'XAUTHORITY', 'NOVENTRA_BENCH_XVFB_PID', 'HOME', 'USER', 'LOGNAME', 'LANG')
           if key in os.environ}
    env.update(PATH='/usr/bin:/bin', XDG_RUNTIME_DIR=str(runtime),
               DBUS_SESSION_BUS_ADDRESS='unix:path=/tmp/noventra-no-session-bus',
               NOVENTRA_CONFIG_HOME=str(args.output / 'noventra-config'),
               NOVENTRA_TRANSFER_OWNED_HOME=str(args.output))
    write(args.output / 'namespace-launch.json', {
        'argv': argv, 'home_isolated': True, 'network_isolated_no_egress': True,
        'fixture_sha256': sha(fixture), 'browser_sha256': sha(args.browser),
        'resources_sha256': sha(args.browser.parent / 'resources.pak')})
    run = subprocess.run(argv, env=env, timeout=960)
    write(args.output / 'namespace-result.json', {'exit_code': run.returncode,
        'home_isolated': True, 'network_isolated_no_egress': True,
        'fixture_sha256_after': sha(fixture)})
    raise SystemExit(run.returncode)


@contextmanager
def pages():
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            data = PAGE.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}'
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


def normal_close(browser):
    endpoint = request(browser.port, '/json/version')['webSocketDebuggerUrl']
    script = '''const socket=new WebSocket(process.argv[1]);let sent=false;
      const timer=setTimeout(()=>process.exit(1),10000);
      socket.onopen=()=>{sent=true;socket.send(JSON.stringify({id:1,method:'Browser.close'}))};
      socket.onclose=()=>{clearTimeout(timer);process.exit(sent?0:1)};'''
    run = subprocess.run(['/usr/bin/node', '-e', script, endpoint],
                         capture_output=True, text=True, timeout=15)
    assert run.returncode == 0, 'Owned normal browser quit failed'
    code = browser.process.wait(timeout=15)
    assert code == 0, 'Owned browser did not exit normally'
    return code


@contextmanager
def browser_session(args, profile, output, result, phase, restore=False):
    flags = ['--password-store=basic', '--ui-toolkit=gtk', '--force-renderer-accessibility']
    profile.mkdir(parents=True, exist_ok=True)
    fixture = profile / 'fixture-controller'
    if not fixture.exists():
        fixture = extension_fixture(profile)
        manifest = json.loads((fixture / 'manifest.json').read_text())
        manifest['permissions'].append('privacy')
        write(fixture / 'manifest.json', manifest)
    flags.append('--load-extension=' + str(fixture))
    if restore:
        flags.append('--restore-last-session')
    browser = LayoutBrowser(args.browser, profile, output, extra_args=flags)
    browser.__enter__()
    try:
        targets = request(browser.port, '/json/list')
        result.setdefault('controller_startup', []).append({'phase': phase,
            'endpoint_types': [target['type'] for target in targets],
            'fixture_worker_count': sum(target['type'] == 'service_worker'
                and target['url'].endswith('/worker.js') for target in targets)})
        # MV3 workers can idle out. Discover the owned controller once, while
        # startup is active; subsequent API reads use its persistent page.
        control(browser, 'true')
        yield browser
    except BaseException as error:
        result.setdefault('failures', []).append({'phase': phase, 'error_type': type(error).__name__,
            'message': str(error) if isinstance(error, AssertionError) else 'Private diagnostic details omitted'})
        raise
    finally:
        try:
            if browser.process.poll() is None:
                # Cleanup must not leave a private browser waiting on its own
                # in-progress-transfer beforeunload warning after a test fails.
                try:
                    ui(browser, '''component.peer_?.close();component.review_=null;
                      window.removeEventListener('beforeunload',component.leaving_);return true;''')
                    call(browser, 'cancel')
                except BaseException:
                    pass
                result['normal_shutdowns'].append({'phase': phase, 'exit': normal_close(browser)})
        finally:
            browser.__exit__(None, None, None)


def rtc_preflight(browser, result):
    observed = evaluate(browser, '''(async()=>{
      const result={available:typeof RTCPeerConnection==='function',connected:false,payload:false,
        candidateCounts:[0,0],candidateErrors:0};
      if(!result.available)return result;
      let first,second;
      try {
        first=new RTCPeerConnection({iceServers:[]});
        second=new RTCPeerConnection({iceServers:[]});
        const channel=first.createDataChannel('owned-loopback');
        first.onicecandidate=e=>{if(e.candidate){result.candidateCounts[0]++;
          second.addIceCandidate(e.candidate).catch(()=>{result.candidateErrors++})}};
        second.onicecandidate=e=>{if(e.candidate){result.candidateCounts[1]++;
          first.addIceCandidate(e.candidate).catch(()=>{result.candidateErrors++})}};
        const received=new Promise((resolve,reject)=>{
          const timer=setTimeout(()=>reject(Error('owned RTC timeout')),12000);
          second.ondatachannel=e=>{e.channel.onmessage=message=>{
            clearTimeout(timer);resolve(message.data==='owned loopback payload')}};
        });
        await first.setLocalDescription(await first.createOffer());
        await second.setRemoteDescription(first.localDescription);
        await second.setLocalDescription(await second.createAnswer());
        await first.setRemoteDescription(second.localDescription);
        await new Promise((resolve,reject)=>{if(channel.readyState==='open'){resolve();return}
          const timer=setTimeout(()=>reject(Error('open timeout')),12000);
          channel.onopen=()=>{clearTimeout(timer);resolve()}});
        result.connected=first.connectionState==='connected';
        channel.send('owned loopback payload');result.payload=await received;
      }catch(error){result.error=String(error.message);result.states=[first?.iceConnectionState,second?.iceConnectionState]}
      finally{first?.close();second?.close()}
      return result;
    })()''')
    check(result, 'WebUI supports an actual isolated RTCDataChannel with no STUN or relay',
          observed['available'] and observed['connected'] and observed['payload']
          and all(observed['candidateCounts']), observed)


def seed_profile(profile, base, source):
    default = profile / 'Default'
    default.mkdir(parents=True)
    spaces = {'': 'Personal'}
    if source:
        spaces.update({WORK: 'Portable Work Café', ARCHIVE: 'Portable Archive 東京'})
    else:
        spaces[RECEIVER_WORK] = 'Keep receiver Space'
    preferences = {
        'helium': {'completed_onboarding': True, 'browser': {
            'zen_mode': True, 'zen_mode_sidebar_pinned': True,
            'zen_mode_top_chrome_pinned': True}},
        'session': {'restore_on_startup': 1},
        'noventra': {'browser': {
            'account_spaces': spaces, 'account_space_order': list(spaces),
            'archived_account_spaces': [ARCHIVE] if source else [],
            'space_appearance': {
                '': {'color': 'blue', 'icon': 'home', 'theme': 'black-glass'},
                WORK: {'color': 'purple', 'icon': 'code', 'theme': 'forest'},
                ARCHIVE: {'color': 'teal', 'icon': 'book', 'theme': 'ocean'},
            } if source else {
                '': {'color': 'orange', 'icon': 'home', 'theme': 'solid-white'},
                RECEIVER_WORK: {'color': 'green', 'icon': 'work', 'theme': 'sand'},
            },
            'glass_enabled': not source, 'tab_density': 1 if source else 0,
            'bookmark_opening': 1 if source else 0,
            'workspace_routes': [{'host': 'portable-route.invalid', 'space_id': WORK}] if source else [],
        }},
        'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 180},
    }
    write(default / 'Preferences', preferences)
    title = BOOKMARK_TITLE if source else EXISTING_TITLE
    roots = {}
    for key, identifier in [('bookmark_bar', '1'), ('other', '2'), ('synced', '3')]:
        children = []
        if key == 'bookmark_bar':
            children = [{'name': title, 'type': 'url', 'id': '4',
                'guid': 'cc000000-0000-4000-8000-000000000001',
                'url': base + ('/saved-source' if source else '/saved-receiver'),
                'date_added': '13300000000000000'}]
        roots[key] = {'name': key, 'type': 'folder', 'id': identifier,
            'date_added': '13300000000000000', 'date_modified': '0', 'children': children}
    write(default / 'Bookmarks', {'version': 1, 'roots': roots})
    marker = profile / 'owned-preserve-marker.bin'
    marker.write_bytes(b'owned profile unrelated data must be retained\x00\xff')


def control(browser, expression):
    if not hasattr(browser, 'portable_controller'):
        worker = wait_for(lambda: next((p for p in request(browser.port, '/json/list')
            if p['type'] == 'service_worker' and p['url'].endswith('/worker.js')), None), browser.process)
        page = request(browser.port,
            '/json/new?' + worker['url'].rsplit('/', 1)[0] + '/control.html', 'PUT')
        wait_for(lambda: evaluate(browser, '''document.readyState==='complete'&&
            typeof chrome.tabs?.query==='function'&&typeof chrome.bookmarks?.getTree==='function' ''', page),
            browser.process, timeout=10)
        browser.portable_controller = page
    return evaluate(browser, '(async()=>{return (await(' + expression + '))??null})()', browser.portable_controller)


def open_manager(browser):
    if not hasattr(browser, 'portable_manager'):
        browser.portable_manager = request(browser.port, '/json/new?chrome://password-manager/passwords', 'PUT')
        wait_for(lambda: evaluate(browser, 'document.readyState==="complete"&&!!chrome.passwordsPrivate',
                                 browser.portable_manager), browser.process)
    return browser.portable_manager


def seed_passwords(browser, source):
    target = open_manager(browser)
    values = [{'url': 'https://portable-password.invalid/', 'username': 'existing-conflict',
               'password': SOURCE_PASSWORD if source else RECEIVER_PASSWORD,
               'note': 'Keep receiver note' if not source else 'Source note excluded',
               'useAccountStore': False}]
    if source:
        values.append({'url': 'https://portable-new-password.invalid/', 'username': 'new-login',
                       'password': NEW_PASSWORD, 'note': '', 'useAccountStore': False})
    evaluate(browser, '(async()=>{for(const value of ' + json.dumps(values) +
        ')await chrome.passwordsPrivate.addPassword(value);return true})()', target)
    wait_for(lambda: evaluate(browser,
        '(async()=>{const rows=await chrome.passwordsPrivate.getSavedPasswordList();'
        'return rows.filter(e=>["existing-conflict","new-login"].includes(e.username)).length})()',
        target) == len(values), browser.process)


def passwords_match(browser, imported):
    target = open_manager(browser)
    return evaluate(browser, '''(async()=>{
      const rows=await chrome.passwordsPrivate.getSavedPasswordList();
      const existing=rows.find(row=>row.username==='existing-conflict');
      const added=rows.find(row=>row.username==='new-login');
      const oldMatches=!!existing&&await chrome.passwordsPrivate.requestPlaintextPassword(
          existing.id,chrome.passwordsPrivate.PlaintextReason.VIEW)===OLD;
      const newMatches=!!added&&await chrome.passwordsPrivate.requestPlaintextPassword(
          added.id,chrome.passwordsPrivate.PlaintextReason.VIEW)===NEW;
      return {existing:oldMatches,added:newMatches,count:rows.length};
    })()'''.replace('OLD', json.dumps(RECEIVER_PASSWORD)).replace('NEW', json.dumps(NEW_PASSWORD)), target)


def space_call(browser, action='list', identifier='', extra=''):
    return evaluate(browser, '''(async()=>{const {sendWithPromise}=await import('chrome://resources/js/cr.js');
      return sendWithPromise('noventraSpaces',ACTION,IDENTIFIER,EXTRA)})()'''
      .replace('ACTION', json.dumps(action)).replace('IDENTIFIER', json.dumps(identifier)).replace('EXTRA', json.dumps(extra)))


def make_page(browser, url, marker):
    page = request(browser.port, '/json/new?' + url, 'PUT')
    wait_for(lambda: evaluate(browser, '!!window.portableToken', page), browser.process)
    value = evaluate(browser, '''(()=>{document.querySelector('#draft').value=MARKER;
      document.cookie='portableWitness='+MARKER;
      localStorage.setItem('portableWitness',MARKER);
      return {token:window.portableToken,timeOrigin:performance.timeOrigin,
          draft:document.querySelector('#draft').value,url:location.href}})()'''.replace('MARKER', json.dumps(marker)), page)
    return page, value


def prepare_documents(browser, base, source):
    personal = make_page(browser, base + ('/source-personal' if source else '/receiver-retained'),
                         'source-personal' if source else 'receiver-retained')
    if source:
        space_call(browser, 'switch', WORK)
        account = make_page(browser, base + '/source-account', 'source-account')
        pinned = make_page(browser, base + '/source-pin', 'source-pin')
        control(browser, 'chrome.tabs.query({}).then(tabs=>chrome.tabs.update('
            'tabs.find(tab=>tab.url===' + json.dumps(base + '/source-pin') + ').id,{pinned:true}))')
        space_call(browser, 'switch', '')
        return {'personal': personal, 'account': account, 'pinned': pinned}
    return {'personal': personal}


def model(browser, base):
    bookmarks = control(browser, 'chrome.bookmarks.getTree().then(roots=>{const rows=[];'
        'const visit=nodes=>nodes.forEach(node=>{if(node.url)rows.push({title:node.title,url:node.url});'
        'visit(node.children||[])});visit(roots);return rows})')
    tabs = control(browser, 'chrome.tabs.query({}).then(tabs=>tabs.filter(tab=>tab.url?.startsWith(' +
        json.dumps(base) + ')).map(tab=>({id:tab.id,url:tab.url,pinned:tab.pinned})))')
    spaces = space_call(browser)
    prefs = evaluate(browser, '''(async()=>{const out={};for(const key of [
      'noventra.browser.glass_enabled','noventra.browser.tab_density',
      'noventra.browser.bookmark_opening']){
      out[key]=await new Promise(resolve=>chrome.settingsPrivate.getPref(key,p=>resolve(p.value)))}
      return out})()''')
    prefs['noventra.browser.workspace_routes'] = evaluate(browser, '''(async()=>{
      const {sendWithPromise}=await import('chrome://resources/js/cr.js');
      return (await sendWithPromise('noventraWorkspaceTools','list',{})).routes})()''')
    return {'bookmarks': bookmarks, 'tabs': tabs, 'spaces': spaces, 'prefs': prefs}


def space_appearance(rows):
    return {row['id']: {key: row[key] for key in ('color', 'icon', 'theme')}
            for row in rows}


def check_imported_space_themes(result, baseline, imported):
    before = space_appearance(baseline['spaces'])
    after = space_appearance(imported['spaces'])
    named = {row['name']: row for row in imported['spaces']}
    check(result, 'named Space themes survive encrypted export, review and import',
          named['Portable Work Café']['theme'] == 'forest'
          and named['Portable Archive 東京']['theme'] == 'ocean')
    check(result, 'destination Personal and existing named Space appearances are preserved',
          after[''] == before[''] and after[RECEIVER_WORK] == before[RECEIVER_WORK],
          {'personal': after[''], 'existing_space': after[RECEIVER_WORK]})


def witness(browser, prepared):
    page, expected = prepared
    actual = evaluate(browser, '''({token:window.portableToken,timeOrigin:performance.timeOrigin,
      draft:document.querySelector('#draft').value,url:location.href})''', page)
    return actual == expected


def focus_owned(browser, inp):
    """Raise/focus only this test process's X11 window on the private display."""
    x, display = inp.pointer.x, inp.pointer.d
    x.XDefaultRootWindow.argtypes = [C.c_void_p]
    x.XDefaultRootWindow.restype = C.c_ulong
    x.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    x.XInternAtom.restype = C.c_ulong
    x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
    x.XGetWindowProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_long, C.c_long, C.c_int, C.c_ulong,
        C.POINTER(C.c_ulong), C.POINTER(C.c_int), C.POINTER(C.c_ulong), C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ubyte))]
    x.XSetInputFocus.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong]
    x.XRaiseWindow.argtypes = [C.c_void_p, C.c_ulong]
    class Attributes(C.Structure):
        _fields_ = [(name,C.c_int) for name in ('x','y','width','height','border_width','depth')] + [
            ('visual',C.c_void_p),('root',C.c_ulong),('klass',C.c_int),
            ('bit_gravity',C.c_int),('win_gravity',C.c_int),('backing_store',C.c_int),
            ('backing_planes',C.c_ulong),('backing_pixel',C.c_ulong),('save_under',C.c_int),
            ('colormap',C.c_ulong),('map_installed',C.c_int),('map_state',C.c_int),
            ('all_event_masks',C.c_long),('your_event_mask',C.c_long),
            ('do_not_propagate_mask',C.c_long),('override_redirect',C.c_int),('screen',C.c_void_p)]
    x.XGetWindowAttributes.argtypes = [C.c_void_p,C.c_ulong,C.POINTER(Attributes)]
    root, parent, children, count = C.c_ulong(), C.c_ulong(), C.POINTER(C.c_ulong)(), C.c_uint()
    assert x.XQueryTree(display, x.XDefaultRootWindow(display), C.byref(root), C.byref(parent), C.byref(children), C.byref(count))
    found = []
    atom = x.XInternAtom(display, b'_NET_WM_PID', 0)
    try:
        for index in range(count.value):
            actual, fmt, items, after, data = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong(), C.POINTER(C.c_ubyte)()
            x.XGetWindowProperty(display, children[index], atom, 0, 1, 0, 0,
                C.byref(actual), C.byref(fmt), C.byref(items), C.byref(after), C.byref(data))
            try:
                if items.value and C.cast(data, C.POINTER(C.c_ulong))[0] == browser.process.pid:
                    attributes = Attributes()
                    if x.XGetWindowAttributes(display, children[index], C.byref(attributes)) and attributes.map_state == 2 and not attributes.override_redirect:
                        found.append((attributes.width * attributes.height, children[index]))
            finally:
                if data:
                    x.XFree(data)
    finally:
        x.XFree(children)
    assert found, 'Mapped owned browser X11 window not found'
    # Dismissed suggestion bubbles may retain unmapped same-PID windows.
    # The mapped main window is the largest ordinary owned surface.
    main_window = max(found)[1]
    x.XRaiseWindow(display, main_window)
    x.XSetInputFocus(display, main_window, 2, 0)
    x.XFlush(display)


def click(browser, tree, inp, selector):
    request(browser.port, '/json/activate/' + browser.settings['id'])
    focus_owned(browser, inp)
    point = ui(browser, '''const element=root.querySelector(SELECTOR);
      if(!element||element.disabled||!element.checkVisibility())throw Error('Unavailable control');
      element.scrollIntoView({block:'center',behavior:'instant'});
      const rect=element.getBoundingClientRect();return {x:rect.x+rect.width/2,
        y:rect.y+rect.height/2,w:innerWidth,h:innerHeight};'''.replace('SELECTOR', json.dumps(selector)))
    tree.read()
    candidates = [tree.rect(node) for node in tree.nodes.values()
        if ' INVISIBLE ' not in node['line'] and 'class_name=ContentsContainerView ' in node['line']
        and abs(node['bounds'][2] - point['w']) <= 2 and abs(node['bounds'][3] - point['h']) <= 2]
    assert len(candidates) == 1, 'Owned content viewport is ambiguous'
    content = candidates[0]
    assert 0 < point['x'] < content['w'] and 0 < point['y'] < content['h']
    inp.pointer.click(content['x'] + point['x'], content['y'] + point['y'])


def fill(browser, tree, inp, selector, value):
    click(browser, tree, inp, selector)
    native_key(inp, 'a', ('Control_L',))
    # DevTools inserts text as a browser input operation, avoiding OS clipboard
    # state and preserving long private codes without logging their content.
    protocol(browser, 'Input.insertText', {'text': value})
    assert ui(browser, 'return root.querySelector(SELECTOR).value===VALUE;'
              .replace('SELECTOR', json.dumps(selector)).replace('VALUE', json.dumps(value)))


def open_transfer(browser):
    browser.settings = request(browser.port, '/json/new?chrome://settings/dataDevices', 'PUT')
    wait_for(lambda: evaluate(browser, 'location.pathname==="/dataDevices"&&document.readyState==="complete"&&!!document.querySelector("settings-ui")?.shadowRoot'), browser.process)
    wait_for(lambda: ui(browser, 'return component.available_ && page.dataDevicesPage && page.shadowRoot.querySelector("#dataDevicesContent")?.checkVisibility();'), browser.process)
    return NativeUITree(browser)


def capture(browser, inp, output, name):
    request(browser.port, '/json/activate/' + browser.settings['id'])
    focus_owned(browser, inp)
    wait_for(lambda: evaluate(browser, 'document.visibilityState==="visible"'), browser.process)
    ui(browser, '''for(const element of root.querySelectorAll('textarea,input[type=password]')){
      element.dataset.previousVisibility=element.style.visibility;element.style.visibility='hidden'}
      return true;''')
    try:
        ui(browser, '''return new Promise(resolve=>requestAnimationFrame(()=>
          requestAnimationFrame(()=>resolve(true))));''')
        inp.capture(output / (name + '.png')).close()
    finally:
        ui(browser, '''for(const element of root.querySelectorAll('[data-previous-visibility]')){
          element.style.visibility=element.dataset.previousVisibility;delete element.dataset.previousVisibility}return true;''')


@contextmanager
def downloads(browser, path):
    path.mkdir(mode=0o700)
    endpoint = request(browser.port, '/json/version')['webSocketDebuggerUrl']
    script = '''const [url,path]=process.argv.slice(1);const socket=new WebSocket(url);
      let count=0;const pending=new Map();
      await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject});
      socket.onmessage=event=>{const value=JSON.parse(event.data),job=pending.get(value.id);
        if(job){pending.delete(value.id);value.error?job.reject(Error('protocol')):job.resolve(value.result)}};
      const call=(method,params)=>new Promise((resolve,reject)=>{const id=++count;
        pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}))});
      await call('Browser.setDownloadBehavior',{behavior:'allow',downloadPath:path,eventsEnabled:true});
      process.stdout.write('ready\\n');process.stdin.resume();
      await new Promise(resolve=>process.stdin.on('end',resolve));
      await call('Browser.setDownloadBehavior',{behavior:'default',eventsEnabled:false});socket.close();'''
    process = subprocess.Popen(['/usr/bin/node', '--input-type=module', '-e', script, endpoint, str(path)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert process.stdout.readline().strip() == 'ready', 'Owned download override failed'
        yield
    finally:
        _, error = process.communicate('', timeout=5)
        assert process.returncode == 0, 'Owned download override did not close'


def choose_file(browser, tree, inp, path):
    click(browser, tree, inp, '#chooseTransfer')
    def ready():
        try:
            return focus_picker(inp)
        except AssertionError:
            return None
    picker = wait_for(ready, browser.process, timeout=8)
    x, display = inp.pointer.x, inp.pointer.d
    x.XMoveResizeWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint]
    x.XMoveResizeWindow(display, picker, 20, 20, 1000, 700)
    x.XFlush(display)
    time.sleep(.2)
    native_key(inp, 'l', ('Control_L',))
    native_key(inp, 'a', ('Control_L',))
    # The location field is a GTK widget, so type through the private native
    # keyboard. Paths are owned ASCII fixture paths, never secrets.
    native_text(inp, str(path))
    # Fitting only the owned private GTK transient keeps Open within display.
    inp.pointer.click(20 + 1000 - 52, 20 + 700 - 23)
    wait_for(lambda: ui(browser, 'return component.fileName_===NAME && !component.busy_;'
                        .replace('NAME', json.dumps(path.name))), browser.process)


def review_file(browser, tree, inp, path, password=PASSWORD):
    choose_file(browser, tree, inp, path)
    fill(browser, tree, inp, '#transferPassword', password)
    if ui(browser, 'return root.querySelector("#previewTransfer").disabled;'):
        return None
    click(browser, tree, inp, '#previewTransfer')
    wait_for(lambda: ui(browser, 'return !component.busy_ && (!!component.review_ || !!component.error_);'), browser.process)
    return ui(browser, 'return component.review_ ? {bookmarks:component.review_.bookmarks,spaces:component.review_.spaces,'
              'tabs:component.review_.tabs,pins:component.review_.pins,extensions:component.review_.extensions,'
              'password_counts:component.review_.password_counts,categories:component.review_.categories}:null;')


def idle(browser):
    return ui(browser, 'return !component.busy_;')


def selected(browser, tree, inp, key, value, review=False):
    selector = ('#transferReview ' if review else '.choices ') + '[data-category="' + key + '"]'
    if ui(browser, 'return root.querySelector(SELECTOR).checked;'.replace('SELECTOR', json.dumps(selector))) != value:
        click(browser, tree, inp, selector)


def apply_review(browser, tree, inp):
    click(browser, tree, inp, '#applyTransfer')
    wait_for(lambda: ui(browser, 'return !component.busy_ && (!component.review_ || !!component.error_);'),
             browser.process, timeout=30)
    check_error = ui(browser, 'return component.error_;')
    assert not check_error, 'Native import confirmation failed'
    assert ui(browser, 'return component.status_.startsWith("Import finished.");'), 'Native import returned partial failure'


def layout_checks(browser, result):
    # Emulation state belongs to its CDP session. Keep that session attached
    # through layout/paint and measurement, then explicitly clear before close.
    expression = '(async()=>{await new Promise(requestAnimationFrame);await new Promise(requestAnimationFrame);' + FIND + '''
      const index=deep(document,'settings-appearance-page-index');
      const page=index.shadowRoot.querySelector('settings-appearance-page#dataDevices');
      const component=page.shadowRoot.querySelector('noventra-transfer');
      const root=component.shadowRoot;
      const bounds=component.getBoundingClientRect();
      const cards=[...root.querySelectorAll('.card,.choices,.review')].filter(e=>e.checkVisibility())
        .map(e=>{const r=e.getBoundingClientRect();return {left:r.left,right:r.right,width:r.width}});
      return {width:innerWidth,body:document.documentElement.scrollWidth,cards,
        component:{left:bounds.left,right:bounds.right}};})()'''
    script = '''let input='';for await(const block of process.stdin)input+=block;
      const config=JSON.parse(input),socket=new WebSocket(process.argv[1]);
      let sequence=0;const pending=new Map();
      socket.onmessage=event=>{const v=JSON.parse(event.data),p=pending.get(v.id);if(p){
        clearTimeout(p.timer);pending.delete(v.id);v.error?p.reject(Error('protocol')):p.resolve(v.result)}};
      const rpc=(method,params={})=>new Promise((resolve,reject)=>{const id=++sequence;
        const timer=setTimeout(()=>{pending.delete(id);reject(Error('timeout'))},20000);
        pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params}))});
      await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=()=>reject(Error('connection'))});
      const rows=[];try{for(const width of config.widths){
        await rpc('Emulation.setDeviceMetricsOverride',{width,height:760,deviceScaleFactor:1,mobile:false});
        const r=await rpc('Runtime.evaluate',{expression:config.expression,awaitPromise:true,returnByValue:true});
        if(r.exceptionDetails)throw Error('evaluation');rows.push({requested:width,actual:r.result.value});
      }}finally{await rpc('Emulation.clearDeviceMetricsOverride');socket.close()}
      process.stdout.write(JSON.stringify(rows));'''
    run = subprocess.run(['/usr/bin/node', '--input-type=module', '-e', script,
        browser.settings['webSocketDebuggerUrl']], input=json.dumps({'widths': [340, 480],
        'expression': expression}), capture_output=True, text=True, timeout=60)
    if run.returncode:
        raise RuntimeError('Owned persistent viewport measurement failed')
    for row in json.loads(run.stdout):
        width, observed = row['requested'], row['actual']
        check(result, f'{width}px transfer cards stay within the Settings viewport',
            observed['width'] == width and observed['body'] <= width + 1
            and all(card['left'] >= -1 and card['right'] <= width + 1 for card in observed['cards']), observed)


def file_flow(args, result, base):
    source_profile, receiver_profile = args.output / 'file-source-profile', args.output / 'file-receiver-profile'
    seed_profile(source_profile, base, True)
    seed_profile(receiver_profile, base, False)
    inp = Input('x11')
    imported_state = None
    try:
        with browser_session(args, source_profile, args.output / 'file-source', result, 'file-source') as source:
            with browser_session(args, receiver_profile, args.output / 'file-receiver', result, 'file-receiver') as receiver:
                seed_passwords(source, True)
                seed_passwords(receiver, False)
                source_documents = prepare_documents(source, base, True)
                receiver_documents = prepare_documents(receiver, base, False)
                source_tree, receiver_tree = open_transfer(source), open_transfer(receiver)
                check(result, 'Settings exposes file/direct choices and truthful one-time-transfer scope',
                    ui(source, 'return !!root.querySelector("#exportTransfer")&&!!root.querySelector("#startDirectSend")&&'
                        'root.textContent.includes("one-time move")&&root.textContent.includes("No Noventra account");'))
                selected(source, source_tree, inp, 'spaces', False)
                check(result, 'deselecting Spaces also deselects account tab transfer',
                    ui(source, 'return !component.categories_.spaces&&!component.categories_.tabs;'))
                selected(source, source_tree, inp, 'tabs', True)
                check(result, 'selecting account tabs includes their required Spaces',
                    ui(source, 'return component.categories_.spaces&&component.categories_.tabs;'))
                selected(source, source_tree, inp, 'passwords', True)
                fill(source, source_tree, inp, '#transferPassword', 'short')
                fill(source, source_tree, inp, '#repeatTransferPassword', 'short')
                check(result, 'short transfer passwords cannot start export',
                    ui(source, 'return root.querySelector("#exportTransfer").disabled;'))
                fill(source, source_tree, inp, '#transferPassword', PASSWORD)
                fill(source, source_tree, inp, '#repeatTransferPassword', PASSWORD + ' mismatch')
                check(result, 'password confirmation mismatch cannot start export',
                    ui(source, 'return root.querySelector("#exportTransfer").disabled;'))
                fill(source, source_tree, inp, '#repeatTransferPassword', PASSWORD)
                download_path = args.output / 'encrypted-downloads'
                with downloads(source, download_path):
                    click(source, source_tree, inp, '#exportTransfer')
                    wait_for(lambda: idle(source), source.process, timeout=30)
                    path = wait_for(lambda: next((p for p in download_path.glob('*.noventra')
                        if p.is_file() and not list(download_path.glob('*.crdownload'))), None), source.process)
                data = path.read_bytes()
                forbidden = [BOOKMARK_TITLE.encode(), b'/source-account', SOURCE_PASSWORD.encode(), NEW_PASSWORD.encode()]
                check(result, 'actual Settings download is bounded ciphertext without fixture plaintext',
                    64 <= len(data) <= 32 * 1024 * 1024 + 64
                    and all(marker not in data for marker in forbidden), {'bytes': len(data), 'sha256': sha(path)})
                check(result, 'export clears both password inputs after preparing the encrypted file',
                    ui(source, 'return !root.querySelector("#transferPassword").value&&!root.querySelector("#repeatTransferPassword").value;'))
                check(result, 'export preserves source documents and their unsaved drafts',
                    all(witness(source, value) for value in source_documents.values()))
                baseline = model(receiver, base)
                first = review_file(receiver, receiver_tree, inp, path)
                check(result, 'native file chooser unlocks a count-only review without importing',
                    first and first['bookmarks'] >= 1 and first['spaces'] == 2
                    and first['tabs'] >= 3 and first['pins'] >= 1
                    and first['password_counts']['new_passwords'] == 1
                    and first['password_counts']['kept_passwords'] == 1
                    and model(receiver, base) == baseline, first)
                check(result, 'local unpacked controller is excluded from extension reinstalls',
                    first['extensions'] == 0, {'extensions': first['extensions']})
                click(receiver, receiver_tree, inp, '#cancelTransferReview')
                wait_for(lambda: ui(receiver, 'return !component.review_;'), receiver.process)
                check(result, 'canceling the review leaves all receiving data unchanged', model(receiver, base) == baseline)
                invalid_dir = args.output / 'invalid-files'
                invalid_dir.mkdir()
                tampered = invalid_dir / 'tampered.noventra'
                altered = bytearray(data)
                altered[-1] ^= 1
                tampered.write_bytes(altered)
                truncated = invalid_dir / 'truncated.noventra'
                truncated.write_bytes(data[:-1])
                oversized = invalid_dir / 'oversized.noventra'
                with oversized.open('wb') as stream:
                    stream.truncate(32 * 1024 * 1024 + 65)
                for name, candidate, password in [('wrong password', path, 'Wrong Owned Password!'),
                        ('tampered ciphertext', tampered, PASSWORD), ('truncated ciphertext', truncated, PASSWORD),
                        ('oversized file', oversized, PASSWORD)]:
                    review = review_file(receiver, receiver_tree, inp, candidate, password)
                    check(result, name + ' is rejected before receiver mutation',
                        review is None and ui(receiver, 'return !!component.error_;')
                        and model(receiver, base) == baseline)
                review_file(receiver, receiver_tree, inp, path)
                for key in ('tabs', 'settings', 'extensions', 'passwords', 'spaces'):
                    selected(receiver, receiver_tree, inp, key, False, review=True)
                apply_review(receiver, receiver_tree, inp)
                bookmark_only = model(receiver, base)
                check(result, 'selected bookmarks-only import keeps receiver Spaces, tabs and settings',
                    len([row for row in bookmark_only['bookmarks'] if row['title'] == BOOKMARK_TITLE]) == 1
                    and bookmark_only['spaces'] == baseline['spaces'] and bookmark_only['tabs'] == baseline['tabs']
                    and bookmark_only['prefs'] == baseline['prefs'])
                check(result, 'bookmarks import retains preexisting receiver bookmark',
                    any(row['title'] == EXISTING_TITLE for row in bookmark_only['bookmarks']))
                initial_passwords = passwords_match(receiver, False)
                check(result, 'unselected passwords remain unchanged', initial_passwords['existing']
                    and not initial_passwords['added'] and initial_passwords['count'] == 1)
                review_file(receiver, receiver_tree, inp, path)
                capture(receiver, inp, args.output, 'file-import-review')
                layout_checks(receiver, result)
                apply_review(receiver, receiver_tree, inp)
                imported_state = model(receiver, base)
                imported_spaces = {row['name']: row for row in imported_state['spaces']}
                source_work = imported_spaces.get('Portable Work Café')
                source_archive = imported_spaces.get('Portable Archive 東京')
                check(result, 'Space import creates fresh local account identities and preserves receiver Space',
                    source_work and source_archive and source_work['id'] not in ('', WORK, ARCHIVE, RECEIVER_WORK)
                    and source_archive['id'] not in ('', WORK, ARCHIVE, RECEIVER_WORK, source_work['id'])
                    and any(row['id'] == RECEIVER_WORK for row in imported_state['spaces']))
                check(result, 'Space names, archive state, color and icon round-trip',
                    source_work['color'] == 'purple' and source_work['icon'] == 'code'
                    and not source_work['archived'] and source_archive['archived'])
                check_imported_space_themes(result, baseline, imported_state)
                check(result, 'pins and source tab URLs are restored without replacing original receiver tabs',
                    any(row['url'] == base + '/source-pin' and row['pinned'] for row in imported_state['tabs'])
                    and any(row['url'] == base + '/source-account' for row in imported_state['tabs'])
                    and {row['id'] for row in baseline['tabs']} <= {row['id'] for row in imported_state['tabs']})
                check(result, 'later category import does not duplicate already imported bookmarks',
                    len([row for row in imported_state['bookmarks'] if row['title'] == BOOKMARK_TITLE]) == 1)
                passwords = passwords_match(receiver, True)
                check(result, 'native password transfer adds the new login and keeps conflicting receiver password',
                    passwords['existing'] and passwords['added'] and passwords['count'] == 2, passwords)
                check(result, 'allowed appearance preferences and routed Space use restored local identity',
                    imported_state['prefs']['noventra.browser.glass_enabled'] is False
                    and imported_state['prefs']['noventra.browser.tab_density'] == 1
                    and imported_state['prefs']['noventra.browser.bookmark_opening'] == 1
                    and any(row['host'] == 'portable-route.invalid' and row['space_id'] == source_work['id']
                        for row in imported_state['prefs']['noventra.browser.workspace_routes']))
                check(result, 'receiving documents retain their live identity and unsaved drafts',
                    all(witness(receiver, value) for value in receiver_documents.values()))
                imported_account = next(row for row in imported_state['tabs'] if row['url'] == base + '/source-account')
                control(receiver, 'chrome.tabs.update(' + str(imported_account['id']) + ',{active:true})')
                account_page = wait_for(lambda: next((page for page in request(receiver.port, '/json/list')
                    if page['type'] == 'page' and page['url'] == base + '/source-account'), None), receiver.process)
                wait_for(lambda: evaluate(receiver, '!!window.portableToken', account_page), receiver.process)
                storage = evaluate(receiver, '({cookie:document.cookie,storage:localStorage.getItem("portableWitness")})', account_page)
                check(result, 'imported account pages start with a fresh cookie/storage partition',
                    storage == {'cookie': '', 'storage': None}, storage)
                open_transfer(receiver)
                receiver_tree = NativeUITree(receiver)
                after_first = model(receiver, base)
                review_file(receiver, receiver_tree, inp, path)
                apply_review(receiver, receiver_tree, inp)
                check(result, 'importing the identical file again is idempotent across categories',
                    model(receiver, base) == after_first and passwords_match(receiver, True)['count'] == 2)
                check(result, 'unrelated profile marker bytes remain unchanged',
                    receiver_profile.joinpath('owned-preserve-marker.bin').read_bytes() ==
                    b'owned profile unrelated data must be retained\x00\xff')
                imported_state = model(receiver, base)
                capture(receiver, inp, args.output, 'file-import-finished')
        with browser_session(args, receiver_profile, args.output / 'file-receiver-restart', result,
                             'file-receiver-restart', restore=True) as receiver:
            open_transfer(receiver)
            persisted = model(receiver, base)
            check(result, 'normal restart retains imported bookmarks and Space identities',
                persisted['bookmarks'] == imported_state['bookmarks']
                and persisted['spaces'] == imported_state['spaces'] and persisted['prefs'] == imported_state['prefs'])
            check(result, 'normal restart retains every imported/original tab URL and pin state',
                sorted((row['url'], row['pinned']) for row in persisted['tabs']) ==
                sorted((row['url'], row['pinned']) for row in imported_state['tabs']))
            saved_passwords = passwords_match(receiver, True)
            check(result, 'normal restart retains imported and preserved passwords',
                saved_passwords['existing'] and saved_passwords['added'] and saved_passwords['count'] == 2)
    finally:
        inp.close()


def mutate_connection_code(code, transform):
    import base64
    value = json.loads(base64.urlsafe_b64decode(code[4:] + '=' * ((-len(code[4:])) % 4)))
    transform(value)
    return 'NV1-' + base64.urlsafe_b64encode(json.dumps(value).encode()).decode().rstrip('=')


def peer_flow(args, result, base):
    source_profile, receiver_profile = args.output / 'peer-source-profile', args.output / 'peer-receiver-profile'
    seed_profile(source_profile, base, True)
    seed_profile(receiver_profile, base, False)
    inp = Input('x11')
    try:
        with browser_session(args, source_profile, args.output / 'peer-source', result, 'peer-source') as source:
            with browser_session(args, receiver_profile, args.output / 'peer-receiver', result, 'peer-receiver') as receiver:
                source_documents = prepare_documents(source, base, True)
                receiver_documents = prepare_documents(receiver, base, False)
                source_tree, receiver_tree = open_transfer(source), open_transfer(receiver)
                baseline = model(receiver, base)
                policies = {id(browser): control(browser,
                    'chrome.privacy.network.webRTCIPHandlingPolicy.get({}).then(value=>({value:value.value,control:value.levelOfControl}))')
                    for browser in (source, receiver)}
                for browser, tree in ((source, source_tree), (receiver, receiver_tree)):
                    if ui(browser, 'return root.querySelector("#transferStun").checked;'):
                        click(browser, tree, inp, '#transferStun')
                check(result, 'direct candidate flow uses no diagnostic RTC preference or external STUN',
                    not args.rtc_policy_diagnostic
                    and not ui(source, 'return component.useStun_;')
                    and not ui(receiver, 'return component.useStun_;'))
                click(source, source_tree, inp, '#startDirectSend')
                wait_for(lambda: ui(source, 'return component.peerState_?.phase==="waiting";'), source.process, timeout=25)
                offer = ui(source, 'return component.peerState_.code;')
                check(result, 'the sending browser creates a private connection code', offer.startswith('NV1-'))
                click(receiver, receiver_tree, inp, '#startDirectReceive')
                fill(receiver, receiver_tree, inp, '#incomingCode', 'invalid-owned-code')
                click(receiver, receiver_tree, inp, '#connectDirect')
                wait_for(lambda: ui(receiver, 'return !!component.error_;'), receiver.process)
                check(result, 'malformed connection code is rejected before importing', model(receiver, base) == baseline)
                expired = mutate_connection_code(offer, lambda value: value.update(expires=1))
                fill(receiver, receiver_tree, inp, '#incomingCode', expired)
                click(receiver, receiver_tree, inp, '#connectDirect')
                wait_for(lambda: ui(receiver, 'return !!component.error_;'), receiver.process)
                check(result, 'expired connection code is rejected before importing', model(receiver, base) == baseline)
                fill(receiver, receiver_tree, inp, '#incomingCode', offer)
                click(receiver, receiver_tree, inp, '#connectDirect')
                wait_for(lambda: ui(receiver, 'return !!component.peerState_?.code;'), receiver.process, timeout=25)
                answer = ui(receiver, 'return component.peerState_.code;')
                wrong_answer = mutate_connection_code(answer, lambda value: value.update(
                    session='ffffffff-ffff-4fff-8fff-ffffffffffff'))
                fill(source, source_tree, inp, '#incomingCode', wrong_answer)
                click(source, source_tree, inp, '#connectDirect')
                wait_for(lambda: ui(source, 'return !!component.error_;'), source.process)
                check(result, 'reply from another session is rejected before importing', model(receiver, base) == baseline)
                fill(source, source_tree, inp, '#incomingCode', answer)
                click(source, source_tree, inp, '#connectDirect')
                for browser in (source, receiver):
                    wait_for(lambda: ui(browser, 'return component.peerState_?.phase==="connected"&&!!component.peerState_.comparison;'),
                             browser.process, timeout=50)
                comparison = ui(source, 'return component.peerState_.comparison;')
                check(result, 'two real browser profiles establish a direct channel with matching device identities',
                    comparison == ui(receiver, 'return component.peerState_.comparison;'))
                check(result, 'starting direct Settings connections does not change the profile-wide RTC policy',
                    all(control(browser, 'chrome.privacy.network.webRTCIPHandlingPolicy.get({})'
                        '.then(value=>({value:value.value,control:value.levelOfControl}))') == policies[id(browser)]
                        for browser in (source, receiver)))
                ordinary = evaluate(receiver, '''(async()=>{
                  const peer=new RTCPeerConnection({iceServers:[]});let candidates=0;
                  try{peer.onicecandidate=event=>{if(event.candidate)candidates++};
                    peer.createDataChannel('owned-website-policy');
                    await peer.setLocalDescription(await peer.createOffer());
                    await new Promise(resolve=>{const timer=setTimeout(resolve,3000);
                      peer.onicegatheringstatechange=()=>{if(peer.iceGatheringState==='complete'){
                        clearTimeout(timer);resolve()}}});
                    return candidates;
                  }finally{peer.close()}})()''', receiver_documents['personal'][0])
                check(result, 'ordinary owned websites retain the original restricted host-candidate policy', ordinary == 0,
                      {'host_candidates': ordinary})
                check(result, 'no payload/import review exists before either device confirms',
                    ui(source, 'return root.querySelector("#sendDirectTransfer").disabled;')
                    and ui(receiver, 'return !component.review_;') and model(receiver, base) == baseline)
                click(source, source_tree, inp, '#confirmDirectDevice')
                wait_for(lambda: ui(receiver, 'return component.peerState_.remoteConfirmed;'), receiver.process)
                check(result, 'one-sided confirmation cannot send data',
                    ui(source, 'return root.querySelector("#sendDirectTransfer").disabled;')
                    and model(receiver, base) == baseline)
                click(receiver, receiver_tree, inp, '#confirmDirectDevice')
                wait_for(lambda: ui(source, 'return component.peerState_.remoteConfirmed;'), source.process)
                capture(source, inp, args.output, 'direct-confirmed-masked')
                click(source, source_tree, inp, '#sendDirectTransfer')
                wait_for(lambda: ui(receiver, 'return !!component.review_&&!component.busy_;'), receiver.process, timeout=45)
                wait_for(lambda: ui(source, 'return component.peerState_.phase==="sent";'), source.process, timeout=15)
                check(result, 'actual direct payload arrives as the same native count-only review before mutation',
                    ui(receiver, 'return component.review_.bookmarks>=1&&component.review_.spaces===2&&component.review_.tabs>=3;')
                    and model(receiver, base) == baseline)
                capture(receiver, inp, args.output, 'direct-receive-review-masked')
                apply_review(receiver, receiver_tree, inp)
                imported = model(receiver, base)
                check(result, 'direct native import preserves receiver work and restores the chosen data',
                    any(row['title'] == BOOKMARK_TITLE for row in imported['bookmarks'])
                    and any(row['title'] == EXISTING_TITLE for row in imported['bookmarks'])
                    and any(row['name'] == 'Portable Work Café' and row['id'] != WORK for row in imported['spaces'])
                    and any(row['url'] == base + '/source-pin' and row['pinned'] for row in imported['tabs'])
                    and all(witness(receiver, value) for value in receiver_documents.values()))
                check_imported_space_themes(result, baseline, imported)
                check(result, 'direct export preserves the source live documents and drafts',
                    all(witness(source, value) for value in source_documents.values()))
                def closed_state(browser, phases):
                    state = ui(browser, '''return {phase:component.peerState_?.phase,
                      code:!!component.peerState_?.code,password:!!component.peer_?.password,
                      connection:!!component.peer_?.pc,channel:!!component.peer_?.channel};''')
                    return state if state['phase'] in phases and not any(
                        state[key] for key in ('code', 'password', 'connection', 'channel')) else None
                click(source, source_tree, inp, '#closeDirectTransfer')
                sender_closed = wait_for(lambda: closed_state(source, ('closed',)), source.process, timeout=5)
                check(result, 'disconnect clears private connection material on sender',
                    bool(sender_closed), sender_closed)
                # The sender's native disconnect can close the remote channel
                # before a second button click. Require complete remote cleanup,
                # rather than demanding a control after it has been removed.
                if ui(receiver, '''const button=root.querySelector('#closeDirectTransfer');
                    return !!button&&!button.disabled&&button.checkVisibility();'''):
                    try:
                        click(receiver, receiver_tree, inp, '#closeDirectTransfer')
                    except RuntimeError:
                        # A remote close may race the availability observation.
                        # Tolerate only the observed terminal, fully cleared state.
                        if not closed_state(receiver, ('closed', 'error')):
                            raise
                receiver_closed = wait_for(lambda: closed_state(receiver, ('closed', 'error')),
                    receiver.process, timeout=5)
                check(result, 'disconnect clears private connection material on receiver',
                    bool(receiver_closed), receiver_closed)
                check(result, 'disconnect keeps completed imported data and offers encrypted-file fallback',
                    model(receiver, base) == imported
                    and ui(receiver, 'return !!root.querySelector("#chooseTransfer")&&!root.querySelector("#chooseTransfer").disabled;'))
                check(result, 'disconnect retains both browsers live documents and unsaved drafts',
                    all(witness(source, value) for value in source_documents.values())
                    and all(witness(receiver, value) for value in receiver_documents.values()))
                check(result, 'disconnect leaves the original profile-wide RTC policy unchanged',
                    all(control(browser, 'chrome.privacy.network.webRTCIPHandlingPolicy.get({})'
                        '.then(value=>({value:value.value,control:value.levelOfControl}))') == policies[id(browser)]
                        for browser in (source, receiver)))
    finally:
        inp.close()


def harness_preflight(args, result, base):
    """Validate API seeding/model helpers before a new candidate is available."""
    source_profile, receiver_profile = args.output / 'harness-source-profile', args.output / 'harness-receiver-profile'
    seed_profile(source_profile, base, True)
    seed_profile(receiver_profile, base, False)
    with browser_session(args, source_profile, args.output / 'harness-source', result, 'harness-source') as source:
        with browser_session(args, receiver_profile, args.output / 'harness-receiver', result, 'harness-receiver') as receiver:
            seed_passwords(source, True)
            seed_passwords(receiver, False)
            source_docs = prepare_documents(source, base, True)
            receiver_docs = prepare_documents(receiver, base, False)
            observed = model(source, base)
            check(result, 'owned source seed exposes two account Spaces, bookmark and actual pin',
                len(observed['spaces']) == 3
                and any(row['title'] == BOOKMARK_TITLE for row in observed['bookmarks'])
                and any(row['url'] == base + '/source-pin' and row['pinned'] for row in observed['tabs']))
            check(result, 'owned source and receiver keep independent local credential fixtures',
                passwords_match(receiver, False)['existing'] and not passwords_match(receiver, False)['added'])
            check(result, 'owned fixture documents have stable live tokens and unsaved drafts',
                all(witness(source, value) for value in source_docs.values())
                and all(witness(receiver, value) for value in receiver_docs.values()))


def explicit_policy_flow(args, result, base):
    profile = args.output / 'policy-profile'
    seed_profile(profile, base, False)
    path = profile / 'Default/Preferences'
    preferences = json.loads(path.read_text())
    preferences['webrtc'] = {'ip_handling_policy': 'default_public_interface_only'}
    write(path, preferences)
    inp = Input('x11')
    try:
        with browser_session(args, profile, args.output / 'policy-browser', result, 'explicit-policy') as browser:
            documents = prepare_documents(browser, base, False)
            tree = open_transfer(browser)
            baseline = model(browser, base)
            before = control(browser, 'chrome.privacy.network.webRTCIPHandlingPolicy.get({})'
                '.then(value=>({value:value.value,control:value.levelOfControl}))')
            if ui(browser, 'return root.querySelector("#transferStun").checked;'):
                click(browser, tree, inp, '#transferStun')
            click(browser, tree, inp, '#startDirectSend')
            # Honoring an explicit policy does not require rejecting signaling.
            # With STUN off in this private LAN, public-only handling can create
            # an offer, but must not expose local ICE candidates or send data.
            wait_for(lambda: ui(browser, 'return !!component.error_ || '
                '["waiting","error","closed"].includes(component.peerState_?.phase);'),
                browser.process, timeout=25)
            observation = ui(browser, '''const state=component.peerState_;
              const sdp=component.peer_?.pc?.localDescription?.sdp||'';
              const send=root.querySelector('#sendDirectTransfer');
              return {phase:state?.phase||'error',candidates:(sdp.match(/^a=candidate:/gm)||[]).length,
                approved:!!(state?.localConfirmed||state?.remoteConfirmed),
                sendAvailable:!!send&&!send.disabled,review:!!component.review_};''')
            check(result, 'direct transfer respects an explicitly configured restrictive RTC policy',
                control(browser, 'chrome.privacy.network.webRTCIPHandlingPolicy.get({})'
                    '.then(value=>({value:value.value,control:value.levelOfControl}))') == before
                and observation['candidates'] == 0 and not observation['approved']
                and not observation['sendAvailable'] and not observation['review'], observation)
            check(result, 'unapproved policy-limited connection leaves existing data and documents unchanged',
                model(browser, base) == baseline
                and all(witness(browser, value) for value in documents.values()))
            if ui(browser, '''const close=root.querySelector('#closeDirectTransfer');
              return !!close&&!close.disabled&&close.checkVisibility();'''):
                click(browser, tree, inp, '#closeDirectTransfer')
            check(result, 'encrypted-file fallback stays available after policy-limited direct disconnect',
                call(browser, 'status')['available']
                and not ui(browser, 'return root.querySelector("#chooseTransfer").disabled;')
                and control(browser, 'chrome.privacy.network.webRTCIPHandlingPolicy.get({})'
                    '.then(value=>({value:value.value,control:value.levelOfControl}))') == before)
    finally:
        inp.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('rtc-preflight', 'harness-preflight', 'file', 'peer', 'policy', 'all'), default='all')
    parser.add_argument('--inside-owned-home', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--rtc-policy-diagnostic', action='store_true',
                        help='Preflight only: owned profile uses default RTC IP handling')
    args = parser.parse_args()
    assert not args.rtc_policy_diagnostic or args.case == 'rtc-preflight'
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    owner = os.environ.get('NOVENTRA_BENCH_XVFB_PID', '')
    assert owner.isdigit() and Path('/proc', owner, 'comm').read_text().strip() == 'Xvfb'
    assert Path(os.environ['XAUTHORITY']).resolve().is_relative_to(ROOT / 'work/ram-investigation')
    isolate(args)
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(__file__, args.output / 'executed-test.py')
    helpers = {}
    imported = {Path(module.__file__).resolve() for module in sys.modules.values()
        if getattr(module, '__file__', None) and str(module.__file__).endswith('.py')}
    imported.add(ROOT / 'tests/noventra/cdp_eval.mjs')
    for path in sorted(imported):
        if not path.is_relative_to(ROOT / 'tests/noventra') or path == Path(__file__).resolve():
            continue
        name = str(path.relative_to(ROOT / 'tests/noventra'))
        helpers[name] = sha(path)
        copy = args.output / 'executed-modules' / name
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, copy)
    result = {'passed': False, 'checks': [], 'normal_shutdowns': [],
        'runtime': str(args.browser), 'binary_sha256': args.expected_binary,
        'resources_sha256': args.expected_resources, 'test_sha256': sha(__file__),
        'helpers_sha256': helpers, 'case': args.case, 'home_isolated': True,
        'rtc_policy_diagnostic': args.rtc_policy_diagnostic,
        'network_isolated_no_egress': True, 'external_network_requested': False,
        'credential_seed': 'Existing user-level addPassword API on owned fixture only',
        'scope': 'Linux software X11; isolated synthetic LAN, not physical-device or relayed connectivity'}
    started = time.monotonic()
    def exhausted(*_):
        raise TimeoutError('Portable transfer fixture exceeded its bounded work budget')
    previous = signal.signal(signal.SIGALRM, exhausted)
    signal.setitimer(signal.ITIMER_REAL, 900)
    try:
        if args.case == 'rtc-preflight':
            profile = args.output / 'preflight-profile'
            if args.rtc_policy_diagnostic:
                (profile / 'Default').mkdir(parents=True)
                write(profile / 'Default/Preferences', {'webrtc': {'ip_handling_policy': 'default'}})
            with browser_session(args, profile, args.output / 'preflight', result, 'rtc-preflight') as browser:
                rtc_preflight(browser, result)
        else:
            with pages() as base:
                if args.case == 'harness-preflight':
                    harness_preflight(args, result, base)
                if args.case in ('file', 'all'):
                    file_flow(args, result, base)
                if args.case in ('peer', 'all'):
                    peer_flow(args, result, base)
                if args.case in ('policy', 'all'):
                    explicit_policy_flow(args, result, base)
        result['passed'] = True
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)
        result['elapsed_seconds'] = time.monotonic() - started
        result['source_unchanged'] = sha(__file__) == result['test_sha256']
        result['helpers_unchanged'] = all(sha(ROOT / 'tests/noventra' / name) == value for name, value in helpers.items())
        write(args.output / 'result.json', result)
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks']),
                      'normal_shutdowns': len(result['normal_shutdowns'])}))


if __name__ == '__main__':
    main()
