"""Real native Incognito Spaces: one OS window, OTR storage and last-tab cleanup.

Run with the private Xvfb wrapper and Pillow environment. This fixture further
isolates HOME/network via the established no-egress namespace adapter. --case
create is an early widget/focus/lifecycle check; lifecycle adds two independent
private jars, ordinary shortcuts, host resizing, empty regular model and restart
privacy checks. No personal profile or desktop session is driven.
"""
import argparse
import ctypes as C
import http.server
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import threading
import time
import urllib.parse

from account_spaces_smoke import Handler, READ, write_state
from compact_chrome_smoke import FLAGS, type_text
from dock_rail_regressions_smoke import Input
from extension_smoke import ROOT, request, wait_for
from interface_polish_smoke import native_key
from layout_test_support import LayoutBrowser, apply_native_test_layout
from native_ui_tree import NativeUITree
from onboarding_managers_smoke import normal_close
from portable_transfer_smoke import isolate, sha, write
from theme_tab_contrast_smoke import WindowAttributes


class PrivateHandler(Handler):
    """Keep a real worker busy so Space closure proves it was stopped."""
    def do_GET(self):
        path = urllib.parse.urlsplit(self.path)
        if path.path == '/worker-ping':
            label = urllib.parse.parse_qs(path.query)['space'][0]
            with self.server.heartbeat_lock:
                self.server.heartbeats[label] = self.server.heartbeats.get(label, 0) + 1
            data, kind = b'ok', 'text/plain'
        elif path.path == '/live-worker.js':
            data = b'''self.addEventListener('install',()=>self.skipWaiting());
              self.addEventListener('activate',e=>e.waitUntil(self.clients.claim()));
              self.addEventListener('message',e=>e.waitUntil((async()=>{
                const end=Date.now()+240000;
                while(Date.now()<end){
                  await fetch('/worker-ping?space='+encodeURIComponent(e.data));
                  await new Promise(r=>setTimeout(r,250));
                }
              })()));'''
            kind = 'application/javascript'
        else:
            return super().do_GET()
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def heartbeats(server, label):
    with server.heartbeat_lock:
        return server.heartbeats.get(label, 0)


def start_worker(browser, target, server, label):
    browser.ev('''(async()=>{
      const r=await navigator.serviceWorker.register('/live-worker.js');
      await navigator.serviceWorker.ready;
      await new Promise(resolve=>{
        const ready=()=>r.active?.state==='activated' &&
          r.active.scriptURL.endsWith('/live-worker.js') ? resolve() : setTimeout(ready,50);
        ready();
      });
      r.active.postMessage(LABEL);return true;
    })()'''.replace('LABEL', json.dumps(label)), target)
    wait_for(lambda: heartbeats(server, label) >= 3, browser.process)


def check(result, name, condition):
    result['checks'].append({'name': name, 'passed': bool(condition)})
    assert condition, name


def owned_windows(browser, inp):
    """Enumerate actual mapped OS top-level windows, not BrowserList objects."""
    x, d = inp.pointer.x, inp.pointer.d
    x.XDefaultRootWindow.argtypes = [C.c_void_p]
    x.XDefaultRootWindow.restype = C.c_ulong
    x.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    x.XInternAtom.restype = C.c_ulong
    x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
        C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
    x.XGetWindowProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong,
        C.c_long, C.c_long, C.c_int, C.c_ulong, C.POINTER(C.c_ulong),
        C.POINTER(C.c_int), C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
        C.POINTER(C.POINTER(C.c_ubyte))]
    x.XGetWindowAttributes.argtypes = [C.c_void_p, C.c_ulong,
                                     C.POINTER(WindowAttributes)]
    x.XFree.argtypes = [C.c_void_p]
    root, parent, children, count = (C.c_ulong(), C.c_ulong(),
                                    C.POINTER(C.c_ulong)(), C.c_uint())
    assert x.XQueryTree(d, x.XDefaultRootWindow(d), C.byref(root),
                       C.byref(parent), C.byref(children), C.byref(count))
    atom = x.XInternAtom(d, b'_NET_WM_PID', 0)
    found = []
    try:
        candidates = [children[i] for i in range(count.value)]
        # A real WM may reparent browser client windows inside frame windows.
        # EWMH lists actual OS clients, including those reparented clients.
        clients = C.POINTER(C.c_ubyte)()
        actual, fmt, total, after = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
        x.XGetWindowProperty(d, root.value, x.XInternAtom(d, b'_NET_CLIENT_LIST', 0),
            0, 4096, 0, 0, C.byref(actual), C.byref(fmt), C.byref(total),
            C.byref(after), C.byref(clients))
        try:
            if fmt.value == 32 and total.value:
                values = C.cast(clients, C.POINTER(C.c_ulong))
                candidates = [values[i] for i in range(total.value)]
        finally:
            if clients:
                x.XFree(clients)
        for candidate in candidates:
            actual, fmt, total, after = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
            data = C.POINTER(C.c_ubyte)()
            x.XGetWindowProperty(d, candidate, atom, 0, 1, 0, 0,
                C.byref(actual), C.byref(fmt), C.byref(total), C.byref(after),
                C.byref(data))
            try:
                if not total.value or C.cast(data, C.POINTER(C.c_ulong))[0] != browser.process.pid:
                    continue
                attrs = WindowAttributes()
                assert x.XGetWindowAttributes(d, candidate, C.byref(attrs))
                if attrs.map_state == 2 and not attrs.override_redirect:
                    found.append(int(candidate))
            finally:
                if data:
                    x.XFree(data)
    finally:
        if children:
            x.XFree(children)
    return sorted(found)


def x_focus(inp):
    x, display = inp.pointer.x, inp.pointer.d
    x.XGetInputFocus.argtypes = [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)]
    window, revert = C.c_ulong(), C.c_int()
    x.XGetInputFocus(display, C.byref(window), C.byref(revert))
    return window.value


def activate_x_window(inp, window, process):
    """Explicit initial setup or owned user focus change, never modal recovery."""
    x, display = inp.pointer.x, inp.pointer.d
    def viewable():
        attrs = WindowAttributes()
        return bool(x.XGetWindowAttributes(display, window, C.byref(attrs)) and
                    attrs.map_state == 2)
    # A WM handles MapRequest/reparenting asynchronously. X_SetInputFocus on an
    # unmapped/reparenting client is a fatal Xlib BadMatch, not a browser bug.
    wait_for(viewable, process, timeout=5)
    has_wm = private_window_manager(inp)
    if has_wm:
        class Data(C.Union):
            _fields_ = [('b', C.c_char * 20), ('s', C.c_short * 10), ('l', C.c_long * 5)]
        class Message(C.Structure):
            _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                ('display', C.c_void_p), ('window', C.c_ulong), ('message_type', C.c_ulong),
                ('format', C.c_int), ('data', Data)]
        class Event(C.Union):
            _fields_ = [('client', Message), ('pad', C.c_long * 24)]
        event = Event()
        event.client.type, event.client.send_event = 33, 1
        event.client.display, event.client.window = display, window
        event.client.message_type = x.XInternAtom(display, b'_NET_ACTIVE_WINDOW', 0)
        event.client.format = 32
        event.client.data.l[0] = 1
        x.XSendEvent.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.POINTER(Event)]
        assert x.XSendEvent(display, x.XDefaultRootWindow(display), 0,
                            (1 << 20) | (1 << 19), C.byref(event))
    else:
        x.XRaiseWindow.argtypes = [C.c_void_p, C.c_ulong]
        x.XSetInputFocus.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong]
        x.XRaiseWindow(display, window)
        x.XSetInputFocus(display, window, 2, 0)
    x.XFlush(display)
    wait_for(lambda: x_focus(inp) == window, process, timeout=5)
    if has_wm:
        def active_client():
            actual, fmt, count, after = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
            data = C.POINTER(C.c_ubyte)()
            x.XGetWindowProperty(display, x.XDefaultRootWindow(display),
                x.XInternAtom(display, b'_NET_ACTIVE_WINDOW', 0), 0, 1, 0, 0,
                C.byref(actual), C.byref(fmt), C.byref(count), C.byref(after), C.byref(data))
            try:
                return bool(fmt.value == 32 and count.value and
                            C.cast(data, C.POINTER(C.c_ulong))[0] == window)
            finally:
                if data:
                    x.XFree(data)
        wait_for(active_client, process, timeout=5)


