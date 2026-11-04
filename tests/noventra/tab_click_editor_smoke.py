"""Native active-tab URL editor regression on an owned private X11 display.

Fixture setup uses local documents, disposable profiles and the real Settings
API. Measured tab clicks, URL typing, navigation, dismissal, resizing and tab
gestures use native input. --case reproduce stops after the original symptom.
Use --normal-launcher to exercise the selected default without a runtime override.
"""

import ctypes as C
import http.server
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import threading
import time

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / 'tests/noventra').is_dir())
sys.path.insert(0, str(ROOT / 'tests/noventra/inline_footer'))
from sidebar_common import (
    Budget, EDITOR, FIELD, NEW_TAB, activate_settings, admit, ancestor, check,
    cli, native, native_key, prefs, rect, request, session, sha, wait_for, write,
)
from dock_controls_smoke import Handler
from sidebar_command_footer_smoke import extension_fixture
from sidebar_command_smoke import type_text
from sidebar_inline_address_smoke import make_page
from sidebar_tab_resting_smoke import editor_value
from sidebar_tab_resting_smoke import assert_painted_input
from sidebar_footer_smoke import popup_labels


def bounds(tree, node):
    """Match native clamped scrolling, including stale AX scroll attributes."""
    result = tree.rect(node)
    current = node
    while current:
        parent = tree.nodes.get(current['parent'])
        if parent and re.search(r'class_name=ScrollView ', parent['line']) and current is not node:
            identifier = int(re.match(r'^\+*id=(\d+)', current['line'])[1])
            contents = [n for n in tree.nodes.values() if n['parent'] == identifier]
            assert len(contents) == 1, ('Ambiguous native viewport', current, contents)
            for axis, dimension in [('x', 2), ('y', 3)]:
                match = re.search(r' scroll_' + axis + r'=(-?\d+)', parent['line'])
                if match:
                    maximum = max(0, contents[0]['bounds'][dimension] - current['bounds'][dimension])
                    result[axis] -= max(0, min(int(match[1]), maximum))
        current = parent
    return result


def box(tree, pattern):
    return bounds(tree, native.one(tree.read(), pattern))


def tab_node(tree, title, selected=False):
    nodes = native.visible(tree.read(), r'class_name=TabView name=' + re.escape(title) + r'(?: - Pinned)? name_from=')
    if selected:
        nodes = [node for node in nodes if 'selected=true' in node['line']]
    assert len(nodes) == 1, ('Expected one native tab', title, selected, nodes)
    return nodes[0]


def row(tree, title, selected=False):
    return bounds(tree, tab_node(tree, title, selected))


def center(value):
    return value['x'] + value['w'] / 2, value['y'] + value['h'] / 2


def click_tab(tree, inp, title, selected=False):
    value = row(tree, title, selected)
    # A hovered ordinary rail row puts Close over the icon. Its leading inset
    # remains the tab's own click target; expanded rows expose their title.
    point = (value['x'] + 2, center(value)[1]) if value['w'] <= 48 else center(value)
    inp.move(*point)
    node = tab_node(tree, title, selected)
    try:
        viewport = bounds(tree, ancestor(tree, node, r'class_name=ScrollView::Viewport '))
    except AssertionError:
        # Horizontal tab layouts can use a direct container without scrolling.
        viewport = box(tree, r'class_name=BrowserView ')
    assert native.inside(value, viewport), ('Tab hit target outside actual viewport', value, viewport)
    for child in native.visible(tree, r'class_name=(?:TabCloseButton|AlertIndicatorButton) '):
        try:
            owner = ancestor(tree, child, r'class_name=TabView ')
        except AssertionError:
            continue
        if owner == node:
            child_box = bounds(tree, child)
            assert not (child_box['x'] <= point[0] < child_box['x'] + child_box['w']
                        and child_box['y'] <= point[1] < child_box['y'] + child_box['h']), (point, child_box)
    inp.pointer.click(*point)


def opened(browser, tree):
    try:
        wait_for(lambda: len(native.visible(tree.read(), FIELD)) == 1,
                 browser.process, timeout=3)
        return True
    except TimeoutError:
        return False


