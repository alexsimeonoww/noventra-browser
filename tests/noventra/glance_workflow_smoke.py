"""Native Glance feature acceptance; disposable profiles, local pages, owned Xvfb."""
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/noventra/inline_footer'))
from sidebar_common import (Budget, activate_settings, admit, check, cli, click_dom,
                            native, native_key, prefs, request, session, wait_for, write)
from sidebar_command_footer_smoke import extension_fixture
from sidebar_inline_address_smoke import screen_rect
import tab_click_editor_smoke as tabs
from workspace_tools_smoke import native_text

ENABLED = 'noventra.browser.link_preview'
TRIGGER = 'noventra.browser.link_preview_trigger'
PINNED = 'noventra.browser.link_preview_pinned'


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(min(int(self.headers.get('Content-Length', '0')), 4096))
        self.do_GET()

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        self.server.requests.append({'method': self.command, 'path': self.path, 'host': self.headers.get('Host'), 'cookie': self.headers.get('Cookie', '')})
        self.send_response(200)
        self.send_header('Cache-Control', 'no-store')
        if parsed.path == '/set':
            self.send_header('Set-Cookie', 'glance=' + query['owner'][0] + '; HttpOnly; SameSite=Lax; Path=/')
            body = b'ok'
        elif parsed.path == '/cookie':
            body = self.headers.get('Cookie', '').encode()
        elif parsed.path == '/download':
            self.send_header('Content-Disposition', 'attachment; filename="glance-fixture.txt"')
            body = b'Owned Glance download fixture'
        else:
            port = self.server.server_port
            other = f'http://127.0.0.2:{port}'
            seed = query.get('seed', ['plain'])[0]
            if parsed.path == '/frame':
                target = f'http://127.0.0.3:{port}/target?seed=frame'
                control = (f'<button onclick="top.location=\'{target}-script\'">Navigate top through script</button>'
                           if 'script' in query else f'<a href="{target}" target="_top">Frame external link</a>')
                body = ('<!doctype html><style>body{margin:0}a,button{display:block;box-sizing:border-box;width:100%;height:58px;padding:20px}</style>' + control).encode()
            elif parsed.path.startswith('/source'):
                body = f'''<!doctype html><meta charset="utf-8"><title>Glance source {seed}</title>
                  <style>body{{margin:20px;font:14px sans-serif}}a,button{{display:block;margin:12px 0}}input{{max-width:90%}}</style>
                  <input id="draft" value="source draft"><input id="control" type="checkbox">
                  <a id="same" href="/same?seed={seed}">Same website</a>
                  <a id="external" href="{other}/target?seed={seed}-external" onclick="window.clicks=(window.clicks||0)+1">Other website</a>
                  <a id="blank" target="_blank" href="{other}/target?seed={seed}-blank">Other website in new tab</a>
                  <a id="opener" target="_blank" rel="opener" href="{other}/target?seed={seed}-opener">New tab with opener</a>
                  <a id="prevented" href="{other}/target?seed={seed}-prevented" onclick="event.preventDefault();window.prevented=true">Cancelled link</a>
                  <a id="script" href="javascript:window.scriptRan=true">Script link</a>
                  <a id="fragment" href="#section">Same-page section</a>
                  <a id="download" download="glance-fixture.txt" href="/download?seed={seed}">Download file</a>
                  <button id="popup" onclick="window.open('{other}/target?seed={seed}-popup','_blank','width=480,height=360')">Login popup</button>
                  <button id="noopenerPopup" onclick="window.open('{other}/target?seed={seed}-noopener-popup','_blank','noopener')">Login popup without opener</button>
                  <form id="post" method="post" action="{other}/posted?seed={seed}"><button id="submit">Submit form</button></form>
                  <iframe id="crossFrame" src="{other}/frame" style="width:90%;height:80px"></iframe>
                  <div id="section">Section</div><script>window.marker=crypto.randomUUID()</script>'''.encode()
            else:
                body = b'''<!doctype html><meta charset="utf-8"><title>Glance target</title>
                  <style>body{margin:20px;font:14px sans-serif}input{max-width:90%}</style>
                  <input id="draft" value="target draft"><a href="/next">Next</a>
                  <script>window.marker=crypto.randomUUID()</script>'''
            self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def popup_origin(tree, source_box, source_view=None):
    """Map tab-modal local AX coordinates through its native host geometry.

    Current previews center inside the source viewport. Older dialogs used the
    source ContentsContainerView and toolbar anchor, less BubbleFrameView's top
    inset. Aura child dialogs have no X11 window of their own, so querying X11
    geometry cannot locate this root.
    """
    root = native.one(tree, r'class_name=RootView name=Link preview name_from=')
    side_controls = native.visible(tree, r'class_name=\S+ name=Preview controls name_from=')
    if side_controls or native.visible(tree, r'class_name=ImageButton name=Open in tab without reloading name_from='):
        # New viewport-sized previews center inside the real source WebView.
        # Keep the old branch below for baseline binaries with toolbar-anchored
        # MdTextButton previews; using new coordinates there would hide a
        # regression behind an incorrectly calibrated native click.
        view = source_view or source_box
        margin = min(12, min(view['w'], view['h']) // 20)
        return {'x': view['x'] + (view['w'] - root['bounds'][2]) // 2,
                'y': view['y'] + margin,
                'source': view,
                'mode': 'frameless-side-controls' if side_controls else 'viewport-centered'}
    browser = native.one(tree, r'class_name=BrowserView ')
    toolbars = [n for n in tree.nodes.values() if 'class_name=ToolbarView ' in n['line']]
    assert len(toolbars) == 1, toolbars
    toolbar = tree.rect(toolbars[0])
    client = native.one(tree, r'class_name=DialogClientView ')
    width = root['bounds'][2]
    middle = source_box['x'] + source_box['w'] // 2
    return {'x': min(max(middle - width // 2, 0), browser['bounds'][2] - width),
            'y': toolbar['y'] + toolbar['h'] - 3 - client['bounds'][1],
            'source': source_box, 'toolbar': toolbar, 'inset': client['bounds'][1]}


class Flow:
    def __init__(self, b, tree, inp, result, output, base, requests):
        self.b, self.tree, self.inp = b, tree, inp
        self.result, self.output, self.base = result, output, base
        self.requests = requests
        self.api = tabs.controller(b)

    def targets(self):
        return [t for t in request(self.b.port, '/json/list') if t['type'] == 'page']

    def pref(self, name):
        return self.b.ev('new Promise(r=>chrome.settingsPrivate.getPref(' + json.dumps(name) + ',p=>r(p.value)))')

    def set_pref(self, name, value):
        assert self.b.ev('new Promise(r=>chrome.settingsPrivate.setPref(' + json.dumps(name) + ',' + json.dumps(value) + ',"",r))')
        assert self.pref(name) == value

    def all_tabs(self):
        return self.api('chrome.tabs.query({})')

    def tab(self, page):
        return self.api('chrome.tabs.query({}).then(ts=>ts.find(t=>t.url===' + json.dumps(self.b.ev('location.href', page)) + '))')

    def create_source(self, seed, pinned=False):
        before = {p['id'] for p in self.targets()}
        url = self.base + '/source?seed=' + seed
        native_key(self.inp, 't', ('Control_L',))
        native_key(self.inp, 'l', ('Control_L',))
        native_text(self.inp, url)
        native_key(self.inp, 'Return')
        page = wait_for(lambda: next((p for p in self.targets() if p['id'] not in before and p['url'] == url), None), self.b.process, timeout=8)
        wait_for(lambda: self.b.ev('document.readyState==="complete"&&!!document.querySelector("#external")', page), self.b.process, timeout=10)
        if pinned:
            self.api('chrome.tabs.update(' + str(self.tab(page)['id']) + ',{pinned:true})')
        self.activate(page)
        return page

    def activate(self, page):
        request(self.b.port, '/json/activate/' + page['id'])
        wait_for(lambda: self.b.ev('document.visibilityState==="visible"', page), self.b.process, timeout=5)

    def click_page(self, page, selector, modifiers=()):
        self.activate(page)
        point = self.b.ev('''(()=>{const e=document.querySelector(SELECTOR);if(!e)throw Error('Missing fixture element');
          e.scrollIntoView({block:'center',behavior:'instant'});const r=e.getBoundingClientRect();
          return {x:r.x+r.width/2,y:r.y+r.height/2,w:innerWidth,h:innerHeight}})()'''.replace('SELECTOR', json.dumps(selector)), page)
        self.tree.read()
        boxes = []
        for container in native.visible(self.tree, r'class_name=ContentsContainerView '):
            container_id = next(key for key, value in self.tree.nodes.items() if value is container)
            # Split panes add 3 px of framing, whereas ordinary tabs add 1 px.
            # Use the actual child viewport rather than guessing that inset.
            for child in self.tree.nodes.values():
                if child['parent'] == container_id and re.search(r'class_name=(?:WebView|ContentsWebView) ', child['line']) and child['bounds'][2:] == (point['w'], point['h']):
                    boxes.append((screen_rect(self.tree, child), screen_rect(self.tree, container)))
        assert boxes, (selector, point)
        # This fixture always grows a side-by-side split from its original
        # leftmost source pane. Equal-sized right panes are not click targets.
        box, container = min(boxes, key=lambda pair: pair[0]['x'])
        self.result.setdefault('native_page_clicks', []).append({'selector': selector, 'page': page['id'], 'dom': point, 'view': box, 'container': container})
        tabs.native_button(self.inp, 1, (box['x'] + point['x'], box['y'] + point['y']), modifiers)
        time.sleep(.25)

    def preview(self, url):
        def ready():
            for page in self.targets():
                if page['url'] != url:
                    continue
                try:
                    if self.b.ev('document.readyState==="complete"&&!!document.querySelector("#draft")', page):
                        return page
                except subprocess.CalledProcessError:
                    # The owned initial-empty _blank target may disappear
                    # while the actual preview is being created.
                    continue
            return None
        page = wait_for(ready, self.b.process, timeout=8)
        self.last_preview = page
        check(self.result, 'preview is an on-demand page outside the tab strip', not any(t.get('url') == url for t in self.all_tabs()), url)
        return page

    def link_url(self, page, selector):
        return self.b.ev('document.querySelector(' + json.dumps(selector) + ').href', page)

    def close_preview(self, page):
        self.action('Close preview (Esc)')
        wait_for(lambda: not any(p['id'] == page['id'] for p in self.targets()), self.b.process, timeout=6)

    def action(self, name):
        pattern = r'class_name=(?:ImageButton|MdTextButton) name=' + re.escape(name) + r' name_from='
        node = native.one(self.tree.read(), pattern)
        box = self.tree.rect(node)
        click = self.result['native_page_clicks'][-1]
        origin = popup_origin(self.tree, click['container'], click['view'])
        box = {**box, 'x': box['x'] + origin['x'], 'y': box['y'] + origin['y']}
        assert box['w'] > 0 and box['h'] > 0, (name, box)
        record = {'name': name, 'node': node['line'], 'rect': box, 'origin': origin,
                  'roots': [n['line'] for n in self.tree.nodes.values() if 'class_name=RootView ' in n['line']]}
        if hasattr(self, 'last_preview'):
            record['preview_window'] = self.b.ev('({screenX,screenY,outerWidth,outerHeight,innerWidth,innerHeight})', self.last_preview)
        actions = self.result.setdefault('native_preview_actions', [])
        actions.append(record)
        native.capture(self.tree, self.inp, self.output, 'before-preview-action-' + str(len(actions)))
        self.inp.pointer.click(box['x'] + box['w'] / 2, box['y'] + box['h'] / 2)
        time.sleep(.25)

    def state(self, page):
        return self.b.ev('({url:location.href,marker:window.marker,timeOrigin:performance.timeOrigin,draft:document.querySelector("#draft").value,clicks:window.clicks||0})', page)

    def modify_preview(self, page, label):
        self.b.ev('document.querySelector("#draft").value=' + json.dumps(label) + ';true', page)
        return self.state(page)

    def select_trigger(self, value):
        activate_settings(self.b)
        click_dom(self.b, self.tree, self.inp, self.result, '[data-section="customizeBehavior"]', 'page')
        point = self.b.ev('''(()=>{const find=(r,s)=>{const e=r.querySelector(s);if(e)return e;for(const n of r.querySelectorAll('*'))if(n.shadowRoot){const f=find(n.shadowRoot,s);if(f)return f}};
          const host=find(document,'#noventraPreviewTrigger'), e=host?.shadowRoot.querySelector('select');
          if(!e||e.disabled)throw Error('Missing/disabled preview shortcut');e.scrollIntoView({block:'center',behavior:'instant'});
          const r=e.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2,w:innerWidth,h:innerHeight,options:[...e.options].filter(o=>!o.disabled&&!o.hidden).map(o=>o.textContent.trim()),disabledOptions:[...e.options].filter(o=>o.disabled).map(o=>o.textContent.trim())}})()''')
        check(self.result, 'shortcut exposes Alt, Ctrl and Shift choices', point['options'] == ['Alt + click', 'Ctrl + click', 'Shift + click'], point['options'])
        check(self.result, 'generic custom placeholder is not a selectable shortcut', point['disabledOptions'] == ['Custom'], point['disabledOptions'])
        boxes = [screen_rect(self.tree, n) for n in native.visible(self.tree.read(), r'class_name=ContentsContainerView ') if abs(n['bounds'][2] - point['w']) <= 2 and abs(n['bounds'][3] - point['h']) <= 2]
        assert len(boxes) == 1, boxes
        box = boxes[0]
        self.inp.pointer.click(box['x'] + point['x'], box['y'] + point['y'])
        native_key(self.inp, 'Home')
        for _ in range(value):
            native_key(self.inp, 'Down')
        native_key(self.inp, 'Return')
        wait_for(lambda: self.pref(TRIGGER) == value, self.b.process, timeout=5)
        check(self.result, 'native shortcut selection persists value ' + str(value), self.pref(TRIGGER) == value)


def modifiers(flow):
    check(flow.result, 'preview defaults are enabled, Alt and automatic pinned links', flow.pref(ENABLED) is True and flow.pref(TRIGGER) == 0 and flow.pref(PINNED) is True)
    source = flow.create_source('modifiers')
    before = flow.state(source)
    url = flow.link_url(source, '#external')
    for value, key in enumerate(('Alt_L', 'Control_L', 'Shift_L')):
        flow.select_trigger(value)
        flow.click_page(source, '#external', (key,))
        page = flow.preview(url)
        check(flow.result, key + ' opens a preview while retaining source', flow.state(source) == before)
        if value == 2:
            flow.b.ev('document.title="Long title "+"Reading in Noventra ".repeat(30);true', page)
            native.capture(flow.tree, flow.inp, flow.output, 'long-title-and-split-action')
        flow.close_preview(page)
    flow.set_pref(ENABLED, False)
    known = {p['id'] for p in flow.targets()}
    flow.click_page(source, '#external', ('Shift_L',))
    page = wait_for(lambda: next((p for p in flow.targets() if p['id'] not in known and p['url'] == url), None), flow.b.process, timeout=8)
    check(flow.result, 'disabled modifier preview restores native Shift-click window', any(t['url'] == page['url'] for t in flow.all_tabs()))
    flow.api('chrome.tabs.query({}).then(ts=>Promise.all(ts.filter(t=>t.url===' + json.dumps(url) + ').map(t=>chrome.tabs.remove(t.id))))')
    flow.click_page(source, '#control')
    check(flow.result, 'disabled previews preserve normal controls', flow.b.ev('document.querySelector("#control").checked', source) is True)
    flow.set_pref(ENABLED, True)


def transferred_preferences(flow):
    activate_settings(flow.b)
    def transfer(action, payload):
        return flow.b.ev('(async()=>{const {sendWithPromise}=await import("chrome://resources/js/cr.js");return sendWithPromise("noventraTransfer",' + json.dumps(action) + ',' + json.dumps(payload) + ')})()')
    password = 'Owned Glance fixture password 2026'
    categories = {'settings': True}
    exported = transfer('export', {'password': password, 'categories': categories})
    assert exported.get('data'), exported
    flow.set_pref(TRIGGER, 0)
    flow.set_pref(PINNED, False)
    plan = transfer('preview', {'password': password, 'data': exported['data']})
    assert plan.get('plan_id'), plan
    applied = transfer('apply', {'plan_id': plan['plan_id'], 'categories': categories})
    check(flow.result, 'encrypted settings transfer restores Shift trigger and automatic pin choice', flow.pref(TRIGGER) == 2 and flow.pref(PINNED) is True, applied)


def pinned(flow):
    source = flow.create_source('pinned', True)
    original = flow.state(source)
    count = len(flow.all_tabs())
    for selector in ('#external', '#blank', '#opener'):
        url = flow.link_url(source, selector)
        flow.click_page(source, selector)
        page = flow.preview(url)
        current = flow.state(source)
        check(flow.result, selector + ' keeps pinned document and draft', all(current[k] == original[k] for k in ('url', 'marker', 'timeOrigin', 'draft')), current)
        check(flow.result, selector + ' leaves no extra blank tab', len(flow.all_tabs()) == count, flow.all_tabs())
        flow.close_preview(page)
    check(flow.result, 'automatic preview runs original link click handler', flow.b.ev('window.clicks', source) == 1)
    frame_url = flow.base.replace('127.0.0.1', '127.0.0.3') + '/target?seed=frame'
    flow.click_page(source, '#crossFrame')
    frame_preview = flow.preview(frame_url)
    check(flow.result, 'cross-origin iframe link previews without replacing the pin', flow.state(source)['marker'] == original['marker'] and len(flow.all_tabs()) == count)
    flow.close_preview(frame_preview)
    flow.click_page(source, '#control')
    check(flow.result, 'pinned checkbox receives its original click', flow.b.ev('document.querySelector("#control").checked', source) is True)
    flow.click_page(source, '#prevented')
    check(flow.result, 'page can prevent an off-site navigation', flow.b.ev('window.prevented===true', source) and len(flow.all_tabs()) == count and not any('pinned-prevented' in p['url'] for p in flow.targets()))
    flow.click_page(source, '#script')
    check(flow.result, 'ordinary pinned script link retains page behavior', flow.b.ev('window.scriptRan===true', source))
    flow.click_page(source, '#download')
    wait_for(lambda: (flow.output / 'downloads/glance-fixture.txt').is_file(), flow.b.process, timeout=10)
    check(flow.result, 'native download attribute still downloads without a preview', (flow.output / 'downloads/glance-fixture.txt').read_text() == 'Owned Glance download fixture' and not any('/download?' in p['url'] for p in flow.targets()))
    native_key(flow.inp, 'Escape')
    for selector in ('#popup', '#noopenerPopup'):
        expected = flow.b.ev('document.querySelector(' + json.dumps(selector) + ').getAttribute("onclick")', source)
        known = {p['id'] for p in flow.targets()}
        flow.click_page(source, selector)
        page = wait_for(lambda: next((p for p in flow.targets() if p['id'] not in known and '/target?seed=pinned-' in p['url']), None), flow.b.process, timeout=8)
        wait_for(lambda: flow.b.ev('document.readyState==="complete"&&!!document.querySelector("#draft")', page), flow.b.process, timeout=8)
        check(flow.result, selector + ' remains a real script-created browser tab or popup', any(t['url'] == page['url'] for t in flow.all_tabs()), expected)
        flow.api('chrome.tabs.query({}).then(ts=>Promise.all(ts.filter(t=>t.url===' + json.dumps(page['url']) + ').map(t=>chrome.tabs.remove(t.id))))')
        flow.activate(source)
    flow.click_page(source, '#fragment')
    wait_for(lambda: flow.b.ev('location.hash', source) == '#section', flow.b.process, timeout=5)
    check(flow.result, 'pinned fragment stays in the same document', flow.state(source)['marker'] == original['marker'])
    flow.click_page(source, '#same')
    wait_for(lambda: flow.b.ev('location.pathname', source) == '/same', flow.b.process, timeout=8)
    check(flow.result, 'same-site pinned link navigates normally', flow.tab(source)['pinned'] is True)
    posted = flow.create_source('form', True)
    flow.click_page(posted, '#submit')
    wait_for(lambda: flow.b.ev('location.pathname', posted) == '/posted', flow.b.process, timeout=8)
    check(flow.result, 'pinned form submission retains native POST navigation', any(r['method'] == 'POST' and r['path'] == '/posted?seed=form' for r in flow.requests) and flow.tab(posted)['pinned'] is True)
    for remote in (True, False):
        frame_source = flow.create_source('frame-negative-' + str(remote), True)
        origin = flow.base.replace('127.0.0.1', '127.0.0.2') if remote else flow.base
        frame_url = origin + '/frame?script=1'
        flow.b.ev('new Promise(r=>{const f=document.querySelector("#crossFrame");f.onload=()=>r(true);f.src=' + json.dumps(frame_url) + '})', frame_source)
        flow.click_page(frame_source, '#crossFrame')
        wait_for(lambda: flow.b.ev('location.pathname', frame_source) == '/target', flow.b.process, timeout=8)
        check(flow.result, ('cross-process' if remote else 'same-origin') + ' iframe script navigation keeps native behavior', flow.tab(frame_source)['url'].endswith('/target?seed=frame-script'))
    same_frame = flow.create_source('frame-same-origin', True)
    original_frame = flow.state(same_frame)
    flow.b.ev('new Promise(r=>{const f=document.querySelector("#crossFrame");f.onload=()=>r(true);f.src=' + json.dumps(flow.base + '/frame') + '})', same_frame)
    flow.click_page(same_frame, '#crossFrame')
    frame_page = flow.preview(flow.base.replace('127.0.0.1', '127.0.0.3') + '/target?seed=frame')
    check(flow.result, 'same-origin iframe anchor preserves its pinned parent', flow.state(same_frame) == original_frame)
    flow.close_preview(frame_page)
    source = flow.create_source('disabled-pinned', True)
    flow.set_pref(PINNED, False)
    expected = flow.link_url(source, '#external')
    flow.click_page(source, '#external')
    wait_for(lambda: flow.b.ev('location.href', source) == expected, flow.b.process, timeout=8)
    check(flow.result, 'automatic pinned preview toggle restores normal navigation', flow.tab(source)['url'] == expected)


def split(flow, pinned_source=False):
    source = flow.create_source('split', pinned_source)
    source_tab = flow.tab(source)
    before_source = flow.state(source)
    for number in range(2, 5):
        url = flow.base.replace('127.0.0.1', '127.0.0.2') + '/target?seed=split-pane-' + str(number)
        flow.b.ev('document.querySelector("#external").href=' + json.dumps(url) + ';true', source)
        flow.click_page(source, '#external', () if pinned_source else ('Alt_L',))
        preview = flow.preview(url)
        state = flow.modify_preview(preview, 'retained pane ' + str(number))
        flow.action('Open in split without reloading')
        wait_for(lambda: any(t.get('url') == url and t.get('splitViewId', -1) != -1 for t in flow.all_tabs()), flow.b.process, timeout=8)
        check(flow.result, 'split retains preview contents, history and unsaved form ' + str(number), flow.state(preview) == state)
        members = [t for t in flow.all_tabs() if t.get('splitViewId', -1) == flow.tab(source).get('splitViewId')]
        check(flow.result, 'preview joins original source in ' + str(number) + '-pane split', len(members) == number and any(t['id'] == source_tab['id'] for t in members) and all(t['pinned'] == pinned_source for t in members), members)
        check(flow.result, 'original source survives split ' + str(number), all(flow.state(source)[k] == before_source[k] for k in ('url', 'marker', 'timeOrigin', 'draft')))
    url = flow.base.replace('127.0.0.1', '127.0.0.2') + '/target?seed=split-capacity'
    flow.b.ev('document.querySelector("#external").href=' + json.dumps(url) + ';true', source)
    flow.click_page(source, '#external', () if pinned_source else ('Alt_L',))
    preview = flow.preview(url)
    state = flow.modify_preview(preview, 'full split preview retained')
    disabled = native.one(flow.tree.read(), r'class_name=ImageButton name=A split can contain up to four tabs name_from=')
    check(flow.result, 'four-pane split disables further split promotion', 'restriction=disabled' in disabled['line'], disabled['line'])
    flow.action('Open in tab without reloading')
    wait_for(lambda: any(t.get('url') == url and t.get('splitViewId', -1) == -1 for t in flow.all_tabs()), flow.b.process, timeout=8)
    check(flow.result, 'Open in tab remains available without reloading at capacity', flow.state(preview) == state)
    flow.api('chrome.tabs.remove(' + str(source_tab['id']) + ')')
    wait_for(lambda: not any(t['id'] == source_tab['id'] for t in flow.all_tabs()), flow.b.process, timeout=5)
    check(flow.result, 'promoted page survives source closure', flow.state(preview) == state)
    native.capture(flow.tree, flow.inp, flow.output, 'retained-split-panes')


def storage(flow):
    personal = flow.create_source('personal')
    other = flow.base.replace('127.0.0.1', '127.0.0.2')
    flow.b.ev('location.href=' + json.dumps(other + '/target?seed=personal-cookie') + ';true', personal)
    wait_for(lambda: flow.b.ev('location.pathname==="/target"&&document.readyState==="complete"', personal), flow.b.process, timeout=8)
    flow.b.ev('fetch("/set?owner=personal").then(()=>{localStorage.setItem("account","personal");return true})', personal)
    activate_settings(flow.b)
    flow.b.call('noventraSpaces', 'create', '', 'Work')
    spaces = flow.b.call('noventraSpaces', 'list', '', '')
    work_id = next(s['id'] for s in spaces if s['name'] == 'Work')
    flow.b.call('noventraSpaces', 'switch', work_id, '')
    source = flow.create_source('work', True)
    # Seed this account on the destination origin through a temporary Work tab.
    seed = flow.create_source('work-seed')
    flow.b.ev('location.href=' + json.dumps(other + '/target?seed=work-cookie') + ';true', seed)
    wait_for(lambda: flow.b.ev('location.pathname==="/target"&&document.readyState==="complete"', seed), flow.b.process, timeout=8)
    flow.b.ev('fetch("/set?owner=work").then(()=>{localStorage.setItem("account","work");return true})', seed)
    flow.api('chrome.tabs.remove(' + str(flow.tab(seed)['id']) + ')')
    flow.click_page(source, '#external')
    preview = flow.preview(flow.link_url(source, '#external'))
    account = flow.b.ev('fetch("/cookie").then(r=>r.text()).then(cookie=>({cookie,local:localStorage.getItem("account")}))', preview)
    check(flow.result, 'automatic preview uses Work cookie jar and local storage', 'glance=work' in account['cookie'] and account['local'] == 'work', account)
    state = flow.modify_preview(preview, 'Work preview retained')
    flow.action('Open in split without reloading')
    wait_for(lambda: flow.tab(source).get('splitViewId', -1) != -1, flow.b.process, timeout=8)
    check(flow.result, 'split promotion retains Work document and account', flow.state(preview) == state and flow.b.ev('localStorage.getItem("account")', preview) == 'work')
    activate_settings(flow.b)
    flow.b.call('noventraSpaces', 'switch', '', '')
    personal_account = flow.b.ev('fetch("/cookie").then(r=>r.text()).then(cookie=>({cookie,local:localStorage.getItem("account")}))', personal)
    check(flow.result, 'Personal account remains separate after Work split', 'glance=personal' in personal_account['cookie'] and personal_account['local'] == 'personal', personal_account)


def main():
    parser = cli(__doc__)
    parser.add_argument('--phase', required=True, choices=['modifiers', 'pinned', 'split', 'split-pinned', 'storage'])
    parser.add_argument('--horizontal', action='store_true')
    args = parser.parse_args()
    result = admit(args, Path(__file__))
    owner = os.environ.get('NOVENTRA_TRANSFER_OWNED_HOME')
    assert owner and args.output.resolve().is_relative_to(Path(owner).resolve()), 'Run through the owned namespace wrapper'
    links = json.loads(subprocess.check_output(['/usr/bin/ip', '-j', 'link', 'show'], text=True))
    assert {link['ifname'] for link in links} == {'lo', 'nv-test0'}
    budget = Budget()
    budget.work_deadline, budget.deadline = budget.started + 240, budget.started + 280
    budget.arm(budget.work_deadline)
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    server.requests = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = args.output / 'profile'
    prefs(profile)
    preferences = profile / 'Default/Preferences'
    value = json.loads(preferences.read_text())
    value['download'] = {'default_directory': str(args.output / 'downloads'), 'prompt_for_download': False}
    write(preferences, value)
    extension = extension_fixture(profile)
    try:
        with session(args, profile, args.output / 'native', result, budget, (*native.FLAGS, '--load-extension=' + str(extension))) as (b, tree, inp):
            if args.horizontal:
                native.apply(b, native.top_layout(b.state()['layout']))
            flow = Flow(b, tree, inp, result, args.output, f'http://127.0.0.1:{server.server_port}', server.requests)
            {'modifiers': modifiers, 'pinned': pinned, 'split': split,
             'split-pinned': lambda f: split(f, True), 'storage': storage}[args.phase](flow)
        if args.phase == 'modifiers':
            with session(args, profile, args.output / 'restart', result, budget, (*native.FLAGS, '--load-extension=' + str(extension))) as (b, tree, inp):
                flow = Flow(b, tree, inp, result, args.output, f'http://127.0.0.1:{server.server_port}', server.requests)
                check(result, 'preview shortcut and enable choices survive restart', flow.pref(TRIGGER) == 2 and flow.pref(ENABLED) is True and flow.pref(PINNED) is True)
                source = flow.create_source('restart')
                flow.click_page(source, '#external', ('Shift_L',))
                page = flow.preview(flow.link_url(source, '#external'))
                check(result, 'saved Shift-click trigger executes after native restart', True)
                flow.close_preview(page)
                transferred_preferences(flow)
        check(result, 'owned browser shuts down normally', all(r['exit'] == 0 for r in result['normal_shutdowns']), result['normal_shutdowns'])
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        result['requests'] = server.requests
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