class PositionedNativeUITree(NativeUITree):
    """Native browser geometry calibrated to its real reparented X11 client."""
    def __init__(self, browser, inp):
        windows = owned_windows(browser, inp)
        assert len(windows) == 1, windows
        self.inp, self.window = inp, windows[0]
        self.bound_browser_session = None
        super().__init__(browser)

    def bind_browser(self, target):
        self.bound_browser_session = cdp(self.browser, 'Browser.getWindowForTarget',
            {'targetId': target['id']})['windowId']

    def unbind_browser(self):
        self.bound_browser_session = None

    def read(self):
        if self.bound_browser_session is None:
            return super().read()
        self.text = self.browser.ev('''(async()=>{
          const {addWebUiListener,removeWebUiListener}=await import('chrome://resources/js/cr.js');
          return new Promise((resolve,reject)=>{
            const timer=setTimeout(()=>reject(Error('Native tree timed out')),5000);
            const listener=addWebUiListener('showOrRefreshTree', result=>{
              clearTimeout(timer);removeWebUiListener(listener);resolve(result.tree);
            });
            chrome.send('requestNativeUITree',[{sessionId:SESSION_ID,
              requestType:'showOrRefreshTree',filters:{allow:'*',allowEmpty:'',deny:''}}]);
          });
        })()'''.replace('SESSION_ID', str(self.bound_browser_session)), self.target)
        # NativeUITree has no separate parser API. Keep its parsing semantics
        # here for this explicitly owned Browser, without changing that helper.
        self.nodes = {}
        for line in self.text.splitlines():
            identifier = re.match(r'^\+*id=(\d+) (?:\w+ )+class_name=', line)
            bounds = re.search(r'\((-?\d+), (-?\d+)\)-\((\d+), (\d+)\)', line)
            parent = re.search(r'offset_container_id=(\d+)', line)
            if 'class_name=' in line and identifier and bounds:
                self.nodes[int(identifier[1])] = {
                    'line': line, 'bounds': tuple(map(int, bounds.groups())),
                    'parent': int(parent[1]) if parent else None,
                }
        return self

    def rect(self, node):
        rect = super().rect(node)
        current = node
        while current and 'class_name=BrowserRootView ' not in current['line']:
            current = self.nodes.get(current['parent'])
        if current:
            x, display = self.inp.pointer.x, self.inp.pointer.d
            x.XTranslateCoordinates.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong,
                C.c_int, C.c_int, C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_ulong)]
            left, top, child = C.c_int(), C.c_int(), C.c_ulong()
            assert x.XTranslateCoordinates(display, self.window, x.XDefaultRootWindow(display),
                0, 0, C.byref(left), C.byref(top), C.byref(child))
            rect['x'] += left.value - current['bounds'][0]
            rect['y'] += top.value - current['bounds'][1]
        return rect


def assert_native_address_entry(browser, tree, inp, result, label, marker):
    native_key(inp, 'l', ('Control_L',))
    for letter in marker:
        native_key(inp, letter)
    tree.read()
    check(result, label, any(' INVISIBLE ' not in node['line'] and
        'class_name=OmniboxViewViews ' in node['line'] and
        re.search(r'\bvalue=' + re.escape(marker) + r'(?:\s|$)', node['line'])
        for node in tree.nodes.values()))
    native_key(inp, 'Escape')


def native_js_modal(tree, message):
    tree.read()
    return any(' INVISIBLE ' not in node['line'] and
        'class_name=Label name=' + message in node['line']
        for node in tree.nodes.values())


class PageSession:
    """Keep Page enabled across a real dialog for background-dismissal probes."""
    SCRIPT = '''import{createInterface}from'node:readline';
      const url=new URL(process.argv[1]);
      if(url.protocol!=='ws:'||url.hostname!=='127.0.0.1')throw Error('Owned endpoint required');
      const socket=new WebSocket(url),pending=new Map();let next=0;
      socket.onmessage=e=>{const m=JSON.parse(e.data),p=pending.get(m.id);
        if(p){pending.delete(m.id);clearTimeout(p.timer);m.error?p.reject(m.error):p.resolve(m.result)}};
      await new Promise((r,j)=>{socket.onopen=r;socket.onerror=j});
      process.stdout.write('ready\\n');
      for await(const line of createInterface({input:process.stdin})){
        const request=JSON.parse(line),id=++next;
        try{const result=await new Promise((resolve,reject)=>{
          const timer=setTimeout(()=>reject(Error('Protocol timeout')),20000);
          pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,...request}))});
          process.stdout.write(JSON.stringify({result})+'\\n');
        }catch(error){process.stdout.write(JSON.stringify({error:String(error.message||error)})+'\\n')}
      }socket.close();'''

    def __init__(self, target):
        self.process = subprocess.Popen(['node', '--input-type=module', '-e', self.SCRIPT,
            target['webSocketDebuggerUrl']], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        assert self.process.stdout.readline().strip() == 'ready'
        self.call('Page.enable')

    def call(self, method, params=None):
        self.process.stdin.write(json.dumps({'method': method, 'params': params or {}}) + '\n')
        self.process.stdin.flush()
        result = json.loads(self.process.stdout.readline())
        assert 'error' not in result, result
        return result['result']

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=3)


class OwnedFocusClient:
    def __init__(self, inp):
        self.inp = inp
        x, display = inp.pointer.x, inp.pointer.d
        x.XCreateSimpleWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int,
            C.c_uint, C.c_uint, C.c_uint, C.c_ulong, C.c_ulong]
        x.XCreateSimpleWindow.restype = C.c_ulong
        x.XStoreName.argtypes = [C.c_void_p, C.c_ulong, C.c_char_p]
        x.XMapWindow.argtypes = [C.c_void_p, C.c_ulong]
        x.XChangeProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong,
            C.c_ulong, C.c_int, C.c_int, C.POINTER(C.c_ubyte), C.c_int]
        self.window = x.XCreateSimpleWindow(display, x.XDefaultRootWindow(display),
                                           1220, 780, 40, 12, 0, 0, 0)
        assert self.window
        x.XStoreName(display, self.window, b'Owned external focus witness')
        pid = (C.c_ulong * 1)(os.getpid())
        x.XChangeProperty(display, self.window, x.XInternAtom(display, b'_NET_WM_PID', 0),
            x.XInternAtom(display, b'CARDINAL', 0), 32, 0,
            C.cast(pid, C.POINTER(C.c_ubyte)), 1)
        x.XMapWindow(display, self.window)
        x.XFlush(display)

    def close(self):
        x, display = self.inp.pointer.x, self.inp.pointer.d
        x.XDestroyWindow.argtypes = [C.c_void_p, C.c_ulong]
        x.XDestroyWindow(display, self.window)
        x.XFlush(display)