def closed(browser, tree):
    wait_for(lambda: not native.visible(tree.read(), FIELD), browser.process, timeout=4)


def shows_url(tree, expected):
    # Native steady-state formatting may omit HTTP's scheme. Keep the complete
    # host, port and path exact; user-typed navigation is checked separately.
    value = editor_value(tree)
    return value == expected or (expected.startswith('http://') and value == expected[7:])


def page_focus(tree, inp):
    content = box(tree, r'class_name=MultiContentsView ')
    inp.pointer.click(content['x'] + content['w'] / 2,
                      content['y'] + content['h'] - 36)


def centered(tree, result, label):
    field = box(tree, FIELD)
    content = box(tree, r'class_name=MultiContentsView ')
    editor = box(tree, EDITOR)
    check(result, label + ': native editor centered inside page',
          native.inside(field, content) and native.inside(editor, field)
          and abs(center(field)[0] - center(content)[0]) <= 8
          and abs(center(field)[1] - center(content)[1]) <= 8
          and field['h'] == 48 and 300 < field['w'] <= 640,
          {'field': field, 'editor': editor, 'content': content})


def dock(tree):
    node = native.one(tree.read(), r'class_name=VerticalTabStripRegionView ')
    return bounds(tree, ancestor(tree, node, r'class_name=NoventraDockView '))


def resize(tree, inp, side, width):
    before = dock(tree)
    handle = box(tree, r'class_name=ResizeArea name=Vertical Tab Strip Resize Handle ')
    start = center(handle)
    inp.move(*start)
    inp.button(True)
    try:
        inp.move(start[0] + (width - before['w']) * (1 if side == 'left' else -1), start[1])
        time.sleep(.2)
    finally:
        inp.button(False)
    wait_for(lambda: dock(tree)['w'] == width, tree.browser.process, timeout=5)


def activate(browser, tree, inp, page):
    request(browser.port, '/json/activate/' + page['id'])
    wait_for(lambda: browser.ev(native.JS['visible'], page), browser.process)
    page_focus(tree, inp)


def native_button(inp, button, point, modifiers=()):
    pointer = inp.pointer
    pointer.x.XStringToKeysym.argtypes = [C.c_char_p]
    pointer.x.XStringToKeysym.restype = C.c_ulong
    pointer.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
    pointer.x.XKeysymToKeycode.restype = C.c_uint
    pointer.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    codes = [pointer.x.XKeysymToKeycode(pointer.d, pointer.x.XStringToKeysym(key.encode()))
             for key in modifiers]
    assert all(codes)
    inp.move(*point)
    for code in codes:
        pointer.xt.XTestFakeKeyEvent(pointer.d, code, 1, 0)
    try:
        pointer.xt.XTestFakeButtonEvent(pointer.d, button, 1, 0)
        pointer.xt.XTestFakeButtonEvent(pointer.d, button, 0, 0)
    finally:
        for code in reversed(codes):
            pointer.xt.XTestFakeKeyEvent(pointer.d, code, 0, 0)
        pointer.x.XFlush(pointer.d)


def original_flow(browser, tree, inp, result, one, two):
    activate(browser, tree, inp, one[0])
    native_key(inp, 'l', ('Control_L',))
    check(result, 'existing Ctrl+L still opens the native editor', opened(browser, tree))
    centered(tree, result, 'Ctrl+L')
    native_key(inp, 'Escape')
    closed(browser, tree)
    click_tab(tree, inp, TWO)
    wait_for(lambda: browser.ev(native.JS['visible'], two[0]), browser.process)
    check(result, 'inactive click selects the page without opening its URL',
          not native.visible(tree.read(), FIELD))
    click_tab(tree, inp, TWO)
    check(result, 'active tab click opens centered URL editor', opened(browser, tree))
    centered(tree, result, 'active click')
    check(result, 'active click shows the current editable URL', shows_url(tree, two[1]['url']), editor_value(tree))


ONE = 'Editor first page'
TWO = 'Editor second page'
PIN = 'Editor pinned page'


