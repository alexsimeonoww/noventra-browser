"""Native new-tab/chrome visual and interaction checks on an owned Xvfb.

Run with the existing Pillow venv through work/ram-investigation/with-xvfb.py.
Use --browser and a NEW --output directory. --observe-only records the same
target failures on an old runtime but never qualifies it. The 200s work budget
reserves 30s for diagnostics and cleanup. No host display/profile is touched.
Settings/Mojo prepare themes, layout and worst-case shortcut data; measured
search, shortcut dialogs and Customize use genuine X11 pointer/key events.
Only narrow captures use a persistent CDP device-emulation session; no product
DOM, style or event handler is replaced. This is scale-1 solid-X11 evidence,
not a Wayland, touch, RTL, high-contrast or exhaustive custom-theme qualification.
"""

import argparse
import base64
from collections import Counter
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
from pathlib import Path
import signal
import threading
import time
import traceback
from types import SimpleNamespace

from PIL import ImageChops

from dock_rail_regressions_smoke import Input
from extension_smoke import ROOT, request, wait_for
from glass_appearance_smoke import Appearance
from interface_polish_smoke import native_key
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree
from sidebar_footer_smoke import COLORS, extension_fixture, inside, one, pin_extensions, visible
from workspace_management_smoke import protocol
from workspace_tools_smoke import emulated_viewport, native_text


FLAGS = ('--force-renderer-accessibility', '--force-device-scale-factor=1',
         '--touch-ui=disabled', '--lang=en-US')
HOME = '''const app=document.querySelector('ntp-app'),root=app.shadowRoot;
const search=root.querySelector('ntp-searchbox'),mv=root.querySelector('cr-most-visited');
const sr=mv.shadowRoot,custom=root.querySelector('ntp-customize-buttons');'''
SEARCH_INPUT = "search.shadowRoot.querySelector('cr-searchbox-input').shadowRoot.querySelector('input')"
ADD = "sr.querySelector('#addShortcut')"
CUSTOMIZE = "custom.shadowRoot.querySelector('#customizeButton')"
NEW_TAB = r'class_name=(?:DynamicNewTabToolbarButton|ToolbarButton) name=New [Tt]ab '
EXTENSIONS = r'class_name=ExtensionsToolbarButton '
MENU = r'class_name=BrowserAppMenuButton '

MEASURE = '''(()=>{''' + HOME + '''
 const rect=e=>{if(!e)return null;const b=e.getBoundingClientRect(),s=getComputedStyle(e);
 return {x:b.x,y:b.y,w:b.width,h:b.height,radius:s.borderRadius,border:s.border,
   background:s.backgroundColor,color:s.color,shadow:s.boxShadow,display:s.display,
   overflowX:s.overflowX,textOverflow:s.textOverflow,scrollWidth:e.scrollWidth,
   clientWidth:e.clientWidth};};
 const boxes={};for(const id of ['noventraHome','noventraBrand','noventraMark',
 'searchboxContainer','noventraSearch','noventraFavorites','favoritesTitle'])
 boxes[id]=rect(root.querySelector('#'+id));
 const input=search.shadowRoot.querySelector('cr-searchbox-input');
 const glyph=input.shadowRoot.querySelector('cr-searchbox-icon').shadowRoot.querySelector('#icon');
 return {width:innerWidth,height:innerHeight,body:rect(document.body),boxes,
 search:{wrapper:rect(search.shadowRoot.querySelector('#inputWrapper')),
 input:rect(input.shadowRoot.querySelector('input')),
 placeholder:getComputedStyle(input.shadowRoot.querySelector('input'),'::placeholder').color,
 icon:rect(input.shadowRoot.querySelector('cr-searchbox-icon')),
 glyph:{...rect(glyph),maskSize:getComputedStyle(glyph).maskSize,
 maskImage:getComputedStyle(glyph).maskImage}},
 add:rect(sr.querySelector('#addShortcut')),addIcon:rect(sr.querySelector('#addShortcutIcon')),
 addFill:rect(sr.querySelector('#addShortcut .tile-icon')),
 addLabel:rect(sr.querySelector('#addShortcut .tile-title span')),
 custom:rect(custom.shadowRoot.querySelector('#customizeButton')),
 customIcon:rect(custom.shadowRoot.querySelector('.customize-icon')),
 tiles:[...sr.querySelectorAll('.tile')].filter(e=>e.getClientRects().length).map(e=>({
 title:e.title,box:rect(e),fill:rect(e.querySelector('.tile-icon')),
 favicon:rect(e.querySelector('img')),label:rect(e.querySelector('.tile-title span'))})),
 container:rect(sr.querySelector('#container')),
 overflow:document.documentElement.scrollWidth>innerWidth+1};})()'''


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check(result, case, name, actual, expected=True):
    result['checks'].append({'case': case, 'name': name, 'actual': actual,
                             'expected': expected, 'passed': actual == expected})


