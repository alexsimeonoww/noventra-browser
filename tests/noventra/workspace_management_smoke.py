"""Bounded native workspace ordering and Archive/Restore on a private display.

Run from this checkout, with a NEW output directory:
  work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    work/ram-investigation/with-xvfb.py \
    work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    tests/noventra/workspace_management_smoke.py --browser /absolute/runtime \
    --output work/workspace-management-2026-10-25/test-candidate-r1

All profiles and HTTP documents are disposable and owned by this test. Native
input measures dot drag/drop, cancellation, shortcuts, menu archive and Settings
buttons. CDP prepares fixtures and reads witnesses; it never repairs a measured
interaction. Same-session document identity/drafts and account storage must
survive archive/restore. Across restart only tab URLs, storage, names, order and
archive flags are required; a restarted document is not the same live document.
--observe-only preserves failures and missing-flow reasons, never qualification.
Work is bounded to 360s plus 40s for evidence/cleanup. X11 scale 1, English, LTR;
no system policy, user profile, build, launcher or desktop is changed.
"""

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import threading
import time
import traceback

import chrome_surface_smoke as surface
import compact_chrome_smoke as n
import sidebar_footer_smoke as footer
import workspace_dots_smoke as dots
from glass_appearance_smoke import Appearance
from interface_polish_smoke import DEEP


STALE = '00000000-0000-4000-8000-000000000001'
ARCHIVE_LABEL = 'Archive current space'
STATE = '''({token:window.workspaceToken,origin:performance.timeOrigin,
  draft:document.querySelector('#draft')?.value,url:location.href,
  cookie:document.cookie,storage:localStorage.getItem('workspaceWitness')})'''
VISIBILITY = 'window.workspaceVisibility.slice()'
PAGE = '''<!doctype html><meta charset="utf-8"><title>Workspace fixture KEY</title>
<style>body{margin:32px;background:#eef3f8;font:16px sans-serif}
input{display:block;width:280px;height:36px;margin-top:12px}</style>
<label for="draft">Unsaved workspace fixture KEY</label><input id="draft">
<script>window.workspaceToken=crypto.randomUUID();window.workspaceVisibility=[];
document.addEventListener('visibilitychange',()=>workspaceVisibility.push(document.visibilityState));</script>'''


class Budget(n.Budget):
    def __init__(self):
        super().__init__()
        self.work_deadline = self.started + 360
        self.deadline = self.started + 400
        self.arm(self.work_deadline)

    @staticmethod
    def expired(*_):
        raise n.SmokeTimeout('Workspace management exhausted its bounded time budget')


@contextmanager
def local_pages():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            key = self.path.removeprefix('/')
            if key not in ('alpha', 'bravo', 'charlie', 'windowalpha', 'windowbravo'):
                self.send_error(404)
                return
            data = PAGE.replace('KEY', key).encode()
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


def check(result, case, name, actual, expected):
    surface.check(result, case, name, actual, expected)
    return actual == expected


def reply(browser, *args, target=None):
    """Keep rejected Promises distinct from a handler that never answers."""
    expression = '''(async()=>{
      const {sendWithPromise}=await import('chrome://resources/js/cr.js');
      return await Promise.race([
        sendWithPromise('noventraSpaces',...ARGS).then(
          value=>({status:'resolved',value}),error=>({status:'rejected',error:String(error)})),
        new Promise(resolve=>setTimeout(()=>resolve({status:'timeout'}),1800))]);
    })()'''.replace('ARGS', json.dumps(args))
    return browser.ev(expression, target)


def api(browser, action='list', space='', value='', target=None):
    response = reply(browser, action, space, value, target=target)
    assert response['status'] == 'resolved', (action, space, response)
    assert isinstance(response['value'], list), response
    return response['value']


def ids(spaces, active=False):
    return [space['id'] for space in spaces if not active or not space.get('archived', False)]


def names(spaces, active=False):
    return [space['name'] for space in spaces if not active or not space.get('archived', False)]


def moved(order, source, target, after=False):
    result = [value for value in order if value != source]
    result.insert(result.index(target) + int(after), source)
    return result


def settle(browser, predicate, timeout=3):
    """A failed expectation still produces a check and allows baseline evidence."""
    try:
        return n.wait_for(predicate, browser.process, timeout=timeout)
    except TimeoutError:
        return False


def current(tree):
    node = n.one(tree.read(), dots.MORE)
    return re.search(r'name=Account spaces: (.*?) name_from=', node['line'])[1]


def capture(tree, inp, output, label):
    with surface.capture(tree, inp, output, label):
        pass


def protocol(browser, method, params, target=None):
    steps = [{'method': method, 'params': params, 'returnValue': True}]
    run = subprocess.run(['node', str(n.ROOT / 'tests/noventra/layout_editor_input.mjs'),
        (target or browser.settings)['webSocketDebuggerUrl'], json.dumps(steps)],
        capture_output=True, text=True, timeout=25, check=True)
    return json.loads(run.stdout)


def target_window(browser, target):
    return protocol(browser, 'Browser.getWindowForTarget', {'targetId': target['id']})['windowId']