def main():
    parser = cli(__doc__)
    parser.add_argument('--case', choices=['reproduce', 'editing', 'gestures'], default='editing')
    parser.add_argument('--normal-launcher', action='store_true')
    args = parser.parse_args()
    result = admit(args, Path(__file__))
    budget = Budget()
    expected = args.browser
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    old_env = {key: os.environ.get(key) for key in ['NOVENTRA_PROFILE', 'NOVENTRA_RUNTIME']}
    try:
        with tempfile.TemporaryDirectory(prefix='owned-editor-profile-', dir=args.output) as tmp:
            profile = Path(tmp).resolve()
            prefs(profile)
            extension = extension_fixture(profile)
            if args.normal_launcher:
                os.environ['NOVENTRA_PROFILE'] = str(profile)
                os.environ.pop('NOVENTRA_RUNTIME', None)
                args.browser = ROOT / 'run-noventra.sh'
            with session(args, profile, args.output / 'native', result, budget,
                         (*native.FLAGS, '--load-extension=' + str(extension.resolve()))) as (browser, tree, inp):
                activate_settings(browser)
                actual = Path('/proc', str(browser.process.pid), 'exe').resolve()
                check(result, 'running executable and resources match qualified candidate',
                      actual == expected and sha(actual) == args.expected_binary
                      and sha(actual.parent / 'resources.pak') == args.expected_resources, str(actual))
                # Keep an extension page alive before the optional service
                # worker goes idle during the longer native editing matrix.
                controller(browser)
                one = make_page(browser, base + '/first', ONE)
                two = make_page(browser, base + '/second', TWO)
                original_flow(browser, tree, inp, result, one, two)
                inp.move(1000, 700)
                native.capture(tree, inp, args.output / 'native', 'active-click-centered-editor')
                if args.case != 'reproduce':
                    exercise(browser, tree, inp, result, args, base, one, two)
            check(result, 'owned browser closes normally', all(v['exit'] == 0 for v in result['normal_shutdowns']))
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        for key, value in old_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        server.shutdown()
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        write(args.output / 'result.json', result)