@contextmanager
def local_site():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            body = ('<!doctype html><title>Owned new tab destination</title>'
                    '<h1 id="destination">Owned new tab destination</h1>'
                    '<input id="draft" value="retained">').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}'
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def ev(b, home, body):
    return b.ev('(()=>{' + HOME + body + '})()', home)


def ready(b, home):
    wait_for(lambda: b.ev('!!document.querySelector("ntp-app")?.shadowRoot'
                         '?.querySelector("cr-most-visited")?.shadowRoot'
                         '?.querySelector("#addShortcut")', home), b.process, timeout=12)


def activate(b, home):
    request(b.port, '/json/activate/' + home['id'])
    time.sleep(.25)


def content(tree):
    return tree.rect(one(tree.read(), r'class_name=MultiContentsView '))


def point(b, home, expression):
    return ev(b, home, 'const e=' + expression + ';if(!e)throw Error("Missing measured control");'
              'const r=e.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2,w:r.width,h:r.height};')


def click(b, tree, inp, home, expression):
    r = point(b, home, expression)
    assert r['w'] > 0 and r['h'] > 0, (expression, r)
    c = content(tree)
    inp.pointer.click(c['x'] + r['x'], c['y'] + r['y'])


def capture(tree, inp, output, name):
    (output / (name + '.txt')).write_text(tree.read().text)
    return inp.capture(output / (name + '.png'))


def layout(b, extension_side='top'):
    model = b.state()['layout']
    order = ['back', 'forward', 'reload', 'address', 'new-tab', 'extensions', 'menu', 'tabs', 'spaces']
    model['items'].sort(key=lambda e: order.index(e['id']) if e['id'] in order else 99)
    for index, item in enumerate(model['items']):
        item.update(order=index, visible=item['id'] in order,
                    edge='left' if item['id'] in ('tabs', 'spaces') else
                         extension_side if item['id'] == 'extensions' else 'top')
    assert b.apply(model)['status'] == 'ok'
    time.sleep(.3)


def empty_home(b, inp, home):
    native_key(inp, 'l', ('Control_L',))
    native_text(inp, 'chrome://newtab/')
    native_key(inp, 'Return')
    ready(b, home)
    time.sleep(.3)


def theme_geometry(b, tree, inp, appearance, home, output, result):
    for mode, label in [(1, 'light'), (2, 'dark'), (1, 'light-return')]:
        appearance.scheme(mode)
        activate(b, home)
        c = content(tree)
        inp.pointer.click(c['x'] + 60, c['y'] + 100)
        inp.move(c['x'] + 20, c['y'] + 20)
        time.sleep(.4)
        data = b.ev(MEASURE, home)
        tree.read()
        new = tree.rect(one(tree, NEW_TAB))
        ext = tree.rect(one(tree, EXTENSIONS))
        menu = tree.rect(one(tree, MENU))
        region = tree.rect(one(tree, r'class_name=ExtensionsToolbarDesktop '))
        data['native'] = {'new': new, 'extensions': ext, 'menu': menu, 'region': region, 'content': c}
        with capture(tree, inp, output, label) as shot:
            with shot.crop((c['x'] + 35, c['y'] + 35, c['x'] + 55, c['y'] + 55)) as crop:
                page_color = Counter(crop.getdata()).most_common(1)[0][0]
            with shot.crop((new['x'], new['y'], new['x'] + new['w'], new['y'] + new['h'])) as crop:
                chrome_color = Counter(crop.getdata()).most_common(1)[0][0]
        data['pixels'] = {'page': list(page_color), 'chrome': list(chrome_color)}
        result.setdefault('measurements', {})[label] = data
        check(result, label, 'actual page and native chrome background match', page_color, chrome_color)
        check(result, label, 'no document horizontal overflow', data['overflow'], False)
        for name in ('noventraBrand', 'noventraSearch', 'noventraFavorites'):
            r = data['boxes'][name]
            check(result, label, name + ' centered in page', abs(r['x'] + r['w']/2 - data['width']/2) < 1)
        check(result, label, 'empty add-shortcut center aligned', abs(data['add']['x'] + data['add']['w']/2 - data['width']/2) < 1)
        check(result, label, 'search height', data['boxes']['noventraSearch']['h'], 44)
        check(result, label, 'search corner radius', data['boxes']['noventraSearch']['radius'], '6px')
        # The search glyph paints a 16px mask inside an intentionally larger
        # canvas. Preserve that canvas geometry without requiring it to shrink.
        check(result, label, 'search painted glyph size', data['search']['glyph']['maskSize'], '16px auto')
        check(result, label, 'search glyph mask is visible', data['search']['glyph']['maskImage']!='none' and data['search']['glyph']['display']!='none')
        check(result, label, 'add target size', [data['add']['w'], data['add']['h']], [88, 88])
        check(result, label, 'add fill size', [data['addFill']['w'], data['addFill']['h']], [32, 32])
        check(result, label, 'add glyph size', [data['addIcon']['w'], data['addIcon']['h']], [16, 16])
        check(result, label, 'add tile and fill corners', [data['add']['radius'], data['addFill']['radius']], ['6px', '6px'])
        check(result, label, 'idle add target has no border', data['add']['border'].startswith('0px none'))
        check(result, label, 'full Add label fits without ellipsis', data['addLabel']['scrollWidth']<=data['addLabel']['clientWidth'] and data['addLabel']['w']>0 and data['addLabel']['h']>0)
        check(result, label, 'Add label stays inside its target', all(data['addLabel'][axis]>=data['add'][axis]-.5 and data['addLabel'][axis]+data['addLabel'][dimension]<=data['add'][axis]+data['add'][dimension]+.5 for axis,dimension in [('x','w'),('y','h')]))
        check(result, label, 'Customize size', [data['custom']['w'], data['custom']['h']], [28, 28])
        check(result, label, 'Customize corners', data['custom']['radius'], '6px')
        check(result, label, 'Customize glyph size', [data['customIcon']['w'], data['customIcon']['h']], [16, 16])
        check(result, label, 'Customize glyph centered', all(abs(data['customIcon'][axis]+data['customIcon'][dimension]/2-data['custom'][axis]-data['custom'][dimension]/2)<1 for axis,dimension in [('x','w'),('y','h')]))
        check(result, label, 'Customize neutral idle background', data['custom']['background'], 'rgba(0, 0, 0, 0)')
        check(result, label, 'all top-right targets 28px', all(r['w'] == r['h'] == 28 for r in (new, ext, menu)))
        check(result, label, 'top-right control centers aligned', len({r['y'] + r['h']/2 for r in (new, ext, menu)}) == 1)
        check(result, label, 'top-right adjacent gaps', [ext['x'] - new['x'] - new['w'], menu['x'] - ext['x'] - ext['w']], [3, 3])
        check(result, label, 'single Extensions region has no extra padding', region['w'], 28)
    check(result, 'live theme', 'page changes with scheme', result['measurements']['light']['pixels']['page'] != result['measurements']['dark']['pixels']['page'])
    check(result, 'live theme', 'return to light restores original surface', result['measurements']['light-return']['pixels']['page'], result['measurements']['light']['pixels']['page'])