def modal_focus_guards(browser, tree, inp, result, private, windows):
    session = PageSession(private)
    helper = None
    try:
        message = 'Owned inactive-window modal guard'
        browser.ev('window.modalGuardDone=false;setTimeout(()=>{confirm(' +
            json.dumps(message) + ');window.modalGuardDone=true},0);true', private)
        wait_for(lambda: native_js_modal(tree, message), browser.process, timeout=8)
        helper = OwnedFocusClient(inp)
        activate_x_window(inp, helper.window, browser.process)
        session.call('Page.handleJavaScriptDialog', {'accept': False})
        wait_for(lambda: browser.ev('window.modalGuardDone', private), browser.process)
        wait_for(lambda: not native_js_modal(tree, message), browser.process)
        time.sleep(.35)
        check(result, 'Dismissing a private modal while an owned external client is active does not reclaim OS focus',
              x_focus(inp) == helper.window and not browser.ev('document.hasFocus()', private))
        # Explicit user return occurs only after the no-focus-steal assertion.
        activate_x_window(inp, windows[0], browser.process)
        helper.close()
        helper = None
        message = 'Owned changed-Space modal guard'
        browser.ev('window.modalGuardDone=false;setTimeout(()=>{confirm(' +
            json.dumps(message) + ');window.modalGuardDone=true},0);true', private)
        wait_for(lambda: native_js_modal(tree, message), browser.process, timeout=8)
        # The existing Settings command changes the real catalog while the
        # original private prompt is pending; no production test hook is used.
        browser.call('noventraSpaces', 'switch', '', '')
        session.call('Page.handleJavaScriptDialog', {'accept': False})
        wait_for(lambda: browser.ev('window.modalGuardDone', private), browser.process)
        wait_for(lambda: not native_js_modal(tree, message), browser.process)
        check(result, 'A modal close cannot reselect its old private Space after a real catalog switch',
              not browser.ev('document.visibilityState==="visible"', private) and
              any('class_name=AccountSpaceDotButton name=Workspace: Personal name_from=' in node['line']
                  and 'selected=true' in node['line'] for node in tree.nodes.values()))
        # Activation-order diagnostics may still list the hidden private Browser
        # first. Bind this assertion and the next click to the real regular tab.
        tree.bind_browser(browser.settings)
        assert_native_address_entry(browser, tree, inp, result,
            'The regular presentation retains native shortcuts after the private modal closes', 'qregular')
        select_dot(browser, tree, inp, 'Incognito')
        tree.unbind_browser()
    finally:
        if helper is not None:
            helper.close()
        session.close()


def cdp(browser, method, params=None):
    endpoint = request(browser.port, '/json/version')['webSocketDebuggerUrl']
    output = subprocess.check_output(['node', str(ROOT / 'tests/noventra/layout_editor_input.mjs'),
        endpoint, json.dumps([{'method': method, 'params': params or {}, 'returnValue': True}])],
        text=True, timeout=25)
    return json.loads(output)


def close_os_window(inp, window):
    class Data(C.Union):
        _fields_ = [('b', C.c_char * 20), ('s', C.c_short * 10), ('l', C.c_long * 5)]
    class Message(C.Structure):
        _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
            ('display', C.c_void_p), ('window', C.c_ulong), ('message_type', C.c_ulong),
            ('format', C.c_int), ('data', Data)]
    class Event(C.Union):
        _fields_ = [('client', Message), ('pad', C.c_long * 24)]
    x, display = inp.pointer.x, inp.pointer.d
    event = Event()
    event.client.type = 33
    event.client.send_event = 1
    event.client.display = display
    event.client.window = window
    event.client.message_type = x.XInternAtom(display, b'WM_PROTOCOLS', 0)
    event.client.format = 32
    event.client.data.l[0] = x.XInternAtom(display, b'WM_DELETE_WINDOW', 0)
    x.XSendEvent.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.POINTER(Event)]
    assert x.XSendEvent(display, window, 0, 0, C.byref(event))
    x.XFlush(display)


def focus_round_trip(inp, window, process):
    """Use an owned helper window to generate genuine desktop blur/focus."""
    helper = OwnedFocusClient(inp)
    try:
        activate_x_window(inp, helper.window, process)
        activate_x_window(inp, window, process)
    finally:
        helper.close()


def pages(browser):
    return [p for p in request(browser.port, '/json/list') if p['type'] == 'page']


def goto(browser, target, url):
    steps = [{'method': 'Page.navigate', 'params': {'url': url}, 'returnValue': True}]
    subprocess.check_output(['node', str(ROOT / 'tests/noventra/layout_editor_input.mjs'),
        target['webSocketDebuggerUrl'], json.dumps(steps)], text=True, timeout=25)
    wait_for(lambda: browser.ev('location.href===' + json.dumps(url) +
        ' && document.readyState==="complete"', target), browser.process)


def new_native_tab(browser, inp, *, private_space=False):
    before = {p['id'] for p in pages(browser)}
    native_key(inp, 'n' if private_space else 't',
               ('Control_L', 'Shift_L') if private_space else ('Control_L',))
    return wait_for(lambda: next((p for p in pages(browser)
        if p['id'] not in before), None), browser.process)


def select_dot(browser, tree, inp, name):
    tree.read()
    matches = [n for n in tree.nodes.values() if ' INVISIBLE ' not in n['line']
        and re.search(r'class_name=AccountSpaceDotButton name=Workspace: ' +
                      re.escape(name) + r' name_from=', n['line'])
        and n['bounds'][2] and n['bounds'][3]]
    assert len(matches) == 1, (name, tree.text)
    inp.pointer.click(*tree.center(matches[0]))
    time.sleep(.35)


def capture(tree, inp, output, name):
    tree.read()
    (output / (name + '.txt')).write_text(tree.text)
    inp.capture(output / (name + '.png')).close()


def click_caption(tree, inp, name):
    matches = [node for node in tree.read().nodes.values()
        if ' INVISIBLE ' not in node['line'] and
        re.search(r'class_name=(?:FrameCaptionButton|ImageButton) name=' +
                  re.escape(name) + r' name_from=', node['line']) and
        node['bounds'][2] and node['bounds'][3]]
    assert len(matches) == 1, (name, tree.text)
    inp.pointer.click(*tree.center(matches[0]))


def beforeunload_dialog(tree):
    tree.read()
    dialogs = [(identifier, node) for identifier, node in tree.nodes.items()
        if ' INVISIBLE ' not in node['line'] and
        re.search(r'\bdialog class_name=RootView name=Leave site\? name_from=', node['line'])
        and node['bounds'][2] and node['bounds'][3]]
    if not dialogs:
        return None
    assert len(dialogs) == 1, tree.text
    identifier, dialog = dialogs[0]
    def belongs(node):
        while node:
            if node is dialog:
                return True
            node = tree.nodes.get(node['parent'])
        return False
    buttons = [node for node in tree.nodes.values()
        if ' INVISIBLE ' not in node['line'] and belongs(node) and
        node['bounds'][2] and node['bounds'][3]]
    assert all(any(re.search(r'\bbutton .*name=' + name + r' name_from=', node['line'])
                   for node in buttons) for name in ['Cancel', 'Leave']), tree.text
    return {'id': identifier, 'native_root': dialog['line']}


def native_tab(tree, title):
    matches = [node for node in tree.read().nodes.values()
        if ' INVISIBLE ' not in node['line'] and
        re.search(r'class_name=Tab name=' + re.escape(title) + r' name_from=', node['line'])
        and node['bounds'][2] and node['bounds'][3]]
    assert len(matches) == 1, (title, tree.text)
    return matches[0]


def modified_click(inp, point, modifier):
    p = inp.pointer
    code = p.x.XKeysymToKeycode(p.d, p.x.XStringToKeysym(modifier.encode()))
    assert code
    p.xt.XTestFakeKeyEvent(p.d, code, 1, 0)
    p.x.XFlush(p.d)
    try:
        p.click(*point)
    finally:
        p.xt.XTestFakeKeyEvent(p.d, code, 0, 0)
        p.x.XFlush(p.d)