def exercise(browser, tree, inp, result, args, base, one, two):
    native_key(inp, 'Escape')
    closed(browser, tree)
    if args.case == 'gestures':
        exercise_gestures(browser, tree, inp, result, args, base, one, two)
        return

    for side in (('left',) if args.normal_launcher else ('left', 'right')):
        if not args.normal_launcher:
            native.preset(browser, 'minimal-' + side)
        activate(browser, tree, inp, two[0])
        for width in [144, 320, 48]:
            label = side + '-' + str(width)
            resize(tree, inp, side, width)
            check(result, label + ': no resting address row', not native.visible(tree.read(), FIELD))
            click_tab(tree, inp, TWO)
            check(result, label + ': active click opens editor', opened(browser, tree))
            centered(tree, result, label)
            check(result, label + ': editor contains current URL', shows_url(tree, two[1]['url']), editor_value(tree))
            type_text(inp, 'draftquery')
            wait_for(lambda: editor_value(tree) == 'draftquery', browser.process)
            check(result, label + ': native typing edits URL and opens suggestions',
                  bool(native.visible(tree.read(), r'class_name=OmniboxPopupViewViews ')))
            if side == 'left' and width == 144:
                inp.move(1000, 700)
                native.capture(tree, inp, args.output / 'native', 'typed-centered-editor')
                assert_painted_input(args.output / 'native/typed-centered-editor.png', box(tree, EDITOR), 'draftquery')
                check(result, label + ': native edited text paints readably', True)
            native_key(inp, 'Escape')
            closed(browser, tree)
            check(result, label + ': Escape cancels editing without navigating',
                  browser.ev(native.JS['draft'], two[0]) == two[1])
            click_tab(tree, inp, TWO)
            check(result, label + ': active click reopens original URL', opened(browser, tree)
                  and shows_url(tree, two[1]['url']))
            page_focus(tree, inp)
            closed(browser, tree)
            check(result, label + ': outside page click dismisses editor',
                  browser.ev(native.JS['draft'], two[0]) == two[1])
            if width == 48:
                inp.move(1000, 700)
                native.capture(tree, inp, args.output / 'native', side + '-rail-after-cancel')
        resize(tree, inp, side, 144)
        click_tab(tree, inp, TWO)
        check(result, side + ': active click can commit a different URL', opened(browser, tree))
        new_url = base + '/edited-' + side
        type_text(inp, new_url)
        check(result, side + ': native URL edit is exact before Return', editor_value(tree) == new_url)
        native_key(inp, 'Return')
        wait_for(lambda: browser.ev('location.href', two[0]) == new_url, browser.process)
        closed(browser, tree)
        check(result, side + ': Return navigates the same tab and closes editor', True)
        # The committed navigation intentionally replaces this document. Prepare
        # its next witness only after verifying the actual native navigation.
        wait_for(lambda: browser.ev('document.readyState === "complete"', two[0]), browser.process)
        browser.ev(native.JS['page'].replace('TITLE', json.dumps(TWO)), two[0])
        two = (two[0], browser.ev(native.JS['draft'], two[0]))
        native.retained(browser, [one, two])
        check(result, side + ': other loaded page and unsaved draft survive editing', True)

    pinned = make_page(browser, base + '/pinned', PIN)
    api = controller(browser)
    api('chrome.tabs.query({}).then(tabs=>chrome.tabs.update(tabs.find(t=>t.title===' + json.dumps(PIN) + ').id,{pinned:true}))')
    activate(browser, tree, inp, two[0])
    click_tab(tree, inp, PIN)
    wait_for(lambda: browser.ev(native.JS['visible'], pinned[0]), browser.process)
    check(result, 'inactive pinned icon selects without opening editor', not native.visible(tree.read(), FIELD))
    click_tab(tree, inp, PIN)
    check(result, 'active pinned icon opens its current URL', opened(browser, tree)
          and shows_url(tree, pinned[1]['url']))
    centered(tree, result, 'active pin')
    native_key(inp, 'Escape')
    closed(browser, tree)
    native.retained(browser, [one, two, pinned])
    check(result, 'pin editing preserves all local documents and unsaved drafts', True)

    before = {target['id'] for target in request(browser.port, '/json/list') if target['type'] == 'page'}
    plus = box(tree, NEW_TAB)
    inp.pointer.click(*center(plus))
    fresh = wait_for(lambda: next((target for target in request(browser.port, '/json/list')
                    if target['type'] == 'page' and target['id'] not in before
                    and target['url'].startswith(('chrome://newtab', 'chrome://new-tab-page'))), None), browser.process)
    wait_for(lambda: browser.ev('document.readyState === "complete"', fresh), browser.process)
    check(result, 'bottom New Tab still opens homepage without URL overlay', not native.visible(tree.read(), FIELD))
    wait_for(lambda: browser.ev(native.JS['visible'], fresh), browser.process)
    click_tab(tree, inp, 'New Tab', selected=True)
    check(result, 'active new tab opens native URL editor', opened(browser, tree))
    centered(tree, result, 'active new tab')
    native_key(inp, 'Escape')
    closed(browser, tree)


def controller(browser):
    if hasattr(browser, 'tab_editor_api'):
        return browser.tab_editor_api
    worker = wait_for(lambda: next((target for target in request(browser.port, '/json/list')
                      if target['type'] == 'service_worker' and target['url'].endswith('/worker.js')), None), browser.process)
    page = request(browser.port, '/json/new?' + worker['url'].rsplit('/', 1)[0] + '/control.html', 'PUT')
    browser.tab_editor_api = lambda expression: browser.ev('(async()=>{const value=await(' + expression + ');return value??null})()', page)
    return browser.tab_editor_api


