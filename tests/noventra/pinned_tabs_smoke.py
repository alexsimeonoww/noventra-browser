"""Native persistent-pin regression on an authenticated private X11 display.

Each case uses a fresh disposable profile and a loopback server. Pin edits,
validation, reset/use-current, duplication and close/reopen use trusted input.
Menu widgets are outside Chromium's browser AX dump: their current screenshot
labels are measured with the existing OCR helper, never stale coordinates.
Fields, buttons and pin labels use the real native accessibility tree.
"""

import http.server
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import threading
import time
import urllib.parse

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / 'tests/noventra').is_dir())
sys.path.insert(0, str(ROOT / 'tests/noventra/inline_footer'))

from sidebar_common import (
    Budget, activate_settings, admit, check, cli, native, native_key, prefs,
    request, session, sha, wait_for, write,
)
from sidebar_command_footer_smoke import extension_fixture
from sidebar_command_smoke import type_text
from sidebar_footer_smoke import popup_labels
import tab_click_editor_smoke as tab

EDIT = 'Edit pinned tab'
RESET = 'Reset to home'
USE_CURRENT = 'Use current page as home'
NAME = r'class_name=Textfield name=Name name_from='
HOME = r'class_name=Textfield name=Home URL name_from='
SAVE = r'class_name=MdTextButton name=(?:Save|OK) name_from='
CANCEL = r'class_name=MdTextButton name=Cancel name_from='


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == '/set':
            owner = urllib.parse.parse_qs(parsed.query)['owner'][0]
            self.send_response(200)
            self.send_header('Set-Cookie', 'owner=' + owner
                             + '; HttpOnly; SameSite=Lax; Path=/; Max-Age=3600')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if parsed.path == '/cookie':
            data = self.headers.get('Cookie', '').encode()
            kind = 'text/plain'
        else:
            title = 'pin ' + parsed.path.strip('/')
            data = ('<!doctype html><meta charset="utf-8"><title>' + title + '</title>'
                    '<style>body{margin:32px;font:16px sans-serif;background:#eef3f8}</style>'
                    '<h1>' + title + '</h1><input id="draft" value="original">'
                    '<script>window.pinToken=crypto.randomUUID();</script>').encode()
            kind = 'text/html; charset=utf-8'
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def pages(browser):
    return [page for page in request(browser.port, '/json/list')
            if page['type'] == 'page']


def ready(browser, page, url=None):
    wanted = ('location.href===' + json.dumps(url) + '&&') if url else ''
    return wait_for(lambda: browser.ev(
        wanted + 'document.readyState==="complete"&&!!document.querySelector("#draft")', page),
        browser.process, timeout=8)


def navigate(browser, page, url):
    browser.ev('location.href=' + json.dumps(url) + ';true', page)
    ready(browser, page, url)


def open_page(browser, tree, inp, url):
    # Trusted New Tab inherits the selected account Space. Navigating that
    # actual blank page avoids CDP creating a tab with an ambiguous owner.
    before = {page['id'] for page in pages(browser)}
    native_key(inp, 't', ('Control_L',))
    page = wait_for(lambda: next((p for p in pages(browser)
                    if p['id'] not in before), None), browser.process, timeout=6)
    navigate(browser, page, url)
    tab.activate(browser, tree, inp, page)
    return page


def state(browser, page):
    return browser.ev('''(async()=>({url:location.href,title:document.title,
      token:window.pinToken,timeOrigin:performance.timeOrigin,
      draft:document.querySelector('#draft')?.value,scriptCookie:document.cookie,
      cookie:await(await fetch('/cookie')).text(),
      owner:localStorage.getItem('owner'),discarded:document.wasDiscarded}))()''', page)


def seed_account(browser, page, owner):
    browser.ev('''(async()=>{const owner=OWNER;await fetch('/set?owner='+owner);
      localStorage.setItem('owner',owner);document.querySelector('#draft').value=owner+' unsaved';
      return true;})()'''.replace('OWNER', json.dumps(owner)), page)