def make_document(browser, tree, inp, keys, base, key, space, storage_value=None):
    api(browser, 'switch', space)
    target = n.request(browser.port, '/json/new?' + base + '/' + key, 'PUT')
    n.wait_for(lambda: browser.ev('!!window.workspaceToken', target), browser.process, timeout=8)
    n.activate(browser, tree, inp, target)  # Fixture setup, before measurement.
    content = tree.rect(n.one(tree.read(), r'class_name=MultiContentsView '))
    field = browser.ev('(()=>{const r=document.querySelector("#draft").getBoundingClientRect();'
        'return {x:r.x+r.width/2,y:r.y+r.height/2}})()', target)
    inp.pointer.click(content['x'] + field['x'], content['y'] + field['y'])
    keys.type(key)
    if storage_value is not None:
        browser.ev('document.cookie="workspaceWitness="+' + json.dumps(storage_value) +
            '+"; Path=/; Max-Age=3600; SameSite=Lax";'
            'localStorage.setItem("workspaceWitness",' + json.dumps(storage_value) + ');true', target)
    state = browser.ev(STATE, target)
    assert state['token'] and state['draft'] == key, state
    return {'target': target, 'before': state, 'space': space}


def retained(browser, pages, result, case):
    after = {key: browser.ev(STATE, page['target']) for key, page in pages.items()}
    check(result, case, 'same loaded documents, typed drafts and account storage', after,
          {key: page['before'] for key, page in pages.items()})
    result.setdefault('documents_after', {})[case] = after
    return after


def select_document(browser, tree, inp, page):
    api(browser, 'switch', page['space'])
    n.activate(browser, tree, inp, page['target'])  # Establish the precondition only.


def native_switch(browser, tree, inp, name, page, result, case):
    dot = n.one(tree.read(), dots.DOT + re.escape(name) + r' name_from=')
    inp.pointer.click(*tree.center(dot))
    visible = settle(browser, lambda: browser.ev(n.JS['visible'], page['target']))
    check(result, case, 'native dot selects the retained target document', bool(visible), True)


def native_order(browser, tree, inp, keys, pages, result, output):
    cases = [('left', 48, False), ('left', 220, True),
             ('right', 48, False), ('right', 220, True)]
    for side, width, after in cases:
        case = f'drag-{side}-{width}'
        result['phase'] = case
        dots.layout(browser, side)
        if dots.rail(tree)['w'] != width:
            dots.resize_rail(tree, inp, side, width)
        select_document(browser, tree, inp, pages['alpha'])
        before = api(browser)
        order = ids(before)
        # Include a Personal drag. Other cases drag a nonselected workspace.
        source = '' if after else order[-1]
        destination = order[-1] if after else order[0]
        if source == destination:
            destination = order[0]
        labels = {space['id']: space['name'] for space in before}
        start_node = n.one(tree.read(), dots.DOT + re.escape(labels[source]) + r' name_from=')
        end_node = n.one(tree, dots.DOT + re.escape(labels[destination]) + r' name_from=')
        end_rect = tree.rect(end_node)
        check(result, case, 'native source and target remain 24px cells',
              [[tree.rect(node)['w'], tree.rect(node)['h']] for node in (start_node, end_node)],
              [[24, 24], [24, 24]])
        end = (end_rect['x'] + (18 if after else 6), end_rect['y'] + 12) if width == 220 else (
            end_rect['x'] + 12, end_rect['y'] + (18 if after else 6))
        visibility = browser.ev(VISIBILITY, pages['alpha']['target'])
        expected = moved(order, source, destination, after)
        start = tree.center(start_node)
        before_ids = dots.identities(tree)
        inp.move(*start)
        time.sleep(.1)
        inp.button(True)
        try:
            for step in range(1, 31):
                inp.move(*(a + (b - a) * step / 30 for a, b in zip(start, end)))
                time.sleep(.03)
            capture(tree, inp, output, case + '-held')
        finally:
            inp.button(False)
        settle(browser, lambda: ids(api(browser)) == expected, timeout=1)
        actual = api(browser)
        check(result, case, 'native drag saves the intended order', ids(actual), expected)
        check(result, case, 'drag does not switch the active workspace', current(tree), 'Personal')
        check(result, case, 'active document never becomes hidden during drag',
              browser.ev(VISIBILITY, pages['alpha']['target']), visibility)
        check(result, case, 'dot display follows the saved model order',
              [dots.dot_name(node) for node in dots.ordered_dots(tree.read())], names(actual))
        result.setdefault('native_control_ids', {})[case] = {
            'before': before_ids, 'after': dots.identities(tree)}
        retained(browser, pages, result, case)
        capture(tree, inp, output, case)
    # Escape and releasing outside the footer must not commit a partial drag.
    for cancel in ('outside', 'escape'):
        case = 'drag-cancel-' + cancel
        select_document(browser, tree, inp, pages['alpha'])
        before = api(browser)
        source = next(node for node in dots.dots(tree.read()) if 'selected=false' in node['line'])
        start = tree.center(source)
        content = tree.rect(n.one(tree, r'class_name=MultiContentsView '))
        end = (content['x'] + content['w'] / 2, content['y'] + content['h'] / 2)
        if cancel == 'escape':
            target = dots.ordered_dots(tree)[-1]
            if target is source:
                target = dots.ordered_dots(tree)[0]
            bounds = tree.rect(target)
            end = (bounds['x'] + 18, bounds['y'] + 12)
        visibility = browser.ev(VISIBILITY, pages['alpha']['target'])
        inp.move(*start)
        inp.button(True)
        try:
            for step in range(1, 16):
                inp.move(*(a + (b - a) * step / 15 for a, b in zip(start, end)))
                time.sleep(.02)
            if cancel == 'escape':
                capture(tree, inp, output, case + '-held')
                keys.key('Escape')
        finally:
            inp.button(False)
        time.sleep(.2)
        check(result, case, 'cancel leaves saved order unchanged', api(browser), before)
        check(result, case, 'cancel leaves active document visible throughout',
              browser.ev(VISIBILITY, pages['alpha']['target']), visibility)
        capture(tree, inp, output, case)