def keyboard_focus(b, tree, inp, home, expression, output, label):
    c = content(tree)
    inp.pointer.click(c['x'] + 60, c['y'] + 100)
    found = False
    for _ in range(22):
        native_key(inp, 'Tab')
        if ev(b, home, 'return (' + expression + ').matches(":focus");'):
            found = True
            break
    capture(tree, inp, output, label).close()
    assert found, 'Native Tab traversal did not focus ' + expression


def native_flows(b, tree, inp, home, base, output, result):
    for label, via_keyboard in [('search-click', False), ('search-keyboard', True)]:
        if via_keyboard:
            keyboard_focus(b, tree, inp, home, SEARCH_INPUT, output, label + '-focus')
        else:
            click(b, tree, inp, home, SEARCH_INPUT)
        focus = ev(b, home, 'const e=search.shadowRoot.querySelector("#inputWrapper"),s=getComputedStyle(e,"::after");'
                   'return {focused:(' + SEARCH_INPUT + ').matches(":focus"),border:s.border,borderColor:s.borderColor,borderWidth:s.borderWidth,shadow:s.boxShadow};')
        check(result, label, 'search receives real native focus', focus['focused'])
        check(result, label, 'search has visible focus treatment', (float(focus['borderWidth'].removesuffix('px')) > 0 and focus['borderColor'] != 'rgba(0, 0, 0, 0)') or focus['shadow'] != 'none')
        result.setdefault('focus', {})[label] = focus
        destination = base + '/' + label
        native_text(inp, destination)
        native_key(inp, 'Return')
        wait_for(lambda: b.ev('location.href', home) == destination, b.process, timeout=8)
        check(result, label, 'native submission reaches owned destination', b.ev('!!document.querySelector("#destination")', home))
        empty_home(b, inp, home)

    before_tiles = ev(b, home, 'return mv.tiles_.map(t=>({name:t.title,url:t.url}));')
    click(b, tree, inp, home, ADD)
    wait_for(lambda: ev(b, home, 'return sr.querySelector("#dialog").open;'), b.process, timeout=4)
    check(result, 'shortcut cancel', 'native add opens real dialog', True)
    native_key(inp, 'Escape')
    check(result, 'shortcut cancel', 'Escape preserves prior shortcuts', ev(b, home, 'return mv.tiles_.map(t=>({name:t.title,url:t.url}));'), before_tiles)

    click(b, tree, inp, home, ADD)
    wait_for(lambda: ev(b, home, 'return sr.querySelector("#dialog").open;'), b.process, timeout=4)
    for selector, text in [('#dialogInputName', 'Native shortcut'), ('#dialogInputUrl', base + '/shortcut')]:
        click(b, tree, inp, home, 'sr.querySelector(' + json.dumps(selector) + ').shadowRoot.querySelector("input")')
        native_text(inp, text)
    click(b, tree, inp, home, 'sr.querySelector("#dialog .action-button")')
    wait_for(lambda: ev(b, home, 'return mv.tiles_.some(t=>t.title==="Native shortcut");'), b.process, timeout=5)
    check(result, 'shortcut add', 'native save persists name and URL', ev(b, home, 'return mv.tiles_.map(t=>({name:t.title,url:t.url}));'), before_tiles + [{'name': 'Native shortcut', 'url': base + '/shortcut'}])

    tile = "[...sr.querySelectorAll('.tile')].find(e=>e.title==='Native shortcut')"
    r = point(b, home, tile);c = content(tree)
    inp.move(c['x'] + r['x'], c['y'] + r['y']);time.sleep(.3)
    click(b, tree, inp, home, tile + ".querySelector('#actionMenuButton')")
    click(b, tree, inp, home, "sr.querySelector('#actionMenuViewOrEdit')")
    wait_for(lambda: ev(b, home, 'return sr.querySelector("#dialog").open;'), b.process, timeout=4)
    click(b, tree, inp, home, 'sr.querySelector("#dialogInputName").shadowRoot.querySelector("input")')
    native_key(inp, 'a', ('Control_L',));native_text(inp, 'Renamed shortcut')
    click(b, tree, inp, home, 'sr.querySelector("#dialog .action-button")')
    wait_for(lambda: ev(b, home, 'return mv.tiles_.some(t=>t.title==="Renamed shortcut");'), b.process, timeout=5)
    check(result, 'shortcut edit', 'native save updates title', True)
    tile = "[...sr.querySelectorAll('.tile')].find(e=>e.title==='Renamed shortcut')"
    r = point(b, home, tile);c = content(tree)
    inp.move(c['x'] + r['x'], c['y'] + r['y']);time.sleep(.3)
    click(b, tree, inp, home, tile + ".querySelector('#actionMenuButton')")
    click(b, tree, inp, home, "sr.querySelector('#actionMenuRemove')")
    wait_for(lambda: ev(b, home, 'return !mv.tiles_.some(t=>t.title==="Renamed shortcut");'), b.process, timeout=5)
    check(result, 'shortcut remove', 'native removal preserves prior shortcuts', ev(b, home, 'return mv.tiles_.map(t=>({name:t.title,url:t.url}));'), before_tiles)

    keyboard_focus(b, tree, inp, home, ADD, output, 'add-keyboard-focus')
    ring = ev(b, home, 'const e=' + ADD + ';const s=getComputedStyle(e);return {shadow:s.boxShadow,outline:s.outline,outlineStyle:s.outlineStyle,outlineWidth:s.outlineWidth};')
    check(result, 'shortcut keyboard', 'focused shortcut has visible ring', ring['shadow'] != 'none' or (ring['outlineStyle'] != 'none' and float(ring['outlineWidth'].removesuffix('px')) > 0))
    result['focus']['add'] = ring
    native_key(inp, 'Return')
    check(result, 'shortcut keyboard', 'native Enter opens add dialog', ev(b, home, 'return sr.querySelector("#dialog").open;'))
    native_key(inp, 'Escape')