def private_window_manager(inp):
    """The display guard already restricts this to our private Xvfb socket."""
    p = inp.pointer
    atom = p.x.XInternAtom(p.d, b'_NET_SUPPORTING_WM_CHECK', 0)
    actual, fmt, total, after = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
    data = C.POINTER(C.c_ubyte)()
    p.x.XGetWindowProperty(p.d, p.x.XDefaultRootWindow(p.d), atom, 0, 1, 0, 0,
        C.byref(actual), C.byref(fmt), C.byref(total), C.byref(after), C.byref(data))
    try:
        return bool(total.value and fmt.value == 32 and
                    C.cast(data, C.POINTER(C.c_ulong))[0])
    finally:
        if data:
            p.x.XFree(data)


def address_key_batch(inp, chords):
    """Balanced real XTest events, acknowledged without per-key sleeps."""
    assert inp.platform == 'x11'
    p = inp.pointer
    p.x.XStringToKeysym.argtypes = [C.c_char_p]
    p.x.XStringToKeysym.restype = C.c_ulong
    p.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
    p.x.XKeysymToKeycode.restype = C.c_uint
    p.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    p.x.XSync.argtypes = [C.c_void_p, C.c_int]
    events = []
    for key, modifiers in chords:
        sequence = [(modifier, True) for modifier in modifiers]
        sequence += [(key, True), (key, False)]
        sequence += [(modifier, False) for modifier in reversed(modifiers)]
        for name, down in sequence:
            code = p.x.XKeysymToKeycode(p.d, p.x.XStringToKeysym(name.encode()))
            assert code, name
            events.append({'name': name, 'code': int(code), 'down': down})
    started = time.monotonic()
    for event in events:
        assert p.xt.XTestFakeKeyEvent(p.d, event['code'], int(event['down']), 0)
    # XSync confirms the server processed every real key request. No view or
    # compositor focus command and no CDP input is part of this batch.
    p.x.XSync(p.d, False)
    finished = time.monotonic()
    p.x.XQueryKeymap.argtypes = [C.c_void_p, C.POINTER(C.c_ubyte)]
    keymap = (C.c_ubyte * 32)()
    p.x.XQueryKeymap(p.d, keymap)
    codes = {event['code'] for event in events}
    pressed = sorted(code for code in codes if keymap[code // 8] & (1 << (code % 8)))
    balanced = all(sum(1 if event['down'] else -1 for event in events
                       if event['code'] == code) == 0 for code in codes)
    return {'started': started, 'finished': finished, 'events': events,
            'server_round_trip': True, 'pressed_codes_after': pressed,
            'all_keys_released': balanced and not pressed}


def address_popup_flow(args, result):
    """Exercise the floating dock editor without changing lifecycle fixtures."""
    profile = args.output / 'profile'
    (profile / 'Default').mkdir(parents=True)
    write(profile / 'Default/Preferences', {'helium': {'browser': {'layout': 2,
        'zen_mode': True, 'zen_mode_sidebar_pinned': True,
        'zen_mode_top_chrome_pinned': True}}, 'vertical_tabs': {
        'collapsed_state': False, 'uncollapsed_width': 240}})
    class AddressHandler(Handler):
        def do_GET(self):
            self.server.requests.append(self.path)
            super().do_GET()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), AddressHandler)
    server.requests = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with LayoutBrowser(args.browser, profile, args.output / 'first', FLAGS) as browser:
            inp, tree = Input(browser.platform), None
            try:
                assert browser.platform == 'x11'
                state = browser.state()
                preset = state['presets']['minimal-left'].copy()
                preset['revision'] = state['layout']['revision']
                applied = browser.apply(preset, state['layout']['revision'])
                write(args.output / 'address-layout-setup.json',
                      {'state': state, 'preset': preset, 'response': applied})
                assert applied['status'] == 'ok', applied
                tree = PositionedNativeUITree(browser, inp)
                regular = request(browser.port, '/json/new?' + base + '/regular', 'PUT')
                wait_for(lambda: browser.ev('location.href===' + json.dumps(base + '/regular') +
                    '&&document.readyState==="complete"&&!!document.querySelector("#draft")', regular), browser.process)
                request(browser.port, '/json/activate/' + regular['id'])
                original_windows = owned_windows(browser, inp)
                assert len(original_windows) == 1, original_windows
                result['private_window_manager_present'] = private_window_manager(inp)
                assert result['private_window_manager_present'], 'Address-popup timing qualification requires an actual private WM'
                activate_x_window(inp, original_windows[0], browser.process)
                inp.pointer.click(1000, 650)  # Initial setup only.
                regular_context = cdp(browser, 'Target.getTargetInfo', {'targetId': regular['id']})['targetInfo']['browserContextId']
                def has_address(value):
                    tree.read()
                    return any(' INVISIBLE ' not in node['line'] and
                        'class_name=OmniboxViewViews ' in node['line'] and
                        re.search(r'\bvalue=' + re.escape(value) + r'(?:\s|$)', node['line'])
                        for node in tree.nodes.values())
                def exercise(label, target):
                    tree.bind_browser(target)
                    context = cdp(browser, 'Target.getTargetInfo', {'targetId': target['id']})['targetInfo']['browserContextId']
                    native_key(inp, 'l', ('Control_L',))
                    type_text(inp, 'qbeforepopup')
                    check(result, label + ': initial native address entry creates suggestions',
                          has_address('qbeforepopup') and any(
                              ' INVISIBLE ' not in node['line'] and
                              'class_name=OmniboxPopupViewViews ' in node['line'] and
                              node['bounds'][2] and node['bounds'][3]
                              for node in tree.nodes.values()))
                    capture(tree, inp, args.output, label + '-before-escape')
                    native_key(inp, 'Escape')
                    native_key(inp, 'l', ('Control_L',))
                    type_text(inp, 'qreopened')
                    check(result, label + ': Escape then CtrlL reopens the actual native editor',
                          has_address('qreopened') and any(
                              ' INVISIBLE ' not in node['line'] and
                              'class_name=OmniboxPopupViewViews ' in node['line'] and
                              node['bounds'][2] and node['bounds'][3]
                              for node in tree.nodes.values()))
                    # Dispatch Escape/reopen rapidly, then acknowledge native
                    # focus before typing. CtrlL is a nonreserved shortcut and
                    # its renderer reply can arrive after the next key event.
                    batch = address_key_batch(inp, [('Escape', ()),
                        ('l', ('Control_L',))])
                    batch['dispatch_seconds'] = batch['finished'] - batch['started']
                    result.setdefault('rapid_address_batches', {})[label] = batch
                    check(result, label + ': balanced native Escape/reopen dispatch is under 82ms',
                          batch['finished'] - batch['started'] < .082 and batch['all_keys_released'])
                    readiness_started = time.monotonic()
                    def editor_ready():
                        tree.read()
                        fields = [node for node in tree.nodes.values()
                            if ' INVISIBLE ' not in node['line'] and
                            ' FOCUSABLE ' in node['line'] and
                            'class_name=OmniboxViewViews ' in node['line'] and
                            node['bounds'][2] and node['bounds'][3] and
                            not re.search(r'\bvalue=qreopened(?:\s|$)', node['line'])]
                        if not fields or browser.ev('document.hasFocus()', target):
                            return None
                        assert len(fields) == 1, tree.text
                        return {'native_field': fields[0]['line'],
                                'target_document_has_focus': False}
                    ready = wait_for(editor_ready, browser.process, timeout=5)
                    readiness_finished = time.monotonic()
                    result.setdefault('rapid_address_readiness', {})[label] = {
                        'started': readiness_started, 'finished': readiness_finished,
                        'latency_seconds': readiness_finished - readiness_started,
                        **ready}
                    text_batch = address_key_batch(inp,
                        [(letter, ()) for letter in 'qrapid'])
                    text_batch['dispatch_seconds'] = text_batch['finished'] - text_batch['started']
                    result.setdefault('rapid_address_text_batches', {})[label] = text_batch
                    time.sleep(.35)
                    preserved = has_address('qrapid')
                    capture(tree, inp, args.output, label + '-rapid-after-teardown')
                    native_key(inp, 'z')
                    check(result, label + ': post-readiness native text and editability remain intact',
                          text_batch['all_keys_released'] and preserved and has_address('qrapidz'))
                    native_key(inp, 'a', ('Control_L',))
                    destination = base + '/address-' + label
                    # The shared short-marker helper does not map period.
                    # Use exact X keysyms for every URL punctuation character.
                    for character in destination:
                        native_key(inp, {':': 'semicolon', '/': 'slash',
                            '.': 'period', '-': 'minus'}.get(character, character),
                            ('Shift_L',) if character == ':' else ())
                    check(result, label + ': exact physical loopback URL is in the native editor',
                          has_address(destination))
                    capture(tree, inp, args.output, label + '-url-before-return')
                    native_key(inp, 'Return')
                    wait_for(lambda: browser.ev('location.href===' + json.dumps(destination) +
                        '&&document.readyState==="complete"&&!!document.querySelector("#draft")', target), browser.process)
                    check(result, label + ': Return navigates the focused document in its original context/window',
                          browser.ev('document.hasFocus()&&document.visibilityState==="visible"', target)
                          and cdp(browser, 'Target.getTargetInfo', {'targetId': target['id']})['targetInfo']['browserContextId'] == context
                          and owned_windows(browser, inp) == original_windows
                          and '/address-' + label in server.requests)
                    capture(tree, inp, args.output, label + '-address-navigated')
                exercise('regular', regular)
                retained = browser.ev('window.addressToken=crypto.randomUUID();({token:window.addressToken,time:performance.timeOrigin})', regular)
                private = new_native_tab(browser, inp, private_space=True)
                check(result, 'Address controls create a genuinely private context in the same physical window',
                      cdp(browser, 'Target.getTargetInfo', {'targetId': private['id']})['targetInfo']['browserContextId'] != regular_context
                      and owned_windows(browser, inp) == original_windows)
                exercise('private', private)
                native_key(inp, 'w', ('Control_L',))
                wait_for(lambda: all(page['id'] != private['id'] for page in pages(browser)) and
                         browser.ev('document.visibilityState==="visible"', regular), browser.process)
                tree.bind_browser(regular)
                capture(tree, inp, args.output, 'address-private-closed')
                check(result, 'Address regression last-tab close preserves the regular document and removes private Space',
                      browser.ev('({token:window.addressToken,time:performance.timeOrigin})', regular) == retained
                      and 'Workspace: Incognito ' not in tree.text
                      and owned_windows(browser, inp) == original_windows)
                result['normal_shutdowns'] = [normal_close(browser)]
            except BaseException:
                if tree and browser.process.poll() is None:
                    capture(tree, inp, args.output, 'failure')
                    write(args.output / 'failure-targets.json', pages(browser))
                raise
            finally:
                inp.close()
    finally:
        result['address_requests'] = server.requests
        server.shutdown(); server.server_close(); thread.join(timeout=4)


