"""Bounded native Spaces footer regression; use an owned Xvfb/Sway display.

Example (a NEW output directory, Node, Pillow and Tesseract required):
  python3 work/ram-investigation/with-xvfb.py python3 \
    tests/noventra/sidebar_footer_smoke.py --browser /absolute/candidate \
    --output work/checks/sidebar-footer-new
Replace the wrapper with tests/noventra/with_private_sway.py for Wayland.
The default 180-second work budget excludes bounded diagnostics/owned cleanup.
Setup uses Settings APIs; measured space switches and dialogs use native input.
No existing profile, build, source patch or desktop session is modified.
"""
import argparse
from collections import Counter
from copy import deepcopy
import csv
import io
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

from PIL import Image, ImageChops

from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from interface_polish_smoke import native_key as x11_native_key
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree


LONG_NAME = '🧭 研究 — café e\u0301 — Дълго работно пространство за проекти'
COLORS = {'Footer Alpha': (230, 30, 90), 'Footer Bravo': (20, 190, 70)}
CONTROLS = ('back', 'forward', 'reload', 'new-tab', 'action:settings')


class FooterInput(Input):
    """Keep one Wayland keyboard through popup interaction.

    Creating/removing a virtual keyboard for each key replaces the seat keymap
    and can dismiss Chromium's native menu before the key reaches it.
    """
    def __init__(self, platform):
        super().__init__(platform)
        self.keyboard = None
        try:
            if platform == 'wayland':
                self.keyboard = subprocess.Popen([os.environ['NOVENTRA_TEST_KEYBOARD']],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                assert self.keyboard.stdout.readline().strip() == 'ready'
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            if self.keyboard:
                self.keyboard.stdin.close()
                try:
                    self.keyboard.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.keyboard.kill()
                    self.keyboard.wait()
        finally:
            super().close()


def native_key(inp, key, modifiers=()):
    if inp.platform != 'wayland':
        return x11_native_key(inp, key, modifiers)
    codes = {'Home': 102, 'Return': 28, 'Escape': 1, 'ctrl': 29,
             'a': 30, 'c': 46, 'd': 32, 'e': 18, 'i': 23, 'r': 19, 's': 31}
    def send(name, down):
        inp.keyboard.stdin.write(f'key {codes[name]} {down}\n')
        inp.keyboard.stdin.flush()
        assert inp.keyboard.stdout.readline().strip() == 'ok'
    for modifier in modifiers:
        send(modifier, 1)
    send(key, 1)
    send(key, 0)
    for modifier in reversed(modifiers):
        send(modifier, 0)
    time.sleep(.2)


def visible(tree, pattern):
    return [node for node in tree.nodes.values()
            if ' INVISIBLE ' not in node['line'] and re.search(pattern, node['line'])
            and node['bounds'][2] > 0 and node['bounds'][3] > 0]


def one(tree, pattern):
    nodes = visible(tree, pattern)
    assert len(nodes) == 1, ('Expected one visible native control', pattern, nodes)
    return nodes[0]


def identity(tree, node):
    return next(key for key, value in tree.nodes.items() if value is node)


def inside(rect, outer):
    return (rect['x'] >= outer['x'] and rect['y'] >= outer['y'] and
            rect['x'] + rect['w'] <= outer['x'] + outer['w'] and
            rect['y'] + rect['h'] <= outer['y'] + outer['h'])


def overlaps(a, b):
    return (min(a['x'] + a['w'], b['x'] + b['w']) > max(a['x'], b['x']) and
            min(a['y'] + a['h'], b['y'] + b['h']) > max(a['y'], b['y']))


def capture(tree, inp, output, name):
    tree.read()
    (output / f'{name}.txt').write_text(tree.text)
    image = inp.capture(output / f'{name}.png')
    image.close()


def apply_layout(browser, side, *, horizontal=False, extensions=False, hidden=False,
                 saved_order=()):
    layout = browser.state()['layout']
    # By default deliberately save Spaces BEFORE tabs and all other controls.
    order = saved_order or ['spaces', *CONTROLS, 'extensions', 'tabs']
    layout['items'].sort(key=lambda item: order.index(item['id']) if item['id'] in order else 99)
    for index, item in enumerate(layout['items']):
        item.update(order=index, edge=side,
                    visible=item['id'] in ('spaces', 'tabs', *CONTROLS))
        if item['id'] == 'tabs' and horizontal:
            item['edge'] = 'top'
        if item['id'] == 'extensions':
            item['visible'] = extensions
        if item['id'] == 'spaces' and hidden:
            item['visible'] = False
    response = browser.apply(layout)
    assert response['status'] == 'ok', response
    time.sleep(.35)
    if saved_order:
        stored = sorted(browser.state()['layout']['items'], key=lambda item: item['order'])
        assert [item['id'] for item in stored[:len(saved_order)]] == list(saved_order), (
            'Saved-order fixture was normalized away', saved_order, stored)


def footer_sibling_order(tree, footer):
    # requestNativeUITree recursively emits ChildAtIndex order; NativeUITree
    # preserves that order. Include hidden/zero-sized siblings as well.
    # This checks native tree traversal, not actual keyboard focus traversal:
    # the diagnostic API does not expose the focused View or focus actions.
    children = [key for key, node in tree.nodes.items()
                if node['parent'] == footer['parent']]
    assert children and children[-1] == identity(tree, footer), (
        'Spaces must follow every other native dock child',
        [tree.nodes[key]['line'] for key in children])
    return children


def footer_geometry(tree, side, name, *, compact=False, horizontal=False):
    footer = one(tree, r'class_name=AccountSpacesButton ')
    footer_id = identity(tree, footer)
    dock = tree.nodes[footer['parent']]
    rect, bounds = tree.rect(footer), tree.rect(dock)
    browser = tree.rect(one(tree, r'class_name=BrowserView '))
    assert 'class_name=NoventraDockView ' in dock['line'], dock
    tablist = one(tree, r'tabList .*class_name=View name=Workspaces name_from=')
    assert tablist['parent'] == footer_id, tablist
    dots = [node for node in visible(tree, r'class_name=AccountSpaceDotButton name=Workspace: ')
            if node['parent'] == identity(tree, tablist)]
    assert dots, 'Spaces footer has no direct workspace dots'
    expected_height = 16 + 24 * (len(dots) + 1) if rect['w'] < 124 else 40
    assert rect['h'] == expected_height and rect['y'] + rect['h'] == bounds['y'] + bounds['h'], (
        'Spaces dots are not a bottom-anchored footer of the expected height', rect, bounds, expected_height)
    assert inside(rect, bounds) and inside(bounds, browser), (rect, bounds, browser)
    assert bounds['y'] + bounds['h'] == browser['y'] + browser['h'], (
        'Sidebar footer does not reach the bottom of the browser', bounds, browser)
    if side == 'left':
        assert bounds['x'] == browser['x'], (side, bounds, browser)
    else:
        assert bounds['x'] + bounds['w'] == browser['x'] + browser['w'], (side, bounds, browser)
    siblings = [node for node in visible(tree, 'class_name=')
                if node['parent'] == footer['parent'] and node is not footer]
    for sibling in siblings:
        peer = tree.rect(sibling)
        assert inside(peer, bounds) and not overlaps(rect, peer), ('Footer overlap/clipping', rect, peer, sibling)
    tabs = visible(tree, r'class_name=VerticalTabStripRegionView ')
    assert bool([node for node in tabs if node['parent'] == footer['parent']]) != horizontal, (
        'Wrong tabs arrangement for this case', horizontal, tabs)
    selector = one(tree, r'class_name=AccountSpaceDotButton name=Account spaces: ' + re.escape(name) + r' name_from=')
    assert selector['parent'] == footer_id and 'haspopup=menu' in selector['line'], selector
    buttons = dots + [selector]
    selected = [dot for dot in dots if 'selected=true' in dot['line']]
    assert len(selected) == 1 and ('name=Workspace: ' + name + ' name_from=') in selected[0]['line'], selected
    if compact:
        assert rect['w'] < 124 and len(dots) <= 4, (rect, dots)
    assert not [node for node in visible(tree, r'class_name=MdTextButton ')
                if node['parent'] == footer_id], 'Legacy text selector/plus still visible'
    assert not visible(tree, r'name=Browsing spaces name_from='), 'Legacy space chips still visible'
    for button in buttons:
        hit = tree.rect(button)
        assert hit['w'] == hit['h'] == 24 and inside(hit, rect), ('Footer button clipped or wrongly sized', button, rect)
        if button is not selector:
            assert re.search(r'\btab\b', button['line']) and inside(hit, tree.rect(tablist)), button
        for peer in buttons:
            if peer is not button:
                assert not overlaps(hit, tree.rect(peer)), ('Footer controls overlap', button, peer)
    return {'footer': rect, 'dock': bounds, 'selector_id': identity(tree, selector),
            'native_child_order': footer_sibling_order(tree, footer)}


def draft(browser, target):
    return browser.ev('({token:window.footerToken,origin:performance.timeOrigin,'
                      'draft:document.querySelector("input")?.value,visible:document.visibilityState})', target)


def make_page(browser, token):
    page = request(browser.port, '/json/new?about:blank', 'PUT')
    browser.ev('document.title=' + json.dumps(token) + ';window.footerToken=' + json.dumps(token) +
               ';document.body.innerHTML="<input id=draft>";document.querySelector("input").value=' +
               json.dumps(token + ' — unsaved') + ';true', page)
    return page, draft(browser, page)


def retained(browser, page, before, active):
    after = draft(browser, page)
    expected = dict(before, visible='visible' if active else 'hidden')
    assert after == expected, ('Native switch selected the wrong target or lost a document/draft', page['id'], expected, after)


def popup_labels(output, name, labels, *, required=True):
    """Read ASCII labels from owned captures of separate native widgets.

    Unicode is asserted on the footer's accessible name, not by English OCR.
    Retain the original capture, enlarged input, raw TSV and OCR diagnostics.
    """
    ocr = output / f'{name}-ocr.png'
    with Image.open(output / f'{name}.png') as shot:
        with shot.resize((shot.width * 2, shot.height * 2), Image.Resampling.LANCZOS) as enlarged:
            enlarged.save(ocr)
    found = {}
    for psm in ('11', '6'):
        run = subprocess.run(['tesseract', str(ocr), 'stdout', '--psm', psm, 'tsv'],
                             capture_output=True, text=True, timeout=12)
        (output / f'{name}-{psm}.tsv').write_text(run.stdout)
        (output / f'{name}-{psm}.stderr').write_text(run.stderr)
        run.check_returncode()
        lines = {}
        for word in csv.DictReader(io.StringIO(run.stdout), delimiter='\t', quoting=csv.QUOTE_NONE):
            if word['text'].strip() and float(word['conf']) >= 40:
                key = tuple(word[k] for k in ('block_num', 'par_num', 'line_num'))
                lines.setdefault(key, []).append(word)
        for words in lines.values():
            # A focused text field's caret can be OCR'd as a trailing pipe.
            # Strip punctuation only at token edges; require the whole label.
            tokens = [word['text'].casefold().strip('.,…:|') for word in words]
            for label in labels:
                assert label.isascii(), ('OCR only verifies ASCII labels', label)
                wanted = label.casefold().split()
                for start in range(len(tokens) - len(wanted) + 1):
                    if tokens[start:start + len(wanted)] != wanted:
                        continue
                    matched = words[start:start + len(wanted)]
                    left = min(int(word['left']) for word in matched)
                    top = min(int(word['top']) for word in matched)
                    right = max(int(word['left']) + int(word['width']) for word in matched)
                    bottom = max(int(word['top']) + int(word['height']) for word in matched)
                    found[label] = ((left + right) / 4, (top + bottom) / 4)
        if set(found) == set(labels):
            break
    if required:
        assert set(found) == set(labels), ('Native popup labels missing', name, set(labels) - set(found), str(ocr))
    return found


def open_menu(tree, inp, output, name, names):
    selector = one(tree.read(), r'class_name=AccountSpaceDotButton name=Account spaces: ')
    inp.pointer.click(*tree.center(selector))
    capture(tree, inp, output, name)
    return popup_labels(output, name, [*[label for label in names if label.isascii()],
        'New account space', 'Rename current space', 'Manage spaces in Settings'])


def interactions(browser, tree, inp, output, result, pages, names):
    personal, work = pages
    retained(browser, *personal, active=True)
    retained(browser, *work, active=False)
    initial_id = identity(tree.read(), one(tree, r'class_name=AccountSpaceDotButton name=Account spaces: '))
    for label, selected, hidden in [('Work fixture', work, personal), (LONG_NAME, personal, work)]:
        positions = open_menu(tree, inp, output, 'menu-' + ('work' if selected == work else 'personal'), names)
        if label == LONG_NAME:
            # Native ShowMenu always places the default (empty-ID) space first.
            # Home selects that row without pretending OCR reads the Unicode.
            native_key(inp, 'Home')
            native_key(inp, 'Return')
        else:
            inp.pointer.click(*positions[label])
        # Never activate a CDP target after the measured native click.
        wait_for(lambda: browser.ev('document.visibilityState==="visible"', selected[0]),
                 browser.process, timeout=5)
        retained(browser, *selected, active=True)
        retained(browser, *hidden, active=False)
        result.setdefault('switches', []).append({'space': label,
            'selected_target': selected[0]['id'], 'selected': draft(browser, selected[0]),
            'hidden_target': hidden[0]['id'], 'hidden': draft(browser, hidden[0])})
        capture(tree, inp, output, 'switched-' + ('work' if selected == work else 'personal'))
        selector = one(tree, r'class_name=AccountSpaceDotButton name=Account spaces: ' + re.escape(label) + r' name_from=')
        assert identity(tree, selector) == initial_id, 'Space switching recreated the selector'
    before_spaces = browser.call('noventraSpaces', 'list', '', '')
    result['cancel_expected_preferences'] = {space['id']: space['name'] for space in before_spaces}
    before_targets = {page['id'] for page in request(browser.port, '/json/list') if page['type'] == 'page'}
    # New remains available through More; Cancel must not create anything.
    for route in ('menu',):
        positions = open_menu(tree, inp, output, 'menu-new', names)
        inp.pointer.click(*positions['New account space'])
        capture(tree, inp, output, 'new-dialog-' + route)
        popup_labels(output, 'new-dialog-' + route, ['New account space', 'Cancel'])
        # Naming dialogs are owned widgets in the native tree, unlike menus.
        # Use their accessible field/value instead of OCR beside its border.
        one(tree, r'class_name=Textfield name=Name name_from=')
        native_key(inp, 'a', ('ctrl' if inp.platform == 'wayland' else 'Control_L',))
        for key in 'discarded':
            native_key(inp, key)
        capture(tree, inp, output, 'edited-dialog-' + route)
        field = one(tree, r'class_name=Textfield name=Name name_from=')
        assert re.search(r' value=discarded(?: |$)', field['line']), field
        positions = popup_labels(output, 'edited-dialog-' + route, ['Cancel'])
        inp.pointer.click(*positions['Cancel'])
        inp.move(600, 300)
        capture(tree, inp, output, 'cancelled-' + route)
        assert not popup_labels(output, 'cancelled-' + route, ['Cancel'], required=False), 'Cancel left the naming dialog open'
        assert browser.call('noventraSpaces', 'list', '', '') == before_spaces
        assert {p['id'] for p in request(browser.port, '/json/list') if p['type'] == 'page'} == before_targets
        retained(browser, *personal, active=True)
        retained(browser, *work, active=False)
    result['checks'].append('Native More menu OCR verifies every ASCII fixture and New/Rename/Manage labels; mouse/keyboard switches retain both documents and drafts; New cancels an edited name without live mutation (Unicode verified in footer accessibility only)')


def extension_fixture(base):
    paths = []
    for name, color in COLORS.items():
        path = base / name.replace(' ', '-')
        path.mkdir()
        Image.new('RGB', (16, 16), color).save(path / 'icon.png')
        (path / 'manifest.json').write_text(json.dumps({'manifest_version': 3, 'name': name,
            'version': '1.0', 'action': {'default_title': name, 'default_icon': {'16': 'icon.png'}}}))
        paths.append(path)
    return '--load-extension=' + ','.join(map(str, paths))


def pin_extensions(browser):
    manager = request(browser.port, '/json/new?chrome://extensions', 'PUT')
    try:
        wait_for(lambda: browser.ev('!!chrome.developerPrivate', manager), browser.process, timeout=5)
        names = browser.ev('(async()=>{const all=await chrome.developerPrivate.getExtensionsInfo({includeDisabled:true,includeTerminated:true});'
            'const names=' + json.dumps(list(COLORS)) + ';for(const e of all)await chrome.developerPrivate.updateExtensionConfiguration('
            '{extensionId:e.id,pinnedToToolbar:names.includes(e.name)});return all.filter(e=>names.includes(e.name)).map(e=>e.name);})()', manager)
        assert set(names) == set(COLORS), names
    finally:
        request(browser.port, '/json/close/' + manager['id'])


def toolbar_geometry(tree, inp, output, side):
    row = tree.rect(one(tree, r'class_name=ExtensionsToolbarDesktop '))
    assert row['h'] == 40, ('Extension region is not a 40px row', row)
    actions = visible(tree, r'class_name=(ToolbarActionView|ExtensionsToolbarButton) ')
    assert len(actions) >= 3, actions
    for node in actions:
        rect = tree.rect(node)
        assert rect['w'] == rect['h'] == 28 and inside(rect, row), (rect, row)
        assert abs(rect['y'] + rect['h'] / 2 - (row['y'] + 20)) <= 1, ('Extension action is not vertically centered', rect, row)
    inp.move(600, 300)
    time.sleep(.25)
    with inp.capture(output / f'{side}-icons.png') as image:
        for name, color in COLORS.items():
            rect = tree.rect(one(tree, r'class_name=ToolbarActionView name=' + re.escape(name) + r'(?: name_from=| \()'))
            points = [(x, y) for y in range(rect['y'], rect['y'] + rect['h'])
                      for x in range(rect['x'], rect['x'] + rect['w'])
                      if max(abs(a - b) for a, b in zip(image.getpixel((x, y)), color)) < 15]
            assert len(points) >= 200, ('Fixture icon clipped/missing', name, len(points))
            center = [(min(p[axis] for p in points) + max(p[axis] for p in points) + 1) / 2 for axis in (0, 1)]
            assert abs(center[0] - (rect['x'] + 14)) <= 1 and abs(center[1] - (row['y'] + 20)) <= 1, (name, center, rect, row)
    for label, pattern in [('reload', r'class_name=ReloadButton name=Reload '),
                           ('settings', r'class_name=PinnedActionToolbarButton name=Settings ')]:
        node = one(tree, pattern)
        rect = tree.rect(node)
        assert rect['w'] == rect['h'] == 40, ('Native toolbar hit target is not 40x40', rect)
        box = (rect['x'], rect['y'], rect['x'] + rect['w'], rect['y'] + rect['h'])
        inp.move(600, 300)
        time.sleep(.25)
        with inp.capture(output / f'{side}-{label}-idle.png') as shot:
            idle = shot.crop(box)
        # These two controls have only a glyph on a flat idle background.
        # Bounding the high-contrast glyph catches padding/centering regressions
        # without depending on the theme's particular foreground color.
        background = Counter(idle.getdata()).most_common(1)[0][0]
        glyph = [(x, y) for y in range(40) for x in range(40)
                 if max(abs(a - b) for a, b in zip(idle.getpixel((x, y)), background)) >= 50]
        assert len(glyph) >= 12, ('Toolbar icon is missing', label)
        center = [(min(p[axis] for p in glyph) + max(p[axis] for p in glyph) + 1) / 2 for axis in (0, 1)]
        assert all(abs(value - 20) <= 2 for value in center), ('Toolbar glyph is off-center', label, center)
        inp.move(*tree.center(node))
        time.sleep(.2)
        with inp.capture(output / f'{side}-{label}-hover.png') as shot:
            hover = shot.crop(box)
        difference = ImageChops.difference(idle, hover)
        changed = sum(max(pixel) >= 8 for pixel in difference.getdata())
        assert changed >= 20, ('Native toolbar hover did not paint inside its hit target', label, changed)
        idle.close(); hover.close(); difference.close()
    inp.move(600, 300)


def failure_diagnostics(browser, tree, inp, output, result):
    # Called INSIDE LayoutBrowser, before input/browser/profile teardown.
    for name, action in [('tree', lambda: (output / 'failure.txt').write_text(tree.read().text) if tree else None),
                         ('screenshot', lambda: inp.capture(output / 'failure.png').close()),
                         ('targets', lambda: result.update(failure_targets=request(browser.port, '/json/list')))]:
        try:
            action()
        except Exception as error:
            result.setdefault('diagnostic_errors', {})[name] = repr(error)


def exercise(args, result):
    with tempfile.TemporaryDirectory(prefix='profile-', dir=args.output) as temp:
        base = Path(temp)
        extensions = args.case in ('all', 'extensions')
        flags = ['--force-renderer-accessibility']
        if extensions:
            flags.append(extension_fixture(base))
        with LayoutBrowser(args.browser, base / 'profile', args.output, flags) as browser:
            inp = FooterInput(browser.platform)
            tree = None
            try:
                tree = NativeUITree(browser)
                for name in ('Work fixture', 'Studio A', 'Studio B', 'Studio C'):
                    spaces = browser.call('noventraSpaces', 'create', '', name)
                browser.call('noventraSpaces', 'rename', '', LONG_NAME)
                work_id = next(space['id'] for space in spaces if space['name'] == 'Work fixture')
                browser.call('noventraSpaces', 'switch', work_id, '')
                work = make_page(browser, 'work-document')
                browser.call('noventraSpaces', 'switch', '', '')
                personal = make_page(browser, 'personal-document')
                names = [space['name'] for space in browser.call('noventraSpaces', 'list', '', '')]
                if extensions:
                    pin_extensions(browser)
                request(browser.port, '/json/activate/' + personal[0]['id'])  # Setup only.
                # A newly activated blank page can leave the address editor over
                # native toolbar controls. Dismiss it through actual page input.
                inp.pointer.click(600, 300)
                for side in ('left', 'right'):
                    result['phase'] = side + '-expanded'
                    apply_layout(browser, side, extensions=extensions)
                    capture(tree, inp, args.output, result['phase'])
                    geometry = footer_geometry(tree, side, LONG_NAME)
                    result.setdefault('geometry', {})[result['phase']] = geometry
                    if extensions:
                        toolbar_geometry(tree, inp, args.output, side)
                    if args.case in ('all', 'layout'):
                        result['phase'] = side + '-horizontal-tabs'
                        apply_layout(browser, side, horizontal=True)
                        capture(tree, inp, args.output, result['phase'])
                        result['geometry'][result['phase']] = footer_geometry(tree, side, LONG_NAME, horizontal=True)
                apply_layout(browser, 'left', extensions=extensions)
                if args.case in ('all', 'interaction'):
                    result['phase'] = 'native-interactions'
                    interactions(browser, tree, inp, args.output, result, (personal, work), names)
                if args.case in ('all', 'layout'):
                    result['phase'] = 'short-expanded'
                    inp.pointer.click(550, 300)
                    inp.resize(700, 480)
                    capture(tree, inp, args.output, result['phase'])
                    result['geometry'][result['phase']] = footer_geometry(tree, 'left', LONG_NAME)
                    client = tree.rect(one(tree, r'class_name=BrowserView '))
                    assert client['w'] <= 700 and client['h'] <= 480, ('Resize did not exercise short window', client)
                    for side in ('left', 'right'):
                        result['phase'] = side + '-short-interleaved'
                        apply_layout(browser, side, saved_order=('back', 'spaces', 'reload'))
                        capture(tree, inp, args.output, result['phase'])
                        geometry = footer_geometry(tree, side, LONG_NAME)
                        result['geometry'][result['phase']] = geometry
                        client = tree.rect(one(tree, r'class_name=BrowserView '))
                        assert client['w'] <= 700 and client['h'] <= 480, ('Interleaved fixture is not short', client)
                        back = one(tree, r'class_name=BackForwardButton name=Back name_from=')
                        reload = one(tree, r'class_name=ReloadButton ')
                        footer = one(tree, r'class_name=AccountSpacesButton ')
                        assert back['parent'] == reload['parent'] == footer['parent'], (back, reload, footer)
                        back_rect, reload_rect = tree.rect(back), tree.rect(reload)
                        assert geometry['dock']['w'] >= 80, geometry
                        assert back_rect['w'] == back_rect['h'] == reload_rect['w'] == reload_rect['h'] == 40, (back_rect, reload_rect)
                        assert back_rect['y'] == reload_rect['y'] and reload_rect['x'] == back_rect['x'] + 40, (
                            'Saved Spaces split a native button row', back_rect, reload_rect)
                    result['checks'].append('Saved Back/Spaces/Reload keeps Back and Reload in one 40px row in short left/right docks; Spaces follows all native dock siblings (keyboard focus traversal untested)')
                    result['phase'] = 'short-expanded-restored'
                    apply_layout(browser, 'left', extensions=extensions)
                    capture(tree, inp, args.output, result['phase'])
                    geometry = footer_geometry(tree, 'left', LONG_NAME)
                    result['geometry'][result['phase']] = geometry
                    # Collapse using the actual native divider, then keep that state on both sides.
                    dock = geometry['dock']
                    inp.drag((dock['x'] + dock['w'] - 4, 250), (45, 250))
                    for side in ('left', 'right'):
                        result['phase'] = side + '-short-collapsed'
                        apply_layout(browser, side)
                        capture(tree, inp, args.output, result['phase'])
                        geometry = footer_geometry(tree, side, LONG_NAME, compact=True)
                        # With four visible dots the footer needs 136px. Five
                        # 40px controls plus the 160px tab minimum need 496px
                        # in one column, beyond this 480px window. The existing
                        # crowded-dock policy therefore allocates two columns.
                        assert geometry['dock']['w'] == 80, ('Crowded collapsed dock did not allocate two columns', geometry)
                        tabstrip = tree.rect(one(tree, r'class_name=VerticalTabStripRegionView '))
                        assert tabstrip['h'] >= 160, ('Crowded dock lost its minimum tab area', tabstrip)
                        result['geometry'][result['phase']] = geometry
                    open_menu(tree, inp, args.output, 'compact-menu', names)
                    native_key(inp, 'Escape')
                    result['checks'].append('Workspace dot footer stays in bounds below all siblings despite saved-first order, on both sides, with horizontal tabs and short windows; crowded 480px docks widen to 80px to retain the 136px footer and at least 160px of tabs')
                # A same-space selection/insertion must also keep the selector object alive.
                before = identity(tree.read(), one(tree, r'class_name=AccountSpaceDotButton name=Account spaces: '))
                extra = request(browser.port, '/json/new?about:blank', 'PUT')
                request(browser.port, '/json/activate/' + personal[0]['id'])
                request(browser.port, '/json/close/' + extra['id'])
                time.sleep(.25)
                after = identity(tree.read(), one(tree, r'class_name=AccountSpaceDotButton name=Account spaces: '))
                assert before == after, 'Same-space insertion/selection/close recreated the selector'
                retained(browser, *personal, active=True)
                state = browser.state()
                cap = next(cap for cap in state['capabilities'] if cap['id'] == 'spaces')
                assert set(cap['edges']) == {'left', 'right'} and cap['hideable'], cap
                apply_layout(browser, 'right', hidden=True)
                capture(tree, inp, args.output, 'spaces-hidden')
                assert not visible(tree, r'class_name=AccountSpacesButton ')
                result['checks'].append('Full Unicode accessible label, stable selector identity, side-only capabilities and hideability are preserved')
                if extensions:
                    result['checks'].append('Pinned 28px extension actions/icons center in 40px rows on both sides; 40px Reload/Settings controls center their glyphs and paint native hover feedback')
                template = state['layout']
            except BaseException:
                signal.alarm(20)
                failure_diagnostics(browser, tree, inp, args.output, result)
                raise
            finally:
                inp.close()
        # Browser shutdown flushes pending preferences before checking Cancel's
        # persisted effect; an immediate on-disk read could miss delayed writes.
        if 'cancel_expected_preferences' in result:
            stored = json.loads((base / 'profile/Default/Preferences').read_text())['noventra']['browser']['account_spaces']
            result['cancel_persisted_preferences'] = stored
            assert stored == result['cancel_expected_preferences'], ('Cancel mutated persisted space names/IDs', stored)
            result['checks'].append('After shutdown flushed preferences, cancelled New preserved every persisted space name and ID')
        return template


def migration(args, result, template):
    for old_edge, side, shown in [('top', 'left', True), ('bottom', 'right', False)]:
        result['phase'] = 'migrate-' + old_edge
        output = args.output / result['phase']
        output.mkdir()
        with tempfile.TemporaryDirectory(prefix='profile-', dir=output) as temp:
            profile = Path(temp)
            (profile / 'Default').mkdir()
            layout = deepcopy(template)
            layout['revision'] = 73
            for index, item in enumerate(layout['items']):
                item.update(order=index, edge=side, visible=item['id'] == 'tabs')
                if item['id'] == 'spaces':
                    item.update(edge=old_edge, visible=shown)
            preferences = profile / 'Default/Preferences'
            preferences.write_text(json.dumps({'noventra': {'browser': {'dock_layout': layout}}}))
            with LayoutBrowser(args.browser, profile, output, ['--force-renderer-accessibility']) as browser:
                inp = FooterInput(browser.platform)
                tree = None
                try:
                    tree = NativeUITree(browser)
                    upgraded = browser.state()['layout']
                    capture(tree, inp, output, 'migrated')
                    items = {item['id']: item for item in upgraded['items']}
                    assert upgraded['revision'] == 73, upgraded
                    assert items['spaces']['edge'] == side and items['spaces']['visible'] == shown, items['spaces']
                    for item in layout['items']:
                        if item['id'] != 'spaces':
                            assert items[item['id']] == item, ('Migration changed an unrelated control', item, items[item['id']])
                    if shown:
                        footer_geometry(tree, side, 'Personal')
                    else:
                        assert not visible(tree, r'class_name=AccountSpacesButton ')
                    assert browser.state()['layout'] == upgraded, 'Migration repeated on read'
                except BaseException:
                    signal.alarm(20)
                    failure_diagnostics(browser, tree, inp, output, result)
                    raise
                finally:
                    inp.close()
            stored = json.loads(preferences.read_text())['noventra']['browser']['dock_layout']
            assert stored == upgraded, 'Migration was not persisted'
        result['checks'].append(old_edge + ' Spaces migrates to ' + side + ' without losing visibility, revision or unrelated placements')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('all', 'layout', 'interaction', 'extensions'), default='all')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'checks': [], 'browser': str(args.browser), 'case': args.case}
    started = time.monotonic()

    def expired(*_):
        raise TimeoutError('Sidebar footer smoke exceeded its bounded time budget')

    previous = signal.signal(signal.SIGALRM, expired)
    signal.alarm(180)
    try:
        template = exercise(args, result)
        if args.case in ('all', 'layout'):
            migration(args, result, template)
        result.update(passed=True, phase='complete')
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous)
        result['elapsed_s'] = round(time.monotonic() - started, 2)
        (args.output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
