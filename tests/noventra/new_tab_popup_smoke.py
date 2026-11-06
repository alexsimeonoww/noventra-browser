"""Native new-tab intent: no blank tab, URL/search submit, cancellation and accounts.

Run through the owned namespace/private Xvfb adapter with exact runtime hashes.
CDP prepares local documents and reads witnesses. Keys and pointer clicks drive
all measured new-tab invocation, input, submission and cancellation.
"""
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import urllib.parse

from glance_workflow_smoke import (
    Budget, Flow, activate_settings, admit, check, cli, extension_fixture,
    native, native_key, prefs, request, session, wait_for, write)
from portable_transfer_smoke import protocol
from sidebar_inline_address_smoke import EDITOR, FIELD, screen_rect
from sidebar_tab_resting_smoke import editor_value
from workspace_tools_smoke import native_text


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        body = ('''<!doctype html><meta charset="utf-8"><title>Popup fixture</title>
<style>body{margin:30px;background:#edf2f8;font:18px sans-serif}input{padding:12px}</style>
<h1>Native new-tab fixture</h1><input id="draft" value="retained source draft">
<script>window.marker=crypto.randomUUID();document.title='Popup '+location.pathname;</script>''').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def page_ids(flow):
    return {page['id'] for page in flow.targets()
            if not page['url'].startswith('chrome://omnibox-popup')}


def ready_page(flow, url):
    def ready():
        for page in flow.targets():
            if page['url'] == url:
                try:
                    if flow.b.ev('document.readyState==="complete"&&!!document.querySelector("#draft")', page):
                        return page
                except subprocess.CalledProcessError:
                    pass
        return None
    return wait_for(ready, flow.b.process, timeout=10)


def prepare_source(flow, label):
    url = flow.base + '/source-' + label
    page = request(flow.b.port, '/json/new?' + urllib.parse.quote(url, safe=':/?='), 'PUT')
    page = ready_page(flow, url)
    flow.activate(page)
    return page


def witness(flow, page):
    return flow.b.ev('({url:location.href,marker:window.marker,timeOrigin:performance.timeOrigin,'
                     'draft:document.querySelector("#draft").value})', page)


def context(flow, page):
    return protocol(flow.b, 'Target.getTargetInfo', {'targetId': page['id']}, page)['targetInfo']['browserContextId']


def popup_geometry(flow, label):
    tree = flow.tree.read()
    field = screen_rect(tree, native.one(tree, FIELD))
    editor = screen_rect(tree, native.one(tree, EDITOR))
    content = screen_rect(tree, native.one(tree, r'class_name=MultiContentsView '))
    check(flow.result, label + ': new-tab URL editor is centered in the current page',
          native.inside(field, content) and native.inside(editor, field) and
          abs(field['x'] + field['w'] / 2 - content['x'] - content['w'] / 2) <= 8 and
          abs(field['y'] + field['h'] / 2 - content['y'] - content['h'] / 2) <= 8 and
          field['h'] == 48 and 250 < field['w'] <= 640,
          {'field': field, 'editor': editor, 'content': content})
    return field


def open_popup(flow, source, label, *, plus=False):
    flow.activate(source)
    before = page_ids(flow)
    retained = witness(flow, source)
    if plus:
        node = native.one(flow.tree.read(),
            r'class_name=(?:NewTabButton|ToolbarButton) name=New [Tt]ab(?: name_from=| \()')
        box = screen_rect(flow.tree, node)
        flow.inp.pointer.click(box['x'] + box['w'] / 2, box['y'] + box['h'] / 2)
    else:
        native_key(flow.inp, 't', ('Control_L',))
    time.sleep(.25)
    check(flow.result, label + ': opening new-tab input creates no tab', page_ids(flow) == before,
          {'before': sorted(before), 'after': {page['id']: page['url'] for page in flow.targets()}})
    popup_geometry(flow, label)
    check(flow.result, label + ': new-tab input starts empty', editor_value(flow.tree) == '')
    check(flow.result, label + ': opening input retains the source document and draft',
          witness(flow, source) == retained)
    native.capture(flow.tree, flow.inp, flow.output, label + '-empty-popup')
    return before, retained


