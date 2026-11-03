"""Workspace route and settings-file flows on an authenticated private Xvfb.

Run from this checkout with a NEW output directory:
  work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    work/ram-investigation/with-xvfb.py \
    work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    tests/noventra/workspace_tools_smoke.py --browser /absolute/runtime \
    --output work/browser-gaps-2026-10-25/routing-test/candidate-r1

Measured navigation uses native keyboard/bookmark clicks or an owned secondary
browser process forwarding a URL to the same disposable profile. CDP never
creates a measured destination; it prepares Settings/bookmarks and reads live
document, cookie and UI witnesses. Native input also drives actual JSON download,
GTK file choosing, import confirmation/cancellation and focus checks. Import is
settings-only: fresh signed-out copies, never an account backup or device sync.

Work is bounded to 720 seconds plus 60 for evidence and owned cleanup. Existing
helpers are reused unchanged. --observe-only preserves missing-flow evidence
without qualifying a runtime. No host profile, desktop bus, global service,
launcher, source build or machine policy is modified. Narrow checks use measured
340/480 CSS viewports, not physical desktop window sizes.
"""

import argparse
import base64
from contextlib import contextmanager
from copy import deepcopy
import ctypes as C
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import tempfile
import threading
import time
import traceback

import chrome_surface_smoke as surface
import compact_chrome_smoke as n
import workspace_dots_smoke as dots
import workspace_management_smoke as wm
from glass_appearance_smoke import Appearance
from layout_editor_smoke import FIND
from onboarding_import_smoke import focus_picker


WORK = '00000000-0000-4000-8000-000000000021'
ARCHIVE = '00000000-0000-4000-8000-000000000022'
STALE = '00000000-0000-4000-8000-000000000023'
WORK_NAME = 'Работа Café'
ARCHIVE_NAME = 'Архив 東京'
WITNESS = '''({token:window.toolsToken,origin:performance.timeOrigin,
  draft:document.querySelector('#draft')?.value,url:location.href,
  cookie:document.cookie,storage:localStorage.getItem('toolsAccount')})'''
PAGE = '''<!doctype html><meta charset="utf-8"><title>Workspace tools fixture</title>
<style>body{margin:24px;background:#eef3f8;font:16px sans-serif}
input,button,a{display:block;margin:14px 0;padding:8px;max-width:280px}</style>
<h1>Owned workspace tools document</h1><input id="draft" aria-label="Unsaved draft">
<a id="same-page" href="/inside">Ordinary page link</a>
<button id="script-open" onclick="window.open('/script-child','tools-child')">Script opener</button>
<form method="post" action="/form-result"><input name="message" value="owned-form">
<button id="form-submit" type="submit">Submit form</button></form>
<script>window.toolsToken=crypto.randomUUID();document.title='Tools '+location.pathname;</script>'''


class Budget(n.Budget):
    def __init__(self):
        super().__init__()
        self.work_deadline = self.started + 720
        self.deadline = self.started + 780
        self.arm(self.work_deadline)

    @staticmethod
    def expired(*_):
        raise n.SmokeTimeout('Workspace tools exceeded its bounded time budget')


@contextmanager
def local_pages(output):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            requests.append({'path': self.path, 'method': 'GET',
                             'host': self.headers.get('Host'),
                             'cookie': self.headers.get('Cookie')})
            if self.path == '/favicon.ico':
                self.send_error(404)
                return
            if self.path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/redirected')
                self.end_headers()
                return
            data = PAGE.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            length = int(self.headers.get('Content-Length', '0'))
            assert 0 <= length <= 4096
            requests.append({'path': self.path, 'method': 'POST',
                             'body': self.rfile.read(length).decode()})
            self.do_GET()

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}'
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)
        n.write_json(output / 'owned-http-requests.json', requests)


def check(result, case, name, actual, expected=True):
    if isinstance(actual, set):
        actual = sorted(actual)
    if isinstance(expected, set):
        expected = sorted(expected)
    surface.check(result, case, name, actual, expected)
    return actual == expected


def reply(browser, action='list', payload=None, target=None):
    return browser.ev('''(async()=>{
      const {sendWithPromise}=await import('chrome://resources/js/cr.js');
      return await Promise.race([
        sendWithPromise('noventraWorkspaceTools',ACTION,PAYLOAD).then(
          value=>({status:'resolved',value}),error=>({status:'rejected',error:String(error)})),
        new Promise(resolve=>setTimeout(()=>resolve({status:'timeout'}),2500))]);
    })()'''.replace('ACTION', json.dumps(action)).replace('PAYLOAD', json.dumps(payload or {})), target)


def api(browser, action='list', payload=None):
    response = reply(browser, action, payload)
    assert response['status'] == 'resolved', (action, response)
    return response['value']


def snapshot(browser):
    return {'spaces': wm.api(browser), 'routes': api(browser)['routes'],
            'layout': browser.state()['layout']}


def ui(browser, body):
    return browser.ev('(async()=>{' + FIND + '''
      const page=deep(document,'settings-appearance-page');
      if(!page)throw Error('Appearance page missing');const root=page.shadowRoot;
    ''' + body + '})()')


def wait_ui(browser, body, timeout=8):
    return n.wait_for(lambda: ui(browser, body), browser.process, timeout=timeout)


def native_text(inp, text):
    """Batch genuine XTest keys on the owned seat; no DOM value assignment."""
    assert inp.platform == 'x11' and text.isascii(), text
    p = inp.pointer
    p.x.XStringToKeysym.argtypes = [C.c_char_p]
    p.x.XStringToKeysym.restype = C.c_ulong
    p.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
    p.x.XKeysymToKeycode.restype = C.c_uint
    p.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    shift = p.x.XKeysymToKeycode(p.d, p.x.XStringToKeysym(b'Shift_L'))
    punctuation = {' ': ('space', False), ':': ('semicolon', True),
                   '/': ('slash', False), '.': ('period', False),
                   '-': ('minus', False), '_': ('minus', True),
                   '?': ('slash', True), '=': ('equal', False),
                   '&': ('7', True), '%': ('5', True), '@': ('2', True)}
    for char in text:
        key, shifted = punctuation.get(char, (char.lower(), char.isupper()))
        code = p.x.XKeysymToKeycode(p.d, p.x.XStringToKeysym(key.encode()))
        assert code, char
        if shifted:
            p.xt.XTestFakeKeyEvent(p.d, shift, 1, 0)
        p.xt.XTestFakeKeyEvent(p.d, code, 1, 0)
        p.xt.XTestFakeKeyEvent(p.d, code, 0, 0)
        if shifted:
            p.xt.XTestFakeKeyEvent(p.d, shift, 0, 0)
        p.x.XFlush(p.d)
        time.sleep(.008)
    time.sleep(.15)