def horizontal_flow(args, result):
    profile = args.output / 'profile'
    profile.mkdir()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with LayoutBrowser(args.browser, profile, args.output / 'first', FLAGS) as browser:
            inp, tree = Input(browser.platform), None
            try:
                assert browser.platform == 'x11'
                state = browser.state()
                preset = state['presets']['classic-top'].copy()
                preset['revision'] = state['layout']['revision']
                applied = browser.apply(preset, state['layout']['revision'])
                write(args.output / 'horizontal-layout-setup.json',
                      {'state': state, 'preset': preset, 'response': applied})
                assert applied['status'] == 'ok', applied
                tree = PositionedNativeUITree(browser, inp)
                # Keep only the accessibility diagnostic tab in the regular
                # model, so its empty caption disagrees with private tabs 2/3.
                regular = tree.target
                regular_context = cdp(browser, 'Target.getTargetInfo', {'targetId': regular['id']})['targetInfo']['browserContextId']
                for page in pages(browser):
                    if page['id'] != regular['id']:
                        request(browser.port, '/json/close/' + page['id'])
                wait_for(lambda: len(pages(browser)) == 1, browser.process)
                request(browser.port, '/json/activate/' + regular['id'])
                original_windows = owned_windows(browser, inp)
                check(result, 'Horizontal fixture has one regular tab and one real OS browser window',
                      len(original_windows) == 1)
                activate_x_window(inp, original_windows[0], browser.process)
                has_wm = private_window_manager(inp)
                result['private_window_manager_present'] = has_wm
                if args.case == 'window-controls':
                    assert has_wm, 'Window-controls qualification requires an actual WM on the private Xvfb display; bare Xvfb cannot qualify minimize/restore'
                private_tabs = []
                for index in range(3):
                    page = new_native_tab(browser, inp, private_space=index == 0)
                    goto(browser, page, base + '/page?title=Drag-private-' + str(index + 1))
                    browser.ev('window.dragOrigin=performance.timeOrigin;document.querySelector("#draft").value="retained-private-draft";true', page)
                    private_tabs.append(page)
                private_context = cdp(browser, 'Target.getTargetInfo', {'targetId': private_tabs[0]['id']})['targetInfo']['browserContextId']
                hidden = new_native_tab(browser, inp, private_space=True)
                goto(browser, hidden, base + '/page?title=Hidden-private-space')
                # Top-only layouts hide the footer. Use the real previous-Space
                # accelerator, then bind geometry to the known private owner.
                native_key(inp, 'Left', ('Control_L', 'Alt_L'))
                wait_for(lambda: any(browser.ev('document.visibilityState==="visible"', page)
                                     for page in private_tabs) and
                         not browser.ev('document.visibilityState==="visible"', hidden), browser.process)
                tree.bind_browser(private_tabs[0])
                inp.pointer.click(*tree.center(native_tab(tree, 'Drag-private-1')))
                wait_for(lambda: browser.ev('document.visibilityState==="visible"', private_tabs[0]) and
                         not browser.ev('document.visibilityState==="visible"', hidden), browser.process)
                physical_window = cdp(browser, 'Browser.getWindowForTarget', {'targetId': private_tabs[0]['id']})['windowId']
                def bounds():
                    return cdp(browser, 'Browser.getWindowBounds', {'windowId': physical_window})['bounds']
                for index in (1, 2):
                    node = native_tab(tree, 'Drag-private-' + str(index + 1))
                    inp.pointer.click(*tree.center(node))
                    wait_for(lambda: browser.ev('document.visibilityState==="visible"', private_tabs[index]), browser.process)
                before_double = bounds()
                point = tree.center(native_tab(tree, 'Drag-private-3'))
                inp.move(*point)
                for unused in range(2):
                    inp.button(True)
                    inp.button(False)
                    time.sleep(.05)
                time.sleep(.35)
                check(result, 'Private tab clicks/double-click use visible tab geometry rather than hidden regular caption',
                      bounds() == before_double and
                      browser.ev('document.visibilityState==="visible"', private_tabs[2]))
                start = tree.center(native_tab(tree, 'Drag-private-3'))
                destination = tree.center(native_tab(tree, 'Drag-private-1'))
                inp.drag(start, destination)
                first = tree.rect(native_tab(tree, 'Drag-private-1'))
                moved = tree.rect(native_tab(tree, 'Drag-private-3'))
                check(result, 'Native private tab reorder remains in its logical model and the original OS window',
                      moved['x'] < first['x'] and owned_windows(browser, inp) == original_windows and
                      all(cdp(browser, 'Target.getTargetInfo', {'targetId': page['id']})['targetInfo']['browserContextId'] == private_context
                          for page in private_tabs))
                def drag_all_and_cancel(label):
                    ordered = sorted([(tree.rect(native_tab(tree, 'Drag-private-' + str(index + 1))), index)
                                      for index in range(3)], key=lambda item: item[0]['x'])
                    inp.pointer.click(*tree.center(native_tab(tree, 'Drag-private-' + str(ordered[0][1] + 1))))
                    end = tree.center(native_tab(tree, 'Drag-private-' + str(ordered[-1][1] + 1)))
                    modified_click(inp, end, 'Shift_L')
                    tree.read()
                    check(result, label + ': native range selection includes all three private tabs',
                          sum('selected=true' in native_tab(tree, 'Drag-private-' + str(i + 1))['line'] for i in range(3)) == 3)
                    inp.move(*end)
                    inp.button(True)
                    try:
                        for step in range(1, 9):
                            inp.move(end[0] + step * 4, end[1] + step * 7)
                            time.sleep(.04)
                        time.sleep(.3)
                        native_key(inp, 'Escape')
                    finally:
                        inp.button(False)
                    check(result, label + ': whole-private-tab drag/cancel preserves the host and actual private documents',
                          owned_windows(browser, inp) == original_windows and
                          len(pages(browser)) == 5 and
                          all(browser.ev('performance.timeOrigin===window.dragOrigin && document.querySelector("#draft").value==="retained-private-draft"', page)
                              for page in private_tabs) and
                          cdp(browser, 'Target.getTargetInfo', {'targetId': regular['id']})['targetInfo']['browserContextId'] == regular_context)
                drag_all_and_cancel('Restored window')
                if has_wm:
                    click_caption(tree, inp, 'Minimize')
                    wait_for(lambda: bounds()['windowState'] == 'minimized', browser.process)
                    cdp(browser, 'Browser.setWindowBounds', {'windowId': physical_window, 'bounds': {'windowState': 'normal'}})
                    wait_for(lambda: bounds()['windowState'] == 'normal' and
                             any(browser.ev('document.visibilityState==="visible"', page) for page in private_tabs), browser.process)
                    check(result, 'Minimize/restore revives only the selected private surface',
                          not browser.ev('document.visibilityState==="visible"', hidden) and
                          not browser.ev('document.visibilityState==="visible"', regular) and
                          owned_windows(browser, inp) == original_windows)
                    click_caption(tree, inp, 'Maximize')
                    wait_for(lambda: bounds()['windowState'] == 'maximized', browser.process)
                    drag_all_and_cancel('Maximized window')
                    if bounds()['windowState'] == 'maximized':
                        click_caption(tree, inp, 'Restore')
                    else:
                        cdp(browser, 'Browser.setWindowBounds', {'windowId': physical_window, 'bounds': {'windowState': 'normal'}})
                    wait_for(lambda: bounds()['windowState'] == 'normal', browser.process)
                else:
                    result['qualification_limits'] = ['Bare private Xvfb has no WM: minimize/restore, native maximize/restore and maximized all-tab drag require --case window-controls under an actual private WM.']
                capture(tree, inp, args.output, 'horizontal-private-controls')
                result['normal_shutdowns'] = [normal_close(browser)]
            except BaseException:
                if tree and browser.process.poll() is None:
                    capture(tree, inp, args.output, 'failure')
                    write(args.output / 'failure-targets.json', pages(browser))
                raise
            finally:
                inp.close()
    finally:
        server.shutdown()
        server.server_close()