def narrow_and_long(b, tree, inp, home, base, output, result):
    # The real URL-search checks populate native Top Sites. Prepare the exact
    # stress fixture through its real handler, without changing rendered data.
    ev(b,home,'for(const tile of [...mv.tiles_])mv.browserProxy_.handler.deleteMostVisitedTile(tile);return true;')
    wait_for(lambda:ev(b,home,'return mv.tiles_.length===0;'),b.process,timeout=5)
    names = ['A', 'VeryLongUnbrokenShortcutName' * 5, '研究 — café — Дълго име 🧭',
             'Long shortcut title with several words and spaces', 'مرحبا بالعالم', 'Работа']
    for index, name in enumerate(names):
        value = b.ev('(async()=>{' + HOME + 'return await mv.browserProxy_.handler.addMostVisitedTile('
                     + json.dumps(base + '/seed-' + str(index)) + ',' + json.dumps(name) + ');})()', home)
        assert value['success'], value
    wait_for(lambda: ev(b, home, 'return mv.tiles_.length===6;'), b.process, timeout=5)
    for width in (480, 340, 320):
        with emulated_viewport(SimpleNamespace(settings=home), width, 700):
            time.sleep(.35)
            data = b.ev(MEASURE, home)
            label = 'narrow-' + str(width)
            check(result, label, 'actual viewport width', data['width'], width)
            check(result, label, 'no document horizontal overflow', data['overflow'], False)
            boxes = [*data['boxes'].values(), data['custom'], data['container'], data['add'],
                     *[t['box'] for t in data['tiles']]]
            check(result, label, 'all visible controls contained', all(r['x'] >= -1 and r['x'] + r['w'] <= width + 1 for r in boxes if r['w'] and r['h']))
            check(result, label, 'full Add label fits without ellipsis', data['addLabel']['scrollWidth']<=data['addLabel']['clientWidth'] and data['addLabel']['w']>0 and data['addLabel']['h']>0)
            check(result, label, 'Add label stays inside its target', all(data['addLabel'][axis]>=data['add'][axis]-.5 and data['addLabel'][axis]+data['addLabel'][dimension]<=data['add'][axis]+data['add'][dimension]+.5 for axis,dimension in [('x','w'),('y','h')]))
            check(result, label, 'all six worst-case shortcuts retained', len(data['tiles']), 6)
            check(result, label, 'shortcut favicons stay 16px', all(t['favicon']['w'] == t['favicon']['h'] == 16 for t in data['tiles']))
            size=84 if width<=420 else 88
            check(result, label, 'responsive shortcut target size', all(t['box']['w'] == t['box']['h'] == size for t in data['tiles']))
            left=min(t['box']['x'] for t in data['tiles']);right=max(t['box']['x']+t['box']['w'] for t in data['tiles'])
            check(result,label,'shortcut group remains centered',abs((left+right)/2-width/2)<1)
            check(result, label, 'long labels stay within their tiles', all(t['label']['x'] >= t['box']['x'] - 1 and t['label']['x'] + t['label']['w'] <= t['box']['x'] + t['box']['w'] + 1 for t in data['tiles']))
            result.setdefault('narrow', {})[label] = data
            shot = protocol(b, 'Page.captureScreenshot', {'format': 'png'}, home)
            (output / (label + '.png')).write_bytes(base64.b64decode(shot['data']))
    check(result, 'narrow cleanup', 'native viewport restored', b.ev('innerWidth', home), content(tree)['w'])
    capture(tree, inp, output, 'multiple-shortcuts').close()