def submit_url(flow, source, label, *, plus=False, select_with_ctrl_l=False):
    before, retained = open_popup(flow, source, label, plus=plus)
    url = flow.base + '/submitted-' + label
    native_text(flow.inp, url)
    time.sleep(.2)
    check(flow.result, label + ': typing does not create a tab', page_ids(flow) == before)
    if select_with_ctrl_l:
        native_key(flow.inp, 'l', ('Control_L',))
        native_text(flow.inp, url)
        check(flow.result, label + ': Ctrl+L within new-tab input does not create a tab',
              page_ids(flow) == before)
    check(flow.result, label + ': native omnibox contains the typed URL',
          editor_value(flow.tree) == url)
    native.capture(flow.tree, flow.inp, flow.output, label + '-typed-popup')
    native_key(flow.inp, 'Return')
    page = ready_page(flow, url)
    check(flow.result, label + ': Enter creates exactly one target with the typed URL',
          page_ids(flow) - before == {page['id']} and before <= page_ids(flow))
    active = wait_for(lambda: flow.b.ev('document.visibilityState==="visible"&&document.hasFocus()', page),
                      flow.b.process, timeout=6)
    check(flow.result, label + ': submitted tab is active', active)
    check(flow.result, label + ': submission leaves source content and draft intact',
          witness(flow, source) == retained)
    return page


def submit_empty(flow, source, label, *, plus=False, clear=False):
    before, retained = open_popup(flow, source, label, plus=plus)
    if clear:
        native_text(flow.inp, 'cleared draft')
        native_key(flow.inp, 'a', ('Control_L',))
        native_key(flow.inp, 'BackSpace')
        check(flow.result, label + ': clearing input leaves it empty', editor_value(flow.tree) == '')
    native_key(flow.inp, 'Return')

    def opened():
        return next((page for page in flow.targets() if page['id'] not in before and
                     page['url'] == 'chrome://newtab/'), None)

    try:
        page = wait_for(opened, flow.b.process, timeout=6)
    except TimeoutError as error:
        raise AssertionError(label + ': empty Enter did not open the New Tab page') from error
    check(flow.result, label + ': empty Enter opens exactly one New Tab page',
          page_ids(flow) - before == {page['id']} and before <= page_ids(flow))
    wait_for(lambda: flow.b.ev('document.readyState==="complete"&&document.visibilityState==="visible"', page),
             flow.b.process, timeout=6)
    check(flow.result, label + ': New Tab opens in the source browser context',
          context(flow, page) == context(flow, source))
    check(flow.result, label + ': empty Enter preserves the source document and draft',
          witness(flow, source) == retained)
    def centered_input_visible():
        tree = flow.tree.read()
        fields = native.visible(tree, FIELD)
        if not fields:
            return False
        field = screen_rect(tree, fields[0])
        content = screen_rect(tree, native.one(tree, r'class_name=MultiContentsView '))
        return (field['h'] == 48 and
                abs(field['x'] + field['w'] / 2 - content['x'] - content['w'] / 2) <= 8 and
                abs(field['y'] + field['h'] / 2 - content['y'] - content['h'] / 2) <= 8)

    wait_for(lambda: not centered_input_visible(), flow.b.process, timeout=6)
    check(flow.result, label + ': empty Enter dismisses the centered input',
          not centered_input_visible())
    native.capture(flow.tree, flow.inp, flow.output, label + '-home')
    return page


def cancel_cases(flow, source, label):
    for method in ('escape', 'outside', 'repeat'):
        before, retained = open_popup(flow, source, label + '-' + method)
        native_text(flow.inp, 'cancelled draft')
        if method == 'escape':
            native_key(flow.inp, 'Escape')
        elif method == 'repeat':
            native_key(flow.inp, 't', ('Control_L',))
        else:
            content = screen_rect(flow.tree.read(), native.one(flow.tree, r'class_name=MultiContentsView '))
            flow.inp.pointer.click(content['x'] + content['w'] / 2,
                                   content['y'] + content['h'] - 25)
        wait_for(lambda: flow.b.ev('document.hasFocus()', source), flow.b.process, timeout=6)
        check(flow.result, label + ': ' + method + ' cancels without creating a blank tab',
              page_ids(flow) == before and witness(flow, source) == retained)


