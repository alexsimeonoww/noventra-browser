r"""Native inline address editor gate on an owned private X11 display.

Run only after packaging the inline-address candidate, using a NEW output:
  python3 work/ram-investigation/with-xvfb.py python3 \
    tests/noventra/sidebar_inline_address_smoke.py --browser /absolute/candidate \
    --output work/checks/sidebar-inline-new

Settings APIs and CDP prepare the layout/local fixture documents. Measured
address editing, navigation, tab switching, scrolling, bookmarks, New tab,
closing and sidebar resizing use native input. CDP reads the real bookmark model
and retained documents. The inherited bounded session collects failure evidence
before owned cleanup. Nothing uses an existing profile or desktop display.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import threading
import time

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'tests/noventra').is_dir())
sys.path.insert(0, str(ROOT / 'tests/noventra'))
import compact_chrome_smoke as native
from dock_controls_smoke import Handler
from extension_smoke import request, wait_for
from interface_polish_smoke import native_key
from sidebar_unified_smoke import close_normally

EDITOR = r'class_name=OmniboxViewViews '
FIELD = r'class_name=LocationBarView '
SEARCH = r'class_name=ToolbarButton name=Search or enter address '
NEW_TAB = r'class_name=ToolbarButton name=New [Tt]ab '


def screen_rect(tree, node):
    # Native diagnostics serialize content layout bounds separately from the
    # ScrollView's scroll_x/scroll_y attributes. Apply those ancestor offsets
    # for descendants, while keeping the viewport itself fixed on screen.
    rect = tree.rect(node)
    current = node
    while current:
        parent = tree.nodes.get(current['parent'])
        if parent and re.search(r'class_name=ScrollView ', parent['line']):
            if current is not node:
                for axis in ('x', 'y'):
                    match = re.search(r' scroll_' + axis + r'=(-?\d+)', parent['line'])
                    if match:
                        rect[axis] -= int(match[1])
        current = parent
    return rect


def tab(tree, title):
    return native.one(tree, r'class_name=TabView name=' + re.escape(title) + r' name_from=')


def ancestor(tree, node, pattern):
    current = tree.nodes.get(node['parent'])
    while current:
        if re.search(pattern, current['line']):
            return current
        current = tree.nodes.get(current['parent'])
    raise AssertionError(('Missing native ancestor', pattern, node))


def page_focus(tree, inp):
    bounds = screen_rect(tree, native.one(tree.read(), r'class_name=MultiContentsView '))
    inp.pointer.click(bounds['x'] + bounds['w'] / 2, bounds['y'] + bounds['h'] / 2)


def editor_value(tree):
    field = native.one(tree.read(), EDITOR)
    match = re.search(r' value=([^ ]*)', field['line'])
    assert match, ('Native omnibox value missing', field)
    return match[1]


def inline_geometry(tree, title):
    tree.read()
    row_node = tab(tree, title)
    row = screen_rect(tree, row_node)
    field = screen_rect(tree, native.one(tree, FIELD))
    editor = screen_rect(tree, native.one(tree, EDITOR))
    viewport = screen_rect(tree, ancestor(tree, row_node, r'class_name=ScrollView::Viewport '))
    assert field['w'] > 0 and editor['w'] > 0, (field, editor)
    assert native.inside(field, row), ('Real address field escaped its active row', field, row)
    assert native.inside(field, viewport), ('Real address field escaped the native scroll clip', field, viewport)
    assert native.inside(editor, field), ('Native editor is clipped by its field', editor, field)
    assert abs(field['y'] + field['h'] / 2 - row['y'] - row['h'] / 2) <= 1, (field, row)
    return {'row': row, 'field': field, 'editor': editor, 'viewport': viewport}


def wait_inline(browser, tree, title):
    def ready():
        try:
            return inline_geometry(tree, title)
        except (AssertionError, StopIteration):
            return None
    return wait_for(ready, browser.process, timeout=6)


def make_page(browser, url, title):
    target = request(browser.port, '/json/new?' + url, 'PUT')
    # /json/new can return while the target still contains its initial blank
    # document. Wait for this fixture URL before setting its title/draft.
    wait_for(lambda: browser.ev('location.href === ' + json.dumps(url) +
                               ' && document.readyState === "complete"', target), browser.process)
    browser.ev(native.JS['page'].replace('TITLE', json.dumps(title)), target)
    return target, browser.ev(native.JS['draft'], target)


def click_tab(browser, tree, inp, target, title):
    row = screen_rect(tree, tab(tree.read(), title))
    # The favicon remains a tab target while the native address editor occupies
    # the active title. Select inactive rows through their real native hit area.
    x = row['x'] + row['w'] - 16 if tree.rtl else row['x'] + 16
    inp.pointer.click(x, row['y'] + row['h'] / 2)
    wait_for(lambda: browser.ev(native.JS['visible'], target), browser.process, timeout=5)
    return wait_inline(browser, tree, title)


def dismiss_edit(tree, inp):
    # Native autocomplete may consume the first Escape. The second reverts text.
    native_key(inp, 'Escape')
    native_key(inp, 'Escape')
    page_focus(tree, inp)


def assert_page_url(tree, inp, path):
    # The native steady-state field intentionally trims paths after the host.
    # Check the full native editing value to distinguish same-host fixture tabs.
    native_key(inp, 'l', ('Control_L',))
    value = editor_value(tree)
    assert value.endswith(path), (path, value)
    dismiss_edit(tree, inp)


def resize_sidebar(tree, inp, side, width):
    tree.read()
    handle = screen_rect(tree, native.one(tree, r'class_name=ResizeArea name=Vertical Tab Strip Resize Handle '))
    region_node = native.one(tree, r'class_name=VerticalTabStripRegionView ')
    dock = screen_rect(tree, tree.nodes[region_node['parent']])
    start = (handle['x'] + handle['w'] / 2, handle['y'] + handle['h'] / 2)
    delta = width - dock['w']
    # ResizeArea::ReportResizeAmount explicitly mirrors its delegate amount in
    # RTL. Map the fixture's requested width through that installed API.
    if tree.rtl:
        delta = -delta
    inp.move(*start)
    inp.button(True)
    try:
        inp.move(start[0] + (delta if side == 'left' else -delta), start[1])
        time.sleep(.3)
    finally:
        inp.button(False)
    time.sleep(.3)
    tree.read()
    region_node = native.one(tree, r'class_name=VerticalTabStripRegionView ')
    actual = screen_rect(tree, tree.nodes[region_node['parent']])
    assert abs(actual['w'] - width) <= 1, (width, actual)
    return actual


def wheel(inp, count):
    pointer = inp.pointer
    button = 5 if count > 0 else 4
    for _ in range(abs(count)):
        pointer.xt.XTestFakeButtonEvent(pointer.d, button, 1, 0)
        pointer.xt.XTestFakeButtonEvent(pointer.d, button, 0, 0)
    pointer.x.XFlush(pointer.d)
    time.sleep(.25)


def run(args):
    assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1', 'Use private X11 for this native wheel fixture'
    owner = os.environ.get('NOVENTRA_BENCH_XVFB_PID', '')
    assert owner.isdigit() and Path(f'/proc/{owner}/comm').read_text().strip() == 'Xvfb', 'Use with-xvfb.py'
    assert Path(os.environ['XAUTHORITY']).resolve().is_relative_to(ROOT / 'work/ram-investigation')
    args.browser = args.browser.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    budget = native.Budget()
    result = {'passed': False, 'checks': [], 'native_candidate': str(args.browser),
              'scope': 'Owned private Linux X11, native input, local HTTP, real bookmarks',
              'rtl': args.rtl, 'ui_acceptance': 'pending'}
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    # Leave private-display pointer room beyond the RTL window for the native
    # mirrored resize gesture; its end position must not be screen-clamped.
    flags = (*native.FLAGS, *(['--force-ui-direction=rtl', '--window-size=900,760'] if args.rtl else []))
    try:
        sides = ('left', 'right') if args.side == 'both' else (args.side,)
        for side in sides:
            result['phase'] = side + '-startup'
            output = args.output / side
            output.mkdir()
            with tempfile.TemporaryDirectory(prefix='profile-', dir=output) as tmp:
                profile = Path(tmp)
                (profile / 'Default').mkdir()
                (profile / 'Default/Preferences').write_text(json.dumps({
                    'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 232},
                    'helium': {'browser': {'zen_mode': True, 'zen_mode_sidebar_pinned': True,
                                           'zen_mode_top_chrome_pinned': True}}}))
                with native.session(args, profile, output, flags, result, budget) as (browser, tree, inp):
                    native.preset(browser, 'minimal-' + side)
                    first = make_page(browser, base + '/first', 'Inline first')
                    second = make_page(browser, base + '/second', 'Inline second')
                    library = request(browser.port, '/json/new?chrome://bookmarks/', 'PUT')
                    wait_for(lambda: browser.ev('typeof chrome.bookmarks === "object"', library), browser.process)
                    native.activate(browser, tree, inp, first[0])
                    resting = wait_inline(browser, tree, 'Inline first')
                    native.capture(tree, inp, output, 'inline-resting')
                    assert_page_url(tree, inp, '/first')
                    assert not native.visible(tree, SEARCH), 'An extra Search row remains visible'
                    region = screen_rect(tree, native.one(tree.read(), r'class_name=VerticalTabStripRegionView '))
                    plus = screen_rect(tree, native.one(tree, NEW_TAB))
                    assert plus['y'] >= region['y'] + region['h'], ('New tab moved above the tab region', plus, region)
                    result['checks'].append(side + ': one native field rests inside the active row and scroll clip; + stays below tabs')

                    result['phase'] = side + '-editing'
                    inp.pointer.click(resting['editor']['x'] + resting['editor']['w'] / 2,
                                      resting['editor']['y'] + resting['editor']['h'] / 2)
                    native_key(inp, 'a', ('Control_L',))
                    native.type_text(inp, 'inlinedraft')
                    assert editor_value(tree) == 'inlinedraft'
                    assert wait_inline(browser, tree, 'Inline first')['field'] == resting['field']
                    native.capture(tree, inp, output, 'native-click-typed')
                    dismiss_edit(tree, inp)
                    assert_page_url(tree, inp, '/first')
                    native_key(inp, 'l', ('Control_L',))
                    selection = native.one(tree.read(), EDITOR)
                    match = re.search(r'sel_start=(\d+) sel_end=(\d+)', selection['line'])
                    value = editor_value(tree)
                    assert match and sorted(map(int, match.groups())) == [0, len(value)], (value, selection)
                    native.type_text(inp, 'ctrldraft')
                    assert editor_value(tree) == 'ctrldraft'
                    assert wait_inline(browser, tree, 'Inline first')['field'] == resting['field']
                    dismiss_edit(tree, inp)
                    native.retained(browser, [first, second])
                    result['checks'].append(side + ': click/Ctrl+L type into the same native row, select all, Escape restores URL and retained drafts')

                    result['phase'] = side + '-bookmark'
                    tree.read()
                    star = native.one(tree, r'class_name=PageActionView name=Bookmark this tab ')
                    info = native.one(tree, r'class_name=LocationIconView name=View site information ')
                    star_bounds, info_bounds = screen_rect(tree, star), screen_rect(tree, info)
                    field = wait_inline(browser, tree, 'Inline first')['field']
                    assert native.inside(star_bounds, field) and native.inside(info_bounds, field)
                    assert (star_bounds['x'] > info_bounds['x']) if args.rtl else (star_bounds['x'] < info_bounds['x'])
                    inp.pointer.click(*tree.center(star))
                    def saved_bookmark():
                        return browser.ev('(async()=>await chrome.bookmarks.search({url:' +
                                          json.dumps(base + '/first') + '}))()', library)
                    saved = wait_for(saved_bookmark, browser.process, timeout=6)
                    assert len(saved) == 1 and saved[0]['url'] == base + '/first', saved
                    tree.read()
                    bubble = native.one(tree, r'class_name=RootView name=Bookmark added ')
                    assert screen_rect(tree, bubble)['w'] > 0
                    native.capture(tree, inp, output, 'native-bookmark-bubble')
                    # The diagnostics tree gives a bubble RootView's local
                    # coordinates, not its native window origin. Activate its
                    # default Done button through the real keyboard instead.
                    native.one(tree, r'class_name=MdTextButton name=Done ')
                    native_key(inp, 'Return')
                    page_focus(tree, inp)
                    assert len(saved_bookmark()) == 1
                    wait_inline(browser, tree, 'Inline first')
                    result['checks'].append(side + ': native bookmark/page-info controls remain in the row; clicking star saves one real bookmark')

                    result['phase'] = side + '-new-tab-navigation'
                    before_ids = {t['id'] for t in request(browser.port, '/json/list') if t['type'] == 'page'}
                    inp.pointer.click(*tree.center(native.one(tree.read(), NEW_TAB)))
                    scratch = wait_for(lambda: next((t for t in request(browser.port, '/json/list')
                        if t['type'] == 'page' and t['id'] not in before_ids), None), browser.process, timeout=6)
                    native_key(inp, 'l', ('Control_L',))
                    native.type_text(inp, 'chrome://version')
                    assert editor_value(tree) == 'chrome://version'
                    native_key(inp, 'Return')
                    wait_for(lambda: browser.ev('location.href', scratch) == 'chrome://version/', browser.process, timeout=8)
                    native.capture(tree, inp, output, 'native-new-tab-navigation')
                    native_key(inp, 'w', ('Control_L',))
                    wait_for(lambda: scratch['id'] not in {t['id'] for t in request(browser.port, '/json/list')}, browser.process, timeout=6)
                    wait_for(lambda: browser.ev(native.JS['visible'], first[0]), browser.process, timeout=6)
                    page_focus(tree, inp)
                    wait_inline(browser, tree, 'Inline first')
                    native.retained(browser, [first, second])
                    result['checks'].append(side + ': the below-tabs + opens a real tab; native address navigation and Ctrl+W retain existing documents')

                    result['phase'] = side + '-native-tab-switch'
                    click_tab(browser, tree, inp, second[0], 'Inline second')
                    assert_page_url(tree, inp, '/second')
                    click_tab(browser, tree, inp, first[0], 'Inline first')
                    assert_page_url(tree, inp, '/first')
                    native.capture(tree, inp, output, 'native-switch-follows-row')
                    result['checks'].append(side + ': native tab switches move the single editor and update its real URL')

                    result['phase'] = side + '-scroll-clip'
                    fillers = []
                    for index in range(20):
                        filler = request(browser.port, '/json/new?about:blank', 'PUT')
                        browser.ev('document.title=' + json.dumps(f'Inline filler {index:02d}') + ';true', filler)
                        fillers.append(filler)
                    native.activate(browser, tree, inp, first[0])
                    native_key(inp, 'l', ('Control_L',))
                    wait_inline(browser, tree, 'Inline first')
                    dismiss_edit(tree, inp)
                    first_geometry = wait_inline(browser, tree, 'Inline first')
                    clip_samples = []
                    for step in range(8):
                        view = first_geometry['viewport']
                        inp.move(view['x'] + view['w'] / 2, view['y'] + view['h'] / 2)
                        wheel(inp, 1)
                        tree.read()
                        row_node = tab(tree, 'Inline first')
                        row = screen_rect(tree, row_node)
                        viewport = screen_rect(tree, ancestor(tree, row_node, r'class_name=ScrollView::Viewport '))
                        fields = native.visible(tree, FIELD)
                        if fields:
                            rect = screen_rect(tree, native.one(tree, FIELD))
                            assert native.inside(rect, row) and native.inside(rect, viewport), ('Scrolled overlay escaped native clip', rect, row, viewport)
                        clip_samples.append({'step': step, 'row': row, 'viewport': viewport,
                                             'visible_fields': len(fields)})
                    tree.read()
                    row = screen_rect(tree, tab(tree, 'Inline first'))
                    assert row['y'] < first_geometry['row']['y'], ('Native wheel did not scroll the tab list', row, first_geometry)
                    assert row['y'] + row['h'] <= first_geometry['viewport']['y'], ('Active row did not scroll fully out of view', row, first_geometry)
                    wait_for(lambda: not native.visible(tree.read(), FIELD), browser.process, timeout=5)
                    native.capture(tree, inp, output, 'active-row-scrolled-out')
                    # Regression: scrolling only the short title can leave its
                    # full row clipped. Ctrl+L must reveal the entire row and
                    # put the actual editor back inside that native viewport.
                    native_key(inp, 'l', ('Control_L',))
                    recovered = wait_inline(browser, tree, 'Inline first')
                    assert native.inside(recovered['row'], recovered['viewport']), recovered
                    native.type_text(inp, 'scrollrecovered')
                    assert editor_value(tree) == 'scrollrecovered'
                    native.capture(tree, inp, output, 'ctrl-l-restores-whole-row')
                    dismiss_edit(tree, inp)
                    (output / 'scroll-clip-samples.json').write_text(json.dumps(clip_samples, indent=2) + '\n')
                    # Return to the start through native wheel input before
                    # removing overflow rows. Native diagnostics can retain a
                    # stale scroll_y after a content-size clamp (observed90
                    # with max22 while pixels had already returned to0).
                    view = recovered['viewport']
                    inp.move(view['x'] + view['w'] / 2, view['y'] + view['h'] / 2)
                    wheel(inp, -8)
                    wait_inline(browser, tree, 'Inline first')
                    for filler in fillers:
                        request(browser.port, '/json/close/' + filler['id'])
                    time.sleep(.3)
                    native.retained(browser, [first, second])
                    result['checks'].append(side + ': native scrolling clips/hides the overlay; Ctrl+L reveals the entire active row and edits inline')

                    result['phase'] = side + '-close-editing'
                    click_tab(browser, tree, inp, second[0], 'Inline second')
                    click_tab(browser, tree, inp, first[0], 'Inline first')
                    native_key(inp, 'l', ('Control_L',))
                    native.type_text(inp, 'closingdraft')
                    assert editor_value(tree) == 'closingdraft'
                    native_key(inp, 'w', ('Control_L',))
                    wait_for(lambda: first[0]['id'] not in {t['id'] for t in request(browser.port, '/json/list')}, browser.process, timeout=6)
                    wait_for(lambda: browser.ev(native.JS['visible'], second[0]), browser.process, timeout=6)
                    wait_inline(browser, tree, 'Inline second')
                    assert_page_url(tree, inp, '/second')
                    dismiss_edit(tree, inp)
                    native.retained(browser, [second])
                    native.capture(tree, inp, output, 'close-focused-row-reanchors')
                    result['checks'].append(side + ': closing the focused address row preserves the native editor and reanchors it to the next retained tab')

                    result['phase'] = side + '-narrow-anchor'
                    dock = resize_sidebar(tree, inp, side, 34)
                    assert not native.visible(tree, FIELD)
                    search = native.one(tree, SEARCH)
                    search_rect = screen_rect(tree, search)
                    viewport_before = browser.ev('({w:innerWidth,h:innerHeight})', second[0])
                    inp.pointer.click(*tree.center(search))
                    wait_for(lambda: native.visible(tree.read(), EDITOR), browser.process, timeout=5)
                    field = screen_rect(tree, native.one(tree, FIELD))
                    client = screen_rect(tree, native.one(tree, r'class_name=BrowserView '))
                    assert native.inside(field, client) and field['w'] <= 280
                    if side == 'left':
                        assert abs(field['x'] - dock['x']) <= 1, (field, dock)
                    else:
                        assert abs(field['x'] + field['w'] - dock['x'] - dock['w']) <= 1, (field, dock)
                    assert abs(field['y'] + field['h'] / 2 - search_rect['y'] - search_rect['h'] / 2) <= 1, (field, search_rect)
                    native.type_text(inp, 'narrowdraft')
                    assert editor_value(tree) == 'narrowdraft'
                    assert browser.ev('({w:innerWidth,h:innerHeight})', second[0]) == viewport_before
                    native.capture(tree, inp, output, 'narrow-sidebar-anchored-editor')
                    dismiss_edit(tree, inp)
                    resize_sidebar(tree, inp, side, 232)
                    wait_inline(browser, tree, 'Inline second')
                    assert_page_url(tree, inp, '/second')
                    native.retained(browser, [second])
                    result['checks'].append(side + ': narrow Search anchors the same editor to its sidebar without resizing the page; widening restores inline placement')
                    result.setdefault('normal_shutdowns', []).append({'side': side, 'exit': close_normally(browser)})
        result['passed'] = True
        result['ui_acceptance'] = 'passed'
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
    parser.add_argument('--side', choices=('left', 'right', 'both'), default='both')
    parser.add_argument('--rtl', action='store_true')
    run(parser.parse_args())