def open_menu(browser, tree, inp, output, label, result):
    active = names(api(browser), active=True)
    positions = footer.open_menu(tree, inp, output, label, active)
    check(result, label, 'menu uses the same saved active order',
          sorted(active, key=lambda name: positions[name][1]), active)
    extra = footer.popup_labels(output, label, [ARCHIVE_LABEL, 'Restore archived spaces'], required=False)
    return dict(positions, **extra)


def cycle(browser, tree, inp, pages, result, case):
    select_document(browser, tree, inp, pages['alpha'])
    expected = names(api(browser), active=True)
    for direction, delta in (('Right', 1), ('Left', -1)):
        sequence = []
        index = expected.index(current(tree))
        for _ in expected:
            n.native_key(inp, direction, ('Control_L', 'Alt_L'))
            index = (index + delta) % len(expected)
            settle(browser, lambda: current(tree) == expected[index], timeout=1)
            sequence.append(current(tree))
        wanted = [expected[(expected.index('Personal') + delta * step) % len(expected)]
                  for step in range(1, len(expected) + 1)]
        check(result, case, 'Ctrl+Alt+' + direction + ' follows saved active order', sequence, wanted)
    retained(browser, pages, result, case)


def settings_js(browser, body, target=None):
    return browser.ev('(async()=>{' + DEEP +
        'const page=deep(document,"settings-appearance-page");'
        'if(!page)throw Error("Appearance page missing");'
        'const root=page.shadowRoot;' + body + '})()', target)


def show_settings(browser, tree, inp):
    browser.ev('location.href="chrome://settings/appearance?section=spaces";true')
    n.wait_for(lambda: browser.ev('(()=>{' + DEEP +
        'return !!deep(document,"#noventraSpaces");})()'), browser.process, timeout=10)
    n.activate(browser, tree, inp, browser.settings)
    time.sleep(.3)


def settings_state(browser):
    return settings_js(browser, '''
      const buttons=row=>[...row.querySelectorAll('[data-action]')].map(e=>({
        action:e.dataset.action,label:e.getAttribute('aria-label'),disabled:e.disabled,
        tabIndex:e.tabIndex,text:e.textContent.trim()}));
      return {active:[...root.querySelectorAll('[data-space-row]')].map(row=>({
        id:row.dataset.spaceRow,buttons:buttons(row)})),
        archived:[...root.querySelectorAll('[data-archived-space]')].map(row=>({
          id:row.dataset.archivedSpace,buttons:buttons(row)})),
        archivedSection:!!root.querySelector('#noventraArchivedSpaces'),
        text:root.querySelector('#noventraSpaces').innerText};
    ''')


def settings_focus(browser):
    return settings_js(browser, '''
      const active=root.activeElement;
      return {space:active?.dataset.space ?? null,
        action:active?.dataset.action ?? null,label:active?.getAttribute('aria-label'),
        tag:active?.tagName};
    ''')


def selector(space, action, archived=False):
    attribute = 'data-archived-space' if archived else 'data-space-row'
    return '[' + attribute + '=' + json.dumps(space) + '] [data-action=' + json.dumps(action) + ']'


def settings_input(browser, tree, inp, keys, query, keyboard=False):
    data = settings_js(browser, '''
      const button=root.querySelector(QUERY);
      if(!button)throw Error('Control missing: '+QUERY);
      if(button.disabled)throw Error('Control disabled: '+QUERY);
      button.scrollIntoView({block:'center',behavior:'instant'});button.focus();
      const r=button.getBoundingClientRect();
      return {x:r.x+r.width/2,y:r.y+r.height/2,label:button.getAttribute('aria-label'),
        focused:root.activeElement===button,tabIndex:button.tabIndex};
    '''.replace('QUERY', json.dumps(query)))
    assert data['focused'] and data['tabIndex'] >= 0, data
    if keyboard:
        keys.key('Return')
    else:
        content = tree.rect(n.one(tree.read(), r'class_name=MultiContentsView '))
        inp.pointer.click(content['x'] + data['x'], content['y'] + data['y'])
    time.sleep(.2)
    return data