def toolbar_interactions(b, tree, inp, home, output, result):
    activate(b, home)
    tree.read()
    c = content(tree)
    inp.move(c['x'] + 50, c['y'] + 50);time.sleep(.2)
    with capture(tree, inp, output, 'toolbar-idle') as shot:
        idle = shot.copy()
    for label, pattern in [('new', NEW_TAB), ('extensions', EXTENSIONS), ('menu', MENU)]:
        rect = tree.rect(one(tree.read(), pattern))
        inp.move(rect['x'] + rect['w']/2, rect['y'] + rect['h']/2);time.sleep(.2)
        with capture(tree, inp, output, 'toolbar-hover-' + label) as shot:
            with ImageChops.difference(idle, shot) as diff:
                with diff.crop((rect['x'], rect['y'], rect['x'] + rect['w'], rect['y'] + rect['h'])) as crop:
                    changed = sum(max(p) >= 8 for p in crop.getdata())
        check(result, 'toolbar hover', label + ' retains native hover feedback', changed >= 20)
    idle.close()
    inp.pointer.click(c['x'] + 30, c['y'] + 80)
    inp.move(c['x'] + 30, c['y'] + 80);time.sleep(.2)
    rect = tree.rect(one(tree.read(), EXTENSIONS))
    box = (rect['x']-3, rect['y']-3, rect['x']+rect['w']+3, rect['y']+rect['h']+3)
    with capture(tree, inp, output, 'toolbar-keyboard-unfocused') as shot:
        unfocused = shot.crop(box)
    focused = False
    attempts = result['toolbar_keyboard_attempts'] = []
    # requestNativeUITree omits focused-view state. Prove native keyboard
    # focus from a newly painted outer ring, then independently require Enter
    # to open Extensions. Pointer remains outside every toolbar target.
    for pane in range(4):
        native_key(inp, 'F6')
        for step in range(16):
            label = f'toolbar-keyboard-{pane}-{step}'
            with inp.capture(output / (label + '.png')) as shot:
                with shot.crop(box) as current, ImageChops.difference(unfocused, current) as diff:
                    changed = sum(max(diff.getpixel((x,y)))>=12
                        for y in range(diff.height) for x in range(diff.width)
                        if not (9<=x<25 and 9<=y<25))
            attempts.append({'pane':pane,'tabs':step,'outer_changed_pixels':changed})
            if changed >= 24:
                focused = True
                result['toolbar_keyboard_evidence'] = label
                check(result,'toolbar keyboard','focus preserves target bounds',tree.rect(one(tree.read(),EXTENSIONS)),rect)
                break
            native_key(inp, 'Tab')
        if focused:
            break
    unfocused.close()
    check(result, 'toolbar keyboard', 'native F6/Tab paints Extensions focus', focused)
    capture(tree, inp, output, 'toolbar-keyboard-focus').close()
    if focused:
        native_key(inp, 'Return');time.sleep(.3)
        check(result, 'toolbar keyboard', 'Enter opens native Extensions menu', bool(visible(tree.read(), r'class_name=ExtensionsMenuView ')))
        capture(tree, inp, output, 'toolbar-keyboard-menu').close()
        native_key(inp, 'Escape')
    else:
        result.setdefault('unrun', []).append('Enter activation after Extensions keyboard focus')

    pin_extensions(b)
    activate(b, home)
    for edge in ('top', 'left'):
        layout(b, edge);activate(b, home)
        tree.read();region = tree.rect(one(tree, r'class_name=ExtensionsToolbarDesktop '))
        nodes = visible(tree, r'class_name=(ToolbarActionView|ExtensionsToolbarButton) ')
        check(result, edge + ' pinned', 'two fixtures plus Extensions are visible', len(nodes), 3)
        check(result, edge + ' pinned', 'native pinned targets remain contained 28px squares', all(inside(tree.rect(n), region) and tree.rect(n)['w'] == tree.rect(n)['h'] == 28 for n in nodes))
        with capture(tree, inp, output, edge + '-pinned') as shot:
            for name, color in COLORS.items():
                rect = tree.rect(one(tree, r'class_name=ToolbarActionView name=' + name + r' '))
                with shot.crop((rect['x'], rect['y'], rect['x'] + rect['w'], rect['y'] + rect['h'])) as crop:
                    points = [(x, y) for y in range(28) for x in range(28) if max(abs(a-b) for a,b in zip(crop.getpixel((x,y)),color)) < 15]
                check(result, edge + ' pinned', name + ' remains fully painted', len(points) >= 240)
                if points:
                    center = [(min(p[i] for p in points) + max(p[i] for p in points) + 1)/2 for i in (0,1)]
                    check(result, edge + ' pinned', name + ' remains centered', all(abs(v-14) <= 1 for v in center))
        inp.pointer.click(*tree.center(one(tree, EXTENSIONS)));time.sleep(.2)
        check(result, edge + ' pinned', 'native Extensions click opens menu', bool(visible(tree.read(), r'class_name=ExtensionsMenuView ')))
        capture(tree, inp, output, edge + '-pinned-menu').close()
        native_key(inp, 'Escape')
    layout(b)


