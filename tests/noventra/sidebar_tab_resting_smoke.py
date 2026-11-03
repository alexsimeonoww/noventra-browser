"""Resting sidebar tabs and new-tab focus, using native input on owned Xvfb.

CDP only prepares local documents/layout and reads actual page state. Measured
tab clicks, URL edits, New Tab, typing, scrolling and resizing use X11 input.
No host browser, display, focus or profile is used. Give --output a new path.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time

from PIL import Image

import compact_chrome_smoke as native
from dock_controls_smoke import Handler
from extension_smoke import ROOT, request, wait_for
from interface_polish_smoke import native_key
from new_tab_design_smoke import HOME, SEARCH_INPUT
from sidebar_inline_address_smoke import (
    EDITOR, FIELD, SEARCH, NEW_TAB, ancestor, dismiss_edit, editor_value,
    make_page, page_focus, resize_sidebar, screen_rect, tab, wait_inline, wheel,
)
from sidebar_unified_smoke import close_normally


def editor_value(tree):
    field = native.one(tree.read(), EDITOR)
    match = re.search(r' value=(.*?)(?= (?:controls_ids=|type=|actions=)|$)', field['line'])
    assert match, field
    return match[1]


def assert_painted_input(path, rect, expected):
    with Image.open(path) as image:
        crop = image.crop((rect['x'], rect['y'], rect['x'] + rect['w'], rect['y'] + rect['h']))
        assert len(crop.convert('RGB').getcolors(100000)) > 1, 'Native editor painted only its background'
        target = path.with_name(path.stem + '-editor.png')
        crop.resize((crop.width * 4, crop.height * 4)).save(target)
    text = subprocess.check_output(['tesseract', str(target), 'stdout', '--psm', '7'],
                                   stderr=subprocess.DEVNULL, text=True)
    assert expected in re.sub('[^a-z]', '', text.lower()), (expected, text)


def type_text(inp, value):
    for char in value:
        if char == ' ':
            native_key(inp, 'space')
        else:
            native.type_text(inp, char)


def resting(browser, tree, title):
    def ready():
        tree.read()
        row = screen_rect(tree, tab(tree, title))
        fields = native.visible(tree, FIELD)
        # TabView is an AX leaf; its painted title Label is intentionally not
        # serialized as a native child. Paint is checked from an owned capture.
        return {'row': row, 'tab': tab(tree, title)['line']} if not fields else None
    state = wait_for(ready, browser.process, timeout=6)
    assert not native.visible(tree, SEARCH), 'Standalone sidebar Search remains'
    return state


def plus_geometry(tree):
    tree.read()
    region = screen_rect(tree, native.one(tree, r'class_name=VerticalTabStripRegionView '))
    plus = screen_rect(tree, native.one(tree, NEW_TAB))
    rows = [screen_rect(tree, n) for n in native.visible(tree, r'class_name=TabView ')]
    rows = [r for r in rows if native.inside(r, region)]
    assert rows, region
    last = max(r['y'] + r['h'] for r in rows)
    assert 0 <= plus['y'] - last <= 20, ('New Tab must follow the actual rows', plus, last, region)
    assert plus['h'] == 30 and plus['w'] > 0, plus
    assert plus['y'] == region['y'] + region['h'], (plus, region)
    return {'region': region, 'plus': plus, 'last_row_bottom': last, 'rows': len(rows)}


def click_active(browser, tree, inp, title):
    row = screen_rect(tree, tab(tree.read(), title))
    inp.pointer.click(row['x'] + row['w'] / 2, row['y'] + row['h'] / 2)
    return wait_inline(browser, tree, title)


def ntp_state(browser, target):
    return browser.ev('(()=>{' + HOME + '''
      const input=''' + SEARCH_INPUT + ''';
      return {focused:input.matches(':focus'),value:input.value,
        documentFocused:document.hasFocus(),visible:!document.hidden};})()''', target)


def new_tab(browser, tree, inp, keyboard=False):
    before = {t['id'] for t in request(browser.port, '/json/list')}
    if keyboard:
        native_key(inp, 't', ('Control_L',))
    else:
        inp.pointer.click(*tree.center(native.one(tree.read(), NEW_TAB)))
    target = wait_for(lambda: next((t for t in request(browser.port, '/json/list')
        if t['type'] == 'page' and t['id'] not in before), None), browser.process, timeout=8)
    def focused():
        try:
            state = ntp_state(browser, target)
            return state if state['focused'] and state['visible'] and state['documentFocused'] else None
        except Exception:
            return None
    wait_for(focused, browser.process, timeout=8)
    assert not native.visible(tree.read(), FIELD), 'New tab opened the sidebar URL editor'
    type_text(inp, 'center')
    # XStringToKeysym requires the name "space"; a literal blank resolves to
    # NoSymbol in the inherited per-character helper and emits no character.
    native_key(inp, 'space')
    type_text(inp, 'draft')
    state = ntp_state(browser, target)
    assert state['value'] == 'center draft', state
    return target


def run(args):
    owner = os.environ.get('NOVENTRA_BENCH_XVFB_PID', '')
    assert owner.isdigit() and Path(f'/proc/{owner}/comm').read_text().strip() == 'Xvfb'
    assert Path(os.environ['XAUTHORITY']).resolve().is_relative_to(ROOT / 'work/ram-investigation')
    args.browser = args.browser.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'checks': [], 'runtime': str(args.browser), 'geometry': {},
              'binary_sha256': hashlib.file_digest(args.browser.open('rb'), 'sha256').hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    budget = native.Budget()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    try:
        for side in ('left', 'right'):
            result['phase'] = side + '-resting'
            output = args.output / side
            output.mkdir()
            with tempfile.TemporaryDirectory(prefix='profile-', dir=output) as tmp:
                profile = Path(tmp)
                (profile / 'Default').mkdir()
                (profile / 'Default/Preferences').write_text(json.dumps({
                    'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 232},
                    'helium': {'browser': {'zen_mode': True, 'zen_mode_sidebar_pinned': True,
                                           'zen_mode_top_chrome_pinned': True}}}))
                with native.session(args, profile, output, native.FLAGS, result, budget) as (browser, tree, inp):
                    native.preset(browser, 'minimal-' + side)
                    first = make_page(browser, base + '/first', 'Resting first')
                    second = make_page(browser, base + '/second', 'Resting second')
                    native.activate(browser, tree, inp, first[0])
                    rest = resting(browser, tree, 'Resting first')
                    result['geometry'][side + '-short'] = plus_geometry(tree)
                    native.capture(tree, inp, output, 'plain-tab-rows')
                    r = rest['row']
                    with Image.open(output / 'plain-tab-rows.png') as image:
                        crop = image.crop((r['x'] + 26, r['y'], r['x'] + r['w'] - 28,
                                           r['y'] + r['h']))
                        crop.resize((crop.width * 4, crop.height * 4)).save(output / 'painted-title.png')
                    text = subprocess.check_output(['tesseract', str(output / 'painted-title.png'),
                        'stdout', '--psm', '7'], stderr=subprocess.DEVNULL, text=True)
                    assert 'restingfirst' in re.sub('[^a-z]', '', text.lower()), text
                    result['checks'].append(side + ': real page title at rest, no omnibox/Search, New Tab immediately below rows')

                    result['phase'] = side + '-click-edit'
                    geometry = click_active(browser, tree, inp, 'Resting first')
                    assert editor_value(tree).endswith('/first')
                    type_text(inp, 'clickdraft')
                    assert editor_value(tree) == 'clickdraft'
                    typed_geometry = wait_inline(browser, tree, 'Resting first')
                    assert typed_geometry['field'] == geometry['field']
                    content = screen_rect(tree, native.one(tree, r'class_name=MultiContentsView '))
                    inp.move(content['x'] + content['w'] / 2, content['y'] + content['h'] - 40)
                    native.capture(tree, inp, output, 'click-reveals-url-editor')
                    assert_painted_input(output / 'click-reveals-url-editor.png', typed_geometry['editor'], 'clickdraft')
                    dismiss_edit(tree, inp)
                    resting(browser, tree, 'Resting first')
                    row = screen_rect(tree, tab(tree.read(), 'Resting second'))
                    inp.pointer.click(row['x'] + row['w'] / 2, row['y'] + row['h'] / 2)
                    wait_for(lambda: browser.ev(native.JS['visible'], second[0]), browser.process)
                    resting(browser, tree, 'Resting second')
                    click_active(browser, tree, inp, 'Resting second')
                    assert editor_value(tree).endswith('/second')
                    dismiss_edit(tree, inp)
                    resting(browser, tree, 'Resting second')
                    native.retained(browser, [first, second])
                    result['checks'].append(side + ': active click edits inline; blur restores title; inactive click switches without editing; drafts retained')

                    result['phase'] = side + '-bookmark'
                    library = request(browser.port, '/json/new?chrome://bookmarks/', 'PUT')
                    wait_for(lambda: browser.ev('typeof chrome.bookmarks === "object"', library), browser.process)
                    native.activate(browser, tree, inp, second[0])
                    click_active(browser, tree, inp, 'Resting second')
                    star = native.one(tree.read(), r'class_name=PageActionView name=Bookmark this tab ')
                    inp.pointer.click(*tree.center(star))
                    saved = lambda: browser.ev('(async()=>await chrome.bookmarks.search({url:' +
                                               json.dumps(base + '/second') + '}))()', library)
                    bookmarks = wait_for(saved, browser.process)
                    assert len(bookmarks) == 1 and bookmarks[0]['url'] == base + '/second'
                    native.one(tree.read(), r'class_name=RootView name=Bookmark added ')
                    native.one(tree, r'class_name=MdTextButton name=Done ')
                    native.capture(tree, inp, output, 'save-current-page')
                    native_key(inp, 'Return')
                    page_focus(tree, inp)
                    resting(browser, tree, 'Resting second')
                    assert len(saved()) == 1
                    result['checks'].append(side + ': editor bookmark icon saves the actual page; native bookmark bubble stays usable; blur restores title')

                    result['phase'] = side + '-new-tab-focus'
                    for keyboard in (False, True):
                        scratch = new_tab(browser, tree, inp, keyboard)
                        native.capture(tree, inp, output, 'center-search-' + ('ctrl-t' if keyboard else 'plus'))
                        native_key(inp, 'l', ('Control_L',))
                        wait_for(lambda: native.visible(tree.read(), EDITOR), browser.process)
                        type_text(inp, 'chrome://version')
                        assert editor_value(tree) == 'chrome://version'
                        native_key(inp, 'Return')
                        wait_for(lambda: browser.ev('location.href', scratch) == 'chrome://version/', browser.process)
                        native_key(inp, 'w', ('Control_L',))
                        wait_for(lambda: scratch['id'] not in {t['id'] for t in request(browser.port, '/json/list')}, browser.process)
                        wait_for(lambda: browser.ev(native.JS['visible'], second[0]), browser.process)
                        page_focus(tree, inp)
                        resting(browser, tree, 'Resting second')
                    result['checks'].append(side + ': native + and CtrlT focus center input; immediate typing lands there; explicit CtrlL navigates in sidebar')

                    result['phase'] = side + '-overflow'
                    fillers = []
                    for index in range(24):
                        filler = request(browser.port, '/json/new?about:blank', 'PUT')
                        browser.ev('document.title=' + json.dumps(f'Resting filler {index:02d}') + ';true', filler)
                        fillers.append(filler)
                    native.activate(browser, tree, inp, first[0])
                    resting(browser, tree, 'Resting first')
                    native_key(inp, 'l', ('Control_L',))
                    geometry = wait_inline(browser, tree, 'Resting first')
                    dismiss_edit(tree, inp)
                    view = geometry['viewport']
                    inp.move(view['x'] + view['w'] / 2, view['y'] + view['h'] / 2)
                    wheel(inp, 9)
                    assert not native.visible(tree.read(), FIELD)
                    native_key(inp, 'l', ('Control_L',))
                    geometry = wait_inline(browser, tree, 'Resting first')
                    assert native.inside(geometry['row'], geometry['viewport'])
                    type_text(inp, 'overflowdraft')
                    assert editor_value(tree) == 'overflowdraft'
                    native.capture(tree, inp, output, 'overflow-editor-recovered')
                    dismiss_edit(tree, inp)
                    inp.move(view['x'] + view['w'] / 2, view['y'] + view['h'] / 2)
                    wheel(inp, -12)
                    for filler in fillers:
                        request(browser.port, '/json/close/' + filler['id'])
                    time.sleep(.3)
                    resting(browser, tree, 'Resting first')
                    result['geometry'][side + '-shrunk'] = plus_geometry(tree)
                    result['checks'].append(side + ': overflow scrolls natively, CtrlL reveals whole row, New Tab follows shrinking list')

                    result['phase'] = side + '-compact'
                    dock = resize_sidebar(tree, inp, side, 34)
                    assert not native.visible(tree, SEARCH) and not native.visible(tree, FIELD)
                    native.capture(tree, inp, output, 'compact-no-search-button')
                    row = screen_rect(tree, tab(tree.read(), 'Resting first'))
                    inp.pointer.click(row['x'] + row['w'] / 2, row['y'] + row['h'] / 2)
                    wait_for(lambda: native.visible(tree.read(), EDITOR), browser.process)
                    type_text(inp, 'rail click')
                    assert editor_value(tree) == 'rail click'
                    dismiss_edit(tree, inp)
                    native_key(inp, 'l', ('Control_L',))
                    wait_for(lambda: native.visible(tree.read(), EDITOR), browser.process)
                    field = screen_rect(tree, native.one(tree, FIELD))
                    assert field['w'] <= 280
                    assert abs((field['x'] - dock['x']) if side == 'left' else
                               (field['x'] + field['w'] - dock['x'] - dock['w'])) <= 1
                    type_text(inp, 'rail draft')
                    assert editor_value(tree) == 'rail draft'
                    dismiss_edit(tree, inp)
                    resize_sidebar(tree, inp, side, 232)
                    resting(browser, tree, 'Resting first')
                    native.retained(browser, [first, second])
                    result['checks'].append(side + ': compact rail has no Search button; CtrlL stays accessible and anchored; widening restores plain title')
                    result.setdefault('normal_shutdowns', []).append({'side': side, 'exit': close_normally(browser)})
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        budget.close()
        server.shutdown()
        server.server_close()
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