def account_is(value, owner):
    return (value['cookie'] == 'owner=' + owner and value['scriptCookie'] == ''
            and value['owner'] == owner)


def active_tab(api):
    return api('chrome.tabs.query({active:true,currentWindow:true})'
               '.then(t=>({id:t[0].id,url:t[0].url,pinned:t[0].pinned,title:t[0].title}))')


def pin(browser, api, page):
    request(browser.port, '/json/activate/' + page['id'])
    value = active_tab(api)
    api('chrome.tabs.update(' + str(value['id']) + ',{pinned:true})')
    return value['id']


def pin_node(browser, tree, title, *, selected=True):
    pattern = (r'class_name=TabView name=' + re.escape(title)
               + r'(?: - Pinned)?(?: - Inactive tab)? name_from=')

    def found():
        nodes = native.visible(tree.read(), pattern)
        if selected:
            nodes = [node for node in nodes if 'selected=true' in node['line']]
        return nodes[0] if len(nodes) == 1 else None

    return wait_for(found, browser.process, timeout=5)


def menu(browser, tree, inp, result, output, page, title, labels):
    tab.activate(browser, tree, inp, page)
    node = pin_node(browser, tree, title)
    bounds = tab.bounds(tree, node)
    browser_bounds = tab.box(tree, r'class_name=BrowserView ')
    assert native.inside(bounds, browser_bounds), (bounds, browser_bounds)
    tab.native_button(inp, 3, tab.center(bounds))
    number = len(result.setdefault('menus', []))
    name = 'pin-menu-' + str(number)
    native.capture(tree, inp, output, name)
    positions = popup_labels(output, name, ['Unpin', *labels], required=False)
    check(result, 'actual pinned menu has its existing Unpin action', 'Unpin' in positions,
          {'capture': name, 'labels': sorted(positions)})
    result['menus'].append({'capture': name, 'title': title, 'positions': positions})
    return positions


def action(browser, tree, inp, result, output, page, title, label):
    positions = menu(browser, tree, inp, result, output, page, title, [label])
    check(result, 'pinned context menu exposes ' + label, label in positions,
          {'labels': sorted(positions), 'expected': label})
    inp.pointer.click(*positions[label])


def field_value(tree, pattern):
    node = native.one(tree.read(), pattern)
    match = re.search(r' value=(.*?)(?: actions=|$)', node['line'])
    return match[1].rstrip() if match else ''


def dialog(browser, tree):
    wait_for(lambda: bool(native.visible(tree.read(), NAME))
             and bool(native.visible(tree, HOME)), browser.process, timeout=5)
    native.one(tree, SAVE)
    native.one(tree, CANCEL)
    return {'name': field_value(tree, NAME), 'home': field_value(tree, HOME)}


def button(tree, inp, pattern):
    inp.pointer.click(*tab.center(tab.box(tree, pattern)))


def fill(tree, inp, pattern, value):
    # The native dialog initially focuses Name. Its next tab stop is Home URL.
    # AX exposes widget-local dialog bounds without a desktop origin, so use
    # keyboard focus and verify the real named field's resulting value.
    if pattern == HOME:
        native_key(inp, 'Tab')
    native_key(inp, 'a', ('Control_L',))
    native_key(inp, 'BackSpace')
    for character in value:
        if character == '@':
            native_key(inp, '2', ('Shift_L',))
        elif character.isupper():
            native_key(inp, character.lower(), ('Shift_L',))
        else:
            type_text(inp, character)
    assert field_value(tree, pattern) == value, (pattern, value, field_value(tree, pattern))


def close_dialog(browser, tree, inp, *, save=False):
    native.one(tree.read(), SAVE if save else CANCEL)
    native_key(inp, 'Return' if save else 'Escape')
    wait_for(lambda: not native.visible(tree.read(), NAME), browser.process, timeout=5)