@contextmanager
def native_modifier(inp, name):
    p = inp.pointer
    code = p.x.XKeysymToKeycode(p.d, p.x.XStringToKeysym(name.encode()))
    assert code, name
    p.xt.XTestFakeKeyEvent(p.d, code, 1, 0)
    p.x.XFlush(p.d)
    try:
        yield
    finally:
        p.xt.XTestFakeKeyEvent(p.d, code, 0, 0)
        p.x.XFlush(p.d)


def click_ui(browser, tree, inp, selector):
    point = ui(browser, '''
      const el=root.querySelector(SELECTOR);if(!el||el.disabled)throw Error('Unavailable '+SELECTOR);
      el.scrollIntoView({block:'center',behavior:'instant'});
      const r=el.getBoundingClientRect();if(!r.width||!r.height)throw Error('Hidden '+SELECTOR);
      return {x:r.x+r.width/2,y:r.y+r.height/2};
    '''.replace('SELECTOR', json.dumps(selector)))
    content = tree.rect(n.one(tree.read(), r'class_name=MultiContentsView '))
    inp.pointer.click(content['x'] + point['x'], content['y'] + point['y'])


def focus_ui(browser, result, case, identifier):
    actual = wait_ui(browser, 'return !page.noventraToolsPending_ && (root.activeElement?.id || "");')
    check(result, case, 'native operation leaves keyboard focus on ' + identifier, actual, identifier)


def show_tools(browser, tree, inp):
    wm.show_settings(browser, tree, inp)
    wait_ui(browser, 'return page.noventraRoutesManageable_ && page.noventraImportable_ && '
            '!!root.querySelector(' + json.dumps('#noventraRouteSpace option[value="' + WORK + '"]') + ');')
    for selector in ('#noventraWorkspaceRoutes', '#noventraWorkspaceBackup'):
        if not ui(browser, 'return root.querySelector(' + json.dumps(selector) + ').open;'):
            click_ui(browser, tree, inp, selector + ' > summary')


def targets(browser):
    return [t for t in n.request(browser.port, '/json/list') if t.get('type') == 'page']


def fresh_target(browser, before, url=None):
    target = n.wait_for(lambda: next((t for t in targets(browser)
        if t['id'] not in before and (url is None or t.get('url') == url)), None),
        browser.process, timeout=10)
    if url:
        n.wait_for(lambda: browser.ev('!!window.toolsToken', target), browser.process, timeout=8)
    return target


def native_url(browser, tree, inp, url, new=False, target=None):
    if target:
        n.activate(browser, tree, inp, target)  # Establish the source, never repair the result.
    before = {t['id'] for t in targets(browser)}
    n.native_key(inp, 'l', ('Control_L',))
    native_text(inp, url)
    n.native_key(inp, 'Return', ('Alt_L',) if new else ())
    if new:
        return fresh_target(browser, before, url)
    assert target is not None
    n.wait_for(lambda: browser.ev('location.href', target) == url and
               browser.ev('!!window.toolsToken', target), browser.process, timeout=10)
    return target


def os_open(browser, url, output, label, result, incognito=False):
    before = {t['id'] for t in targets(browser)}
    command = [str(browser.browser), '--user-data-dir=' + str(browser.profile),
               '--ozone-platform=x11', '--no-first-run', '--no-default-browser-check',
               *(['--incognito'] if incognito else []), url]
    env = os.environ.copy()
    env['NOVENTRA_CONFIG_HOME'] = str(browser.profile / 'test-config')
    with (output / (label + '-secondary.log')).open('w') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            code = process.wait(timeout=15)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=3)
    check(result, label, 'owned URL-forwarding process exits successfully', code, 0)
    target = fresh_target(browser, before, url)
    result.setdefault('native_opens', []).append({'case': label, 'command': command,
        'pid': process.pid, 'target_id': target['id'], 'source_ids': sorted(before)})
    return target


def seed_document(browser, tree, inp, url, space, account):
    wm.api(browser, 'switch', space)
    # Ctrl+L/Alt+Enter is the ordinary browser new-tab navigation path.
    target = native_url(browser, tree, inp, url, new=True)
    browser.ev('document.cookie="toolsAccount=' + account + '; Path=/; Max-Age=3600; SameSite=Lax";'
               'localStorage.setItem("toolsAccount",' + json.dumps(account) + ');true', target)
    click_page(browser, tree, inp, target, '#draft')
    native_text(inp, account + ' unsaved')
    state = browser.ev(WITNESS, target)
    assert state['draft'] == account + ' unsaved', state
    return {'target': target, 'before': state, 'space': space, 'account': account}


def click_page(browser, tree, inp, target, selector):
    point = browser.ev('(()=>{const e=document.querySelector(' + json.dumps(selector) + ');'
        'e.scrollIntoView({block:"center"});const r=e.getBoundingClientRect();'
        'return {x:r.x+r.width/2,y:r.y+r.height/2};})()', target)
    content = tree.rect(n.one(tree.read(), r'class_name=MultiContentsView '))
    inp.pointer.click(content['x'] + point['x'], content['y'] + point['y'])


def retained(browser, pages, result, case):
    states = {name: browser.ev(WITNESS, page['target']) for name, page in pages.items()}
    check(result, case, 'retained token/timeOrigin/draft/URL/cookie/storage stay identical',
          states, {name: page['before'] for name, page in pages.items()})


def account(browser, tree, target, result, case, name, value):
    check(result, case, 'fresh tab selects the partition-derived native space', wm.current(tree), name)
    state = browser.ev(WITNESS, target)
    check(result, case, 'cookie selects the intended account jar', state['cookie'],
          '' if value is None else 'toolsAccount=' + value)
    check(result, case, 'localStorage selects the intended account jar', state['storage'], value)
    return state


