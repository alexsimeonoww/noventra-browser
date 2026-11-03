r"""Compact desktop chrome and retired-bottom migration on an owned Xvfb display.

Run with Node, Pillow, ImageMagick and the existing private-Xvfb dependencies:
  PYTHONDONTWRITEBYTECODE=1 work/mixed-sites-baseline-2026-10-11/venv/bin/python \
    work/ram-investigation/with-xvfb.py \
    work/mixed-sites-baseline-2026-10-11/venv/bin/python \
    tests/noventra/compact_chrome_smoke.py --browser /absolute/candidate \
    --output work/checks/compact-chrome-new

The output directory must be new. Only a fresh, temporary profile is used;
Preferences are edited after its browser has stopped, then checked through two
real restarts. No existing profile, build or desktop display is touched.
The 240s overall budget reserves 20s each for failure evidence and owned cleanup
(200s of test work). result.json records actual elapsed time, phases and limits.

Settings APIs/DOM prepare layouts; native X11 mouse/keyboard events measure hit
targets, tab selection and address editing. Captures and native trees precede
teardown. Desktop scale=1, touch UI disabled, light appearance and English labels
are intentional fixture constraints. Wayland, touch sizing, RTL, caption clicks
and exhaustive Spaces menus remain the responsibility of their existing tests.
Caption exclusion is checked when the native tree exposes caption buttons;
private Xvfb commonly has no visible captions, which is reported explicitly.
"""

import argparse
from collections import Counter
from contextlib import contextmanager
from copy import deepcopy
import json
import os
from pathlib import Path
import re
import signal
import tempfile
import time
import traceback

from PIL import ImageChops

from dock_rail_regressions_smoke import Input
from extension_smoke import ROOT, request, wait_for
from interface_polish_smoke import native_key
from layout_editor_smoke import FIND
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree
from sidebar_footer_smoke import (
    COLORS, apply_layout as apply_footer_layout, capture, extension_fixture,
    footer_geometry, identity, inside, one, overlaps, pin_extensions,
    toolbar_geometry as side_toolbar_geometry, visible,
)


ROW = 34
BUTTON = 28
FLAGS = ('--force-renderer-accessibility', '--force-device-scale-factor=1',
         '--touch-ui=disabled', '--lang=en-US')
BUTTONS = {
    'back': r'class_name=BackForwardButton name=Back ',
    'forward': r'class_name=BackForwardButton name=Forward ',
    'reload': r'class_name=ReloadButton name=Reload ',
    'settings': r'class_name=PinnedActionToolbarButton name=Settings ',
}
TOP_ORDER = ('back', 'forward', 'reload', 'address', 'extensions', 'menu',
             'action:settings', 'new-tab', 'tabs')
DENSE_ORDER = ('back', 'forward', 'reload', 'home', 'new-tab', 'menu',
               'customize', 'profile', 'action:history', 'action:downloads',
               'action:bookmarks', 'action:print', 'action:copy-url',
               'action:save-page', 'address', 'extensions', 'action:settings', 'tabs')

# Complete JS expressions, kept together so they can be parsed offline by Node.
JS = {
    'ready': '(()=>{' + FIND + 'return !!editor?.draft;})()',
    'light': '(()=>{' + FIND + '''
      const control=deep(document,'#noventraColorScheme');
      if(!control) throw Error('Appearance selector missing');
      control.value='1';control.dispatchEvent(new Event('change',{bubbles:true}));
      return true;
    })()''',
    'preset': '(async()=>{' + FIND + '''
      const name=PRESET;
      const button=editor.shadowRoot.querySelector('[data-preset="'+name+'"]');
      if(!button || button.disabled) throw Error('Preset unavailable: '+name);
      button.click();await editor.updateComplete;
      return {layout:editor.draft.current,matches:editor.presetMatches(name)};
    })()''',
    'editor': '(async()=>{' + FIND + '''
      editor.selected='back';await editor.updateComplete;
      const root=editor.shadowRoot;
      const attrs=['data-edge','data-position-edge','data-place-edge','data-quick-edge'];
      return {
        bottom:[...root.querySelectorAll('*')].filter(e=>
          attrs.some(a=>e.getAttribute(a)==='bottom') ||
          /bottom/i.test(e.getAttribute('data-preset')||'')).map(e=>e.outerHTML),
        zones:[...root.querySelectorAll('.zone[data-edge]')].map(e=>e.dataset.edge),
        positions:Object.fromEntries(['tabs','address'].map(id=>[id,
          [...root.querySelectorAll('[data-position-control="'+id+'"]')]
            .map(e=>e.dataset.positionEdge)])),
        presets:[...root.querySelectorAll('[data-preset]')].map(e=>e.dataset.preset),
        quick:[...root.querySelectorAll('[data-quick-edge]')].map(e=>e.dataset.quickEdge),
        labels:[...root.querySelectorAll('button,option')].map(e=>
          ({text:e.textContent.trim(),label:e.getAttribute('aria-label')||''}))
      };
    })()''',
    'show_editor': '(async()=>{' + FIND + '''
      const page=deep(document,'settings-appearance-page');
      page.shadowRoot.querySelector('[data-section=customizeLayout]').click();
      await page.updateComplete;await editor.updateComplete;
      editor.scrollIntoView({block:'start',behavior:'instant'});return true;
    })()''',
    'page': '''(()=>{
      document.title=TITLE;window.compactToken=TITLE;
      document.body.innerHTML='<style>body{margin:32px;background:#eef3f8;font:16px sans-serif}' +
        'input{display:block;width:280px;height:36px;margin-top:12px}</style>' +
        '<label for="draft">Unsaved compact-chrome fixture</label><input id="draft">';
      document.querySelector('#draft').value=TITLE+' unsaved';return true;
    })()''',
    'draft': '''({token:window.compactToken,origin:performance.timeOrigin,
      value:document.querySelector('#draft')?.value,url:location.href})''',
    'visible': 'document.visibilityState === "visible"',
}


