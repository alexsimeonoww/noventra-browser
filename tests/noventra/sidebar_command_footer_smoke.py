"""Native centered-editor and bookmark-slot acceptance on an owned private X11.

CDP prepares only real local documents, the layout and fixture bookmark/tab models.
Measured editing, tab selection, folder expansion and new-tab focus use native
input. A negative result still closes each owned browser normally. No source,
user profile, desktop display, existing test or packaged bytes are changed.
"""
import argparse
import csv
import io
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time

from PIL import Image

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'tests/noventra').is_dir())
sys.path.insert(0, str(ROOT / 'tests/noventra'))
import compact_chrome_smoke as native
from dock_controls_smoke import Handler
from extension_smoke import request, wait_for
from interface_polish_smoke import native_key
from sidebar_inline_address_smoke import (
    EDITOR, FIELD, SEARCH, NEW_TAB, ancestor, make_page, page_focus,
    resize_sidebar, screen_rect, tab,
)
from sidebar_tab_resting_smoke import editor_value, ntp_state, plus_geometry
from sidebar_unified_smoke import close_normally, resize_window


def type_text(inp, value):
    # XStringToKeysym takes symbolic key names; literal punctuation can map to
    # another key or NoSymbol. Keep the URL punctuation in the owned fixture.
    symbols = {'.': 'period', '/': 'slash', ':': 'semicolon', '-': 'minus',
               '?': 'slash', '&': '7', '=': 'equal', ' ': 'space'}
    for char in value:
        native_key(inp, symbols.get(char, char),
                   ('Shift_L',) if char in ':?&' else ())


def page_focus(tree, inp):
    # The command editor occupies the content center. Click a real page target
    # below it, not the inherited helper's now-overlapping center point.
    content = screen_rect(tree.read(), native.one(tree, r'class_name=MultiContentsView '))
    point = (content['x'] + content['w'] / 2, content['y'] + content['h'] - 40)
    for field in native.visible(tree, FIELD):
        bounds = screen_rect(tree, field)
        assert not (bounds['x'] <= point[0] < bounds['x'] + bounds['w'] and
                    bounds['y'] <= point[1] < bounds['y'] + bounds['h']), (point, bounds)
    inp.pointer.click(*point)


def visible(tree, pattern):
    return native.visible(tree, pattern)


def geometry(tree):
    tree.read()
    return {
        'field': screen_rect(tree, native.one(tree, FIELD)),
        'editor': screen_rect(tree, native.one(tree, EDITOR)),
        'content': screen_rect(tree, native.one(tree, r'class_name=MultiContentsView ')),
        'value': editor_value(tree),
    }


def capture(tree, inp, output, name, crop=None):
    inp.move(800, 710)
    native.capture(tree, inp, output, name)
    if not crop:
        return None
    path = output / (name + '.png')
    with Image.open(path) as image:
        area = image.crop((crop['x'], crop['y'], crop['x'] + crop['w'], crop['y'] + crop['h']))
        colors = len(area.convert('RGB').getcolors(100000) or [])
        target = output / (name + '-editor.png')
        area.resize((area.width * 4, area.height * 4)).save(target)
    text = subprocess.check_output(['tesseract', str(target), 'stdout', '--psm', '7'],
                                   stderr=subprocess.DEVNULL, text=True, timeout=15)
    return {'unique_colors': colors, 'ocr': text}