def flow(args, result):
    profile = args.output / 'profile'
    profile.mkdir()
    extension = args.output / 'blocked-extension'
    extension.mkdir()
    write(extension / 'manifest.json', {'manifest_version': 3,
        'name': 'Non-incognito privacy witness', 'version': '1.0',
        'permissions': ['tabs', 'history', 'sessions'],
        'background': {'service_worker': 'worker.js'}, 'incognito': 'split'})
    (extension / 'worker.js').write_text('chrome.runtime.onInstalled.addListener(()=>{});')
    (extension / 'check.html').write_text('<title>Regular extension witness</title>')
    flags = [*FLAGS, '--password-store=basic', '--load-extension=' + str(extension)]
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), PrivateHandler)
    server.heartbeat_lock = threading.Lock()
    server.heartbeats = {}
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    private_url = base + '/page?title=private-space-one-secret'
    private_url2 = base + '/page?title=private-space-two-secret'
    try:
        with LayoutBrowser(args.browser, profile, args.output / 'first', flags) as browser:
            inp = Input(browser.platform)
            tree = None
            try:
                assert browser.platform == 'x11'
                apply_native_test_layout(browser.port, edge='left',
                    visible=['tabs', 'spaces', 'new-tab', 'address', 'extensions', 'menu'])
                tree = PositionedNativeUITree(browser, inp)
                worker = wait_for(lambda: next((p for p in request(browser.port, '/json/list')
                    if p['type'] == 'service_worker' and p['url'].startswith('chrome-extension://')), None), browser.process)
                ext_url = worker['url'].rsplit('/', 1)[0] + '/check.html'
                extension_page = request(browser.port, '/json/new?' + ext_url, 'PUT')
                wait_for(lambda: browser.ev('!!chrome.tabs', extension_page), browser.process)
                regular_url = base + '/page?title=regular-space-witness'
                regular = request(browser.port, '/json/new?' + regular_url, 'PUT')
                wait_for(lambda: browser.ev('location.href===' + json.dumps(regular_url) +
                    ' && document.readyState==="complete"', regular), browser.process)
                browser.ev(write_state('regular'), regular)
                regular_context = cdp(browser, 'Target.getTargetInfo', {'targetId': regular['id']})['targetInfo']['browserContextId']
                request(browser.port, '/json/activate/' + regular['id'])
                original_windows = owned_windows(browser, inp)
                check(result, 'Starts with exactly one real OS browser window', len(original_windows) == 1)
                activate_x_window(inp, original_windows[0], browser.process)
                original_spaces = browser.call('noventraSpaces', 'list', '', '')
                private = new_native_tab(browser, inp, private_space=True)
                wait_for(lambda: len(pages(browser)) >= 4, browser.process)
                check(result, 'New incognito keeps the exact same OS top-level window',
                      owned_windows(browser, inp) == original_windows)
                context = cdp(browser, 'Target.getTargetInfo', {'targetId': private['id']})['targetInfo']['browserContextId']
                check(result, 'Private tab has a genuine different BrowserContext', context != regular_context)
                capture(tree, inp, args.output, 'private-created')
                check(result, 'Native selected Space identifies itself as Incognito',
                      bool(re.search(r'name=Workspace: Incognito name_from=.*selected=true', tree.text)))
                native_key(inp, 'l', ('Control_L',))
                native_key(inp, 'q')
                tree.read()
                check(result, 'Ctrl+L routes native character entry into the private address field',
                      any('class_name=OmniboxViewViews ' in n['line'] and
                          re.search(r'\bvalue=q(?:\s|$)', n['line'])
                          for n in tree.nodes.values()))
                native_key(inp, 'Escape')
                if args.case == 'create':
                    native_key(inp, 'w', ('Control_L',))
                    wait_for(lambda: all(p['id'] != private['id'] for p in pages(browser)), browser.process)
                    capture(tree, inp, args.output, 'private-closed')
                    check(result, 'Last private tab removes the temporary Space and returns regular',
                          'Workspace: Incognito ' not in tree.text and
                          browser.ev('document.visibilityState==="visible"', regular))
                else:
                    goto(browser, private, private_url)
                    state = browser.ev(READ, private)
                    check(result, 'Incognito initially has none of the regular cookie/storage state',
                          not state['cookie'] and state.get('local') is None and
                          not state['cache'] and not state['idb'] and state['workers'] == 0)
                    browser.ev(write_state('privateone'), private)
                    start_worker(browser, private, server, 'privateone')
                    retained = browser.ev('({time:performance.timeOrigin, token:window.loadedToken, draft:document.querySelector("#draft").value})', private)
                    sibling = new_native_tab(browser, inp)
                    goto(browser, sibling, private_url + '-sibling')
                    check(result, 'Ctrl+T stays in the same private context and account jar',
                          cdp(browser, 'Target.getTargetInfo', {'targetId': sibling['id']})['targetInfo']['browserContextId'] == context
                          and 'privateone' in browser.ev(READ, sibling)['cookie'])
                    native_key(inp, 'w', ('Control_L',))
                    wait_for(lambda: all(p['id'] != sibling['id'] for p in pages(browser)), browser.process)
                    check(result, 'Closing one tab keeps its nonempty private Space',
                          any(p['id'] == private['id'] for p in pages(browser)))
                    private2 = new_native_tab(browser, inp, private_space=True)
                    goto(browser, private2, private_url2)
                    state2 = browser.ev(READ, private2)
                    check(result, 'A second private Space has a fresh independent memory jar',
                          not state2['cookie'] and state2.get('local') is None and
                          not state2['idb'] and not state2['cache'])
                    browser.ev(write_state('privatetwo'), private2)
                    start_worker(browser, private2, server, 'privatetwo')
                    check(result, 'Two private Spaces still use just the original OS window',
                          owned_windows(browser, inp) == original_windows)
                    before_shortcuts = {p['id'] for p in pages(browser)}
                    regular_count = sum(not s['archived'] for s in original_spaces)
                    assert regular_count + 2 <= 9
                    native_key(inp, '1', ('Control_L', 'Shift_L'))
                    wait_for(lambda: browser.ev('document.visibilityState==="visible"', regular), browser.process)
                    native_key(inp, str(regular_count + 1), ('Control_L', 'Shift_L'))
                    wait_for(lambda: browser.ev('document.visibilityState==="visible"', private), browser.process)
                    native_key(inp, 'Right', ('Control_L', 'Alt_L'))
                    wait_for(lambda: browser.ev('document.visibilityState==="visible"', private2), browser.process)
                    native_key(inp, 'Left', ('Control_L', 'Alt_L'))
                    wait_for(lambda: browser.ev('document.visibilityState==="visible"', private), browser.process)
                    check(result, 'Native numbered/cycle shortcuts select real regular/private Browsers without extra tabs',
                          {p['id'] for p in pages(browser)} == before_shortcuts and
                          'privateone' in browser.ev(READ, private)['cookie'] and
                          'privatetwo' in browser.ev(READ, private2)['cookie'])
                    native_key(inp, 'l', ('Control_L',))
                    type_text(inp, 'chrome://settings/')
                    native_key(inp, 'Return')
                    def visible_settings():
                        return next((p for p in pages(browser)
                            if p['url'].startswith('chrome://settings') and
                            browser.ev('document.visibilityState==="visible"', p)), None)
                    settings = wait_for(visible_settings, browser.process)
                    check(result, 'Private-forbidden Settings presents the genuine regular Browser',
                          cdp(browser, 'Target.getTargetInfo', {'targetId': settings['id']})['targetInfo']['browserContextId'] == regular_context
                          and not browser.ev('document.visibilityState==="visible"', private))
                    select_dot(browser, tree, inp, 'Incognito')
                    check(result, 'Returning from Settings preserves the private document and draft',
                          browser.ev('({time:performance.timeOrigin, token:window.loadedToken, draft:document.querySelector("#draft").value})', private) == retained)
                    focus_round_trip(inp, original_windows[0], browser.process)
                    native_key(inp, 'l', ('Control_L',))
                    native_key(inp, 'q')
                    tree.read()
                    check(result, 'Desktop reactivation restores private native typing and leaves regular UI hidden',
                          any('class_name=OmniboxViewViews ' in n['line'] and
                              re.search(r'\bvalue=q(?:\s|$)', n['line'])
                              for n in tree.nodes.values()) and
                          browser.ev('document.visibilityState==="visible"', private) and
                          not browser.ev('document.visibilityState==="visible"', settings))
                    native_key(inp, 'Escape')
                    select_dot(browser, tree, inp, 'Personal')
                    check(result, 'Native return to Personal preserves regular document and state',
                          browser.ev(READ, regular)['cookie'].endswith('regular') and
                          browser.ev('window.loadedToken', regular) == 'regular live')
                    select_dot(browser, tree, inp, 'Incognito')
                    check(result, 'Native return to Incognito preserves its actual loaded document',
                          browser.ev('({time:performance.timeOrigin, token:window.loadedToken, draft:document.querySelector("#draft").value})', private) == retained)
                    before_size = browser.ev('({w:innerWidth,h:innerHeight})', private)
                    physical_window = cdp(browser, 'Browser.getWindowForTarget', {'targetId': private['id']})['windowId']
                    cdp(browser, 'Browser.setWindowBounds', {'windowId': physical_window,
                        'bounds': {'width': 1040, 'height': 680}})
                    wait_for(lambda: cdp(browser, 'Browser.getWindowBounds', {'windowId': physical_window})['bounds']['width'] == 1040,
                             browser.process)
                    after_size = browser.ev('({w:innerWidth,h:innerHeight})', private)
                    check(result, 'Host resizing resizes the embedded private viewport',
                          after_size != before_size and after_size['w'] > 600 and after_size['h'] > 300)
                    native_key(inp, 'F11')
                    private_window = cdp(browser, 'Browser.getWindowForTarget', {'targetId': private['id']})['windowId']
                    wait_for(lambda: cdp(browser, 'Browser.getWindowBounds', {'windowId': private_window})['bounds']['windowState'] == 'fullscreen', browser.process)
                    check(result, 'Private fullscreen uses the same physical OS window',
                          owned_windows(browser, inp) == original_windows)
                    native_key(inp, 'F11')
                    wait_for(lambda: cdp(browser, 'Browser.getWindowBounds', {'windowId': private_window})['bounds']['windowState'] != 'fullscreen', browser.process)
                    tree.read()
                    containers = [n for n in tree.nodes.values() if ' INVISIBLE ' not in n['line']
                        and 'class_name=ContentsContainerView ' in n['line'] and n['bounds'][2] > 600 and n['bounds'][3] > 300]
                    assert len(containers) == 1, tree.text
                    content_bounds = tree.rect(containers[0])
                    field = browser.ev('(()=>{const r=document.querySelector("#draft").getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()', private)
                    inp.pointer.click(content_bounds['x'] + field['x'], content_bounds['y'] + field['y'])
                    check(result, 'Private page receives genuine native user activation',
                          browser.ev('navigator.userActivation.hasBeenActive', private))
                    document_state = '({time:performance.timeOrigin,token:window.loadedToken,draft:document.querySelector("#draft").value})'
                    preserved = [(tab,
                        cdp(browser, 'Target.getTargetInfo', {'targetId': tab['id']})['targetInfo']['browserContextId'],
                        browser.ev(document_state, tab)) for tab in [private, private2]]
                    browser.ev('window.onbeforeunload=e=>{e.preventDefault();e.returnValue=""};true', private)
                    for close_kind in ['caption', 'OS']:
                        if close_kind == 'caption':
                            click_caption(tree, inp, 'Close')
                        else:
                            close_os_window(inp, original_windows[0])
                        dialog = wait_for(lambda: beforeunload_dialog(tree), browser.process, timeout=8)
                        check(result, close_kind + ' close opens a genuine native beforeunload dialog with Cancel and Leave',
                              bool(dialog))
                        capture(tree, inp, args.output, close_kind + '-beforeunload-dialog')
                        native_key(inp, 'Escape')
                        wait_for(lambda: beforeunload_dialog(tree) is None, browser.process, timeout=8)
                        check(result, close_kind + ' native Escape cancels close and preserves both private contexts and loaded documents',
                              owned_windows(browser, inp) == original_windows and
                              all(any(p['id'] == tab['id'] for p in pages(browser)) and
                                  cdp(browser, 'Target.getTargetInfo', {'targetId': tab['id']})['targetInfo']['browserContextId'] == context_id and
                                  browser.ev(document_state, tab) == state
                                  for tab, context_id, state in preserved) and
                              browser.ev('document.visibilityState==="visible"', private))
                        assert_native_address_entry(browser, tree, inp, result,
                            close_kind + ' beforeunload cancellation restores native private address entry without refocusing',
                            'qc' if close_kind == 'caption' else 'qo')
                    browser.ev('window.onbeforeunload=null;true', private)
                    for modal in ['confirm', 'alert']:
                        message = 'Owned private ' + modal + ' focus regression'
                        browser.ev('window.modalDone=false;setTimeout(()=>{' + modal + '(' +
                            json.dumps(message) + ');window.modalDone=true},0);true', private)
                        wait_for(lambda: native_js_modal(tree, message), browser.process, timeout=8)
                        capture(tree, inp, args.output, modal + '-native-dialog')
                        native_key(inp, 'Escape')
                        wait_for(lambda: browser.ev('window.modalDone', private) and not native_js_modal(tree, message),
                                 browser.process, timeout=8)
                        assert_native_address_entry(browser, tree, inp, result,
                            'Ordinary private ' + modal + ' dismissal restores native shortcuts without pointer recovery',
                            'q' + modal)
                    first_message = 'Owned first chained private confirm'
                    second_message = 'Owned second chained private confirm'
                    browser.ev('window.modalChainDone=false;setTimeout(()=>{confirm(' +
                        json.dumps(first_message) + ');confirm(' + json.dumps(second_message) +
                        ');window.modalChainDone=true},0);true', private)
                    wait_for(lambda: native_js_modal(tree, first_message), browser.process, timeout=8)
                    native_key(inp, 'Escape')
                    wait_for(lambda: native_js_modal(tree, second_message), browser.process, timeout=8)
                    # Ctrl+L deliberately opens the address popup in regular
                    # browsing too. Test replacement-modal focus without first
                    # moving it away from the dialog ourselves.
                    time.sleep(.35)
                    capture(tree, inp, args.output, 'second-chained-native-dialog')
                    assert native_js_modal(tree, second_message)
                    native_key(inp, 'Escape')
                    wait_for(lambda: not native_js_modal(tree, second_message), browser.process, timeout=8)
                    wait_for(lambda: browser.ev('window.modalChainDone', private), browser.process, timeout=8)
                    check(result, 'The second chained private modal retains native keyboard cancellation while posted focus restoration unwinds',
                          not native_js_modal(tree, second_message) and
                          browser.ev('window.modalChainDone', private))
                    assert_native_address_entry(browser, tree, inp, result,
                        'Private native shortcuts recover after the final chained modal closes', 'qafterchain')
                    modal_focus_guards(browser, tree, inp, result, private, original_windows)
                    native_key(inp, 'w', ('Control_L',))
                    wait_for(lambda: all(p['id'] != private['id'] for p in pages(browser)), browser.process)
                    capture(tree, inp, args.output, 'first-private-space-removed')
                    check(result, 'Last tab removes only its own Space while the other remains',
                          not re.search(r'Workspace: Incognito name_from=', tree.text) and
                          'Workspace: Incognito 2 ' in tree.text and
                          any(p['id'] == private2['id'] for p in pages(browser)))
                    time.sleep(.6)
                    closed_count = heartbeats(server, 'privateone')
                    live_count = heartbeats(server, 'privatetwo')
                    time.sleep(1.3)
                    check(result, 'Closing one Space stops its live service worker while another private worker continues',
                          heartbeats(server, 'privateone') == closed_count and
                          heartbeats(server, 'privatetwo') >= live_count + 2)
                    native_key(inp, 't', ('Control_L', 'Shift_L'))
                    check(result, 'Regular reopen-closed-tab cannot recover the private URL',
                          all(p['url'] != private_url for p in pages(browser)))
                    urls = browser.ev('chrome.tabs.query({}).then(ts=>ts.map(t=>t.url))', extension_page)
                    check(result, 'An extension without incognito access cannot see private tabs',
                          private_url2 not in urls and any('regular-space-witness' in (u or '') for u in urls))
                    fresh_private = new_native_tab(browser, inp, private_space=True)
                    goto(browser, fresh_private, private_url + '-fresh')
                    fresh_state = browser.ev(READ, fresh_private)
                    check(result, 'A recreated Space cannot recover a closed jar while another private Space lives',
                          not fresh_state['cookie'] and fresh_state.get('local') is None and
                          not fresh_state['cache'] and not fresh_state['idb'])
                    native_key(inp, 'w', ('Control_L',))
                    wait_for(lambda: all(p['id'] != fresh_private['id'] for p in pages(browser)), browser.process)
                    select_dot(browser, tree, inp, 'Incognito 2')
                    for p in list(pages(browser)):
                        if cdp(browser, 'Target.getTargetInfo', {'targetId': p['id']})['targetInfo']['browserContextId'] == regular_context:
                            request(browser.port, '/json/close/' + p['id'])
                    check(result, 'Closing every regular tab leaves the private Space alive',
                          any(p['id'] == private2['id'] for p in pages(browser)) and
                          'privatetwo' in browser.ev(READ, private2)['cookie'] and
                          owned_windows(browser, inp) == original_windows)
                    native_key(inp, 'w', ('Control_L',))
                    wait_for(lambda: all(p['id'] != private2['id'] for p in pages(browser)) and bool(pages(browser)), browser.process)
                    check(result, 'Closing the last private tab restores a usable normal window',
                          owned_windows(browser, inp) == original_windows and
                          all(p['url'] not in [private_url, private_url2] for p in pages(browser)))
                result['normal_shutdowns'] = [normal_close(browser)]
            except BaseException:
                if tree and browser.process.poll() is None:
                    try:
                        capture(tree, inp, args.output, 'failure')
                        write(args.output / 'failure-targets.json', pages(browser))
                    except BaseException:
                        pass
                raise
            finally:
                inp.close()
        if args.case == 'lifecycle':
            history = profile / 'Default/History'
            with sqlite3.connect('file:' + str(history) + '?mode=ro', uri=True) as db:
                urls = [r[0] for r in db.execute('SELECT url FROM urls')]
            check(result, 'Private URLs never entered regular History (positive regular witness)',
                  all('private-space-' not in u for u in urls) and
                  any('regular-space-witness' in u for u in urls))
            sessions = profile / 'Default/Sessions'
            check(result, 'Regular session files contain neither private URL',
                  all(private_url.encode() not in p.read_bytes() and private_url2.encode() not in p.read_bytes()
                      for p in sessions.glob('*') if p.is_file()))
            with LayoutBrowser(args.browser, profile, args.output / 'restart',
                               [*flags, '--restore-last-session']) as browser:
                check(result, 'Normal restart does not restore private pages',
                      all(p['url'] not in [private_url, private_url2] for p in pages(browser)))
                tree = NativeUITree(browser).read()
                check(result, 'Normal restart has no transient Incognito Space',
                      'Workspace: Incognito' not in tree.text)
                check(result, 'Regular saved Space metadata is unchanged',
                      browser.call('noventraSpaces', 'list', '', '') == original_spaces)
                result['normal_shutdowns'].append(normal_close(browser))
    finally:
        server.shutdown()
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['create', 'lifecycle', 'horizontal', 'window-controls', 'address-popup'], default='lifecycle')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    args.rtc_policy_diagnostic = False
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    result = {'passed': False, 'case': args.case, 'checks': [],
              'binary_sha256': args.expected_binary,
              'resources_sha256': args.expected_resources,
              'fixture_sha256': sha(Path(__file__)),
              'home_isolated': True, 'network_isolated_no_egress': True,
              'scope': 'Private Linux X11, native keys/Space controls, actual OTR pages, no personal data'}
    try:
        if args.case == 'address-popup':
            address_popup_flow(args, result)
        elif args.case in ['horizontal', 'window-controls']:
            horizontal_flow(args, result)
        else:
            flow(args, result)
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        write(args.output / 'result.json', result)
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