def route_ui(browser, tree, inp, result, host, space):
    show_tools(browser, tree, inp)
    click_ui(browser, tree, inp, '#noventraRouteHost')
    n.native_key(inp, 'a', ('Control_L',))
    native_text(inp, host)
    options = ui(browser, 'return [...root.querySelector("#noventraRouteSpace").options].map(o=>o.value);')
    assert space in options, (space, options)
    click_ui(browser, tree, inp, '#noventraRouteSpace')
    n.native_key(inp, 'Home')
    for _ in range(options.index(space)):
        n.native_key(inp, 'Down')
    n.native_key(inp, 'Return')
    click_ui(browser, tree, inp, '#noventraSaveRoute')
    wait_ui(browser, 'return !page.noventraToolsPending_ && page.noventraToolsStatus_==="Domain rule saved.";')
    focus_ui(browser, result, 'native route save', 'noventraRouteHost')


def navigation_flows(browser, tree, inp, base, pages, output, result):
    route_ui(browser, tree, inp, result, ' 127.0.0.1 ', WORK)
    check(result, 'native route save', 'canonical exact host points to Work',
          next(r for r in api(browser)['routes'] if r['host'] == '127.0.0.1')['space_id'], WORK)
    n.activate(browser, tree, inp, pages['personal']['target'])
    target = os_open(browser, pages['work']['before']['url'], output, 'os-work', result)
    account(browser, tree, target, result, 'os-work', WORK_NAME, 'work')
    check(result, 'os-work', 'same-URL retained Work tab is not reused', target['id'] != pages['work']['target']['id'])
    retained(browser, pages, result, 'os-work')

    target = native_url(browser, tree, inp, base + '/native-new-tab', new=True,
                        target=pages['personal']['target'])
    account(browser, tree, target, result, 'native-new-tab', WORK_NAME, 'work')

    manager = n.request(browser.port, '/json/new?chrome://bookmarks/', 'PUT')  # Setup only.
    n.wait_for(lambda: browser.ev('typeof chrome.bookmarks==="object"', manager), browser.process)
    browser.ev('(async()=>await chrome.bookmarks.create({parentId:"1",title:"Routed bookmark",url:'
               + json.dumps(base + '/bookmark') + '}))()', manager)
    n.activate(browser, tree, inp, pages['personal']['target'])
    before = {t['id'] for t in targets(browser)}
    node = n.one(tree.read(), r'class_name=MdTextButton name=Routed bookmark ')
    inp.pointer.click(*tree.center(node))  # Default CURRENT_TAB native bookmark click.
    bookmark = fresh_target(browser, before, base + '/bookmark')
    account(browser, tree, bookmark, result, 'native-bookmark', WORK_NAME, 'work')
    retained(browser, pages, result, 'native-bookmark')
    n.activate(browser, tree, inp, pages['personal']['target'])
    before = {t['id'] for t in targets(browser)}
    node = n.one(tree.read(), r'class_name=MdTextButton name=Routed bookmark ')
    with native_modifier(inp, 'Control_L'):
        inp.pointer.click(*tree.center(node))
    background = fresh_target(browser, before, base + '/bookmark')
    state = browser.ev(WITNESS, background)
    check(result, 'background bookmark', 'fresh background tab uses Work storage',
          [state['cookie'], state['storage']], ['toolsAccount=work', 'work'])
    check(result, 'background bookmark', 'source remains selected in Personal',
          [wm.current(tree), browser.ev('document.visibilityState', pages['personal']['target'])],
          ['Personal', 'visible'])

    route_ui(browser, tree, inp, result, '127.0.0.1', '')
    n.activate(browser, tree, inp, pages['work']['target'])
    target = os_open(browser, base + '/personal-route', output, 'os-personal', result)
    account(browser, tree, target, result, 'os-personal', 'Personal', 'personal')
    api(browser, 'set-route', {'host': '127.0.0.1', 'space_id': WORK})
    target = os_open(browser, base.replace('127.0.0.1', 'localhost') +
                     '/127.0.0.1?next=127.0.0.1', output, 'url-text-not-host', result)
    account(browser, tree, target, result, 'url-text-not-host', 'Personal', None)
    api(browser, 'set-route', {'host': 'localhost', 'space_id': WORK})
    target = os_open(browser, base.replace('127.0.0.1', 'sub.localhost') +
                     '/exact-host-only', output, 'no-subdomain-inheritance', result)
    account(browser, tree, target, result, 'no-subdomain-inheritance', 'Personal', None)
    api(browser, 'remove-route', {'host': 'localhost'})

    # A fresh blank tab followed by ordinary current-tab typing stays Personal.
    n.activate(browser, tree, inp, pages['personal']['target'])
    before = {t['id'] for t in targets(browser)}
    n.native_key(inp, 't', ('Control_L',))
    scratch = fresh_target(browser, before)
    native_url(browser, tree, inp, base + '/current-typed', target=scratch)
    account(browser, tree, scratch, result, 'ordinary-current-tab', 'Personal', 'personal')
    before = {t['id'] for t in targets(browser)}
    click_page(browser, tree, inp, scratch, '#same-page')
    n.wait_for(lambda: browser.ev('location.pathname', scratch) == '/inside', browser.process)
    check(result, 'ordinary-page-link', 'in-place navigation adds no tab', {t['id'] for t in targets(browser)}, before)
    account(browser, tree, scratch, result, 'ordinary-page-link', 'Personal', 'personal')
    before = {t['id'] for t in targets(browser)}
    click_page(browser, tree, inp, scratch, '#script-open')
    script = fresh_target(browser, before, base + '/script-child')
    account(browser, tree, script, result, 'script-opener', 'Personal', 'personal')
    check(result, 'script-opener', 'real window.open retains its opener', browser.ev('!!window.opener', script))
    n.activate(browser, tree, inp, scratch)
    click_page(browser, tree, inp, scratch, '#form-submit')
    n.wait_for(lambda: browser.ev('location.pathname', scratch) == '/form-result', browser.process)
    account(browser, tree, scratch, result, 'ordinary-form', 'Personal', 'personal')
    # Server redirect follows a current-tab navigation, never a route reassignment.
    n.native_key(inp, 'l', ('Control_L',))
    native_text(inp, base + '/redirect')
    n.native_key(inp, 'Return')
    n.wait_for(lambda: browser.ev('location.pathname', scratch) == '/redirected', browser.process)
    account(browser, tree, scratch, result, 'server-redirect', 'Personal', 'personal')

    wm.api(browser, 'archive', WORK)
    n.activate(browser, tree, inp, pages['personal']['target'])
    target = os_open(browser, base + '/archived-route', output, 'archived-route', result)
    account(browser, tree, target, result, 'archived-route', 'Personal', 'personal')
    check(result, 'archived-route', 'routing never restores the archived target',
          next(s for s in wm.api(browser) if s['id'] == WORK)['archived'])
    check(result, 'archived-route', 'paused rule remains removable',
          any(r['host'] == '127.0.0.1' and r['space_id'] == WORK for r in api(browser)['routes']))
    wm.api(browser, 'restore', WORK)
    spaces_before = wm.api(browser)
    stale_url = base.replace('127.0.0.1', 'stale.localhost') + '/stale-route'
    target = os_open(browser, stale_url, output, 'stale-route', result)
    account(browser, tree, target, result, 'stale-route', 'Personal', None)
    check(result, 'stale-route', 'stale ID never creates/restores a space', wm.api(browser), spaces_before)

    windows_before = {w['sessionId'] for w in wm.window_trees(browser, tree)}
    otr = os_open(browser, base + '/incognito', output, 'incognito', result, incognito=True)
    state = browser.ev(WITNESS, otr)
    check(result, 'incognito', 'regular cookies/storage never enter OTR',
          [state['cookie'], state['storage']], ['', None])
    new_windows = [w for w in wm.window_trees(browser, tree) if w['sessionId'] not in windows_before]
    check(result, 'incognito', 'new private window stays in Personal despite Work rule',
          [re.findall(r'Account spaces: (.*?) name_from=', w['text']) for w in new_windows], [['Personal']])
    n.request(browser.port, '/json/close/' + otr['id'])
    retained(browser, pages, result, 'routing complete')