def exercise_gestures(browser, tree, inp, result, args, base, one, two):
    api = controller(browser)
    activate(browser, tree, inp, two[0])
    for modifiers in [('Control_L',), ('Shift_L',), ('Alt_L',)]:
        native_button(inp, 1, center(row(tree, TWO)), modifiers)
        time.sleep(.15)
        check(result, '+'.join(modifiers) + ' click retains native tab gesture without editor',
              not native.visible(tree.read(), FIELD))
        activate(browser, tree, inp, two[0])
    native_button(inp, 3, center(row(tree, TWO)))
    # Separate native menu widgets are absent from this inspector's tree.
    # Read their real pixels with the existing popup OCR helper instead.
    time.sleep(.2)
    inp.capture(args.output / 'native/tab-context-menu.png').close()
    labels = popup_labels(args.output / 'native', 'tab-context-menu', ['Split with tab', 'Close other tabs'])
    check(result, 'right click opens native tab context menu without editor',
          bool(labels) and not native.visible(tree.read(), FIELD), labels)
    native_key(inp, 'Escape')
    closed(browser, tree)

    # A plain inactive click clears any selection left by the modifier cases.
    click_tab(tree, inp, ONE)
    wait_for(lambda: browser.ev(native.JS['visible'], one[0]), browser.process)
    click_tab(tree, inp, TWO)
    wait_for(lambda: browser.ev(native.JS['visible'], two[0]), browser.process)

    def model_order():
        return api('chrome.tabs.query({currentWindow:true}).then(tabs=>tabs.map(t=>t.title).filter(t=>' + json.dumps([ONE, TWO]) + '.includes(t)))')

    activate(browser, tree, inp, two[0])
    before = model_order()
    check(result, 'real tab model starts in fixture order', before == [ONE, TWO], before)
    source, target = row(tree, TWO), row(tree, ONE)
    start = center(source)
    end = (target['x'] + target['w'] / 2, target['y'] + 3)
    inp.drag(start, end)
    after = model_order()
    check(result, 'dragging active title actually reorders native tab model', after == [TWO, ONE], after)
    check(result, 'drag release does not open URL editor', not native.visible(tree.read(), FIELD))
    native.retained(browser, [one, two])
    check(result, 'native reorder preserves both documents and drafts', True)

    source = row(tree, TWO)
    start = center(source)
    inp.move(*start)
    inp.button(True)
    try:
        inp.move(start[0], start[1] + 12)
        time.sleep(.15)
        native_key(inp, 'Escape')
    finally:
        inp.button(False)
    time.sleep(.2)
    check(result, 'Escape cancels a title drag without editor or reorder',
          model_order() == after and not native.visible(tree.read(), FIELD))
    click_tab(tree, inp, TWO)
    check(result, 'active click still edits after drag cancellation', opened(browser, tree))
    native_key(inp, 'Escape')
    closed(browser, tree)

    closing = make_page(browser, base + '/close-button', 'Editor close button')
    activate(browser, tree, inp, closing[0])
    node = native.one(tree.read(), r'class_name=TabView name=Editor close button name_from=')
    candidates = []
    for child in native.visible(tree, r'class_name=TabCloseButton '):
        try:
            parent = ancestor(tree, child, r'class_name=TabView ')
            if parent == node:
                candidates.append(child)
        except AssertionError:
            pass
    assert len(candidates) == 1, candidates
    inp.pointer.click(*center(bounds(tree, candidates[0])))
    wait_for(lambda: closing[0]['id'] not in {target['id'] for target in request(browser.port, '/json/list')}, browser.process)
    check(result, 'close button closes active tab without URL editor', not native.visible(tree.read(), FIELD))
    middle = make_page(browser, base + '/middle-close', 'Editor middle close')
    activate(browser, tree, inp, middle[0])
    native_button(inp, 2, center(row(tree, 'Editor middle close')))
    wait_for(lambda: middle[0]['id'] not in {target['id'] for target in request(browser.port, '/json/list')}, browser.process)
    check(result, 'middle click closes active tab without URL editor', not native.visible(tree.read(), FIELD))
    native.retained(browser, [one, two])
    check(result, 'closing other tabs preserves original pages and drafts', True)

    # Horizontal tab behavior stays separate from the sidebar editor trigger.
    native.preset(browser, 'classic-top')
    activate(browser, tree, inp, two[0])
    # Classic horizontal chrome uses Chromium's Tab, rather than TabView.
    horizontal = box(tree, r'class_name=Tab name=' + re.escape(TWO) + r' name_from=')
    inp.pointer.click(*center(horizontal))
    check(result, 'horizontal active tab does not invoke centered sidebar popup',
          not any(box(tree, FIELD)['h'] == 48 for _ in native.visible(tree.read(), FIELD)))


if __name__ == '__main__':
    main()