def edit(browser, tree, inp, result, output, page, title, *, name=None, home=None, save=True):
    action(browser, tree, inp, result, output, page, title, EDIT)
    initial = dialog(browser, tree)
    if output.name not in result.setdefault('dialog_captures', []):
        native.capture(tree, inp, output, 'pin-edit-dialog')
        result['dialog_captures'].append(output.name)
    if name is not None:
        fill(tree, inp, NAME, name)
    if home is not None:
        fill(tree, inp, HOME, home)
    close_dialog(browser, tree, inp, save=save)
    return initial


def peek(browser, tree, inp, result, output, page, title):
    return edit(browser, tree, inp, result, output, page, title, save=False)


def reset(browser, tree, inp, result, output, page, title, url, owner):
    identifier = page['id']
    action(browser, tree, inp, result, output, page, title, RESET)
    ready(browser, page, url)
    value = state(browser, page)
    check(result, 'Reset navigates the same existing tab to its saved home',
          page['id'] == identifier and value['url'] == url, value)
    check(result, 'Reset keeps the source Space HTTP-only cookie and site storage',
          account_is(value, owner), value)
    pin_node(browser, tree, title)


def tab_search(browser, tree, inp, result, output, title):
    native_key(inp, 'a', ('Control_L', 'Shift_L'))
    target = wait_for(lambda: next((p for p in request(browser.port, '/json/list')
                      if p['url'].startswith('chrome://tab-search.top-chrome')), None),
                      browser.process, timeout=6)
    expression = '''(()=>{const text=[];const walk=root=>{
      for(const n of root.childNodes){
        if(n.nodeType===Node.TEXT_NODE&&n.parentElement?.tagName!=='STYLE')text.push(n.textContent);
        if(n.shadowRoot)walk(n.shadowRoot);walk(n);
      }};walk(document);return text.join(' ');})()'''
    actual = wait_for(lambda: (value if title in (value := browser.ev(expression, target))
                               else None), browser.process, timeout=5)
    check(result, 'actual native Tab Search exposes the custom pin name', title in actual)
    native.capture(tree, inp, output, 'custom-name-tab-search')
    native_key(inp, 'Escape')