class SmokeTimeout(TimeoutError):
    pass


class Budget:
    def __init__(self):
        self.started = time.monotonic()
        self.work_deadline = self.started + 200
        self.deadline = self.started + 240
        self.previous = signal.signal(signal.SIGALRM, self.expired)
        signal.setitimer(signal.ITIMER_REAL, 200)

    @staticmethod
    def expired(*_):
        raise SmokeTimeout('Compact chrome smoke exhausted its bounded time budget')

    def arm(self, deadline):
        signal.setitimer(signal.ITIMER_REAL, max(.001, deadline - time.monotonic()))

    def close(self):
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, self.previous)


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def diagnostics(browser, tree, inp, output, result, budget):
    """Best-effort evidence while browser and native input are still alive."""
    deadline = min(time.monotonic() + 20, budget.deadline - 20)
    actions = [
        ('tree', lambda: (output / 'failure.txt').write_text(tree.read().text) if tree else None),
        ('screenshot', lambda: inp.capture(output / 'failure.png').close() if inp else None),
        ('targets', lambda: write_json(output / 'failure-targets.json', request(browser.port, '/json/list'))),
        ('layout', lambda: write_json(output / 'failure-layout.json', browser.state())),
    ]
    for name, action in actions:
        if time.monotonic() >= deadline:
            result.setdefault('diagnostic_errors', {})[name] = 'Diagnostic budget exhausted'
            break
        budget.arm(deadline)
        try:
            action()
        except Exception as error:
            result.setdefault('diagnostic_errors', {})[name] = repr(error)
            if isinstance(error, SmokeTimeout):
                break
    budget.arm(budget.deadline)


@contextmanager
def session(args, profile, output, flags, result, budget):
    # The except is inside LayoutBrowser: failure evidence must precede shutdown.
    fixture = LayoutBrowser(args.browser, profile, output, flags)
    finished = False
    try:
        with fixture as browser:
            inp = tree = None
            try:
                inp = Input(browser.platform)
                tree = NativeUITree(browser)
                yield browser, tree, inp
            except BaseException:
                diagnostics(browser, tree, inp, output, result, budget)
                raise
            finally:
                # Do not let the work alarm interrupt stop_browser halfway
                # through cleanup. Keep 2s for a final owned-group kill/wait.
                budget.arm(min(time.monotonic() + 18, budget.deadline - 2))
                if inp:
                    inp.close()
        finished = True
    finally:
        process = fixture.process
        if process and process.poll() is None:
            budget.arm(budget.deadline)
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=2)
        if finished:
            budget.arm(budget.work_deadline)


def apply(browser, layout):
    layout = deepcopy(layout)
    layout['revision'] = browser.state()['layout']['revision']
    response = browser.apply(layout)
    assert response['status'] == 'ok', response
    time.sleep(.35)
    return response['layout']


def preset(browser, name):
    wait_for(lambda: browser.ev(JS['ready']), browser.process, timeout=8)
    draft = browser.ev(JS['preset'].replace('PRESET', json.dumps(name)))
    assert draft['matches'], (name, draft)
    return apply(browser, draft['layout'])


def top_layout(template, order=TOP_ORDER):
    layout = deepcopy(template)
    ids = {item['id'] for item in layout['items']}
    assert set(order) <= ids, ('Fixture controls missing from catalog', set(order) - ids)
    layout['items'].sort(key=lambda item: order.index(item['id']) if item['id'] in order else 999)
    for index, item in enumerate(layout['items']):
        item.update(edge='left' if item['id'] == 'spaces' else 'top',
                    order=index, visible=item['id'] in order)
    return layout


def make_page(browser, title):
    page = request(browser.port, '/json/new?about:blank', 'PUT')
    browser.ev(JS['page'].replace('TITLE', json.dumps(title)), page)
    return page, browser.ev(JS['draft'], page)