def route_validation(browser, tree, inp, result):
    cases = [(' ExAmPlE.COM. ', 'example.com'), ('bücher.example', 'xn--bcher-kva.example'),
             ('[0:0:0:0:0:0:0:1]', '[::1]'), ('localhost', 'localhost')]
    for raw, expected in cases:
        routes = api(browser, 'set-route', {'host': raw, 'space_id': WORK})
        check(result, 'route validation', raw + ' canonicalizes',
              next((r['space_id'] for r in routes if r['host'] == expected), None), WORK)
        api(browser, 'remove-route', {'host': expected})
    bad = ['', '*.example.com', 'https://example.com', 'example.com/path',
           'example.com:443', '[::1]:443', 'user@example.com', 'example.com?q=x',
           'example.com#x', 'example%2ecom', 'ex ample.com', '-bad.example',
           'bad-.example', 'a..example', 'a' * 64 + '.example', 'a.' * 127 + 'a']
    before = snapshot(browser)
    for host in bad:
        response = reply(browser, 'set-route', {'host': host, 'space_id': WORK})
        check(result, 'route validation', repr(host) + ' rejects', response['status'], 'rejected')
    for target in (ARCHIVE, STALE, 'not-a-space'):
        response = reply(browser, 'set-route', {'host': 'invalid-target.example', 'space_id': target})
        check(result, 'route validation', 'reject target ' + target, response['status'], 'rejected')
    check(result, 'route validation', 'all rejected rules preserve model/layout', snapshot(browser), before)
    show_tools(browser, tree, inp)
    click_ui(browser, tree, inp, '#noventraRouteHost')
    native_text(inp, 'example.com:443')
    n.native_key(inp, 'Return')
    wait_ui(browser, 'return !!page.noventraToolsError_ && !page.noventraToolsPending_;')
    focus_ui(browser, result, 'invalid native rule', 'noventraRouteHost')
    check(result, 'invalid native rule', 'error preserves saved rules', api(browser)['routes'], before['routes'])
    # Test native removal and focus on a paused stale rule.
    click_ui(browser, tree, inp, '[data-route-host="stale.localhost"] cr-button')
    wait_ui(browser, 'return !page.noventraRoutes_.some(r=>r.host==="stale.localhost");')
    focus_ui(browser, result, 'remove paused rule', 'noventraRouteHost')
    before = api(browser)['routes']
    capacity = browser.ev('''(async()=>{
      const {sendWithPromise}=await import('chrome://resources/js/cr.js');
      const send=(action,payload={})=>sendWithPromise('noventraWorkspaceTools',action,payload);
      const original=(await send('list')).routes;const created=[];
      try {
        for(let i=original.length;i<100;i++){
          const host='cap-'+i+'.example';await send('set-route',{host,space_id:''});created.push(host);
        }
        const count=(await send('list')).routes.length;let overflow='resolved';
        try{await send('set-route',{host:'over-cap.example',space_id:''});}catch(e){overflow='rejected';}
        await send('set-route',{host:created[0],space_id:WORK});
        const replacement=(await send('list')).routes.find(r=>r.host===created[0]).space_id;
        return {count,overflow,replacement};
      } finally {for(const host of created)await send('remove-route',{host});}
    })()'''.replace('WORK', json.dumps(WORK)))
    check(result, 'route capacity', '100 routes, overflow rejection and replacement at capacity',
          capacity, {'count': 100, 'overflow': 'rejected', 'replacement': WORK})
    check(result, 'route capacity', 'fixture cleanup preserves original rules', api(browser)['routes'], before)