def settings_order(browser, tree, inp, keys, pages, result, output):
    show_settings(browser, tree, inp)
    state = settings_state(browser)
    result['settings_initial'] = state
    spaces = api(browser)
    supported = check(result, 'Settings', 'all active spaces have accessible management rows',
                      [row['id'] for row in state['active']], ids(spaces, active=True))
    if not supported:
        result['unrun'].append('Settings move/archive/restore input: management rows absent')
        capture(tree, inp, output, 'settings-controls-absent')
        return False
    for index, (row, space) in enumerate(zip(state['active'], spaces)):
        controls = {button['action']: button for button in row['buttons']}
        for action, suffix, disabled in (('move-before', 'earlier', index == 0),
                                         ('move-after', 'later', index == len(spaces) - 1)):
            control = controls.get(action, {})
            check(result, 'Settings', space['name'] + ' ' + suffix + ' accessible label',
                  control.get('label'), 'Move ' + space['name'] + ' ' + suffix)
            check(result, 'Settings', space['name'] + ' ' + suffix + ' boundary state',
                  control.get('disabled'), disabled)
        archive = controls.get('archive')
        if space['id']:
            check(result, 'Settings', space['name'] + ' archive accessible label',
                  (archive or {}).get('label'), 'Archive ' + space['name'])
        else:
            check(result, 'Settings', 'Personal cannot be archived from Settings',
                  archive is None or archive.get('disabled') is True, True)
    # Move Personal in both directions, the collapsed-overflow row, and rows
    # into both boundaries. Disabled Move controls must hand focus to that
    # workspace's name input, never to another row or the document body.
    actions = [('', 'auto'), ('', 'reverse'), (spaces[-1]['id'], 'earlier'),
               (None, 'first-boundary'), (None, 'last-boundary')]
    for space_id, direction in actions:
        order = ids(api(browser))
        if direction == 'first-boundary':
            space_id = order[1]
        elif direction == 'last-boundary':
            space_id = order[-2]
        index = order.index(space_id)
        if direction == 'auto':
            previous_action = 'move-before' if index else 'move-after'
            action = previous_action
        elif direction == 'reverse':
            action = 'move-after' if previous_action == 'move-before' else 'move-before'
        elif direction == 'last-boundary':
            action = 'move-after'
        else:
            action = 'move-before'
        other = order[index + (-1 if action == 'move-before' else 1)]
        expected = moved(order, space_id, other, action == 'move-after')
        evidence = settings_input(browser, tree, inp, keys, selector(space_id, action), keyboard=True)
        settle(browser, lambda: ids(api(browser)) == expected)
        check(result, 'Settings keyboard', evidence['label'], ids(api(browser)), expected)
        boundary = (action == 'move-before' and expected.index(space_id) == 0) or (
            action == 'move-after' and expected.index(space_id) == len(expected) - 1)
        expected_action = None if boundary else action
        settle(browser, lambda: settings_focus(browser)['space'] == space_id and
               settings_focus(browser)['action'] == expected_action)
        focus = settings_focus(browser)
        check(result, 'Settings keyboard', 'focus follows its name input at a boundary or its enabled Move control',
              [focus['space'], focus['action']], [space_id, expected_action])
        if boundary:
            check(result, 'Settings keyboard', 'disabled boundary Move hands focus to the same workspace name input',
                  [focus['tag'], focus['label']], ['INPUT', 'Space name'])
    capture(tree, inp, output, 'settings-order-controls')
    retained(browser, pages, result, 'Settings reorder')
    return True


def negative_api(browser, tree, result, phase='negative API'):
    before = api(browser)
    active_before = current(tree)
    nonpersonal = next(space['id'] for space in before if space['id'])
    cases = [
        ('unknown action', ('delete', nonpersonal, '')),
        ('Personal archive', ('archive', '', '')),
        ('malformed archive ID', ('archive', '../not-a-space', '')),
        ('stale archive ID', ('archive', STALE, '')),
        ('stale restore ID', ('restore', STALE, '')),
        ('malformed restore ID', ('restore', '../bad', '')),
        ('stale rename ID', ('rename', STALE, 'stale')),
        ('stale switch ID', ('switch', STALE, '')),
        ('malformed move source', ('move-before', '../bad', nonpersonal)),
        ('stale move source', ('move-before', STALE, nonpersonal)),
        ('stale move target', ('move-after', nonpersonal, STALE)),
        ('malformed move target', ('move-before', nonpersonal, '../bad')),
        ('missing arguments', ('list',)),
        ('nonstring ID', ('archive', {'id': nonpersonal}, '')),
        ('nonstring target', ('move-before', nonpersonal, 7)),
    ]
    for label, args in cases:
        response = reply(browser, *args)
        result.setdefault('api_rejections', {})[label] = response
        check(result, phase, label + ' rejects rather than timing out/succeeding', response['status'], 'rejected')
        check(result, phase, label + ' leaves names/order/archive unchanged', api(browser), before)
        check(result, phase, label + ' leaves the active workspace unchanged', current(tree), active_before)


def require_archive(browser, space, archived):
    assert settle(browser, lambda: next(s for s in api(browser) if s['id'] == space)['archived'] == archived), (
        'Archive state did not change', space, archived, api(browser))