def local_search(flow):
    activate_settings(flow.b)
    page = request(flow.b.port, '/json/new?chrome://settings/searchEngines', 'PUT')
    wait_for(lambda: flow.b.ev('document.readyState==="complete"&&typeof chrome.send==="function"', page), flow.b.process)
    flow.b.ev('chrome.send("searchEngineEditStarted",[0]);chrome.send("searchEngineEditCompleted",'
              '["Popup local fixture","popup",' + json.dumps(flow.base + '/search?q=%s') + ',""]);true', page)
    def engine():
        value = flow.b.ev('import("chrome://resources/js/cr.js").then(m=>m.sendWithPromise("getSearchEnginesList"))', page)
        return next((row for group in value.values() if isinstance(group, list)
                     for row in group if row.get('name') == 'Popup local fixture'), None)
    selected = wait_for(engine, flow.b.process)
    flow.b.ev('chrome.send("setDefaultSearchEngine",[' + str(selected['id']) + ',2,false]);true', page)
    wait_for(lambda: engine().get('default'), flow.b.process)


def search(flow, source, label):
    before, retained = open_popup(flow, source, label)
    query = 'native popup search'
    native_text(flow.inp, query)
    time.sleep(.25)
    check(flow.result, label + ': search text waits for submission', page_ids(flow) == before)
    native_key(flow.inp, 'Return')
    def submitted():
        return next((p for p in flow.targets() if p['id'] not in before and
            p['url'].startswith(flow.base + '/search?') and
            urllib.parse.parse_qs(urllib.parse.urlsplit(p['url']).query).get('q') == [query]), None)
    page = wait_for(submitted, flow.b.process, timeout=10)
    check(flow.result, label + ': Enter opens one search tab using the selected default provider',
          page_ids(flow) - before == {page['id']})
    check(flow.result, label + ': search preserves the source page', witness(flow, source) == retained)


def basic(flow):
    local_search(flow)
    for layout_name in ('left', 'right', 'horizontal'):
        activate_settings(flow.b)
        if layout_name == 'horizontal':
            layout = native.top_layout(flow.b.state()['layout'])
        else:
            layout = native.preset(flow.b, 'minimal-' + layout_name)
        native.apply(flow.b, layout)
        source = prepare_source(flow, layout_name)
        submit_empty(flow, source, layout_name + '-empty-keyboard')
        submit_empty(flow, source, layout_name + '-empty-plus', plus=True)
        submit_empty(flow, source, layout_name + '-cleared', clear=True)
        cancel_cases(flow, source, layout_name)
        submit_url(flow, source, layout_name + '-keyboard', select_with_ctrl_l=True)
        submit_url(flow, source, layout_name + '-plus', plus=True)
        search(flow, source, layout_name + '-search')
        flow.activate(source)
        before = page_ids(flow)
        retained = witness(flow, source)
        native_key(flow.inp, 'l', ('Control_L',))
        native_key(flow.inp, 'BackSpace')
        native_key(flow.inp, 'Return')
        time.sleep(.25)
        check(flow.result, layout_name + ': empty Ctrl+L does not create or navigate a tab',
              page_ids(flow) == before and witness(flow, source) == retained)
        native_key(flow.inp, 'Escape')
        url = flow.base + '/current-' + layout_name
        native_key(flow.inp, 'l', ('Control_L',))
        native_text(flow.inp, url)
        native_key(flow.inp, 'Return')
        ready_page(flow, url)
        check(flow.result, layout_name + ': ordinary Ctrl+L still navigates the current tab',
              page_ids(flow) == before and flow.b.ev('location.href', source) == url)


def current_source(flow, label):
    url = flow.base + '/source-' + label
    native_key(flow.inp, 'l', ('Control_L',))
    native_text(flow.inp, url)
    native_key(flow.inp, 'Return')
    return ready_page(flow, url)


def seed_account(flow, page, name):
    flow.b.ev('localStorage.setItem("popup-account",' + json.dumps(name) + ');'
              'document.cookie=' + json.dumps('popup_account=' + name + ';Path=/;SameSite=Lax') + ';true', page)