def css_rgb(value):
    numbers = [float(n) for n in re.findall(r'[-+]?(?:\d*\.)?\d+', value)]
    if value.startswith('color(srgb '):
        numbers[:3] = [v * 255 for v in numbers[:3]]
    assert len(numbers) in (3, 4), value
    return numbers[:3], numbers[3] if len(numbers) == 4 else 1


def composite(value, background):
    rgb, alpha = css_rgb(value)
    return [v * alpha + b * (1-alpha) for v,b in zip(rgb,background)]


def contrast(foreground, background):
    def luminance(rgb):
        values=[v/255 for v in rgb]
        values=[v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4 for v in values]
        return sum(v*w for v,w in zip(values,[.2126,.7152,.0722]))
    a,b=sorted([luminance(foreground),luminance(background)])
    return (b+.05)/(a+.05)


def suggestion_hover(b, tree, inp, home, base, output, result):
    for suffix in ('first', 'second'):
        native_key(inp,'l',('Control_L',))
        native_text(inp,base+'/suggestion-'+suffix);native_key(inp,'Return')
        wait_for(lambda:b.ev('location.pathname',home)=='/suggestion-'+suffix,b.process,timeout=6)
        time.sleep(.15)
    empty_home(b,inp,home)
    click(b,tree,inp,home,SEARCH_INPUT)
    native_text(inp,'127.0.0.1')
    rows='search.shadowRoot.querySelector("#matches").shadowRoot.querySelectorAll("cr-searchbox-match")'
    wait_for(lambda:ev(b,home,'return [...'+rows+'].some(e=>!e.hasAttribute("selected")&&e.getClientRects().length);'),b.process,timeout=6)
    expression='[...'+rows+'].find(e=>!e.hasAttribute("selected")&&e.getClientRects().length)'
    r=point(b,home,expression);c=content(tree)
    inp.move(c['x']+r['x'],c['y']+r['y']);time.sleep(.3)
    observation=ev(b,home,'const row='+expression+';const part=row.shadowRoot.querySelector(".container");'
        'const style=getComputedStyle(part),drop=search.shadowRoot.querySelector("#matches"),owner=search.shadowRoot.querySelector("#inputWrapper");'
        'const texts=[...row.shadowRoot.querySelectorAll("#contents,#description,#separator,#contents span,#description span")].filter(e=>[...e.childNodes].some(n=>n.nodeType===Node.TEXT_NODE&&n.textContent.trim())&&e.getClientRects().length);'
        'return {selected:row.hasAttribute("selected"),hovered:part.matches(":hover"),background:style.backgroundColor,'
        'dropdownBackground:getComputedStyle(drop.shadowRoot.querySelector("#content")).backgroundColor,page:getComputedStyle(document.body).backgroundColor,'
        'input:getComputedStyle(search.shadowRoot.querySelector("#inputWrapper")).backgroundColor,'
        'texts:texts.map(e=>({text:e.textContent,color:getComputedStyle(e).color})),row:row.id,'
        'corner:style.borderStartEndRadius,dropdownCorner:getComputedStyle(drop.shadowRoot.querySelector("#content")).borderRadius,'
        'dropdownRect:drop.shadowRoot.querySelector("#content").getBoundingClientRect().toJSON(),'
        'paintedOwner:{radius:getComputedStyle(owner).borderRadius,background:getComputedStyle(owner).backgroundColor,rect:owner.getBoundingClientRect().toJSON()}};')
    check(result,'gray128 suggestion','actual pointer hover on unselected suggestion',observation['hovered'] and not observation['selected'])
    page,_=css_rgb(observation['page'])
    underlying=composite(observation['input'],page)
    dropdown=composite(observation['dropdownBackground'],underlying)
    surface=composite(observation['background'],dropdown)
    ratios=[contrast(composite(t['color'],surface),surface) for t in observation['texts']]
    check(result,'gray128 suggestion','all rendered hover text has at least 4.5 contrast',bool(ratios) and all(v>=4.5 for v in ratios))
    check(result,'gray128 suggestion','hover row corner follows chrome',observation['corner'],'6px')
    # The transparent inner dropdown intentionally has square corners; the
    # full-popup input wrapper owns the visible surface and focus outline.
    check(result,'gray128 suggestion','painted popup corners follow chrome',observation['paintedOwner']['radius'],'6px')
    owner=observation['paintedOwner']['rect'];inner=observation['dropdownRect']
    check(result,'gray128 suggestion','painted owner contains full dropdown',owner['left']<=inner['left']+.5 and owner['right']>=inner['right']-.5 and owner['top']<=inner['top']+.5 and owner['bottom']>=inner['bottom']-.5 and css_rgb(observation['paintedOwner']['background'])[1]>0)
    observation['contrast']=ratios;result['gray128_suggestion']=observation
    capture(tree,inp,output,'suggestion-pointer-hover').close()
    native_key(inp,'Escape')