def archive_flows(browser, tree, inp, keys, pages, result, output):
    work = pages['bravo']['space']
    studio = pages['charlie']['space']
    original = api(browser)
    work_name = next(s['name'] for s in original if s['id'] == work)
    select_document(browser, tree, inp, pages['bravo'])
    positions = open_menu(browser, tree, inp, output, 'native-archive-menu', result)
    assert ARCHIVE_LABEL in positions, ('Archive menu item missing', positions)
    inp.pointer.click(*positions[ARCHIVE_LABEL])
    require_archive(browser, work, True)
    check(result, 'native archive', 'active workspace falls back to Personal', current(tree), 'Personal')
    check(result, 'native archive', 'archive preserves its saved position', ids(api(browser)), ids(original))
    check(result, 'native archive', 'archived dot omitted', work_name in [dots.dot_name(d) for d in dots.dots(tree.read())], False)
    retained(browser, pages, result, 'native archive')
    archived = api(browser)
    response = reply(browser, 'switch', work, '')
    check(result, 'archived selection', 'API switch to archived Work rejects', response['status'], 'rejected')
    check(result, 'archived selection', 'rejected switch does not unarchive Work', api(browser), archived)
    check(result, 'archived selection', 'rejected switch leaves Work document hidden',
          browser.ev(n.JS['visible'], pages['bravo']['target']), False)
    select_document(browser, tree, inp, pages['alpha'])
    positions = open_menu(browser, tree, inp, output, 'menu-after-archive', result)
    missing = footer.popup_labels(output, 'menu-after-archive', [work_name], required=False)
    check(result, 'native archive', 'archived workspace omitted from active menu', work_name in missing, False)
    assert ARCHIVE_LABEL in positions, 'Personal menu must expose the disabled archive command'
    before = api(browser)
    inp.pointer.click(*positions[ARCHIVE_LABEL])
    check(result, 'native archive', 'Personal archive menu command cannot mutate spaces', api(browser), before)
    keys.key('Escape')
    cycle(browser, tree, inp, pages, result, 'cycle omits archived')
    positions = open_menu(browser, tree, inp, output, 'restore-discovery-menu', result)
    assert 'Restore archived spaces' in positions, 'Archive recovery must be discoverable from More'
    inp.pointer.click(*positions['Restore archived spaces'])
    opened = settle(browser, lambda: next((t for t in n.request(browser.port, '/json/list')
        if t['url'] == 'chrome://settings/appearance?section=spaces'), None))
    assert opened, 'Restore archived spaces did not open Settings Spaces'
    browser.settings = opened
    n.wait_for(lambda: settings_state(browser)['archived'], browser.process, timeout=8)
    state = settings_state(browser)
    result['settings_after_archive'] = state
    check(result, 'Settings archive', 'archived section contains Work',
          [s['id'] for s in state['archived']], [work])
    restore = state['archived'][0]['buttons'] if state['archived'] else []
    check(result, 'Settings archive', 'Restore accessible label',
          next((b['label'] for b in restore if b['action'] == 'restore'), None), 'Restore ' + work_name)
    settings_input(browser, tree, inp, keys, selector(work, 'restore', True), keyboard=True)
    require_archive(browser, work, False)
    focus = settings_focus(browser)
    check(result, 'Settings restore', 'keyboard focus follows restored Work',
          [focus['space'], focus['action']], [work, 'archive'])
    check(result, 'Settings restore', 'restore leaves active Personal unchanged', current(tree), 'Personal')
    check(result, 'Settings restore', 'restore returns to original saved slot', api(browser), original)
    retained(browser, pages, result, 'Settings restore')
    native_switch(browser, tree, inp, work_name, pages['bravo'], result, 'restored Work')
    show_settings(browser, tree, inp)
    settings_input(browser, tree, inp, keys, selector(studio, 'archive'))
    require_archive(browser, studio, True)
    focus = settings_focus(browser)
    check(result, 'Settings arbitrary archive', 'focus follows Studio to its Restore control',
          [focus['space'], focus['action']], [studio, 'restore'])
    check(result, 'Settings arbitrary archive', 'archiving inactive Studio keeps Personal active', current(tree), 'Personal')
    retained(browser, pages, result, 'Settings arbitrary archive')
    capture(tree, inp, output, 'settings-archived-studio')


def window_trees(browser, tree):
    """Read every owned window, independently of native diagnostics' first window."""
    return browser.ev('''(async()=>{
      const xhr=new XMLHttpRequest();xhr.open('GET','targets-data.json',false);xhr.send();
      const data=JSON.parse(xhr.responseText);
      const {addWebUiListener,removeWebUiListener}=await import('chrome://resources/js/cr.js');
      const trees=[];
      for(const browser of data.browsers){
        const text=await new Promise((resolve,reject)=>{
          const timer=setTimeout(()=>reject(Error('Window tree timed out')),3000);
          const listener=addWebUiListener('showOrRefreshTree',result=>{
            clearTimeout(timer);removeWebUiListener(listener);resolve(result.tree);});
          chrome.send('requestNativeUITree',[{sessionId:browser.sessionId,
            requestType:'showOrRefreshTree',filters:{allow:'*',allowEmpty:'',deny:''}}]);});
        trees.push({sessionId:browser.sessionId,text});
      }return trees;
    })()''', tree.target)