def center_assertions(result, side, label, state, tree, inp, output):
    field, content, editor = (state[k] for k in ('field', 'content', 'editor'))
    check(result, side, label + ': field contained in page content', native.inside(field, content), state)
    check(result, side, label + ': native editor contained in field', native.inside(editor, field), state)
    check(result, side, label + ': centered horizontally',
          abs(field['x'] + field['w'] / 2 - content['x'] - content['w'] / 2) <= 8, state)
    check(result, side, label + ': centered vertically',
          abs(field['y'] + field['h'] / 2 - content['y'] - content['h'] / 2) <= 8, state)
    check(result, side, label + ': readable bounded width',
          min(400, max(0, content['w'] - 32)) <= field['w'] <= min(640, content['w']), state)
    check(result, side, label + ': roomy native field is 48 DIP high', field['h'] == 48, state)
    # OmniboxViewViews itself grows to44 DIP. Measure its actual painted
    # glyphs, whose native line is vertically centered inside that view.
    name = re.sub('[^a-z0-9]+', '-', label.lower()) + '-glyph-proof'
    capture(tree, inp, output, name)
    with Image.open(output / (name + '.png')) as image:
        crop = image.crop((editor['x'], editor['y'], editor['x'] + editor['w'], editor['y'] + editor['h']))
        crop.resize((crop.width * 4, crop.height * 4)).save(output / (name + '-text.png'))
    tsv = subprocess.check_output(['tesseract', str(output / (name + '-text.png')),
                                   'stdout', '--psm', '7', 'tsv'],
                                  stderr=subprocess.DEVNULL, text=True, timeout=15)
    words = [w for w in csv.DictReader(io.StringIO(tsv), delimiter='\t')
             if re.search('[a-z0-9]', w['text'], re.I) and int(w['width']) > 2 and int(w['height']) > 4]
    glyphs = [{'x': editor['x'] + int(w['left']) / 4, 'y': editor['y'] + int(w['top']) / 4,
               'w': int(w['width']) / 4, 'h': int(w['height']) / 4} for w in words]
    check(result, side, label + ': native text has vertical breathing room',
          bool(glyphs) and all(native.inside(glyph, editor) and
                              glyph['y'] - field['y'] >= 6 and
                              field['y'] + field['h'] - glyph['y'] - glyph['h'] >= 6 for glyph in glyphs),
          {'native': state, 'actual_text_words': words, 'actual_glyph_bounds': glyphs})


def bookmark_row(name):
    return r'class_name=(?:MdTextButton|BookmarkRowButton) name=' + re.escape(name) + r' name_from='


def first_suggestion(tree):
    # A real URL can expose both navigation and search rows. The first native
    # row is identified by its actual accessibility position, not cardinality.
    return native.one(tree, r'class_name=OmniboxResultView .*posinset=1 ')


def rounded_surface_assertions(result, side, output, name, state):
    # The local fixture page is a flat, known color at the field midpoint.
    # A square dock/backdrop fills the outer samples; an oversized pill leaves
    # the bounded inner samples untouched. Inspect actual native pixels.
    field = state['field']
    with Image.open(output / (name + '.png')) as image:
        image = image.convert('RGB')
        reference = image.getpixel((field['x'] - 8, field['y'] - 8))
        corners = []
        for flip_x, flip_y in ((False, False), (True, False), (False, True), (True, True)):
            def sample(dx, dy):
                x = field['x'] + (field['w'] - 1 - dx if flip_x else dx)
                y = field['y'] + (field['h'] - 1 - dy if flip_y else dy)
                return image.getpixel((x, y))
            outer = sample(1, 1)
            inner = [sample(16, 2), sample(2, 16)]
            corners.append({'flip_x': flip_x, 'flip_y': flip_y, 'outer': outer,
                            'inner': inner,
                            'outer_page_delta': max(abs(a - b) for a, b in zip(outer, reference)),
                            'inner_page_delta': [max(abs(a - b) for a, b in zip(p, reference)) for p in inner]})
    check(result, side, 'native field has bounded rounded corners without a square backdrop',
          all(c['outer_page_delta'] <= 3 and min(c['inner_page_delta']) >= 16 for c in corners),
          {'page_reference': reference, 'corners': corners, 'field': field,
           'measurement': 'Actual 1x Xvfb desktop pixels, before native suggestion popup opens'})