def gray_theme(browser, output, base, result, gray=119):
    label='gray' if gray==119 else 'gray128'
    output=output/label;output.mkdir()
    theme=output/'theme';theme.mkdir()
    (theme/'manifest.json').write_text(json.dumps({'name':'Owned gray NTP contrast',
        'version':'1.0','manifest_version':3,'theme':{'colors':{
        'ntp_background':[gray,gray,gray],'ntp_text':[255,255,255],
        'toolbar':[28,28,28],'frame':[28,28,28]}}}))
    with LayoutBrowser(browser,output/'gray-profile',output/'gray-browser',
                       [*FLAGS,'--load-extension='+str(theme.resolve())]) as b:
        result[label+'_browser_pid']=b.process.pid
        inp=Input(b.platform)
        try:
            tree=NativeUITree(b)
            home=request(b.port,'/json/new?chrome://newtab/','PUT')
            ready(b,home);activate(b,home)
            value=b.ev('(async()=>{'+HOME+'return await mv.browserProxy_.handler.addMostVisitedTile('
                       +json.dumps(base+'/gray-shortcut')+',"Gray shortcut");})()',home)
            assert value['success'],value
            wait_for(lambda:ev(b,home,'return mv.tiles_.length===1;'),b.process,timeout=5)
            data=b.ev(MEASURE,home)
            appearance=Appearance(b)
            appearance.wait('return root.querySelector("cr-theme-color-picker")?.theme_?.hasThirdPartyTheme===true;')
            theme_state=appearance.ui('return {id:page.themeIdPref_.value,hasThirdPartyTheme:root.querySelector("cr-theme-color-picker").theme_.hasThirdPartyTheme};')
            activate(b,home)
            check(result,label+' theme','real extension theme selected',bool(theme_state['id']) and theme_state['hasThirdPartyTheme'])
            page,_=css_rgb(data['body']['background'])
            check(result,label+' theme','custom NTP background preserved',page,[gray,gray,gray])
            surface=composite(data['search']['wrapper']['background'],page)
            placeholder=composite(data['search']['placeholder'],surface)
            placeholder_ratio=contrast(placeholder,surface)
            check(result,label+' theme','placeholder contrast at least 4.5',placeholder_ratio>=4.5)
            label_ink=composite(data['tiles'][0]['label']['color'],page)
            idle_ratio=contrast(label_ink,page)
            check(result,label+' theme','favorite idle contrast at least 4.5',idle_ratio>=4.5)
            capture(tree,inp,output,'gray-idle').close()
            r=point(b,home,"sr.querySelector('.tile')");c=content(tree)
            inp.move(c['x']+r['x'],c['y']+r['y']);time.sleep(.3)
            hovered=b.ev(MEASURE,home)
            hover_surface=composite(hovered['tiles'][0]['box']['background'],page)
            hover_label=composite(hovered['tiles'][0]['label']['color'],hover_surface)
            hover_ratio=contrast(hover_label,hover_surface)
            check(result,label+' theme','favorite hover contrast at least 4.5',hover_ratio>=4.5)
            capture(tree,inp,output,'gray-hover').close()
            result[label+'_theme']={'theme':theme_state,'idle':data,'hover':hovered,
                'contrast':{'placeholder':placeholder_ratio,'idle':idle_ratio,'hover':hover_ratio}}
            if gray==128:
                suggestion_hover(b,tree,inp,home,base,output,result)
        finally:inp.close()
    result[label+'_browser_stopped']=b.process.poll() is not None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe-only', action='store_true')
    args = parser.parse_args()
    assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1', 'This fixture qualifies private X11 only'
    args.output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    result = {'passed': False, 'qualified': False, 'checks': [], 'observe_only': args.observe_only,
              'browser': str(args.browser.resolve()), 'binary_sha256': sha(args.browser),
              'resources_sha256': sha(args.browser.parent/'resources.pak'),
              'fixture_sha256': sha(__file__), 'scope': __doc__, 'phase': 'setup'}
    (args.output / 'executed-test.py').write_bytes(Path(__file__).read_bytes())
    helpers = ['layout_test_support.py','native_ui_tree.py','dock_rail_regressions_smoke.py',
               'interface_polish_smoke.py','glass_appearance_smoke.py','sidebar_footer_smoke.py',
               'workspace_management_smoke.py','workspace_tools_smoke.py','extension_smoke.py']
    result['helper_hashes'] = {n: sha(ROOT/'tests/noventra'/n) for n in helpers}
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError('200s test bound')))
    signal.alarm(200)
    b = tree = inp = None
    try:
        fixture = args.output / 'extensions';fixture.mkdir()
        extension_arg = extension_fixture(fixture)
        profile = args.output/'profile';(profile/'Default').mkdir(parents=True)
        (profile/'Default/Preferences').write_text(json.dumps({'vertical_tabs': {'collapsed_state': True}}))
        with local_site() as base, LayoutBrowser(args.browser, profile, args.output/'browser', [*FLAGS,extension_arg]) as b:
            result['owned_browser_pid'] = b.process.pid
            inp = Input(b.platform)
            try:
                appearance = Appearance(b)
                layout(b)
                tree = NativeUITree(b)
                home = request(b.port, '/json/new?chrome://newtab/', 'PUT')
                ready(b,home);activate(b,home)
                result['phase']='theme geometry'
                theme_geometry(b,tree,inp,appearance,home,args.output,result)
                result['phase']='native search and shortcuts'
                native_flows(b,tree,inp,home,base,args.output,result)
                result['phase']='worst-case narrow'
                narrow_and_long(b,tree,inp,home,base,args.output,result)
                result['phase']='native toolbar'
                toolbar_interactions(b,tree,inp,home,args.output,result)
                result['phase']='Customize'
                activate(b,home)
                keyboard_focus(b,tree,inp,home,CUSTOMIZE,args.output,'customize-keyboard-focus')
                focus=ev(b,home,'const e='+CUSTOMIZE+';const s=getComputedStyle(e);return {shadow:s.boxShadow,outline:s.outline,outlineStyle:s.outlineStyle,outlineWidth:s.outlineWidth};')
                result['focus']['customize']=focus
                check(result,'Customize','native focus retains visible treatment',focus['shadow']!='none' or (focus['outlineStyle']!='none' and float(focus['outlineWidth'].removesuffix('px'))>0))
                native_key(inp,'Return')
                wait_for(lambda:b.ev('document.visibilityState==="visible"'),b.process,timeout=5)
                check(result,'Customize','native Enter activates real Customize Settings',b.ev('location.href.startsWith("chrome://settings/appearance") && document.visibilityState==="visible"'))
                capture(tree,inp,args.output,'customize-opened-settings').close()
                result['phase']='complete'
            except BaseException:
                signal.alarm(25)
                for name, action in [('capture',lambda:capture(tree,inp,args.output,'failure').close()),('targets',lambda:result.update(failure_targets=request(b.port,'/json/list')))]:
                    try:action()
                    except Exception as error:result.setdefault('diagnostic_errors',{})[name]=repr(error)
                raise
            finally:
                inp.close();inp=None
        result['phase']='gray extension theme'
        with local_site() as gray_base:
            gray_theme(args.browser,args.output,gray_base,result)
            gray_theme(args.browser,args.output,gray_base,result,128)
        result['phase']='complete'
        result['passed']=all(c['passed'] for c in result['checks']) and not result.get('unrun')
        result['qualified']=result['passed'] and not args.observe_only
    except BaseException as error:
        result['error']=repr(error)
        (args.output/'exception.txt').write_text(traceback.format_exc())
    finally:
        signal.alarm(0)
        result['elapsed_seconds']=round(time.monotonic()-started,3)
        result['browser_stopped']=b is not None and b.process.poll() is not None
        result['helpers_unchanged']=all(sha(ROOT/'tests/noventra'/n)==v for n,v in result['helper_hashes'].items())
        result['runtime_unchanged']=sha(args.browser)==result['binary_sha256']
        result['resources_unchanged']=sha(args.browser.parent/'resources.pak')==result['resources_sha256']
        result['summary']={'passed':sum(c['passed'] for c in result['checks']),'failed':sum(not c['passed'] for c in result['checks'])}
        (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('passed','qualified','phase','summary','elapsed_seconds','browser_stopped')}),flush=True)
    if result.get('error') or (not args.observe_only and not result['qualified']):
        raise SystemExit(1)


if __name__=='__main__':
    main()