def window_state(browser, tree, output, label):
    trees = window_trees(browser, tree)
    states = []
    for value in trees:
        (output / f'{label}-window-{value["sessionId"]}.txt').write_text(value['text'])
        active = re.findall(r'class_name=AccountSpaceDotButton name=Account spaces: (.*?) name_from=', value['text'])
        entries = []
        for line in value['text'].splitlines():
            name = re.search(dots.DOT + r'(.*?) name_from=', line)
            bounds = re.search(r'\((-?\d+), (-?\d+)\)-\((\d+), (\d+)\)', line)
            if name and bounds and ' INVISIBLE ' not in line and int(bounds[3]) and int(bounds[4]):
                entries.append((int(bounds[2]), int(bounds[1]), name[1]))
        states.append({'sessionId': value['sessionId'], 'active': active,
                       'dots': [entry[2] for entry in sorted(entries)]})
    return states


def multiwindow(browser, tree, inp, keys, pages, base, result, output):
    work = pages['bravo']['space']
    select_document(browser, tree, inp, pages['bravo'])
    created = protocol(browser, 'Target.createTarget', {
        'url': 'chrome://settings/appearance?section=spaces&workspace-test=second', 'newWindow': True})
    second = n.wait_for(lambda: next((t for t in n.request(browser.port, '/json/list')
        if t['id'] == created['targetId']), None), browser.process)
    n.wait_for(lambda: browser.ev('document.readyState==="complete" && !!document.querySelector("settings-ui")?.shadowRoot', second), browser.process)
    first_window = target_window(browser, browser.settings)
    second_window = target_window(browser, second)
    assert first_window != second_window, 'Multiwindow fixture did not create another native window'
    n.request(browser.port, '/json/activate/' + second['id'])
    # Temporarily route fixture API calls to the Settings in window two.
    first_settings = browser.settings
    browser.settings = second
    try:
        extra = {
            'windowalpha': make_document(browser, tree, inp, keys, base, 'windowalpha', ''),
            'windowbravo': make_document(browser, tree, inp, keys, base, 'windowbravo', work),
        }
        for page in extra.values():
            assert target_window(browser, page['target']) == second_window
        check(result, 'multiwindow setup', 'Personal cookie jar shared only with Personal',
              extra['windowalpha']['before']['storage'], 'alpha')
        check(result, 'multiwindow setup', 'Work cookie jar shared only with Work',
              extra['windowbravo']['before']['storage'], 'bravo')
        all_pages = dict(pages, **extra)
        check(result, 'multiwindow', 'both windows read identical saved order and flags', api(browser), api(browser, target=first_settings))
        before_order = ids(api(browser))
        api(browser, 'move-before', before_order[-1], before_order[0], target=first_settings)
        expected = moved(before_order, before_order[-1], before_order[0])
        check(result, 'multiwindow', 'reordering from one window updates the other', ids(api(browser)), expected)
        observed = window_state(browser, tree, output, 'multiwindow-reordered')
        result['multiwindow_reordered'] = observed
        check(result, 'multiwindow', 'both native footers display the shared active order',
              [state['dots'] for state in observed], [names(api(browser), active=True)] * 2)
        api(browser, 'archive', work, target=first_settings)
        require_archive(browser, work, True)
        observed = window_state(browser, tree, output, 'multiwindow-archived')
        result['multiwindow_archived'] = observed
        check(result, 'multiwindow archive', 'both active windows fall back to Personal',
              [state['active'] for state in observed], [['Personal'], ['Personal']])
        retained(browser, all_pages, result, 'multiwindow archive')
        # External selection is a regression trigger, not a successful switch.
        n.request(browser.port, '/json/activate/' + pages['bravo']['target']['id'])
        time.sleep(.25)
        observed = window_state(browser, tree, output, 'archived-selection-rejected')
        check(result, 'archived selection', 'external target selection cannot leave an archived workspace active',
              [state['active'] for state in observed], [['Personal'], ['Personal']])
        require_archive(browser, work, True)
        # Close every Personal tab in window two, leaving its loaded archived
        # tab. A replacement Personal page must be selected, not archived work.
        second_targets = [t for t in n.request(browser.port, '/json/list') if t['type'] == 'page'
                          and target_window(browser, t) == second_window]
        closed = []
        for target in second_targets:
            if target['id'] != extra['windowbravo']['target']['id']:
                n.request(browser.port, '/json/close/' + target['id'])
                closed.append(target['id'])
        browser.settings = first_settings
        time.sleep(.3)
        observed = window_state(browser, tree, output, 'last-visible-close')
        result['last_visible_close'] = {'closed_owned_targets': closed, 'windows': observed}
        check(result, 'last visible close', 'both windows retain a Personal fallback',
              [state['active'] for state in observed], [['Personal'], ['Personal']])
        check(result, 'last visible close', 'archived document remains loaded',
              browser.ev(STATE, extra['windowbravo']['target']), extra['windowbravo']['before'])
        require_archive(browser, work, True)
        api(browser, 'restore', work)
        require_archive(browser, work, False)
        check(result, 'multiwindow restore', 'restore alone does not activate Work in either window',
              [state['active'] for state in window_state(browser, tree, output, 'multiwindow-restored')],
              [['Personal'], ['Personal']])
        n.request(browser.port, '/json/activate/' + extra['windowbravo']['target']['id'])
        assert settle(browser, lambda: browser.ev(n.JS['visible'], extra['windowbravo']['target']))
        check(result, 'multiwindow restore', 'restored secondary Work tab has its original document and draft',
              browser.ev(STATE, extra['windowbravo']['target']), extra['windowbravo']['before'])
    finally:
        browser.settings = first_settings
    # Only fixture-owned secondary-window targets are closed; main witnesses live.
    cleanup_ids = []
    for target in n.request(browser.port, '/json/list'):
        if target['type'] == 'page' and target_window(browser, target) == second_window:
            n.request(browser.port, '/json/close/' + target['id'])
            cleanup_ids.append(target['id'])
    n.wait_for(lambda: not set(cleanup_ids) & {t['id'] for t in n.request(browser.port, '/json/list')},
               browser.process, timeout=8)
    result['secondary_window_cleanup'] = {
        'closed_targets': cleanup_ids,
        'remaining_targets': n.request(browser.port, '/json/list')}
    check(result, 'multiwindow cleanup', 'only the original native window remains',
          len(window_trees(browser, tree)), 1)
    n.request(browser.port, '/json/activate/' + pages['alpha']['target']['id'])
    retained(browser, pages, result, 'multiwindow complete')