@contextmanager
def owned_download_session(browser, downloads):
    # BrowserHandler owns the download override handle and releases it when
    # its DevTools connection closes. Keep it alive through the native export.
    script = '''const [endpoint,path]=process.argv.slice(1);
      const socket=new WebSocket(endpoint);let sequence=0;const pending=new Map();
      const timer=setTimeout(()=>process.exit(1),30000);
      await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
      socket.onmessage=e=>{const m=JSON.parse(e.data),p=pending.get(m.id);
        if(p){pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result);}};
      function call(method,params={}){const id=++sequence;return new Promise((resolve,reject)=>{
        pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});}
      try{await call('Browser.setDownloadBehavior',{
        behavior:'allow',downloadPath:path,eventsEnabled:true});
        process.stdout.write('ready\\n');process.stdin.resume();
        await new Promise(resolve=>process.stdin.once('end',resolve));
        await call('Browser.setDownloadBehavior',{behavior:'default',eventsEnabled:false});
      }finally{clearTimeout(timer);socket.close();}'''
    process = subprocess.Popen(['node', '--input-type=module', '-e', script,
        browser.settings['webSocketDebuggerUrl'], str(downloads)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([process.stdout], [], [], 3)[0], 'Download session did not start'
        assert process.stdout.readline().strip() == 'ready', 'Download session failed'
        yield
    finally:
        try:
            _, error = process.communicate('', timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)
            raise RuntimeError('Owned download session did not close') from None
        assert process.returncode == 0, 'Owned download session failed: ' + error[-1000:]


def export_download(browser, tree, inp, output, result):
    downloads = output / 'downloads'
    assert downloads.resolve().is_relative_to(n.ROOT / 'work/browser-gaps-2026-10-25/routing-test')
    downloads.mkdir(mode=0o700)
    with owned_download_session(browser, downloads):
        show_tools(browser, tree, inp)
        click_ui(browser, tree, inp, '#noventraExportSettings')
        path = downloads / 'noventra-workspace-settings.json'
        n.wait_for(lambda: path.exists() and not list(downloads.glob('*.crdownload')),
                   browser.process, timeout=10)
        value = json.loads(path.read_text())
        check(result, 'native export', 'actual download equals native settings schema', value, api(browser, 'export'))
        check(result, 'native export', 'schema contains only settings fields', sorted(value),
              ['format', 'layout', 'routes', 'spaces', 'version'])
        check(result, 'native export', 'Unicode space names survive UTF-8 download',
              [s['name'] for s in value['spaces']], ['Personal', WORK_NAME, ARCHIVE_NAME])
        focus_ui(browser, result, 'native export', 'noventraExportSettings')
        return path, value


def route_selection_reorder(browser, tree, inp, result):
    show_tools(browser, tree, inp)
    original = wm.api(browser)
    click_ui(browser, tree, inp, '#noventraRouteHost')
    n.native_key(inp, 'a', ('Control_L',))
    native_text(inp, 'selection.example')
    click_ui(browser, tree, inp, '#noventraRouteSpace')
    n.native_key(inp, 'Home')
    n.native_key(inp, 'Return')
    assert ui(browser, 'return root.querySelector("#noventraRouteSpace").value;') == ''
    wm.api(browser, 'move-before', WORK, '')
    wait_ui(browser, 'return root.querySelector("#noventraRouteSpace").options[0].value===' + json.dumps(WORK) + ';')
    visible = ui(browser, '''const e=root.querySelector('#noventraRouteSpace');
      return {value:e.value,label:e.selectedOptions[0].textContent.trim()};''')
    check(result, 'route selection reorder', 'reused option nodes keep the chosen Personal selection',
          visible, {'value': '', 'label': 'Personal'})
    click_ui(browser, tree, inp, '#noventraSaveRoute')
    wait_ui(browser, 'return !page.noventraToolsPending_ && page.noventraToolsStatus_==="Domain rule saved.";')
    saved = next(r for r in api(browser)['routes'] if r['host'] == 'selection.example')['space_id']
    check(result, 'route selection reorder', 'native Save agrees with the visible account', saved, visible['value'])
    check(result, 'route selection reorder', 'the intended Personal ID is saved', saved, '')
    focus_ui(browser, result, 'route selection reorder', 'noventraRouteHost')
    # Restore only fixture changes after recording both account assertions.
    wm.api(browser, 'move-before', '', WORK)
    api(browser, 'remove-route', {'host': 'selection.example'})
    check(result, 'route selection reorder', 'fixture order is restored', wm.api(browser), original)


def type_path(inp, path):
    """Replace a prefilled GTK location on this fixture's private Xvfb."""
    assert inp.platform == 'x11'
    picker = focus_picker(inp)
    n.native_key(inp, 'l', ('Control_L',))
    inp.pointer.click(300, 20)
    # The click collapses GTK's filename selection. Replace the entire entry.
    n.native_key(inp, 'a', ('Control_L',))
    native_text(inp, str(path))
    p = inp.pointer
    p.x.XGetGeometry.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
        C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_uint),
        C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
    root, left, top = C.c_ulong(), C.c_int(), C.c_int()
    width, height, border, depth = C.c_uint(), C.c_uint(), C.c_uint(), C.c_uint()
    assert p.x.XGetGeometry(p.d, picker, C.byref(root), C.byref(left), C.byref(top),
        C.byref(width), C.byref(height), C.byref(border), C.byref(depth))
    # Observed Open button in the native probe; GTK defaults Return to Cancel.
    p.click(left.value + width.value - 52, top.value + height.value - 23)
    time.sleep(.7)


def choose_file(browser, tree, inp, path, result, await_review=True):
    click_ui(browser, tree, inp, '#noventraChooseImport')
    type_path(inp, path)  # Real GTK chooser, never DOM.setFileInputFiles.
    if await_review:
        wait_ui(browser, 'return !root.querySelector("#noventraImportReview").hidden;')
        focus_ui(browser, result, 'file chosen', 'noventraConfirmImport')


def file_read_races(browser, tree, inp, value, output, result):
    """Delay real File.text reads; retain native chooser/change event behavior."""
    first, latest = output / 'delayed-a.json', output / 'delayed-b.json'
    earlier = deepcopy(value)
    earlier['spaces'] = earlier['spaces'][:2]
    earlier['routes'] = [r for r in earlier['routes'] if r['space_id'] != ARCHIVE]
    n.write_json(first, earlier)
    n.write_json(latest, value)
    before = snapshot(browser)
    ui(browser, '''
      window.toolsOriginalFileText=File.prototype.text;window.toolsFileReads={};
      File.prototype.text=function(){
        if(!['delayed-a.json','delayed-b.json'].includes(this.name))
          return window.toolsOriginalFileText.call(this);
        const name=this.name;
        return new Promise((resolve,reject)=>window.toolsOriginalFileText.call(this).then(
          text=>{window.toolsFileReads[name]={release:()=>resolve(text)};},reject));
      };return true;
    ''')
    try:
        for cancel in (False, True):
            case = 'delayed File.text cancellation' if cancel else 'delayed File.text latest wins'
            ui(browser, 'window.toolsFileReads={};return true;')
            choose_file(browser, tree, inp, first, result, await_review=False)
            wait_ui(browser, 'return !!window.toolsFileReads["delayed-a.json"];')
            choose_file(browser, tree, inp, latest, result, await_review=False)
            wait_ui(browser, 'return !!window.toolsFileReads["delayed-b.json"];')
            ui(browser, 'window.toolsFileReads["delayed-b.json"].release();return true;')
            wait_ui(browser, 'return !root.querySelector("#noventraImportReview").hidden && '
                    'root.querySelector("#noventraImportReview").innerText.includes("delayed-b.json");')
            assert ui(browser, 'return root.querySelector("#noventraImportLayout").checked;')
            click_ui(browser, tree, inp, '#noventraImportLayout')
            chosen = ui(browser, '''return {text:root.querySelector('#noventraImportReview').innerText,
              layout:root.querySelector('#noventraImportLayout').checked,
              hidden:root.querySelector('#noventraImportReview').hidden};''')
            check(result, case, 'native checkbox opts out of layout', chosen['layout'], False)
            if cancel:
                click_ui(browser, tree, inp, '#noventraCancelImport')
                wait_ui(browser, 'return root.querySelector("#noventraImportReview").hidden;')
                focus_ui(browser, result, case, 'noventraChooseImport')
                cancelled_layout = ui(browser, 'return root.querySelector("#noventraImportLayout").checked;')
            ui(browser, '''window.toolsFileReads['delayed-a.json'].release();
              await new Promise(resolve=>setTimeout(resolve,80));await page.updateComplete;return true;''')
            observed = ui(browser, '''return {text:root.querySelector('#noventraImportReview').innerText,
              layout:root.querySelector('#noventraImportLayout').checked,
              hidden:root.querySelector('#noventraImportReview').hidden};''')
            if cancel:
                check(result, case, 'late read cannot resurrect cancelled review', observed['hidden'])
                check(result, case, 'late read cannot reset Include layout', observed['layout'], cancelled_layout)
            else:
                check(result, case, 'late A cannot replace B preview or checkbox', observed, chosen)
            check(result, case, 'read races never mutate saved settings', snapshot(browser), before)
            result.setdefault('file_read_races', []).append({'case': case, 'chosen': chosen, 'after': observed})
            surface.capture(tree, inp, output, 'file-read-' + ('cancel' if cancel else 'latest')).close()
            if not ui(browser, 'return root.querySelector("#noventraImportReview").hidden;'):
                click_ui(browser, tree, inp, '#noventraCancelImport')
    finally:
        ui(browser, '''for(const read of Object.values(window.toolsFileReads))read.release();
          File.prototype.text=window.toolsOriginalFileText;
          delete window.toolsOriginalFileText;delete window.toolsFileReads;return true;''')


def malformed_imports(browser, value, result):
    bad = []
    for field, replacement in [('version', 2), ('spaces', []), ('format', 'not-noventra')]:
        candidate = deepcopy(value)
        candidate[field] = replacement
        bad.append((field, candidate))
    for name, change in [
        ('no Personal', lambda v: v['spaces'].pop(0)),
        ('Personal archived', lambda v: v['spaces'][0].update(archived=True)),
        ('duplicate ID', lambda v: v['spaces'].append(deepcopy(v['spaces'][1]))),
        ('invalid UTF-8 byte-length name', lambda v: v['spaces'][1].update(name='界' * 54)),
        ('unknown route target', lambda v: v['routes'].append({'host': 'new.example', 'space_id': STALE})),
        ('default port', lambda v: v['routes'].append({'host': 'new.example:443', 'space_id': WORK})),
        ('duplicate canonical host', lambda v: v['routes'].extend([
            {'host': 'DUPE.example.', 'space_id': ''}, {'host': 'dupe.example', 'space_id': WORK}])),
        ('101 routes', lambda v: v.update(routes=[{'host': f'h{i}.example', 'space_id': WORK} for i in range(101)])),
        ('over 1000 spaces', lambda v: v.update(spaces=v['spaces'] * 334)),
    ]:
        candidate = deepcopy(value)
        change(candidate)
        bad.append((name, candidate))
    before = snapshot(browser)
    for name, candidate in bad:
        response = reply(browser, 'import', {'text': json.dumps(candidate, ensure_ascii=False), 'applyLayout': False})
        check(result, 'malformed import', name + ' rejects', response['status'], 'rejected')
        check(result, 'malformed import', name + ' leaves all settings unchanged', snapshot(browser), before)
    for name, text in [('broken JSON', '{'), ('over 2 MB', ' ' * (2 * 1024 * 1024 + 1))]:
        # Large payload is constructed in the owned renderer, avoiding OS argv limits.
        response = browser.ev('''(async()=>{const {sendWithPromise}=await import('chrome://resources/js/cr.js');
          try {await sendWithPromise('noventraWorkspaceTools','import',{text:TEXT,applyLayout:false});
          return 'resolved';}catch(error){return 'rejected';}})()'''.replace(
              'TEXT', "' '.repeat(2097153)" if name == 'over 2 MB' else json.dumps(text)))
        check(result, 'malformed import', name + ' rejects', response, 'rejected')
    check(result, 'malformed import', 'broken/oversized JSON leaves settings unchanged', snapshot(browser), before)
    invalid_layout = deepcopy(value)
    invalid_layout['layout'] = {'version': 999, 'items': []}
    response = reply(browser, 'import', {'text': json.dumps(invalid_layout), 'applyLayout': True,
                                        'expectedRevision': before['layout']['revision']})
    check(result, 'malformed import', 'invalid optional layout rejects', response['status'], 'rejected')
    check(result, 'malformed import', 'invalid layout leaves workspace settings unchanged', snapshot(browser), before)


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


def narrow(browser, output, result):
    for width in (480, 340):
        with emulated_viewport(browser, width, 700):
            time.sleep(.3)
            for block in ('#noventraWorkspaceRoutes', '#noventraWorkspaceBackup'):
                data = ui(browser, '''
              const block=root.querySelector(BLOCK);block.scrollIntoView({block:'start'});
              const elements=[block,...block.querySelectorAll('input:not([hidden]),select,cr-button,.noventra-rule-name')]
                .filter(e=>e.getClientRects().length);
              return {width:innerWidth,overflow:document.documentElement.scrollWidth>innerWidth+1,
                controls:elements.map(e=>{const r=e.getBoundingClientRect();return {id:e.id||e.tagName,
                  left:r.left,right:r.right,width:r.width,height:r.height};})};
            '''.replace('BLOCK', json.dumps(block)))
                check(result, 'narrow ' + str(width), block + ' actual viewport width', data['width'], width)
                check(result, 'narrow ' + str(width), block + ' no document overflow', data['overflow'], False)
                check(result, 'narrow ' + str(width), block + ' visible controls contained',
                      bool(data['controls']) and all(c['width'] > 0 and c['height'] > 0 and
                      c['left'] >= -1 and c['right'] <= width + 1 for c in data['controls']))
                result.setdefault('narrow', []).append(data)
                shot = wm.protocol(browser, 'Page.captureScreenshot', {'format': 'png'})
                (output / f'narrow-{width}-{block[1:]}.png').write_bytes(base64.b64decode(shot['data']))


def import_flows(browser, tree, inp, base, pages, path, value, output, result):
    file_read_races(browser, tree, inp, value, output, result)
    before = snapshot(browser)
    click_ui(browser, tree, inp, '#noventraChooseImport')
    focus_picker(inp)
    n.native_key(inp, 'Escape')
    check(result, 'chooser cancel', 'cancelling the real picker changes no settings', snapshot(browser), before)
    focus_ui(browser, result, 'chooser cancel', 'noventraChooseImport')
    choose_file(browser, tree, inp, path, result)
    check(result, 'import review', 'reading a file changes no settings', snapshot(browser), before)
    narrow(browser, output, result)
    click_ui(browser, tree, inp, '#noventraCancelImport')
    wait_ui(browser, 'return root.querySelector("#noventraImportReview").hidden;')
    focus_ui(browser, result, 'review cancel', 'noventraChooseImport')
    check(result, 'review cancel', 'cancelled review changes no settings', snapshot(browser), before)

    choose_file(browser, tree, inp, path, result)
    ui(browser, 'editor.selected="back";const edge=editor.draft.current.items.find(i=>i.id==="back").edge;'
               'editor.move(edge==="right"?"top":"right");await editor.updateComplete;return true;')
    assert ui(browser, 'return editor.dirty();')
    click_ui(browser, tree, inp, '#noventraConfirmImport')
    wait_ui(browser, 'return page.noventraToolsError_.includes("Apply or discard");')
    check(result, 'dirty layout', 'dirty draft blocks import without metadata writes', snapshot(browser), before)
    check(result, 'dirty layout', 'draft remains dirty', ui(browser, 'return editor.dirty();'))
    focus_ui(browser, result, 'dirty layout', 'noventraConfirmImport')
    click_ui(browser, tree, inp, '#noventraCancelImport')
    ui(browser, 'editor.shadowRoot.querySelector("#cancelLayout").click();return true;')  # Fixture cleanup only.

    # A stale API revision must reject before names/order/archive/routes change.
    old_revision = browser.state()['layout']['revision']
    changed = deepcopy(browser.state()['layout'])
    control = next(item for item in changed['items'] if item['id'] == 'reload')
    control['edge'] = 'right' if control['edge'] != 'right' else 'top'
    n.apply(browser, changed)
    before = snapshot(browser)
    response = reply(browser, 'import', {'text': json.dumps(value), 'applyLayout': True,
                                        'expectedRevision': old_revision})
    check(result, 'stale layout revision', 'native optimistic revision rejects', response['status'], 'rejected')
    check(result, 'stale layout revision', 'rejection preserves all metadata and saved layout', snapshot(browser), before)
    malformed_imports(browser, value, result)

    incoming = deepcopy(value)
    incoming['spaces'][0]['name'] = 'Must not rename Personal'
    incoming['spaces'][1]['name'] = 'Импорт 東京 Café'
    incoming['spaces'][2]['name'] = 'Стар Архив'
    incoming['routes'].extend([{'host': 'IMPORT.localhost.', 'space_id': WORK},
                               {'host': 'archive.localhost', 'space_id': ARCHIVE}])
    incoming_path = output / 'incoming-settings.json'
    n.write_json(incoming_path, incoming)
    choose_file(browser, tree, inp, incoming_path, result)
    click_ui(browser, tree, inp, '#noventraImportLayout')  # Spaces-only import.
    assert ui(browser, 'return !root.querySelector("#noventraImportLayout").checked;')
    before = snapshot(browser)
    click_ui(browser, tree, inp, '#noventraConfirmImport')
    wait_ui(browser, 'return !page.noventraToolsPending_ && page.noventraToolsStatus_.startsWith("Imported ");')
    focus_ui(browser, result, 'native import', 'noventraChooseImport')
    after = snapshot(browser)
    originals = before['spaces']
    added = after['spaces'][len(originals):]
    check(result, 'native import', 'existing names/order/archive remain untouched', after['spaces'][:len(originals)], originals)
    check(result, 'native import', 'new order, Unicode names and archive flags follow file',
          [(s['name'], s['archived']) for s in added], [('Импорт 東京 Café', False), ('Стар Архив', True)])
    check(result, 'native import', 'all imported account IDs are fresh',
          len(added) == 2 and all(s['id'] not in {v['id'] for v in value['spaces']} for s in added))
    imported = next(s for s in added if s['name'] == 'Импорт 東京 Café')
    imported_archive = next(s for s in added if s['name'] == 'Стар Архив')
    route_map = {r['host']: r['space_id'] for r in after['routes']}
    check(result, 'native import', 'existing route wins while new routes map to new UUIDs',
          {k: route_map.get(k) for k in ('127.0.0.1', 'import.localhost', 'archive.localhost')},
          {'127.0.0.1': WORK, 'import.localhost': imported['id'], 'archive.localhost': imported_archive['id']})
    check(result, 'native import', 'unchecked layout preserves revision and all fields', after['layout'], before['layout'])
    retained(browser, pages, result, 'native import')
    wm.api(browser, 'switch', imported['id'])
    # Switching creates a blank tab; ordinary typing stays in that new account.
    blank = next(t for t in targets(browser) if t['url'] == 'about:blank' and browser.ev('document.visibilityState==="visible"', t))
    native_url(browser, tree, inp, base + '/imported-signed-out', target=blank)
    account(browser, tree, blank, result, 'imported fresh account', imported['name'], None)

    # Successful checked-layout import exercises the normal current-revision path.
    show_tools(browser, tree, inp)
    choose_file(browser, tree, inp, path, result)
    revision = browser.state()['layout']['revision']
    click_ui(browser, tree, inp, '#noventraConfirmImport')
    wait_ui(browser, 'return !page.noventraToolsPending_ && root.querySelector("#noventraImportReview").hidden;')
    focus_ui(browser, result, 'layout import', 'noventraChooseImport')
    saved = browser.state()['layout']
    check(result, 'layout import', 'checked layout commits one fresh revision', saved['revision'], revision + 1)
    check(result, 'layout import', 'checked layout imports saved controls',
          {k: v for k, v in saved.items() if k != 'revision'},
          {k: v for k, v in value['layout'].items() if k != 'revision'})
    retained(browser, pages, result, 'all imports')
    surface.capture(tree, inp, output, 'workspace-tools-final').close()


def restart(args, profile, base, pages, expected, result, budget):
    with n.session(args, profile, args.output / 'restart', (*n.FLAGS, '--restore-last-session'), result, budget) as (browser, tree, inp):
        check(result, 'normal restart', 'all space/rule/layout settings persist', snapshot(browser), expected)
        for name, page in pages.items():
            url = page['before']['url']
            candidates = [t for t in targets(browser) if t['url'] == url]
            check(result, 'normal restart', name + ' retained URL restored', bool(candidates))
            wm.api(browser, 'switch', page['space'])
            # Duplicate same-URL route tabs may also be restored. Account and
            # persistent storage are asserted; live identity ends at shutdown.
            target = candidates[0]
            n.request(browser.port, '/json/activate/' + target['id'])
            n.wait_for(lambda: browser.ev('!!window.toolsToken', target), browser.process, timeout=10)
            state = account(browser, tree, target, result, 'normal restart ' + name,
                            'Personal' if not page['space'] else WORK_NAME, page['account'])
            check(result, 'normal restart', name + ' is a restarted document', state['token'] != page['before']['token'])
        n.activate(browser, tree, inp, next(t for t in targets(browser) if t['url'] == pages['personal']['before']['url']))
        routed = os_open(browser, base + '/after-restart', args.output, 'after-restart', result)
        account(browser, tree, routed, result, 'after-restart', WORK_NAME, 'work')
        wm.normal_shutdown(browser, result)


def run(args):
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'executed-test.py').write_bytes(Path(__file__).read_bytes())
    result = {'passed': False, 'qualified': False, 'observe_only': args.observe_only,
              'browser': str(args.browser), 'binary_sha256': surface.sha256(args.browser),
              'source_sha256': surface.sha256(Path(__file__)), 'checks': [], 'completed': [],
              'scope': __doc__, 'work_budget_seconds': 720, 'total_budget_seconds': 780,
              'limitations': ['No host desktop, Wayland, touch or RTL qualification.',
                  'Managed preferences are guarded in source, not injected via machine policy.',
                  'Settings-only export cannot restore sessions, credentials or account data.',
                  'Model snapshots compare effective settings; restart reads persisted metadata.']}
    budget = Budget()
    def cancelled(*_):
        raise RuntimeError('Workspace tools cancelled')
    previous = signal.signal(signal.SIGTERM, cancelled)
    try:
        assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1'
        assert str(n.ROOT / 'work/ram-investigation/xvfb-') in os.environ.get('XAUTHORITY', '')
        assert os.environ.get('NOVENTRA_BENCH_XVFB_PID', '').isdigit()
        result['owned_xvfb_pid'] = int(os.environ['NOVENTRA_BENCH_XVFB_PID'])
        os.environ['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=/tmp/noventra-no-session-bus'
        os.environ['GDK_BACKEND'] = 'x11'
        with local_pages(args.output) as base, tempfile.TemporaryDirectory(prefix='profile-', dir=args.output) as temp:
            profile = Path(temp)
            (profile / 'Default').mkdir()
            (profile / 'Default/Preferences').write_text(json.dumps({
                'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 220},
                'noventra': {'browser': {'account_spaces': {WORK: WORK_NAME, ARCHIVE: ARCHIVE_NAME},
                    'account_space_order': ['', WORK, ARCHIVE], 'archived_account_spaces': [ARCHIVE],
                    'workspace_routes': [{'host': 'stale.localhost', 'space_id': STALE}]}}}, ensure_ascii=False))
            flags = (*n.FLAGS, '--password-store=basic', '--ui-toolkit=gtk',
                     '--host-resolver-rules=MAP *.localhost 127.0.0.1', '--no-proxy-server')
            with n.session(args, profile, args.output / 'initial', flags, result, budget) as (browser, tree, inp):
                result.update(owned_browser_pid=browser.process.pid, platform=browser.platform)
                response = reply(browser)
                result['contract'] = response
                if response['status'] != 'resolved':
                    result['unrun'] = ['Routing, export/import, focus and restart: handler absent or not responding.']
                    raise AssertionError(response)
                assert response['value']['manageable'] and response['value']['importable'], response
                Appearance(browser).scheme(1)
                layout = n.top_layout(browser.state()['layout'], surface.ORDER)
                for item in layout['items']:
                    if item['id'] in ('favorites', 'tabs', 'spaces'):
                        item['edge'] = 'left'
                        item['visible'] = True
                        item['order'] = ('favorites', 'tabs', 'spaces').index(item['id'])
                n.apply(browser, layout)
                pages = {name: seed_document(browser, tree, inp, base + '/' + name + '-retained', space, name)
                         for name, space in (('personal', ''), ('work', WORK))}
                result['documents'] = pages
                retained(browser, pages, result, 'setup isolation')
                result['phase'] = 'native routing'
                navigation_flows(browser, tree, inp, base, pages, args.output, result)
                result['completed'].append(result['phase'])
                result['phase'] = 'route validation'
                route_validation(browser, tree, inp, result)
                route_selection_reorder(browser, tree, inp, result)
                result['completed'].append(result['phase'])
                result['phase'] = 'native export and import'
                path, value = export_download(browser, tree, inp, args.output, result)
                import_flows(browser, tree, inp, base, pages, path, value, args.output, result)
                expected = snapshot(browser)
                n.write_json(args.output / 'expected-restart-model.json', expected)
                wm.normal_shutdown(browser, result)
                result['completed'].append(result['phase'])
            saved = json.loads((profile / 'Default/Preferences').read_text())
            n.write_json(args.output / 'persisted-noventra-settings.json', saved.get('noventra', {}))
            result['phase'] = 'normal restart'
            restart(args, profile, base, pages, expected, result, budget)
            result['completed'].append(result['phase'])
        result['passed'] = all(item['passed'] for item in result['checks'])
        result['qualified'] = result['passed'] and not args.observe_only
    except BaseException as error:
        result['error'] = repr(error)
        result['traceback'] = traceback.format_exc()
    finally:
        result['elapsed_seconds'] = round(time.monotonic() - budget.started, 2)
        budget.close()
        signal.signal(signal.SIGTERM, previous)
        n.write_json(args.output / 'result.json', result)
    print(json.dumps({'passed': result['passed'], 'qualified': result['qualified'],
                      'checks': len(result['checks']), 'output': str(args.output),
                      'error': result.get('error')}, ensure_ascii=False))
    return 0 if result['passed'] or args.observe_only else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe-only', action='store_true')
    raise SystemExit(run(parser.parse_args()))