def account(flow, page):
    return flow.b.ev('({local:localStorage.getItem("popup-account"),cookie:document.cookie})', page)


def spaces(flow):
    personal = prepare_source(flow, 'personal-account')
    seed_account(flow, personal, 'personal')
    original = account(flow, personal)
    activate_settings(flow.b)
    catalog = flow.b.call('noventraSpaces', 'create', '', 'Popup Work')
    work_id = next(row['id'] for row in catalog if row['name'] == 'Popup Work')
    flow.b.call('noventraSpaces', 'switch', work_id, '')
    source = current_source(flow, 'work-account')
    seed_account(flow, source, 'work')
    before = account(flow, source)
    home = submit_empty(flow, source, 'work-space-empty')
    native_key(flow.inp, 'w', ('Control_L',))
    wait_for(lambda: home['id'] not in page_ids(flow), flow.b.process)
    page = submit_url(flow, source, 'work-space')
    check(flow.result, 'new-tab popup opens the result in the active Work profile',
          context(flow, page) == context(flow, source) != context(flow, personal))
    check(flow.result, 'Work result retains its isolated cookie and local-storage account',
          account(flow, page) == before and before != original)
    check(flow.result, 'new Work tab does not change the Personal account', account(flow, personal) == original)
    activate_settings(flow.b)
    flow.b.call('noventraSpaces', 'switch', '', '')


def incognito(flow):
    personal = prepare_source(flow, 'regular-private-witness')
    seed_account(flow, personal, 'regular')
    retained = account(flow, personal)
    before = page_ids(flow)
    native_key(flow.inp, 'n', ('Control_L', 'Shift_L'))
    wait_for(lambda: bool(page_ids(flow) - before), flow.b.process)
    source = current_source(flow, 'private-account')
    seed_account(flow, source, 'private')
    private = account(flow, source)
    home = submit_empty(flow, source, 'incognito-empty')
    native_key(flow.inp, 'w', ('Control_L',))
    wait_for(lambda: home['id'] not in page_ids(flow), flow.b.process)
    page = submit_url(flow, source, 'incognito')
    check(flow.result, 'new-tab popup keeps the result in the active off-the-record context',
          context(flow, page) == context(flow, source) != context(flow, personal))
    check(flow.result, 'private result keeps its private storage and cookies',
          account(flow, page) == private and private != retained)
    check(flow.result, 'creating a private result leaves the regular account unchanged',
          account(flow, personal) == retained)
    native_key(flow.inp, 'w', ('Control_L',))
    wait_for(lambda: page['id'] not in page_ids(flow), flow.b.process)
    native_key(flow.inp, 'w', ('Control_L',))
    wait_for(lambda: source['id'] not in page_ids(flow), flow.b.process)


def main():
    parser = cli(__doc__)
    parser.add_argument('--case', choices=('basic', 'spaces', 'incognito', 'all'), default='basic')
    args = parser.parse_args()
    result = admit(args, Path(__file__))
    owner = os.environ.get('NOVENTRA_TRANSFER_OWNED_HOME')
    assert owner and args.output.resolve().is_relative_to(Path(owner).resolve())
    links = json.loads(subprocess.check_output(['/usr/bin/ip', '-j', 'link', 'show'], text=True))
    assert {link['ifname'] for link in links} == {'lo', 'nv-test0'}
    budget = Budget()
    budget.work_deadline, budget.deadline = budget.started + 360, budget.started + 400
    budget.arm(budget.work_deadline)
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = args.output / 'profile'
    prefs(profile)
    extension = extension_fixture(profile)
    try:
        with session(args, profile, args.output / 'native', result, budget,
                     (*native.FLAGS, '--load-extension=' + str(extension))) as (browser, tree, inp):
            flow = Flow(browser, tree, inp, result, args.output,
                        f'http://127.0.0.1:{server.server_port}', [])
            for name in (('basic', 'spaces', 'incognito') if args.case == 'all' else (args.case,)):
                result['case'] = name
                {'basic': basic, 'spaces': spaces, 'incognito': incognito}[name](flow)
        check(result, 'owned browser shuts down normally',
              all(row['exit'] == 0 for row in result['normal_shutdowns']))
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        server.server_close()
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