def stored_preferences(profile):
    data = json.loads((profile / 'Default/Preferences').read_text())
    values = data.get('noventra', {}).get('browser', {})
    return {key: values.get(key) for key in
            ('account_spaces', 'account_space_order', 'archived_account_spaces')}


def normal_shutdown(browser, result):
    """Use the existing account-space regression's normal session-save path.

    LayoutBrowser's owned-group SIGTERM is an emergency teardown, not a normal
    quit: it can terminate renderers before pending closed-tab/session updates
    are committed. Leave that helper unchanged and explicitly quit this fixture.
    """
    endpoint = n.request(browser.port, '/json/version')['webSocketDebuggerUrl']
    script = '''const s=new WebSocket(process.argv[1]);
      s.onopen=()=>s.send(JSON.stringify({id:1,method:'Browser.close'}));
      s.onclose=()=>process.exit(0);setTimeout(()=>process.exit(1),10000);'''
    subprocess.run(['node', '-e', script, endpoint], check=True, timeout=15)
    result['normal_shutdown_exit_code'] = browser.process.wait(timeout=15)
    check(result, 'restart setup', 'owned browser exits after normal session-saving quit',
          result['normal_shutdown_exit_code'], 0)


def restart_checks(args, profile, base, pages, expected, result, budget, supported):
    flags = (*n.FLAGS, '--restore-last-session')
    with n.session(args, profile, args.output / 'restart', flags, result, budget) as (browser, tree, inp):
        check(result, 'restart', 'saved order, names and archive flags round-trip', api(browser), expected)
        native = window_state(browser, tree, args.output, 'restart-native')
        result['restart_native'] = native
        check(result, 'restart', 'restored archived tabs do not become active',
              all(all(name not in names([s for s in expected if s.get('archived')])
                      for name in state['active']) for state in native), True)
        restored = n.request(browser.port, '/json/list')
        check(result, 'restart', 'all original account tab URLs are restored',
              sorted({t['url'] for t in restored if t['url'].startswith(base + '/')}),
              sorted(page['before']['url'] for page in pages.values()))
        if supported:
            for key, page in pages.items():
                space = page['space']
                if next(s for s in api(browser) if s['id'] == space)['archived']:
                    api(browser, 'restore', space)
                api(browser, 'switch', space)
                target = next(t for t in n.request(browser.port, '/json/list') if t['url'] == page['before']['url'])
                # Restore may defer a background tab's renderer. Select the
                # actual restored tab, as a user would, without navigating or
                # reloading it; switching account alone can select Settings.
                n.request(browser.port, '/json/activate/' + target['id'])
                n.wait_for(lambda: browser.ev('!!window.workspaceToken', target), browser.process, timeout=8)
                state = browser.ev(STATE, target)
                check(result, 'restart', key + ' restored tab selects its original workspace',
                      current(tree), next(s['name'] for s in expected if s['id'] == space))
                check(result, 'restart', key + ' cookie and localStorage remain in their original account partition',
                      {k: state[k] for k in ('cookie', 'storage')},
                      {k: page['before'][k] for k in ('cookie', 'storage')})
                check(result, 'restart', key + ' is correctly treated as a restarted document',
                      state['token'] != page['before']['token'] and state['origin'] != page['before']['origin'], True)
        capture(tree, inp, args.output, 'restart-final')