def opaque_surface_assertion(result, side, output, name, contrast_name, state):
    field = state['field']
    with Image.open(output / (name + '.png')) as before, Image.open(output / (contrast_name + '.png')) as after:
        before, after = before.convert('RGB'), after.convert('RGB')
        points = [(field['x'] + 40, field['y'] + 5),
                  (field['x'] + field['w'] // 2, field['y'] + 5),
                  (field['x'] + field['w'] - 40, field['y'] + 5)]
        deltas = [max(abs(a - b) for a, b in zip(before.getpixel(p), after.getpixel(p))) for p in points]
        outside = (field['x'] - 8, field['y'] - 8)
        page_delta = max(abs(a - b) for a, b in zip(before.getpixel(outside), after.getpixel(outside)))
    check(result, side, 'native editor surface stays opaque over changed real page background',
          page_delta >= 32 and max(deltas) <= 3,
          {'native_surface_samples': points, 'surface_pixel_deltas': deltas,
           'actual_page_reference_delta': page_delta})


def suggestion_screen_rect(tree, node, field):
    # Each native suggestion widget has its own RootView at local (0,0).
    # Its TopBackgroundView cutout is aligned to the actual LocationBarView.
    # Translate result bounds using that native cutout, then independently
    # cross-check actual desktop text in suggestion_assertions below.
    cutout = screen_rect(tree, native.one(tree, r'class_name=TopBackgroundView '))
    local = screen_rect(tree, node)
    return ({'x': local['x'] + field['x'] - cutout['x'],
             'y': local['y'] + field['y'] - cutout['y'],
             'w': local['w'], 'h': local['h']}, cutout)


def suggestion_assertions(result, side, tree, output, name, state, token, expected_height=48):
    field = state['field']
    node = first_suggestion(tree)
    row, cutout = suggestion_screen_rect(tree, node, field)
    check(result, side, f'native suggestions reserve the full {expected_height}-DIP editor height',
          cutout['h'] == field['h'] == expected_height and row['y'] >= field['y'] + field['h'],
          {'field': field, 'cutout_local': cutout, 'first_result_screen': row})
    tsv = subprocess.check_output(['tesseract', str(output / (name + '.png')),
                                   'stdout', '--psm', '11', 'tsv'],
                                  stderr=subprocess.DEVNULL, text=True, timeout=15)
    words = list(csv.DictReader(io.StringIO(tsv), delimiter='\t'))
    query_words = [w for w in words if token in re.sub('[^a-z]', '', w['text'].lower())]
    boxes = [{'x': int(w['left']), 'y': int(w['top']), 'w': int(w['width']),
              'h': int(w['height'])} for w in query_words]
    check(result, side, 'first native suggestion text paints readably below the field',
          len(boxes) == 2 and sum(native.inside(box, state['editor']) for box in boxes) == 1 and
          sum(native.inside(box, row) and box['y'] >= field['y'] + field['h'] for box in boxes) == 1,
          {'actual_desktop_query_words': query_words, 'first_result_screen': row,
           'native_editor_screen': state['editor'], 'field': field})


def check(result, side, name, passed, actual=None):
    result['checks'].append({'side': side, 'name': name, 'passed': bool(passed), 'actual': actual})


def dismiss(tree, inp):
    native_key(inp, 'Escape')
    native_key(inp, 'Escape')
    page_focus(tree, inp)
    time.sleep(.15)


def extension_fixture(profile):
    fixture = profile / 'fixture-controller'
    fixture.mkdir()
    (fixture / 'manifest.json').write_text(json.dumps({
        'manifest_version': 3, 'name': 'Sidebar command acceptance controller',
        'version': '1.0', 'permissions': ['tabs', 'bookmarks'],
        'background': {'service_worker': 'worker.js'},
    }))
    (fixture / 'worker.js').write_text('chrome.runtime.onInstalled.addListener(()=>{});')
    (fixture / 'control.html').write_text('<title>Sidebar acceptance controller</title>')
    return fixture


def main(args):
    owner = os.environ.get('NOVENTRA_BENCH_XVFB_PID', '')
    assert owner.isdigit() and Path(f'/proc/{owner}/comm').read_text().strip() == 'Xvfb'
    assert Path(os.environ['XAUTHORITY']).resolve().is_relative_to(ROOT / 'work/ram-investigation')
    assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1'
    args.browser = args.browser.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'checks': [], 'normal_shutdowns': [],
              'runtime': str(args.browser),
              'binary_sha256': hashlib.file_digest(args.browser.open('rb'), 'sha256').hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'scope': 'Actual native X11 UI, local real bookmark/tab models, disposable profiles only'}
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    budget = native.Budget()
    try:
        for side in ('left', 'right'):
            output = args.output / side
            output.mkdir()
            with tempfile.TemporaryDirectory(prefix='profile-', dir=output) as tmp:
                profile = Path(tmp)
                (profile / 'Default').mkdir()
                (profile / 'Default/Preferences').write_text(json.dumps({
                    'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 232},
                    'helium': {'browser': {'zen_mode': True, 'zen_mode_sidebar_pinned': True,
                                           'zen_mode_top_chrome_pinned': True}},
                }))
                fixture = extension_fixture(profile)
                flags = [*native.FLAGS, '--load-extension=' + str(fixture.resolve())]
                with native.session(args, profile, output, flags, result, budget) as (browser, tree, inp):
                    try:
                        result['phase'] = side + '-setup'
                        layout = native.preset(browser, 'minimal-' + side)
                        order = ['back', 'forward', 'reload', 'action:bookmarks', 'extensions',
                                 'address', 'bookmarks', 'tabs', 'new-tab', 'spaces', 'menu']
                        layout['items'].sort(key=lambda item: order.index(item['id']) if item['id'] in order else 99)
                        for index, item in enumerate(layout['items']):
                            item.update(order=index, edge=side, visible=item['id'] in order)
                        native.apply(browser, layout)
                        worker = wait_for(lambda: next((t for t in request(browser.port, '/json/list')
                            if t['type'] == 'service_worker' and t['url'].endswith('/worker.js')), None), browser.process)
                        controller = request(browser.port, '/json/new?' + worker['url'].rsplit('/', 1)[0] + '/control.html', 'PUT')
                        def api(js):
                            return browser.ev('(async()=>{const value=await (' + js + ');return value??null})()', controller)
                        folder = api('chrome.bookmarks.create({parentId:"1",title:"Project folder"})')
                        nested = api('chrome.bookmarks.create({parentId:' + json.dumps(folder['id']) + ',title:"Nested folder"})')
                        api('chrome.bookmarks.create({parentId:' + json.dumps(nested['id']) +
                            ',title:"Saved project page",url:' + json.dumps(base + '/saved-project') + '})')
                        ordinary = make_page(browser, base + '/ordinary', 'Ordinary project')
                        long_url = base + '/project/reports/quarterly/2026/final/summary/readable-address-proof/retained-project-page'
                        pinned = make_page(browser, long_url, 'Pinned project')
                        pin_id = api('chrome.tabs.query({}).then(ts=>ts.find(t=>t.url===' + json.dumps(long_url) + ').id)')
                        api('chrome.tabs.update(' + str(pin_id) + ',{pinned:true})')
                        native.activate(browser, tree, inp, ordinary[0])
                        result['phase'] = side + '-regular'
                        tree.read()
                        check(result, side, 'regular rest has no field or Search',
                              not visible(tree, FIELD) and not visible(tree, SEARCH))
                        check(result, side, 'header Extensions control removed',
                              not visible(tree, r'class_name=ExtensionsToolbarButton name=Extensions '))
                        check(result, side, 'header folder action removed',
                              not visible(tree, r'class_name=ToolbarButton name=Bookmarks ') and
                              not visible(tree, r'class_name=NoventraBookmarksView name=Bookmark menu '))
                        capture(tree, inp, output, 'regular-rest')
                        try:
                            plus = plus_geometry(tree)
                            check(result, side, 'New Tab row follows actual ordinary tabs', True, plus)
                        except AssertionError as error:
                            check(result, side, 'New Tab row follows actual ordinary tabs', False, repr(error))
                        # Read real layout of the slot independently of bookmark behavior.
                        section = native.one(tree, r'class_name=NoventraBookmarksView name=Bookmark library section ')
                        section_rect = screen_rect(tree, section)
                        pin_rect = screen_rect(tree, tab(tree, 'Pinned project - Pinned'))
                        ordinary_rect = screen_rect(tree, tab(tree, 'Ordinary project'))
                        dock_node = tree.nodes[section['parent']]
                        dock_rect = screen_rect(tree, dock_node)
                        spaces_rect = screen_rect(tree, native.one(tree, r'class_name=AccountSpacesButton '))
                        plus_rect = screen_rect(tree, native.one(tree, r'class_name=ToolbarButton name=New [Tt]ab '))
                        check(result, side, 'bookmark section is dock sibling above Spaces and below tabs/New Tab',
                              'class_name=NoventraDockView ' in dock_node['line'] and
                              section_rect['y'] >= pin_rect['y'] + pin_rect['h'] and
                              section_rect['y'] >= ordinary_rect['y'] + ordinary_rect['h'] and
                              section_rect['y'] >= plus_rect['y'] + plus_rect['h'] and
                              section_rect['y'] + section_rect['h'] + 8 == spaces_rect['y'] and
                              spaces_rect['y'] + spaces_rect['h'] == dock_rect['y'] + dock_rect['h'],
                              {'section': section_rect, 'pin': pin_rect, 'ordinary': ordinary_rect,
                               'new_tab': plus_rect, 'spaces': spaces_rect, 'dock': dock_rect})
                        heading = native.one(tree, r'class_name=(?:MdTextButton|BookmarkRowButton) name=Bookmarks name_from=')
                        heading_rect = screen_rect(tree, heading)
                        # Read the real painted Bookmarks caption independently
                        # of its native accessible name.
                        with Image.open(output / 'regular-rest.png') as image:
                            crop = image.crop((heading_rect['x'], heading_rect['y'],
                                               heading_rect['x'] + heading_rect['w'],
                                               heading_rect['y'] + heading_rect['h']))
                            crop.resize((crop.width * 4, crop.height * 4)).save(output / 'bookmarks-caption-crop.png')
                        caption_text = subprocess.check_output(
                            ['tesseract', str(output / 'bookmarks-caption-crop.png'), 'stdout', '--psm', '7'],
                            stderr=subprocess.DEVNULL, text=True, timeout=15)
                        check(result, side, 'bookmark caption is Bookmarks in a 24-DIP native heading',
                              ' name=Bookmarks name_from=' in heading['line'] and
                              heading_rect['h'] == 24 and 'bookmarks' in re.sub('[^a-z]', '', caption_text.lower()),
                              {'heading': heading_rect, 'native_accessible_heading': heading['line'],
                               'actual_caption_crop_ocr': caption_text})
                        create_folder = native.one(tree, bookmark_row('New bookmark folder'))
                        create_rect = screen_rect(tree, create_folder)
                        check(result, side, 'folder plus is a 24-DIP peer beside the Bookmarks caption',
                              create_rect['h'] == 24 and create_rect['w'] == 24 and
                              create_rect['y'] == heading_rect['y'] and native.inside(create_rect, section_rect),
                              {'heading': heading_rect, 'new_folder': create_rect, 'section': section_rect})
                        check(result, side, 'embedded bookmark section has no All bookmarks footer',
                              not visible(tree, bookmark_row('All bookmarks')))
                        # Active title click must open the actual centered editor.
                        row = screen_rect(tree, tab(tree, 'Ordinary project'))
                        inp.pointer.click(row['x'] + row['w'] / 2, row['y'] + row['h'] / 2)
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        state = geometry(tree)
                        center_assertions(result, side, 'regular active click', state, tree, inp, output)
                        capture(tree, inp, output, 'regular-centered-unedited')
                        rounded_surface_assertions(result, side, output, 'regular-centered-unedited', state)
                        original_background = browser.ev('document.body.style.backgroundColor', ordinary[0])
                        browser.ev('document.body.style.backgroundColor="#b87932";true', ordinary[0])
                        try:
                            capture(tree, inp, output, 'regular-centered-contrast-page')
                            opaque_surface_assertion(result, side, output, 'regular-centered-unedited',
                                                     'regular-centered-contrast-page', state)
                        finally:
                            browser.ev('document.body.style.backgroundColor=' + json.dumps(original_background) + ';true', ordinary[0])
                        type_text(inp, 'visiblequery')
                        state = geometry(tree)
                        painted = capture(tree, inp, output, 'regular-centered-edit', state['editor'])
                        check(result, side, 'regular native text paints readably',
                              painted['unique_colors'] > 1 and 'visiblequery' in re.sub('[^a-z]', '', painted['ocr'].lower()), painted)
                        check(result, side, 'native autocomplete popup visible',
                              bool(visible(tree, r'class_name=OmniboxPopupViewViews ')))
                        suggestion_assertions(result, side, tree, output, 'regular-centered-edit', state, 'visiblequery')
                        native_key(inp, 'Escape')
                        native_key(inp, 'Escape')
                        check(result, side, 'Escape dismisses editor without outside click', not visible(tree.read(), FIELD))
                        page_focus(tree, inp)
                        native_key(inp, 'l', ('Control_L',))
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        page_focus(tree, inp)
                        check(result, side, 'outside click closes editor', not visible(tree.read(), FIELD))
                        native_key(inp, 'l', ('Control_L',))
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        center_assertions(result, side, 'regular CtrlL', geometry(tree), tree, inp, output)
                        type_text(inp, base + '/committed')
                        assert editor_value(tree) == base + '/committed', ('Fixture native URL typing mismatch', editor_value(tree))
                        native_key(inp, 'Down')
                        native_key(inp, 'Up')
                        native_key(inp, 'Return')
                        wait_for(lambda: browser.ev('location.pathname', ordinary[0]) == '/committed', browser.process)
                        check(result, side, 'native address commit navigates and closes editor',
                              not visible(tree.read(), FIELD))
                        native_key(inp, 'l', ('Control_L',))
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        type_text(inp, base + '/pointer-committed')
                        assert editor_value(tree) == base + '/pointer-committed'
                        wait_for(lambda: visible(tree.read(), r'class_name=OmniboxResultView '), browser.process, timeout=6)
                        field = geometry(tree)['field']
                        suggestion = first_suggestion(tree)
                        suggestion_rect, cutout = suggestion_screen_rect(tree, suggestion, field)
                        capture(tree, inp, output, 'native-pointer-suggestion')
                        check(result, side, 'pointer-selectable suggestion starts below the roomy editor',
                              suggestion_rect['y'] >= field['y'] + field['h'],
                              {'field': field, 'suggestion': suggestion_rect, 'native_name': suggestion['line']})
                        inp.pointer.click(suggestion_rect['x'] + suggestion_rect['w'] / 2,
                                          suggestion_rect['y'] + suggestion_rect['h'] / 2)
                        wait_for(lambda: browser.ev('location.pathname', ordinary[0]) == '/pointer-committed', browser.process)
                        check(result, side, 'native pointer suggestion commits the actual local URL and closes editor',
                              not visible(tree.read(), FIELD))
                        # The pinned tab stays icon-sized, but gets the same full editor.
                        result['phase'] = side + '-pinned'
                        native.activate(browser, tree, inp, pinned[0])
                        check(result, side, 'active pinned rest has no persistent field', not visible(tree.read(), FIELD))
                        row = screen_rect(tree, tab(tree, 'Pinned project - Pinned'))
                        inp.pointer.click(row['x'] + row['w'] / 2, row['y'] + row['h'] / 2)
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        state = geometry(tree)
                        center_assertions(result, side, 'active pinned click', state, tree, inp, output)
                        check(result, side, 'pinned click retains full long native URL',
                              state['value'].endswith('/project/reports/quarterly/2026/final/summary/readable-address-proof/retained-project-page'),
                              {'value': state['value']})
                        type_text(inp, 'pinneddraft')
                        state = geometry(tree)
                        painted = capture(tree, inp, output, 'pinned-centered-edit', state['editor'])
                        check(result, side, 'pinned native text paints readably',
                              painted['unique_colors'] > 1 and 'pinneddraft' in re.sub('[^a-z]', '', painted['ocr'].lower()), painted)
                        dismiss(tree, inp)
                        native_key(inp, 'l', ('Control_L',))
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        center_assertions(result, side, 'pinned CtrlL', geometry(tree), tree, inp, output)
                        dismiss(tree, inp)
                        # Inline nested folders, not the historical folder popup menu.
                        result['phase'] = side + '-folders'
                        native.activate(browser, tree, inp, ordinary[0])
                        folder_button = native.one(tree.read(), bookmark_row('Project folder'))
                        inp.pointer.click(*tree.center(folder_button))
                        time.sleep(.2)
                        tree.read()
                        nested_button = visible(tree, bookmark_row('Nested folder'))
                        check(result, side, 'root folder expands inline without a menu',
                              bool(nested_button) and not visible(tree, r'class_name=MenuItemView name=Nested folder '))
                        if nested_button:
                            inp.pointer.click(*tree.center(nested_button[0]))
                            time.sleep(.2)
                            saved = visible(tree.read(), bookmark_row('Saved project page'))
                            check(result, side, 'nested folder reveals real saved child inline', bool(saved))
                            if saved:
                                inp.pointer.click(*tree.center(saved[0]))
                                wait_for(lambda: browser.ev('location.pathname', ordinary[0]) == '/saved-project', browser.process)
                                check(result, side, 'inline saved child opens actual bookmark URL', True)
                        else:
                            native_key(inp, 'Escape')
                        # Extensions remain available through the single bottom menu.
                        tree.read()
                        menu = native.one(tree, r'class_name=BrowserAppMenuButton ')
                        inp.pointer.click(*tree.center(menu))
                        time.sleep(.2)
                        capture(tree, inp, output, 'bottom-menu')
                        menu_path = output / 'bottom-menu.png'
                        menu_tsv = subprocess.check_output(
                            ['tesseract', str(menu_path), 'stdout', '--psm', '11', 'tsv'],
                            stderr=subprocess.DEVNULL, text=True, timeout=15)
                        words = list(csv.DictReader(io.StringIO(menu_tsv), delimiter='\t'))
                        entries = [word for word in words
                                   if re.sub('[^a-z]', '', word['text'].lower()) == 'extensions']
                        check(result, side, 'bottom menu exposes Extensions', len(entries) == 1,
                              {'matching_native_menu_words': entries})
                        if len(entries) == 1:
                            entry = entries[0]
                            inp.pointer.click(int(entry['left']) + int(entry['width']) / 2,
                                              int(entry['top']) + int(entry['height']) / 2)
                            time.sleep(.3)
                            capture(tree, inp, output, 'native-installed-extensions')
                            text = subprocess.check_output(
                                ['tesseract', str(output / 'native-installed-extensions.png'),
                                 'stdout', '--psm', '11'], stderr=subprocess.DEVNULL,
                                text=True, timeout=15)
                            normalized = re.sub('[^a-z]', '', text.lower())
                            check(result, side, 'Extensions menu opens actual installed extensions',
                                  'extensions' in normalized and
                                  ('commandacceptance' in normalized or 'ublockorigin' in normalized),
                                  {'native_panel_ocr': text})
                        native_key(inp, 'Escape')
                        page_focus(tree, inp)
                        # Ordinary new tabs still focus the page's center field.
                        result['phase'] = side + '-new-tab'
                        for keyboard in (False, True):
                            before = {t['id'] for t in request(browser.port, '/json/list')}
                            if keyboard:
                                native_key(inp, 't', ('Control_L',))
                            else:
                                plus = native.one(tree.read(), NEW_TAB)
                                inp.pointer.click(*tree.center(plus))
                            fresh = wait_for(lambda: next((t for t in request(browser.port, '/json/list')
                                if t['type'] == 'page' and t['id'] not in before), None), browser.process)
                            def fresh_focused():
                                try:
                                    return ntp_state(browser, fresh)['focused']
                                except Exception:
                                    return False
                            wait_for(fresh_focused, browser.process, timeout=8)
                            type_text(inp, 'center draft')
                            label = 'CtrlT' if keyboard else 'plus'
                            check(result, side, label + ' center input receives native typing',
                                  ntp_state(browser, fresh)['value'] == 'center draft' and not visible(tree.read(), FIELD))
                            capture(tree, inp, output, 'new-tab-center-search-' + label)
                            native_key(inp, 'w', ('Control_L',))
                            wait_for(lambda: fresh['id'] not in {t['id'] for t in request(browser.port, '/json/list')}, browser.process)
                        native.activate(browser, tree, inp, ordinary[0])
                        # Collapsed and narrow windows must use the same centered editor.
                        result['phase'] = side + '-compact'
                        resize_sidebar(tree, inp, side, 34)
                        check(result, side, 'compact rail has no Search button', not visible(tree.read(), SEARCH))
                        native_key(inp, 'l', ('Control_L',))
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        center_assertions(result, side, 'compact CtrlL', geometry(tree), tree, inp, output)
                        capture(tree, inp, output, 'compact-centered-editor')
                        dismiss(tree, inp)
                        resize_sidebar(tree, inp, side, 232)
                        resize_window(browser, ordinary[0], 500)
                        native_key(inp, 'l', ('Control_L',))
                        wait_for(lambda: visible(tree.read(), EDITOR), browser.process, timeout=6)
                        center_assertions(result, side, 'narrow window CtrlL', geometry(tree), tree, inp, output)
                        capture(tree, inp, output, 'narrow-centered-editor')
                        dismiss(tree, inp)
                        resize_window(browser, ordinary[0], 1200)
                    except BaseException:
                        (output / 'live-failure-targets.json').write_text(json.dumps(request(browser.port, '/json/list'), indent=2))
                        capture(tree, inp, output, 'live-failure')
                        raise
                    finally:
                        result['normal_shutdowns'].append({'side': side, 'exit': close_normally(browser)})
        result['failed_checks'] = [c for c in result['checks'] if not c['passed']]
        result['passed'] = not result['failed_checks']
    except BaseException as error:
        result['fixture_error'] = repr(error)
        raise
    finally:
        budget.close()
        server.shutdown()
        server.server_close()
        result['failed_checks'] = [c for c in result['checks'] if not c['passed']]
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks']),
                      'failed': len(result['failed_checks']), 'normal_shutdowns': result['normal_shutdowns']}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    raise SystemExit(main(parser.parse_args()))