def editing(browser, tree, inp, result, args, base):
    api = tab.controller(browser)
    output = args.output / 'native'
    page = open_page(browser, tree, inp, base + '/a')
    seed_account(browser, page, 'editing')
    pin(browser, api, page)
    original_rect = tab.bounds(tree, pin_node(browser, tree, 'pin a'))
    initial = edit(browser, tree, inp, result, output, page, 'pin a',
                   name='cancelled name', home=base + '/c', save=False)
    check(result, 'initial pin captures its original home with no custom name',
          initial == {'name': '', 'home': base + '/a'}, initial)
    check(result, 'Cancel leaves the original pin metadata unchanged',
          peek(browser, tree, inp, result, output, page, 'pin a') == initial)

    navigate(browser, page, base + '/b')
    before = state(browser, page)
    edit(browser, tree, inp, result, output, page, 'pin b', name='mail desk')
    pin_node(browser, tree, 'mail desk')
    check(result, 'renaming a pin preserves its exact live page and draft',
          state(browser, page) == before)
    check(result, 'custom name keeps the compact pin position and dimensions',
          tab.bounds(tree, pin_node(browser, tree, 'mail desk')) == original_rect)
    check(result, 'pin custom name does not overwrite document or extension title',
          browser.ev('document.title', page) == 'pin b' and active_tab(api)['title'] == 'pin b')
    tab_search(browser, tree, inp, result, output, 'mail desk')
    check(result, 'navigating away did not replace the saved home',
          peek(browser, tree, inp, result, output, page, 'mail desk')
          == {'name': 'mail desk', 'home': base + '/a'})
    browser.ev('document.title="pin scripted";true', page)
    pin_node(browser, tree, 'mail desk')
    check(result, 'a website title update preserves the native custom pin label',
          browser.ev('document.title', page) == 'pin scripted')
    reset(browser, tree, inp, result, output, page, 'mail desk', base + '/a', 'editing')

    navigate(browser, page, base + '/b')
    before = state(browser, page)
    action(browser, tree, inp, result, output, page, 'mail desk', USE_CURRENT)
    check(result, 'Use current page as home does not navigate or reload',
          state(browser, page) == before)
    check(result, 'Use current saves the explicit new home and retains the custom name',
          peek(browser, tree, inp, result, output, page, 'mail desk')
          == {'name': 'mail desk', 'home': base + '/b'})
    navigate(browser, page, base + '/c')
    reset(browser, tree, inp, result, output, page, 'mail desk', base + '/b', 'editing')

    before = state(browser, page)
    edit(browser, tree, inp, result, output, page, 'mail desk', home=base + '/c')
    check(result, 'editing the home URL saves without navigating the current page',
          state(browser, page) == before)
    for unsafe in ('javascript:1', 'data:1', 'blob:1',
                   'http://a:b@127.0.0.1:' + str(browser.pin_server_port) + '/a'):
        action(browser, tree, inp, result, output, page, 'mail desk', EDIT)
        dialog(browser, tree)
        old_labels = {node['line'].split(' name=', 1)[-1].split(' name_from=', 1)[0]
                      for node in native.visible(tree.read(), r'class_name=Label name=')}
        fill(tree, inp, HOME, unsafe)
        native.one(tree.read(), SAVE)
        native_key(inp, 'Return')
        wait_for(lambda: bool(native.visible(tree.read(), NAME)), browser.process, timeout=3)
        new_labels = {node['line'].split(' name=', 1)[-1].split(' name_from=', 1)[0]
                      for node in native.visible(tree.read(), r'class_name=Label name=')}
        check(result, 'invalid home is rejected with a visible validation explanation: ' + unsafe,
              bool(new_labels - old_labels), sorted(new_labels - old_labels))
        native.capture(tree, inp, output, 'invalid-home-' + str(len(result.get('invalid', []))))
        result.setdefault('invalid', []).append(unsafe)
        close_dialog(browser, tree, inp)
        check(result, 'rejected home leaves the real page and previous metadata unchanged',
              state(browser, page) == before
              and peek(browser, tree, inp, result, output, page, 'mail desk')
              == {'name': 'mail desk', 'home': base + '/c'})
    reset(browser, tree, inp, result, output, page, 'mail desk', base + '/c', 'editing')
    edit(browser, tree, inp, result, output, page, 'mail desk', name='')
    pin_node(browser, tree, 'pin c')
    check(result, 'blank custom name restores the real page label',
          peek(browser, tree, inp, result, output, page, 'pin c')
          == {'name': '', 'home': base + '/c'})
    edit(browser, tree, inp, result, output, page, 'pin c', name='before close')
    cancel_beforeunload(browser, tree, inp, result, output, page)
    action(browser, tree, inp, result, output, page, 'before close', 'Unpin')
    wait_for(lambda: not active_tab(api)['pinned'], browser.process)
    navigate(browser, page, base + '/b')
    pin(browser, api, page)
    check(result, 'unpin after canceled close clears customization; repin captures its new home',
          peek(browser, tree, inp, result, output, page, 'pin b')
          == {'name': '', 'home': base + '/b'})
    native.capture(tree, inp, output, 'editing-complete')


def switch(browser, tree, inp, name):
    pattern = r'class_name=AccountSpaceDotButton name=Workspace: ' + re.escape(name) + r' name_from='
    button(tree, inp, pattern)
    wait_for(lambda: any('selected=true' in node['line'] for node in
                        native.visible(tree.read(), pattern)), browser.process, timeout=5)


def selected_page(browser, url):
    def found():
        for page in pages(browser):
            if page['url'] == url and browser.ev('document.visibilityState==="visible"', page):
                return page
        return None
    page = wait_for(found, browser.process, timeout=8)
    ready(browser, page, url)
    return page