def run(args):
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'executed-test.py').write_bytes(Path(__file__).read_bytes())
    result = dict(passed=False, qualified=False, observe_only=args.observe_only,
        browser=str(args.browser), binary_sha256=surface.sha256(args.browser),
        source_sha256=surface.sha256(Path(__file__)), checks=[], unrun=[],
        completed=[], scope=__doc__, work_budget_seconds=360, total_budget_seconds=400,
        checkout=str(n.ROOT.resolve()), limitations=[
            'Managed-pref mutation guards are not injected: these custom prefs have no supplied policy binding; no machine policy is changed.',
            'No permanent deletion is requested; unknown delete API action must reject.',
            'No touch, RTL, physical desktop or compositor paint qualification.',
            'Restart checks do not require live document identity or unsaved form data to survive a process restart.'])
    budget = Budget()
    def cancelled(*_):
        raise RuntimeError('Workspace management smoke cancelled')
    previous = signal.signal(signal.SIGTERM, cancelled)
    try:
        assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1', 'This fixture currently requires the supplied private Xvfb wrapper'
        with local_pages() as base, tempfile.TemporaryDirectory(prefix='profile-', dir=args.output) as tmp:
            profile = Path(tmp)
            (profile / 'Default').mkdir()
            (profile / 'Default/Preferences').write_text(json.dumps({
                'vertical_tabs': {'collapsed_state': True, 'uncollapsed_width': 220}}))
            with n.session(args, profile, args.output, n.FLAGS, result, budget) as (browser, tree, inp), dots.Keyboard(inp) as keys:
                result.update(platform=browser.platform, owned_browser_pid=browser.process.pid)
                result['phase'] = 'setup'
                Appearance(browser).scheme(1)
                dots.layout(browser, 'left')
                initial = api(browser)
                supported = all(isinstance(space.get('archived'), bool) for space in initial)
                check(result, 'contract', 'list exposes explicit archived booleans', supported, True)
                spaces = initial
                created_ids = {}
                for name in ('Work fixture', 'Studio fixture', 'Inbox fixture'):
                    before = ids(spaces)
                    spaces = api(browser, 'create', '', name)
                    new = next(s for s in spaces if s['name'] == name)['id']
                    created_ids[name] = new
                    check(result, 'creation', name + ' appends without UUID reshuffling', ids(spaces), before + [new])
                pages = {key: make_document(browser, tree, inp, keys, base, key, space, key)
                         for key, space in (('alpha', ''), ('bravo', created_ids['Work fixture']),
                                            ('charlie', created_ids['Studio fixture']))}
                result['documents'] = pages
                retained(browser, pages, result, 'initial partition isolation')
                result['completed'].append('setup')
                native_order(browser, tree, inp, keys, pages, result, args.output)
                result['completed'].append('native drag and cancellation')
                before = ids(api(browser))
                appended = api(browser, 'create', '', 'Append fixture')
                new_id = next(s['id'] for s in appended if s['name'] == 'Append fixture')
                check(result, 'creation', 'new workspace appends after reordering', ids(appended), before + [new_id])
                api(browser, 'rename', pages['bravo']['space'], 'Renamed work')
                result['phase'] = 'Settings order'
                settings_supported = settings_order(browser, tree, inp, keys, pages, result, args.output)
                select_document(browser, tree, inp, pages['alpha'])
                open_menu(browser, tree, inp, args.output, 'saved-order-menu', result)
                keys.key('Escape')
                cycle(browser, tree, inp, pages, result, 'saved order cycle')
                result['completed'].append('Settings/menu/shortcut order')
                result['phase'] = 'negative API'
                negative_api(browser, tree, result)
                result['completed'].append('negative API')
                if supported and settings_supported:
                    result['phase'] = 'Archive/Restore'
                    archive_flows(browser, tree, inp, keys, pages, result, args.output)
                    result['completed'].append('Archive/Restore')
                    result['phase'] = 'multiple windows and archived selection'
                    multiwindow(browser, tree, inp, keys, pages, base, result, args.output)
                    result['completed'].append('multiple windows and archived selection')
                    api(browser, 'archive', pages['bravo']['space'])
                else:
                    result['unrun'].append('Archive/Restore, multiwindow archive, archived tab selection/close: API archived flag or Settings controls absent')
                result['expected_spaces'] = expected = api(browser)
                retained(browser, pages, result, 'before restart')
                capture(tree, inp, args.output, 'before-restart')
                normal_shutdown(browser, result)
            result['saved_preferences'] = saved = stored_preferences(profile)
            n.write_json(args.output / 'saved-workspace-preferences.json', saved)
            if supported:
                check(result, 'preferences', 'saved order includes active and archived workspaces', saved['account_space_order'], ids(expected))
                check(result, 'preferences', 'archive preference preserves the archived IDs',
                      sorted(saved['archived_account_spaces'] or []), sorted(s['id'] for s in expected if s['archived']))
            result['phase'] = 'restart'
            restart_checks(args, profile, base, pages, expected, result, budget, supported)
            result['completed'].append('restart')
        check(result, 'runtime', 'tested binary unchanged', surface.sha256(args.browser), result['binary_sha256'])
        result['passed'] = bool(result['checks']) and all(entry['passed'] for entry in result['checks']) and not result['unrun']
        result['qualified'] = result['passed'] and not args.observe_only
        result['phase'] = 'complete'
    except BaseException as error:
        result['error'] = repr(error)
        (args.output / 'exception.txt').write_text(traceback.format_exc())
    finally:
        budget.close()
        signal.signal(signal.SIGTERM, previous)
        if 'browser' in locals():
            result['owned_browser_stopped'] = browser.process.poll() is not None
        result['elapsed'] = time.monotonic() - budget.started
        result['failures'] = [entry for entry in result['checks'] if not entry['passed']]
        n.write_json(args.output / 'result.json', result)
        print(json.dumps({key: result.get(key) for key in
            ('passed', 'qualified', 'observe_only', 'elapsed', 'phase', 'completed', 'failures', 'unrun', 'error')}, indent=2))
    # Observation changes only the exit status of completed baseline checks.
    # Fixture exceptions still fail and remain visible in result.json.
    return 0 if result['passed'] or (args.observe_only and 'error' not in result) else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe-only', action='store_true')
    raise SystemExit(run(parser.parse_args()))