def retained(browser, pages):
    for page, before in pages:
        after = browser.ev(JS['draft'], page)
        assert after == before, ('Document or unsaved draft changed', page['id'], before, after)


def activate(browser, tree, inp, page):
    """Fixture setup only; measured native tab clicks never call this helper."""
    request(browser.port, '/json/activate/' + page['id'])
    wait_for(lambda: browser.ev(JS['visible'], page), browser.process, timeout=5)
    content = tree.rect(one(tree.read(), r'class_name=MultiContentsView '))
    inp.pointer.click(content['x'] + content['w'] / 2, content['y'] + content['h'] / 2)


def no_bottom(tree):
    client = tree.rect(one(tree, r'class_name=BrowserView '))
    content = tree.rect(one(tree, r'class_name=MultiContentsView '))
    assert content['y'] + content['h'] == client['y'] + client['h'], (
        'Bottom chrome or a blank bottom strip still consumes page space', content, client)
    docks = [tree.rect(node) for node in visible(tree, r'class_name=NoventraDockView ')]
    for dock in docks:
        assert inside(dock, client), ('Dock extends outside the browser', dock, client)
        # Side docks can reach the footer; horizontal docks must be above content.
        side = (dock['w'] < client['w'] / 2 and
                dock['y'] + dock['h'] == client['y'] + client['h'] and
                (dock['x'] == client['x'] or
                 dock['x'] + dock['w'] == client['x'] + client['w']))
        assert side or dock['y'] + dock['h'] <= content['y'], (
            'Visible bottom dock or unexpected floating chrome', dock, content)
    return {'browser': client, 'content': content, 'docks': docks}