def close_pin(browser, tree, inp, result, output, page):
    # The shortcut remains in the model while its document is unloaded. Read
    # browser-owned tab metadata without evaluating the discarded page.
    api = tab.controller(browser)
    before = active_tab(api)
    assert before['pinned'] and before['url'] == page['url'], before
    count = api('chrome.tabs.query({}).then(ts=>ts.length)')
    native_key(inp, 'w', ('Control_L',))
    # Chromium can replace the WebContents/session tab ID during discard.
    # This fixture has two pins at this URL; only the closed copy is discarded.
    # Keep pin membership/count and its independent name/home checks below.
    def closed_copy():
        matches = api('chrome.tabs.query({}).then(ts=>ts.filter(t=>t.pinned&&'
                      't.discarded&&t.url===' + json.dumps(before['url']) + ').map(t=>('
                      '{id:t.id,url:t.url,pinned:t.pinned,discarded:t.discarded,active:t.active})))')
        return matches[0] if len(matches) == 1 else None
    discarded = wait_for(closed_copy, browser.process, timeout=8)
    check(result, 'one native close shortcut unloads the document and preserves its pin',
          discarded['pinned'] and discarded['discarded'] and not discarded['active']
          and discarded['url'] == before['url']
          and api('chrome.tabs.query({}).then(ts=>ts.length)') == count, discarded)
    native.capture(tree, inp, output, 'closed-pin-shortcut')


def cancel_beforeunload(browser, tree, inp, result, output, page):
    before = state(browser, page)
    browser.ev('window.onbeforeunload=e=>{e.preventDefault();e.returnValue="pending";'
               'return "pending"};true', page)
    # The native page click supplies real sticky user activation, required by
    # Chromium before it will show a beforeunload confirmation.
    tab.page_focus(tree, inp)
    native_key(inp, 'w', ('Control_L',))
    leave = r'class_name=MdTextButton name=Leave name_from='
    wait_for(lambda: bool(native.visible(tree.read(), leave))
             and bool(native.visible(tree, CANCEL)), browser.process, timeout=6)
    native.capture(tree, inp, output, 'pin-canceled-beforeunload')
    native_key(inp, 'Escape')
    wait_for(lambda: not native.visible(tree.read(), leave), browser.process, timeout=5)
    browser.ev('window.onbeforeunload=null;true', page)
    check(result, 'canceling the real beforeunload dialog retains the exact pinned page',
          state(browser, page) == before)


def lifecycle_setup(browser, tree, inp, result, args, base):
    api = tab.controller(browser)
    output = args.output / 'native'
    personal = open_page(browser, tree, inp, base + '/a')
    seed_account(browser, personal, 'personal')
    pin(browser, api, personal)
    edit(browser, tree, inp, result, output, personal, 'pin a', name='personal desk')
    personal_live = state(browser, personal)
    activate_settings(browser)
    result['spaces'] = browser.call('noventraSpaces', 'create', '', 'Work')
    switch(browser, tree, inp, 'Work')
    work = open_page(browser, tree, inp, base + '/a')
    check(result, 'Work starts with an independent same-origin cookie jar',
          state(browser, work)['cookie'] == '' and state(browser, work)['owner'] is None)
    seed_account(browser, work, 'work')
    pin(browser, api, work)
    edit(browser, tree, inp, result, output, work, 'pin a', name='work desk')
    navigate(browser, work, base + '/b')
    reset(browser, tree, inp, result, output, work, 'work desk', base + '/a', 'work')
    navigate(browser, work, base + '/b')

    before = {page['id'] for page in pages(browser)}
    action(browser, tree, inp, result, output, work, 'work desk', 'Duplicate')
    clone = wait_for(lambda: next((p for p in pages(browser) if p['id'] not in before), None),
                     browser.process, timeout=8)
    ready(browser, clone, base + '/b')
    check(result, 'native duplicate retains pinned state, source home/name and Work account',
          active_tab(api)['pinned'] and account_is(state(browser, clone), 'work')
          and peek(browser, tree, inp, result, output, clone, 'work desk')
          == {'name': 'work desk', 'home': base + '/a'})
    edit(browser, tree, inp, result, output, clone, 'work desk',
         name='copy desk', home=base + '/c')
    tab.activate(browser, tree, inp, work)
    check(result, 'duplicate metadata is independent from a pin at the same current URL',
          peek(browser, tree, inp, result, output, work, 'work desk')
          == {'name': 'work desk', 'home': base + '/a'})

    clone_tab = api('chrome.tabs.query({}).then(t=>t.find(t=>t.pinned&&t.url==='
                    + json.dumps(base + '/b') + '&&t.id!==' + str(active_tab(api)['id']) + '))')
    discarded = api('chrome.tabs.discard(' + str(clone_tab['id']) + ')')
    check(result, 'explicit background discard reaches the actual pinned replacement path',
          discarded['discarded'], discarded)
    node = pin_node(browser, tree, 'copy desk', selected=False)
    inp.pointer.click(*tab.center(tab.bounds(tree, node)))
    clone = selected_page(browser, base + '/b')
    check(result, 'discarded pin restores its custom name/home and account',
          browser.ev('document.wasDiscarded', clone)
          and account_is(state(browser, clone), 'work')
          and peek(browser, tree, inp, result, output, clone, 'copy desk')
          == {'name': 'copy desk', 'home': base + '/c'})
    close_pin(browser, tree, inp, result, output, clone)
    node = pin_node(browser, tree, 'copy desk', selected=False)
    inp.pointer.click(*tab.center(tab.bounds(tree, node)))
    clone = selected_page(browser, base + '/b')
    check(result, 'clicking the saved pin reloads its current URL and independent pin metadata',
          browser.ev('document.wasDiscarded', clone)
          and peek(browser, tree, inp, result, output, clone, 'copy desk')
          == {'name': 'copy desk', 'home': base + '/c'}
          and account_is(state(browser, clone), 'work'))
    reset(browser, tree, inp, result, output, clone, 'copy desk', base + '/c', 'work')
    tab.activate(browser, tree, inp, personal)
    check(result, 'Work pin operations leave Personal document and account intact',
          state(browser, personal) == personal_live)
    result['restart'] = [
        {'url': base + '/a', 'name': 'personal desk', 'home': base + '/a', 'owner': 'personal'},
        {'url': base + '/b', 'name': 'work desk', 'home': base + '/a', 'owner': 'work'},
        {'url': base + '/c', 'name': 'copy desk', 'home': base + '/c', 'owner': 'work'},
    ]
    native.capture(tree, inp, output, 'lifecycle-before-restart')


def restored(browser, tree, inp, result, args, *, startup=False):
    api = tab.controller(browser)
    output = args.output / 'restart-native'
    candidates = pages(browser)
    originals = {}
    for expected in result['restart']:
        matches = [p for p in candidates if p['url'] == expected['url']]
        check(result, 'real restored pin exists without re-creating its URL: ' + expected['name'],
              len(matches) == 1, [p['url'] for p in candidates])
        page = matches[0]
        tab.activate(browser, tree, inp, page)
        ready(browser, page, expected['url'])
        value = state(browser, page)
        check(result, 'restored pin preserves its current URL, label, home and correct account',
              active_tab(api)['pinned'] and account_is(value, expected['owner'])
              and peek(browser, tree, inp, result, output, page, expected['name'])
              == {'name': expected['name'], 'home': expected['home']},
              {'expected': expected, 'state': value})
        originals[expected['owner']] = (page, value)
        if startup:
            reset(browser, tree, inp, result, output, page, expected['name'],
                  expected['home'], expected['owner'])
    if not startup:
        work = next(p for p in candidates if p['url'] == result['restart'][1]['url'])
        expected = result['restart'][1]
        reset(browser, tree, inp, result, output, work, expected['name'],
              expected['home'], expected['owner'])
        personal, before = originals['personal']
        tab.activate(browser, tree, inp, personal)
        check(result, 'reset after restart leaves the other Space live and isolated',
              state(browser, personal) == before)
    native.capture(tree, inp, output, 'restored-pins')