def geometry(tree, *, rows=None, horizontal=False, settings=False, top_only=False):
    """Measure native Views, including the real address field, not preview CSS."""
    g = no_bottom(tree)
    back = one(tree, BUTTONS['back'])
    dock_node = tree.nodes[back['parent']]
    assert 'class_name=NoventraDockView ' in dock_node['line'], dock_node
    dock_id = identity(tree, dock_node)
    dock = tree.rect(dock_node)
    g['top'] = dock
    client, content = g['browser'], g['content']
    assert dock['y'] == client['y'] and dock['x'] == client['x'] and dock['w'] == client['w'], g
    assert dock['h'] % ROW == 0, ('Header does not use 34px rows', g)
    count = dock['h'] // ROW
    expected_count = count == rows if rows is not None else 3 <= count <= 4
    assert expected_count, ('Unexpected row count', rows, g)
    assert content['y'] == dock['y'] + dock['h'], ('An extra blank toolbar row consumes page space', g)
    if top_only:
        assert content['x'] == client['x'] and content['w'] == client['w'], ('Top-only layout retained a side dock', g)
        assert not visible(tree, r'class_name=AccountSpacesButton ')

    # The address slot is AX-ignored while the native field overlays it; include
    # that field when deciding whether a wrapped row is occupied.
    address = tree.rect(one(tree, r'class_name=LocationBarView '))
    peers = [node for node in visible(tree, 'class_name=') if node['parent'] == dock_id]
    occupied = {int((address['y'] + address['h'] / 2 - dock['y']) // ROW)}
    for node in peers:
        rect = tree.rect(node)
        assert inside(rect, dock), ('Header child clipped', rect, dock, node)
        if re.match(r'^\+*id=\d+ (?:button|toggleButton|popUpButton) ', node['line']) and 'name=Search or enter address ' not in node['line']:
            assert rect['w'] == rect['h'] == BUTTON, ('Native button is not a 28px hit target', node)
            assert (rect['y'] - dock['y']) % ROW == 3, ('Native button is off-center', node)
        row = int((rect['y'] + rect['h'] / 2 - dock['y']) // ROW)
        occupied.add(row)
    assert occupied == set(range(count)), ('Empty native header row', occupied, count)
    for index, node in enumerate(peers):
        for other in peers[index + 1:]:
            assert not overlaps(tree.rect(node), tree.rect(other)), ('Header siblings overlap', node, other)

    g['buttons'] = {}
    for label in ('back', 'forward', 'reload', *(('settings',) if settings else ())):
        node = one(tree, BUTTONS[label])
        rect = tree.rect(node)
        assert node['parent'] == dock_id and rect['w'] == rect['h'] == BUTTON, (label, rect)
        assert (rect['y'] - dock['y']) % ROW == 3, ('Button is not centered in its row', label, rect, dock)
        g['buttons'][label] = rect
    for first, second in [('back', 'forward'), ('forward', 'reload')]:
        a, b = g['buttons'][first], g['buttons'][second]
        assert a['y'] == b['y'] and b['x'] - a['x'] - a['w'] == 3, ('Navigation gap is not 3px', a, b)

    captions = [tree.rect(node) for node in visible(tree, r'class_name=FrameCaptionButton ')]
    g['caption_exclusion'] = {'visible_buttons': captions,
        'coverage': 'bounds checked' if captions else 'not exposed by this private Xvfb frame'}
    # At the unexcluded leading edge, navigation begins at 6px outer padding.
    leading = [r['x'] + r['w'] for r in captions
               if r['x'] <= client['x'] and r['y'] < g['buttons']['back']['y'] + BUTTON
               and r['y'] + r['h'] > g['buttons']['back']['y']]
    assert g['buttons']['back']['x'] == max([client['x'], *leading]) + 6, (
        'Header leading padding is not 6px', g['buttons']['back'], captions)
    for rect in (tree.rect(node) for node in peers):
        assert all(not overlaps(rect, caption) for caption in captions), ('Caption overlaps a control', rect, captions)
    navigation = [tree.rect(node) for node in peers if 'class_name=HorizontalTabStripRegionView' not in node['line']]
    for rect in navigation:
        assert rect['x'] >= dock['x'] + 6 and rect['x'] + rect['w'] <= dock['x'] + dock['w'] - 6, (
            'Navigation consumed its 6px outer horizontal padding', rect, dock)
    if rows in (1, 2) and not captions:
        row_peers = ([tree.rect(node) for node in peers]
                     if count == 1 else navigation)
        assert max(r['x'] + r['w'] for r in row_peers) == dock['x'] + dock['w'] - 6, (
            'Wide toolbar trailing padding is not 6px', row_peers, dock)

    g['address'] = address
    assert address['h'] == BUTTON and address['w'] >= 140 and inside(address, dock), (
        'Actual address field is not a contained 28px field', address, dock)
    assert (address['y'] - dock['y']) % ROW == 3, ('Address field is off-center', address, dock)
    assert all(not overlaps(address, caption) for caption in captions), ('Address overlaps caption controls', address, captions)
    if rows in (1, 2):
        assert address['y'] == g['buttons']['back']['y'], ('Address and navigation split unnecessarily', g)
    if horizontal:
        tabs = tree.rect(one(tree, r'class_name=HorizontalTabStripRegionView(?:Old)? '))
        assert tabs['h'] == ROW and tabs['y'] == dock['y'] and inside(tabs, dock), tabs
        if count == 1:
            assert tabs['w'] >= 160 and not overlaps(tabs, address), (tabs, address)
            assert tabs['y'] + tabs['h'] / 2 == address['y'] + address['h'] / 2, (tabs, address)
        else:
            assert tabs['w'] >= dock['w'] - 200, ('Wrapped tab row is clipped', tabs, dock)
            assert address['y'] >= tabs['y'] + ROW, ('Wrapped tabs overlap navigation', tabs, address)
        for node in visible(tree, r'class_name=Tab '):
            assert inside(tree.rect(node), tabs), ('Horizontal tab is clipped', node, tabs)
        g['tabs'] = tabs
    else:
        assert not visible(tree, r'class_name=HorizontalTabStripRegionView(?:Old)? ')
    return g


def extension_geometry(tree, inp, output):
    region = tree.rect(one(tree, r'class_name=ExtensionsToolbarDesktop '))
    nodes = visible(tree, r'class_name=(ToolbarActionView|ExtensionsToolbarButton) ')
    assert len(nodes) >= 3, ('Two pinned fixtures plus native extensions button required', nodes)
    measurements = {}
    with inp.capture(output / 'top-extension-icons.png') as shot:
        for node in nodes:
            rect = tree.rect(node)
            assert rect['w'] == rect['h'] == BUTTON and inside(rect, region), (rect, region)
            assert abs(rect['y'] + BUTTON / 2 - (region['y'] + region['h'] / 2)) <= 1, (rect, region)
        for name, color in COLORS.items():
            rect = tree.rect(one(tree, r'class_name=ToolbarActionView name=' + re.escape(name) + r'(?: name_from=| \()'))
            points = [(x, y) for y in range(rect['y'], rect['y'] + rect['h'])
                      for x in range(rect['x'], rect['x'] + rect['w'])
                      if max(abs(a - b) for a, b in zip(shot.getpixel((x, y)), color)) < 15]
            assert len(points) >= 200, ('Pinned extension icon is missing or clipped', name, len(points))
            center = [(min(p[i] for p in points) + max(p[i] for p in points) + 1) / 2 for i in (0, 1)]
            assert abs(center[0] - rect['x'] - 14) <= 1 and abs(center[1] - rect['y'] - 14) <= 1, (name, center, rect)
            measurements[name] = {'bounds': rect, 'paint_center': center, 'pixels': len(points)}
    return measurements


def hover(tree, inp, output):
    measurements = {}
    content = tree.rect(one(tree, r'class_name=MultiContentsView '))
    for name in ('reload', 'settings'):
        node = one(tree, BUTTONS[name])
        rect = tree.rect(node)
        assert rect['w'] == rect['h'] == BUTTON, rect
        box = (rect['x'], rect['y'], rect['x'] + BUTTON, rect['y'] + BUTTON)
        inp.move(content['x'] + content['w'] / 2, content['y'] + 150)
        time.sleep(.25)
        with inp.capture(output / f'{name}-idle.png') as shot:
            idle = shot.crop(box)
        try:
            background = Counter(idle.getdata()).most_common(1)[0][0]
            ink = [(x, y) for y in range(BUTTON) for x in range(BUTTON)
                   if max(abs(a - b) for a, b in zip(idle.getpixel((x, y)), background)) >= 50]
            assert len(ink) >= 12, ('Native glyph is missing', name)
            center = [(min(p[i] for p in ink) + max(p[i] for p in ink) + 1) / 2 for i in (0, 1)]
            assert all(abs(value - 14) <= 2 for value in center), ('28px glyph is not centered', name, center)
            inp.move(*tree.center(node))
            time.sleep(.2)
            with inp.capture(output / f'{name}-hover.png') as shot, shot.crop(box) as hovered:
                with ImageChops.difference(idle, hovered) as difference:
                    changed = sum(max(pixel) >= 8 for pixel in difference.getdata())
            assert changed >= 20, ('28px native hover did not paint', name, changed)
            measurements[name] = {'bounds': rect, 'glyph_center': center, 'hover_pixels': changed}
        finally:
            idle.close()
    return measurements


def type_text(inp, value):
    for char in value:
        native_key(inp, {':': 'semicolon', '/': 'slash'}.get(char, char),
                   ('Shift_L',) if char == ':' else ())


def check_field(tree, expected):
    field = one(tree.read(), r'class_name=OmniboxViewViews ')
    assert re.search(r' value=' + re.escape(expected) + r'(?: |$)', field['line']), (expected, field)
    assert tree.rect(one(tree, r'class_name=LocationBarView '))['h'] == BUTTON
    return field['line']


def native_interactions(browser, tree, inp, output, pages, scratch, result):
    first, second = pages
    # Each measured tab switch is native. CDP below only reads visibility/drafts.
    for page, before in (second, first):
        tab = one(tree.read(), r'class_name=Tab name=' + re.escape(before['token']) + r' name_from=')
        inp.pointer.click(*tree.center(tab))
        wait_for(lambda: browser.ev(JS['visible'], page), browser.process, timeout=5)
        other = first[0] if page['id'] == second[0]['id'] else second[0]
        assert not browser.ev(JS['visible'], other), 'Native click did not switch away from the previous tab'
        retained(browser, pages)
    capture(tree, inp, output, 'native-tab-switches')
    field = one(tree, r'class_name=LocationBarView ')
    inp.pointer.click(*tree.center(field))
    native_key(inp, 'a', ('Control_L',))
    type_text(inp, 'compactdraft')
    capture(tree, inp, output, 'native-address-click-draft')
    check_field(tree, 'compactdraft')
    # Native Chromium closes autocomplete on the first Escape and reverts the
    # edited URL on the second. The matched old/new-runtime control records
    # this behavior; require the restored value before testing Ctrl+L.
    native_key(inp, 'Escape')
    native_key(inp, 'Escape')
    check_field(tree, 'about:blank')
    retained(browser, pages)

    native_key(inp, 'l', ('Control_L',))
    field = one(tree.read(), r'class_name=OmniboxViewViews ')
    match = re.search(r'sel_start=(\d+) sel_end=(\d+)', field['line'])
    assert match and sorted(map(int, match.groups())) == [0, len('about:blank')], (
        'Ctrl+L failed to select the full native address', field)
    type_text(inp, 'keptdraft')
    capture(tree, inp, output, 'native-ctrl-l-draft')
    check_field(tree, 'keptdraft')
    native_key(inp, 'Escape')
    native_key(inp, 'Escape')
    check_field(tree, 'about:blank')
    retained(browser, pages)

    # Cancelled text still leaves the real omnibox focused. Click the page to
    # dismiss its expanded editing pane before targeting the now-visible tabs.
    content = tree.rect(one(tree, r'class_name=MultiContentsView '))
    inp.pointer.click(content['x'] + content['w'] / 2,
                      content['y'] + content['h'] / 2)
    tab = one(tree.read(), r'class_name=Tab name=Compact scratch name_from=')
    inp.pointer.click(*tree.center(tab))
    wait_for(lambda: browser.ev(JS['visible'], scratch), browser.process, timeout=5)
    native_key(inp, 'l', ('Control_L',))
    type_text(inp, 'chrome://version')
    check_field(tree, 'chrome://version')
    native_key(inp, 'Return')
    wait_for(lambda: browser.ev('location.href', scratch) == 'chrome://version/', browser.process, timeout=8)
    capture(tree, inp, output, 'native-address-navigation')
    origin = browser.ev('performance.timeOrigin', scratch)
    inp.pointer.click(*tree.center(one(tree, BUTTONS['reload'])))
    wait_for(lambda: browser.ev('performance.timeOrigin', scratch) != origin, browser.process, timeout=8)
    assert browser.ev('location.href', scratch) == 'chrome://version/'
    retained(browser, pages)
    capture(tree, inp, output, 'native-reload')
    inp.pointer.click(*tree.center(one(tree, BUTTONS['settings'])))

    def settings_visible():
        return next((target for target in request(browser.port, '/json/list')
                     if target['type'] == 'page' and target['url'].startswith('chrome://settings/')
                     and browser.ev(JS['visible'], target)), None)

    settings = wait_for(settings_visible, browser.process, timeout=8)
    capture(tree, inp, output, 'native-settings-click')
    retained(browser, pages)
    result['native_input'] = {'settings_target': settings['id'], 'navigated_target': scratch['id'],
                              'retained_targets': [page['id'] for page, _ in pages]}


def retired_bottom(browser, result):
    state = browser.state()
    assert state['capabilities'] and state['presets'], state
    for cap in state['capabilities']:
        assert cap['edges'] and set(cap['edges']) <= {'top', 'left', 'right'}, cap
        if cap['id'] == 'spaces':
            assert set(cap['edges']) == {'left', 'right'} and cap['hideable'], cap
    for name, layout in state['presets'].items():
        assert 'bottom' not in name.lower(), name
        assert all(item['edge'] in ('top', 'left', 'right') for item in layout['items']), (name, layout)
    editor = browser.ev(JS['editor'])
    result['editor'] = editor
    assert not editor['bottom'], editor
    assert set(editor['zones']) == set(editor['quick']) == {'top', 'left', 'right'}, editor
    assert {'workspace', 'classic-top'} <= set(editor['presets']), editor
    for choices in editor['positions'].values():
        assert len(choices) == 3 and set(choices) == {'top', 'left', 'right'}, choices
    assert not any(re.search(r'\bbottom\b', entry['text'] + ' ' + entry['label'], re.I)
                   for entry in editor['labels']), editor['labels']
    saved = state['layout']
    rejected = {}
    for control in ('back', 'address', 'tabs', 'home', 'spaces'):
        invalid = deepcopy(saved)
        item = next(item for item in invalid['items'] if item['id'] == control)
        item.update(edge='bottom', order=0)
        if control == 'home':
            item['visible'] = False
        response = browser.apply(invalid)
        assert response['status'] == 'invalid', (control, response)
        assert browser.state()['layout'] == saved, ('Rejected bottom placement mutated layout', control)
        rejected[control] = response
    result['rejected_bottom'] = rejected


def legacy_fixture(template, *, sparse):
    """All nonreserved controls occupy valid old bottom slots, including tabs."""
    layout = deepcopy(template)
    layout['revision'] = 73 if sparse else 74
    reserved = {'back': ('top', 0), 'forward': ('top', 2),
                'action:settings': ('top', 255 if sparse else 6),
                'bookmarks': ('left', 7)}
    if not sparse:
        reserved['tabs'] = ('right', 0)
    shown = {'back', 'forward', 'reload', 'address', 'tabs', 'spaces',
             'new-tab', 'extensions', 'menu', 'action:settings'}
    bottom_order = 0
    # Reverse the stored list so migration must respect order, not iteration.
    for item in layout['items']:
        if item['id'] in reserved:
            edge, order = reserved[item['id']]
        else:
            edge, order = 'bottom', bottom_order
            bottom_order += 1
        item.update(edge=edge, order=order, visible=item['id'] in shown)
    layout['items'].reverse()
    assert len({(i['edge'], i['order']) for i in layout['items']}) == len(layout['items'])
    home = next(i for i in layout['items'] if i['id'] == 'home')
    assert home['edge'] == 'bottom' and not home['visible']
    return layout


def check_migration(before, after, spaces_side):
    old = {item['id']: item for item in before['items']}
    new = {item['id']: item for item in after['items']}
    assert set(new) == set(old) and len(after['items']) == len(old), ('Migration lost or duplicated controls', before, after)
    assert (after['version'], after['revision']) == (before['version'], before['revision']), after
    assert len({(i['edge'], i['order']) for i in new.values()}) == len(new), ('Migration produced occupied slots', after)
    for control, item in old.items():
        actual = new[control]
        assert type(actual['order']) is int and 0 <= actual['order'] < 256, actual
        if item['edge'] != 'bottom':
            assert actual == item, ('Migration changed an unrelated placement', item, actual)
        else:
            assert actual == dict(item, edge=spaces_side if control == 'spaces' else 'top',
                                  order=actual['order']), ('Migration lost visibility or item data', item, actual)
    moved = sorted((item for item in old.values() if item['edge'] == 'bottom' and item['id'] != 'spaces'),
                   key=lambda item: item['order'])
    migrated = sorted((new[item['id']] for item in moved), key=lambda item: item['order'])
    assert [i['id'] for i in migrated] == [i['id'] for i in moved], ('Migration changed bottom control order', moved, migrated)
    assert all(item['edge'] != 'bottom' for item in new.values()), after
    return {'moved': [item['id'] for item in moved], 'spaces': new['spaces'],
            'hidden_home': new['home'], 'revision': after['revision']}


def exercise(args, profile, flags, result, budget):
    output = args.output / 'native'
    with session(args, profile, output, flags, result, budget) as (browser, tree, inp):
        result['phase'] = 'editor-and-api'
        wait_for(lambda: browser.ev(JS['ready']), browser.process, timeout=8)
        browser.ev(JS['light'])
        wait_for(lambda: browser.ev('matchMedia("(prefers-color-scheme:light)").matches'), browser.process, timeout=5)
        pin_extensions(browser)
        retired_bottom(browser, result)
        request(browser.port, '/json/activate/' + browser.settings['id'])
        browser.ev(JS['show_editor'])
        capture(tree, inp, output, 'editor-without-bottom')
        result['checks'].append('Editor placement/preview/preset choices and native capabilities exclude bottom; visible and hidden bottom Apply requests fail without mutation')

        pages = [make_page(browser, title) for title in ('Compact draft A', 'Compact draft B')]
        scratch, _ = make_page(browser, 'Compact scratch')
        # Remove only this fresh browser's duplicate startup tab to keep the
        # fixture tabs visible even in the narrow horizontal strip.
        keep = {browser.settings['id'], tree.target['id'], scratch['id'], *(p['id'] for p, _ in pages)}
        for page in request(browser.port, '/json/list'):
            if page['type'] == 'page' and page['id'] not in keep:
                request(browser.port, '/json/close/' + page['id'])
        # These injected about:blank documents otherwise retain the new-tab
        # omnibox focus. Establish each page's normal content focus as setup,
        # before measuring any tab switch (never reactivate after a test click).
        for page, _ in [*pages, (scratch, None)]:
            activate(browser, tree, inp, page)

        result['phase'] = 'workspace'
        workspace = preset(browser, 'workspace')
        activate(browser, tree, inp, pages[0][0])
        capture(tree, inp, output, 'workspace')
        result['geometry']['workspace'] = geometry(tree, rows=1)
        result['geometry']['workspace']['footer'] = footer_geometry(tree, 'left', 'Personal')
        result['checks'].append('Workspace has one 34px top row, a centered 28px native address field and the native workspace-dot footer')

        result['phase'] = 'classic-top'
        classic = preset(browser, 'classic-top')
        activate(browser, tree, inp, pages[0][0])
        capture(tree, inp, output, 'classic-top')
        result['geometry']['classic-top'] = geometry(tree, rows=1, horizontal=True, top_only=True)
        assert not next(item for item in classic['items'] if item['id'] == 'spaces')['visible']
        retained(browser, pages)
        result['checks'].append('Compact top uses one shared 34px tabs/navigation row, with no sidebar or blank toolbar row')

        for side in ('left', 'right'):
            result['phase'] = side + '-unchanged-footer'
            apply_footer_layout(browser, side, extensions=True)
            activate(browser, tree, inp, pages[0][0])
            capture(tree, inp, output, result['phase'])
            result['geometry'][result['phase']] = footer_geometry(tree, side, 'Personal')
            side_toolbar_geometry(tree, inp, output, side)
            retained(browser, pages)
        result['checks'].append('Footer/toolbar assertions pass on both sides: workspace dots, 40px side cells, centered native 28px extension buttons')

        result['phase'] = 'top-only'
        apply(browser, top_layout(workspace))
        activate(browser, tree, inp, pages[0][0])
        capture(tree, inp, output, 'top-only')
        result['geometry']['top-only'] = geometry(tree, rows=1, horizontal=True, settings=True, top_only=True)
        result['extension_icons'] = extension_geometry(tree, inp, output)
        result['hover'] = hover(tree, inp, output)
        result['checks'].append('Horizontal-only chrome contains tabs and controls in one 34px row; native buttons/gaps/padding are 28/3/6px and pinned icons/hover paint are centered')

        result['phase'] = 'native-input'
        native_interactions(browser, tree, inp, output, pages, scratch, result)
        result['checks'].append('Native tab clicks, address click/typing/Escape, Ctrl+L selection/typing/Escape, navigation, Reload and Settings clicks work while both page drafts retain their document identities')

        result['phase'] = 'narrow-wrap'
        apply(browser, top_layout(workspace, DENSE_ORDER))
        activate(browser, tree, inp, pages[0][0])
        inp.resize(700, 480)
        capture(tree, inp, output, 'narrow-wrap')
        narrow = geometry(tree, horizontal=True, settings=True, top_only=True)
        result['geometry']['narrow-wrap'] = narrow
        assert narrow['browser']['w'] <= 700 and narrow['browser']['h'] <= 480, ('Resize did not exercise the narrow window', narrow)
        assert narrow['content']['h'] >= 300, ('Wrapping consumed too much page height', narrow)
        native_key(inp, 'l', ('Control_L',))
        type_text(inp, 'narrowdraft')
        capture(tree, inp, output, 'narrow-native-address-draft')
        check_field(tree, 'narrowdraft')
        native_key(inp, 'Escape')
        native_key(inp, 'Escape')
        check_field(tree, 'about:blank')
        retained(browser, pages)
        result['checks'].append('A crowded 700x480 top-only window wraps into occupied 34px rows; native controls, tabs and 28px field stay contained, Ctrl+L typing/Escape works and drafts survive')
        return browser.state()['layout'], browser.process


def migrations(args, profile, flags, template, stopped_process, result, budget):
    preferences = profile / 'Default/Preferences'
    assert stopped_process.poll() is not None, 'Never write Preferences while its browser is alive'
    assert profile.resolve().is_relative_to(args.output.resolve()) and preferences.is_file()
    for sparse in (True, False):
        name = 'legacy-sparse' if sparse else 'legacy-side-tabs'
        output = args.output / name
        output.mkdir()
        result['phase'] = name + '-seed-stopped-profile'
        assert stopped_process.poll() is not None
        original = legacy_fixture(template, sparse=sparse)
        prefs = json.loads(preferences.read_text())
        prefs.setdefault('noventra', {}).setdefault('browser', {})['dock_layout'] = original
        # Sparse case exercises horizontal tabs migration plus the saved-side
        # fallback. The other case proves vertical tabs override that fallback.
        prefs.setdefault('helium', {}).setdefault('browser', {})['vertical_right_aligned'] = sparse
        marker = 'about:blank#' + name
        prefs['homepage'] = marker
        preferences.write_text(json.dumps(prefs))
        write_json(output / 'legacy-layout.json', original)
        migrated = None
        for run in (1, 2):
            result['phase'] = name + '-restart-' + str(run)
            run_output = output / ('restart-' + str(run))
            with session(args, profile, run_output, flags, result, budget) as (browser, tree, inp):
                current = browser.state()['layout']
                write_json(run_output / 'layout.json', current)
                page = request(browser.port, '/json/new?about:blank', 'PUT')
                activate(browser, tree, inp, page)
                capture(tree, inp, run_output, 'restored')
                evidence = check_migration(original, current, 'right')
                no_bottom(tree)
                footer_geometry(tree, 'right', 'Personal', horizontal=sparse)
                assert browser.state()['layout'] == current, 'A second read repeated migration'
                if migrated is None:
                    migrated = current
                    result.setdefault('migration', {})[name] = evidence
                else:
                    assert current == migrated, 'Restart changed the migrated layout'
                stopped_process = browser.process
            assert stopped_process.poll() is not None
            stored = json.loads(preferences.read_text())
            actual = stored['noventra']['browser']['dock_layout']
            write_json(run_output / 'persisted-layout.json', actual)
            assert actual == migrated, 'Migration did not persist after browser shutdown'
            assert stored['homepage'] == marker, 'Migration reset an unrelated profile preference'
        result['checks'].append(name + ': every valid bottom item migrates without lost IDs/visibility/order; occupied top slots, hidden Home, side Spaces, revision and unrelated prefs survive two restarts')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--browser', type=Path, required=True, help='Candidate desktop executable; no baseline is required')
    parser.add_argument('--output', type=Path, required=True, help='New artifact directory (must not already exist)')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'checks': [], 'geometry': {}, 'phase': 'preflight',
              'browser': str(args.browser), 'budgets_s': {'work': 200, 'diagnostics': 20, 'cleanup': 20, 'total': 240},
              'limits': ['Private X11 only; desktop scale 1; touch UI disabled; light English fixture',
                         'Caption bounds checked only when exposed; caption clicks, RTL and touch UI untested',
                         'Spaces geometry/alignment reused; exhaustive menus remain in sidebar_footer_smoke.py',
                         'Fresh-profile layout fixture; normal first-run defaults remain in workspace_startup_smoke.py']}
    budget = Budget()
    try:
        assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1', 'This smoke requires the private Xvfb wrapper'
        assert str(ROOT / 'work/ram-investigation/xvfb-') in os.environ.get('XAUTHORITY', ''), 'Use work/ram-investigation/with-xvfb.py'
        with tempfile.TemporaryDirectory(prefix='profile-', dir=args.output) as temp:
            base = Path(temp)
            flags = (*FLAGS, extension_fixture(base))
            profile = base / 'profile'
            template, stopped = exercise(args, profile, flags, result, budget)
            migrations(args, profile, flags, template, stopped, result, budget)
        result.update(passed=True, phase='complete')
    except BaseException as error:
        result['error'] = repr(error)
        (args.output / 'failure-traceback.txt').write_text(traceback.format_exc())
        raise
    finally:
        budget.close()
        result['elapsed_s'] = round(time.monotonic() - budget.started, 2)
        write_json(args.output / 'result.json', result)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