def startup_setup(browser, tree, inp, result, args, base):
    api = tab.controller(browser)
    output = args.output / 'native'
    personal = open_page(browser, tree, inp, base + '/a')
    seed_account(browser, personal, 'personal')
    pin(browser, api, personal)
    edit(browser, tree, inp, result, output, personal, 'pin a', name='first desk')
    navigate(browser, personal, base + '/b')
    activate_settings(browser)
    browser.call('noventraSpaces', 'create', '', 'Work')
    switch(browser, tree, inp, 'Work')
    work = open_page(browser, tree, inp, base + '/a')
    seed_account(browser, work, 'work')
    pin(browser, api, work)
    edit(browser, tree, inp, result, output, work, 'pin a', name='second desk')
    navigate(browser, work, base + '/c')
    activate_settings(browser)
    ok = browser.ev('new Promise(r=>chrome.settingsPrivate.setPref('
                    '"session.restore_on_startup",5,"",r))')
    check(result, 'actual startup preference selects New Tab rather than last-session restore', ok)
    result['restart'] = [
        {'url': base + '/b', 'name': 'first desk', 'home': base + '/a', 'owner': 'personal'},
        {'url': base + '/c', 'name': 'second desk', 'home': base + '/a', 'owner': 'work'},
    ]


def main():
    parser = cli(__doc__)
    parser.add_argument('--case', choices=('editing', 'lifecycle', 'startup'), default='editing')
    args = parser.parse_args()
    result = admit(args, Path(__file__))
    result['case'] = args.case
    helpers = (tab, native, sys.modules[extension_fixture.__module__],
               sys.modules[popup_labels.__module__], sys.modules[type_text.__module__],
               sys.modules[native_key.__module__])
    for module in dict.fromkeys(helpers):
        source = Path(module.__file__)
        shutil.copyfile(source, args.output / ('executed-' + source.name))
        result.setdefault('helper_sources', {})[str(source)] = sha(source)
    budget = Budget()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with tempfile.TemporaryDirectory(prefix='owned-pin-profile-', dir=args.output) as tmp:
            profile = Path(tmp)
            prefs(profile)
            extension = extension_fixture(profile)
            flags = (*native.FLAGS, '--load-extension=' + str(extension.resolve()))
            with session(args, profile, args.output / 'native', result, budget, flags) as (browser, tree, inp):
                browser.pin_server_port = server.server_port
                activate_settings(browser)
                if args.case == 'editing':
                    editing(browser, tree, inp, result, args, base)
                elif args.case == 'lifecycle':
                    lifecycle_setup(browser, tree, inp, result, args, base)
                else:
                    startup_setup(browser, tree, inp, result, args, base)
            if args.case != 'editing':
                restart_flags = flags if args.case == 'startup' else (*flags, '--restore-last-session')
                result['restart_flags'] = restart_flags
                with session(args, profile, args.output / 'restart-native', result, budget,
                             restart_flags) as (browser, tree, inp):
                    activate_settings(browser)
                    if args.case == 'startup':
                        check(result, 'pinned startup does not force last-session restoration',
                              '--restore-last-session' not in restart_flags
                              and browser.ev('new Promise(r=>chrome.settingsPrivate.getPref('
                                             '"session.restore_on_startup",p=>r(p.value)))') == 5)
                    restored(browser, tree, inp, result, args, startup=args.case == 'startup')
            check(result, 'all owned browser sessions close normally',
                  all(s['exit'] == 0 for s in result['normal_shutdowns']), result['normal_shutdowns'])
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
