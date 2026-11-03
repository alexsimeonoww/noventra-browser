"""Native workspace dots, retained documents and More on an owned display.

Run with a NEW output directory and the existing Pillow environment:
  work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    work/ram-investigation/with-xvfb.py \
    work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    tests/noventra/workspace_dots_smoke.py --browser /absolute/candidate \
    --output work/checks/workspace-dots-new

Settings APIs prepare profiles/layouts; measured workspace switches, text entry,
menus and dialogs use native input. Each local HTTP document has its own token,
timeOrigin and natively typed draft. CDP never activates a measured switch.
--observe-only records baseline failures and never qualifies a runtime. Exact
paint assertions apply to solid X11 at scale 1; Wayland captures are retained.
The shared fixture bounds test work to 200s, diagnostics/owned cleanup to 40s.
No existing profile, source, build or desktop session is modified.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

from PIL import Image, ImageChops

import chrome_surface_smoke as surface
import compact_chrome_smoke as n
import sidebar_footer_smoke as footer
from glass_appearance_smoke import Appearance


DOT = r'class_name=AccountSpaceDotButton name=Workspace: '
MORE = r'class_name=AccountSpaceDotButton name=Account spaces: '
TABLIST = r'tabList .*class_name=View name=Workspaces name_from='


class Keyboard(surface.Keyboard):
    CODES = dict(surface.Keyboard.CODES, c=46, e=18, m=50, n=49,
                 Return=28, Tab=15, Shift_L=42, space=57, F6=64)

    def key(self, name, control=False, shift=False):
        modifiers = (['Control_L'] if control else []) + (['Shift_L'] if shift else [])
        if not self.process:
            return n.native_key(self.inp, name, modifiers)

        def send(key, down):
            self.process.stdin.write(f'key {self.CODES[key]} {down}\n')
            self.process.stdin.flush()
            assert self.process.stdout.readline().strip() == 'ok'

        for key in modifiers:
            send(key, 1)
        send(name, 1)
        send(name, 0)
        for key in reversed(modifiers):
            send(key, 0)
        time.sleep(.2)


def check(result, case, name, actual, expected):
    surface.check(result, case, name, actual, expected)


def spaces(browser):
    return browser.call('noventraSpaces', 'list', '', '')


def dot_name(node):
    return re.search(r'name=Workspace: (.*?) name_from=', node['line'])[1]


def dots(tree):
    # Native traversal is model order; active overflow is laid out in a prior
    # slot without changing that order. Geometry supplies the display order.
    return footer.visible(tree, DOT)


def ordered_dots(tree):
    return sorted(dots(tree), key=lambda node: (tree.rect(node)['y'], tree.rect(node)['x']))


def identities(tree):
    return {re.search(r'name=(.*?) name_from=', node['line'])[1]: identifier
            for identifier, node in tree.nodes.items()
            if 'class_name=AccountSpaceDotButton ' in node['line']}


def layout(browser, side):
    value = n.top_layout(browser.state()['layout'], surface.ORDER)
    for item in value['items']:
        if item['id'] in ('tabs', 'spaces'):
            item['edge'] = side
    n.apply(browser, value)


def rail(tree):
    return tree.rect(tree.nodes[n.one(tree.read(), r'class_name=AccountSpacesButton ')['parent']])


def resize_rail(tree, inp, side, width):
    before = rail(tree)
    handle = n.one(tree, r'class_name=ResizeArea name=Vertical Tab Strip Resize Handle ')
    start = tree.center(handle)
    delta = (width - before['w']) * (1 if side == 'left' else -1)
    inp.drag(start, (start[0] + delta, start[1]))
    n.wait_for(lambda: rail(tree)['w'] == width, tree.browser.process, timeout=5)
    after = rail(tree)
    assert after['w'] == width, ('Native divider missed fixture width', width, after)


def geometry(tree, names, active, side, width, case, result):
    tree.read()
    container = n.one(tree, r'class_name=AccountSpacesButton ')
    bounds = tree.rect(container)
    dock = tree.rect(tree.nodes[container['parent']])
    controls = ordered_dots(tree)
    cap = 4 if width < 124 else max(1, (width - 16) // 24 - 1)
    expected = names[:cap]
    if active not in expected:
        expected[-1] = active
    expected_height = 16 + 24 * (len(expected) + 1) if width < 124 else 40
    observed = dict(footer=bounds, rail=dock, dots={dot_name(d): tree.rect(d) for d in controls})
    result['cases'][case] = observed
    check(result, case, 'rail width', dock['w'], width)
    check(result, case, 'footer height', bounds['h'], expected_height)
    check(result, case, 'visible workspace order and active overflow', [dot_name(d) for d in controls], expected)
    if not controls:
        # Preserve useful old-runtime geometry and screenshots, but never claim
        # direct-input checks were run when the requested controls are absent.
        return False
    footer.footer_geometry(tree, side, active, compact=width < 124)
    tablist = n.one(tree, TABLIST)
    more = n.one(tree, MORE + re.escape(active) + r' name_from=')
    assert 'haspopup=menu' in more['line'] and re.search(r'\bbutton\b', more['line']), more
    assert tablist['parent'] == more['parent'], (tablist, more)
    selected = []
    for node in controls:
        assert re.search(r'\btab\b', node['line']), node
        assert node['parent'] == footer.identity(tree, tablist), node
        assert re.search(r'\bselected=(true|false)\b', node['line']), node
        if 'selected=true' in node['line']:
            selected.append(dot_name(node))
    check(result, case, 'one selected tab with full workspace name', selected, [active])
    hits = [tree.rect(node) for node in controls + [more]]
    start = (bounds['w'] - (24 if width < 124 else len(hits) * 24)) // 2
    expected_hits = [dict(x=bounds['x'] + start + (0 if width < 124 else i * 24),
                          y=bounds['y'] + 8 + (i * 24 if width < 124 else 0), w=24, h=24)
                     for i in range(len(hits))]
    check(result, case, 'centered 24px native targets with 8px vertical padding', hits, expected_hits)
    observed['ids'] = identities(tree)
    return True


def paint(tree, inp, output, case, result):
    surface.clear_hover(tree, inp)
    with surface.capture(tree, inp, output, case + '-idle') as shot:
        for node in dots(tree):
            rect = tree.rect(node)
            with shot.crop((rect['x'], rect['y'], rect['x'] + 24, rect['y'] + 24)) as crop:
                background = Counter(crop.getdata()).most_common(1)[0][0]
                points = [(x, y) for y in range(24) for x in range(24)
                          if max(abs(a - b) for a, b in zip(crop.getpixel((x, y)), background)) >= 12]
                glyph = None if not points else dict(x=min(x for x, _ in points), y=min(y for _, y in points),
                    w=max(x for x, _ in points) - min(x for x, _ in points) + 1,
                    h=max(y for _, y in points) - min(y for _, y in points) + 1)
                selected = 'selected=true' in node['line']
                size = 8 if selected else 6
                center_delta = max(abs(a - b) for a, b in zip(crop.getpixel((12, 12)), background))
                if inp.platform == 'x11':
                    check(result, case, dot_name(node) + ' centered dot paint', glyph,
                          dict(x=(24 - size) // 2, y=(24 - size) // 2, w=size, h=size))
                    check(result, case, dot_name(node) + ' filled versus hollow', center_delta >= 12, selected)
        inactive = next(node for node in dots(tree) if 'selected=false' in node['line'])
        inp.move(*tree.center(inactive))
        time.sleep(.3)
        with surface.capture(tree, inp, output, case + '-hover') as hover:
            changed = surface.paint_difference(shot, hover, tree.rect(inactive))
        if inp.platform == 'x11':
            check(result, case, 'hover paints only centered 22px square', changed, dict(x=1, y=1, w=22, h=22))


def documents(browser, tree, inp, keys, base, work_id):
    pages = {}
    for key, space_id in (('alpha', ''), ('bravo', work_id)):
        browser.call('noventraSpaces', 'switch', space_id, '')
        target = n.request(browser.port, '/json/new?' + base + '/' + key, 'PUT')
        n.wait_for(lambda: browser.ev('document.readyState==="complete" && !!window.surfaceToken', target),
                   browser.process, timeout=8)
        n.activate(browser, tree, inp, target)  # Fixture setup only.
        content = tree.rect(n.one(tree, r'class_name=MultiContentsView '))
        field = browser.ev('(()=>{const r=document.querySelector("#draft").getBoundingClientRect();'
                           'return {x:r.x+r.width/2,y:r.y+r.height/2}})()', target)
        inp.pointer.click(content['x'] + field['x'], content['y'] + field['y'])
        keys.type(key)
        before = browser.ev(surface.DRAFT, target)
        assert before['token'] and before['value'] == key, before
        pages[key] = (target, before)
    browser.call('noventraSpaces', 'switch', '', '')
    n.wait_for(lambda: browser.ev(n.JS['visible'], pages['alpha'][0]), browser.process, timeout=5)
    surface.page_focus(tree, inp)
    return pages


def retained(browser, pages, active):
    state = surface.retained(browser, pages)
    for key, (target, _) in pages.items():
        assert browser.ev(n.JS['visible'], target) == (key == active), ('Wrong visible document', key, active)
    return state


def direct_switch(browser, tree, inp, pages, name, key, offset, result):
    node = n.one(tree.read(), DOT + re.escape(name) + r' name_from=')
    hit = tree.rect(node)
    inp.pointer.click(hit['x'] + offset, hit['y'] + offset)
    n.wait_for(lambda: browser.ev(n.JS['visible'], pages[key][0]), browser.process, timeout=5)
    current = retained(browser, pages, key)
    selected = [dot_name(dot) for dot in dots(tree.read()) if 'selected=true' in dot['line']]
    assert selected == [name], selected
    result.setdefault('switches', []).append(dict(method='native dot click', name=name,
        local_point=[offset, offset], documents=current, selected_target=pages[key][0]['id']))


def workspace_tooltip_box(before, after, controls, cursor, client):
    """Bound the new popup on this fixture's otherwise empty lower page.

    X11 paints a dark tooltip below the cursor; Wayland paints a light one above.
    Exclude native control feedback and the compositor's cursor, then remove the
    thin popup border that makes Tesseract treat the text as an empty frame.
    """
    assert before.size == after.size, ('Tooltip capture size changed', before.size, after.size)
    cx, cy = map(int, cursor)
    region = (max(client['x'], cx - 96), max(client['y'], cy - 96),
              min(client['x'] + client['w'], cx + 320),
              min(client['y'] + client['h'], cy + 80))

    def excluded(x, y):
        if cx - 3 <= x < cx + 22 and cy - 3 <= y < cy + 32:
            return True
        return any(r['x'] - 1 <= x < r['x'] + r['w'] + 1 and
                   r['y'] - 1 <= y < r['y'] + r['h'] + 1 for r in controls)

    with before.convert('RGB') as base, after.convert('RGB') as current:
        with ImageChops.difference(base, current) as diff:
            points = [(x, y) for y in range(region[1], region[3])
                      for x in range(region[0], region[2])
                      if not excluded(x, y) and max(diff.getpixel((x, y))) >= 12]
    assert points, 'No rendered tooltip appeared outside the native controls and cursor'
    box = (min(x for x, _ in points) + 2, min(y for _, y in points) + 2,
           max(x for x, _ in points) - 1, max(y for _, y in points) - 1)
    assert box[2] - box[0] >= 40 and box[3] - box[1] >= 16, ('Tooltip interior is incomplete', box)
    return box


def native_interactions(browser, tree, inp, keys, pages, output, result):
    before = identities(tree.read())
    direct_switch(browser, tree, inp, pages, 'Work fixture', 'bravo', 1, result)
    direct_switch(browser, tree, inp, pages, 'Personal', 'alpha', 23, result)
    assert identities(tree.read()) == before, 'Switching rebuilt workspace buttons'
    keyboard_focus_dot(tree, inp, keys, output, result, 'Work fixture')
    keys.key('Return')
    n.wait_for(lambda: browser.ev(n.JS['visible'], pages['bravo'][0]), browser.process, timeout=5)
    result['keyboard_documents'] = retained(browser, pages, 'bravo')
    with surface.capture(tree, inp, output, 'keyboard-work'):
        pass
    direct_switch(browser, tree, inp, pages, 'Personal', 'alpha', 12, result)
    # Insert and select another tab in Personal, then use native tab input to
    # return. Structural changes and selection must retain every dot object.
    extra = n.request(browser.port, '/json/new?about:blank', 'PUT')
    time.sleep(.2)
    assert identities(tree.read()) == before, 'Same-space insertion rebuilt dots'
    node = n.one(tree, r'class_name=TabView name=Surface alpha name_from=')
    inp.pointer.click(*tree.center(node))
    n.wait_for(lambda: browser.ev(n.JS['visible'], pages['alpha'][0]), browser.process, timeout=5)
    assert identities(tree.read()) == before, 'Same-space native tab selection rebuilt dots'
    n.request(browser.port, '/json/close/' + extra['id'])
    assert identities(tree.read()) == before, 'Same-space tab close rebuilt dots'
    result['stable_ids'] = before
    retained(browser, pages, 'alpha')
    # Tooltip text is measured in its native popup. The page never renders this
    # label, so an OCR match cannot come from fixture document content.
    surface.clear_hover(tree, inp)
    footer.capture(tree, inp, output, 'workspace-tooltip-before')
    tooltip_dot = n.one(tree, DOT + r'Work fixture name_from=')
    cursor = tree.center(tooltip_dot)
    inp.move(*cursor)
    time.sleep(1)
    footer.capture(tree, inp, output, 'workspace-tooltip')
    tooltip_dot = n.one(tree, DOT + r'Work fixture name_from=')
    assert 'description=Work fixture ' in tooltip_dot['line'], tooltip_dot
    client = tree.rect(n.one(tree, r'class_name=BrowserView '))
    controls = [tree.rect(node) for node in footer.visible(tree, r'class_name=AccountSpaceDotButton ')]
    with Image.open(output / 'workspace-tooltip-before.png') as before_shot, \
            Image.open(output / 'workspace-tooltip.png') as shot:
        box = workspace_tooltip_box(before_shot, shot, controls, cursor, client)
        with shot.crop(box) as crop:
            crop.save(output / 'workspace-tooltip-interior.png')
    result['tooltip'] = dict(interior=box, labels=footer.popup_labels(
        output, 'workspace-tooltip-interior', ['Work fixture']))
    surface.clear_hover(tree, inp)


def keyboard_focus_dot(tree, inp, keys, output, result, name):
    """Find a painted focus ring using bounded native pane/Tab traversal.

    Button mouse presses do not promise focus. Native tree diagnostics omit the
    focused view, so establish focus from the visible ring before Enter, then
    independently require activation of the exact retained target document.
    """
    surface.page_focus(tree, inp)
    surface.clear_hover(tree, inp)
    node = n.one(tree.read(), DOT + re.escape(name) + r' name_from=')
    rect = tree.rect(node)
    assert 'selected=false' in node['line'], node
    box = (rect['x'], rect['y'], rect['x'] + 24, rect['y'] + 24)
    with surface.capture(tree, inp, output, 'keyboard-unfocused') as shot:
        idle = shot.crop(box)
    attempts = result['keyboard_focus_attempts'] = []
    try:
        for pane in range(4):
            keys.key('F6')
            for step in range(16):
                name_capture = f'keyboard-focus-{pane}-{step}'
                with inp.capture(output / (name_capture + '.png')) as shot:
                    focused = shot.crop(box)
                with ImageChops.difference(idle, focused) as diff:
                    ring = [(x, y) for y in range(24) for x in range(24)
                            if not (7 <= x < 17 and 7 <= y < 17)
                            and max(diff.getpixel((x, y))) >= 12]
                focused.close()
                attempts.append(dict(pane=pane, tabs=step, outer_changed_pixels=len(ring)))
                if len(ring) >= 24:
                    node = n.one(tree.read(), DOT + re.escape(name) + r' name_from=')
                    assert tree.rect(node) == rect and 'selected=false' in node['line'], node
                    (output / (name_capture + '.txt')).write_text(tree.text)
                    result['keyboard_focus_evidence'] = name_capture
                    return
                keys.key('Tab')
    finally:
        idle.close()
    raise AssertionError(('Native F6/Tab traversal never painted workspace focus', name, attempts))


def naming(browser, tree, inp, keys, output, command, value):
    labels = [space['name'] for space in spaces(browser)]
    positions = footer.open_menu(tree, inp, output, command + '-menu', labels)
    inp.pointer.click(*positions['New account space' if command == 'create' else 'Rename current space'])
    footer.capture(tree, inp, output, command + '-dialog')
    n.one(tree, r'class_name=Textfield name=Name name_from=')
    keys.key('a', control=True)
    keys.type(value)
    field = n.one(tree.read(), r'class_name=Textfield name=Name name_from=')
    assert re.search(r' value=' + re.escape(value) + r'(?: |$)', field['line']), field
    keys.key('Return')
    n.wait_for(lambda: any(space['name'] == value for space in spaces(browser)), browser.process, timeout=5)
    footer.capture(tree, inp, output, command + '-saved')
    assert not footer.visible(tree, r'class_name=Textfield name=Name name_from='), 'Save left naming dialog open'
    return next(space['id'] for space in spaces(browser) if space['name'] == value)


def preview(browser, tree, inp, output, result):
    n.activate(browser, tree, inp, browser.settings)
    n.wait_for(lambda: browser.ev(n.JS['ready']), browser.process, timeout=8)
    browser.ev(n.JS['show_editor'])
    data = browser.ev('(()=>{' + n.FIND + '''
      const chip=editor.shadowRoot.querySelector('.canvas .chip[data-control=spaces]');
      const dots=[...chip.querySelectorAll('.preview-space-dot')];
      return {count:dots.length,active:dots.filter(e=>e.hasAttribute('data-active')).length,
        sizes:dots.map(e=>{const r=e.getBoundingClientRect();return [r.width,r.height]}),
        more:chip.querySelector('.preview-space-more')?.textContent.trim(),
        text:[...chip.children].filter(e=>!e.matches('.control-label')).map(e=>e.textContent.trim()).join('').trim(),
        labelClip:getComputedStyle(chip.querySelector('.control-label')).clipPath,
        role:chip.getAttribute('role'),
        label:chip.getAttribute('aria-label')};
    })()''')
    result['settings_preview'] = data
    check(result, 'Settings preview', 'three visual dots', data['count'], 3)
    check(result, 'Settings preview', 'one active dot', data['active'], 1)
    check(result, 'Settings preview', '8px active and 6px inactive dots', data['sizes'], [[8, 8], [6, 6], [6, 6]])
    check(result, 'Settings preview', 'More glyph replaces workspace text', data['text'], '⋯')
    check(result, 'Settings preview', 'accessible control label stays visually clipped', data['labelClip'], 'inset(50%)')
    with surface.capture(tree, inp, output, 'settings-preview'):
        pass
    saved = browser.state()['layout']
    result['preview_containment'] = []
    try:
        for width in (480, 340):
            for preset in ('workspace', 'sidebar-top', 'minimal-left', 'minimal-right', 'classic-top'):
                select = '(async()=>{' + n.FIND + '''
                  editor.shadowRoot.querySelector('[data-preset="'+PRESET+'"]').click();
                  await editor.updateComplete;
                  editor.shadowRoot.querySelector('.canvas').scrollIntoView({block:'center',behavior:'instant'});
                  return true;
                })()'''.replace('PRESET', json.dumps(preset))
                case = f'preview-{width}-{preset}'
                measure = '(()=>{' + n.FIND + '''
                  const canvas=editor.shadowRoot.querySelector('.canvas').getBoundingClientRect();
                  const within=(r,p)=>r.left>=p.left-1 && r.top>=p.top-1 && r.right<=p.right+1 && r.bottom<=p.bottom+1;
                  const chips=[...editor.shadowRoot.querySelectorAll('.canvas .chip[data-control=spaces]')];
                  return {viewport:innerWidth,canvasFits:canvas.right<=innerWidth+1,
                    controlsFit:[...editor.shadowRoot.querySelectorAll('.canvas [data-control]')].every(e=>within(e.getBoundingClientRect(),canvas)),
                    spaces:chips.map(chip=>{const r=chip.getBoundingClientRect();
                      const footer=chip.closest('.space-footer').getBoundingClientRect();
                      const dots=[...chip.querySelectorAll('.preview-space-dot')];
                      const glyphs=[...dots,...chip.querySelectorAll('.preview-space-more')].map(e=>e.getBoundingClientRect());
                      return {count:dots.length,footerWidth:footer.width,
                        direction:getComputedStyle(chip.querySelector('.preview-space-dots')||chip).flexDirection,
                        glyphsFit:glyphs.every(g=>within(g,r)) && within(r,footer),
                        noOverlap:glyphs.every((a,i)=>glyphs.every((b,j)=>i===j ||
                          Math.min(a.right,b.right)<=Math.max(a.left,b.left) || Math.min(a.bottom,b.bottom)<=Math.max(a.top,b.top)))};})};
                })()'''
                # Emulation belongs to its CDP session. Keep selection,
                # measurements and capture on that same live connection.
                steps = [
                    {'method': 'Emulation.setDeviceMetricsOverride', 'params': {
                        'width': width, 'height': 700, 'deviceScaleFactor': 1, 'mobile': False}},
                    {'method': 'Runtime.evaluate', 'params': {'expression': select,
                        'awaitPromise': True, 'returnByValue': True}, 'assert': True},
                    {'delay': 200},
                    {'method': 'Runtime.evaluate', 'params': {'expression': measure,
                        'returnByValue': True}, 'returnValue': True},
                    {'method': 'Page.captureScreenshot', 'params': {'format': 'png'},
                        'capture': str(output / (case + '.png'))},
                    {'method': 'Emulation.clearDeviceMetricsOverride', 'params': {}},
                ]
                run = subprocess.run(['node', str(n.ROOT / 'tests/noventra/layout_editor_input.mjs'),
                    browser.settings['webSocketDebuggerUrl'], json.dumps(steps)],
                    check=True, capture_output=True, text=True, timeout=25)
                observed = json.loads(run.stdout)['result']['value']
                result['preview_containment'].append(dict(case=case, **observed))
                check(result, case, 'viewport width', observed['viewport'], width)
                check(result, case, 'canvas and controls contained', observed['canvasFits'] and observed['controlsFit'], True)
                for index, chip in enumerate(observed['spaces']):
                    check(result, case, f'workspace preview {index} has three dots', chip['count'], 3)
                    check(result, case, f'workspace preview {index} glyph containment', chip['glyphsFit'] and chip['noOverlap'], True)
                    check(result, case, f'workspace preview {index} adapts orientation', chip['direction'],
                          'column' if chip['footerWidth'] <= 86 else 'row')
        check(result, 'Settings preview', 'preview presets did not mutate saved layout', browser.state()['layout'], saved)
    finally:
        browser.ev('(()=>{' + n.FIND + 'editor.shadowRoot.querySelector("#cancelLayout").click();return true;})()')


def run(args):
    args.browser = args.browser.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'executed-test.py').write_bytes(Path(__file__).read_bytes())
    result = dict(passed=False, qualified=False, observe_only=args.observe_only,
        browser=str(args.browser), binary_sha256=surface.sha256(args.browser),
        source_sha256=surface.sha256(Path(__file__)), checks=[], cases={},
        scope='Private native X11/Wayland input, 34px/220px left/right geometry, solid-X11 paint, fresh profiles only; no touch/RTL/physical-display qualification')
    budget = n.Budget()

    def cancelled(*_):
        raise RuntimeError('Workspace dots smoke cancelled')

    previous = signal.signal(signal.SIGTERM, cancelled)
    try:
        with surface.local_pages() as base, tempfile.TemporaryDirectory(prefix='profile-', dir=args.output) as tmp:
            profile = Path(tmp)
            (profile / 'Default').mkdir()
            (profile / 'Default/Preferences').write_text(json.dumps({
                'vertical_tabs': {'collapsed_state': True, 'uncollapsed_width': 220}}))
            with n.session(args, profile, args.output, n.FLAGS, result, budget) as (browser, tree, inp), Keyboard(inp) as keys:
                result.update(platform=browser.platform, owned_browser_pid=browser.process.pid)
                appearance = Appearance(browser)
                appearance.scheme(1)
                layout(browser, 'left')
                result['phase'] = 'single-workspace'
                has_dots = geometry(tree, ['Personal'], 'Personal', 'left', 34, result['phase'], result)
                with surface.capture(tree, inp, args.output, result['phase']):
                    pass
                created = browser.call('noventraSpaces', 'create', '', 'Work fixture')
                work_id = next(space['id'] for space in created if space['name'] == 'Work fixture')
                pages = documents(browser, tree, inp, keys, base, work_id)
                result['documents'] = {key: {'target': target['id'], **before} for key, (target, before) in pages.items()}
                names = [space['name'] for space in spaces(browser)]
                for scheme, mode in (('light', 1), ('dark', 2)):
                    appearance.scheme(mode)
                    case = 'two-workspaces-' + scheme
                    if geometry(tree, names, 'Personal', 'left', 34, case, result):
                        paint(tree, inp, args.output, case, result)
                    with surface.capture(tree, inp, args.output, case):
                        pass
                appearance.scheme(1)
                if has_dots:
                    result['phase'] = 'native-direct-and-keyboard-input'
                    native_interactions(browser, tree, inp, keys, pages, args.output, result)
                    result['phase'] = 'native-create-and-rename'
                    new_id = naming(browser, tree, inp, keys, args.output, 'create', 'created')
                    assert naming(browser, tree, inp, keys, args.output, 'rename', 'renamed') == new_id
                    result['native_created_id'] = new_id
                    retained(browser, pages, None)
                else:
                    result['unrun'] = ['Direct dots/keyboard switching, dot paint, tooltip, native create/rename and More interaction: baseline has no dots']
                for index in range(10 - len(spaces(browser))):
                    browser.call('noventraSpaces', 'create', '', 'Studio ' + str(index))
                all_spaces = spaces(browser)
                names = [space['name'] for space in all_spaces]
                active = all_spaces[-1]
                browser.call('noventraSpaces', 'switch', active['id'], '')
                time.sleep(.25)
                for side in ('left', 'right'):
                    layout(browser, side)
                    for width in (34, 220):
                        if rail(tree)['w'] != width:
                            resize_rail(tree, inp, side, width)
                        case = side + '-' + str(width) + '-ten-workspaces'
                        result['phase'] = case
                        geometry(tree, names, active['name'], side, width, case, result)
                        with surface.capture(tree, inp, args.output, case):
                            pass
                    resize_rail(tree, inp, side, 34)
                if has_dots:
                    result['phase'] = 'overflow-menu'
                    hidden = next(space for space in all_spaces if space['name'] not in [dot_name(d) for d in dots(tree.read())])
                    positions = footer.open_menu(tree, inp, args.output, 'all-ten-menu', names)
                    inp.pointer.click(*positions[hidden['name']])
                    n.wait_for(lambda: bool(footer.visible(tree.read(), MORE + re.escape(hidden['name']) + r' name_from=')),
                               browser.process, timeout=5)
                    geometry(tree, names, hidden['name'], 'right', 34, 'active-overflow-replacement', result)
                    direct_switch(browser, tree, inp, pages, 'Personal', 'alpha', 12, result)
                    positions = footer.open_menu(tree, inp, args.output, 'manage-menu', names)
                    inp.pointer.click(*positions['Manage spaces in Settings'])
                    n.wait_for(lambda: any(page['url'] == 'chrome://settings/appearance?section=spaces'
                               for page in n.request(browser.port, '/json/list')), browser.process, timeout=8)
                    result['manage_settings_opened'] = True
                result['phase'] = 'settings-preview'
                preview(browser, tree, inp, args.output, result)
                result['expected_spaces'] = spaces(browser)
                result['final_documents'] = surface.retained(browser, pages)
            stored = json.loads((profile / 'Default/Preferences').read_text())
            expected = {space['id']: space['name'] for space in result['expected_spaces'] if space['id']}
            saved = stored['noventra']['browser']['account_spaces']
            check(result, 'persistence', 'created/renamed workspace IDs and names saved', saved, expected)
            check(result, 'persistence', 'expanded width retained', stored['vertical_tabs']['uncollapsed_width'], 220)
            check(result, 'persistence', 'collapsed state retained', stored['vertical_tabs']['collapsed_state'], True)
            result['phase'] = 'restart'
            with n.session(args, profile, args.output / 'restart', n.FLAGS, result, budget) as (browser, tree, inp):
                check(result, 'restart', 'all workspace IDs and names restored', spaces(browser), result['expected_spaces'])
                geometry(tree, [space['name'] for space in spaces(browser)], 'Personal', 'right', 34, 'restart-collapsed', result)
                with surface.capture(tree, inp, args.output, 'restart-collapsed'):
                    pass
        check(result, 'runtime', 'binary unchanged', surface.sha256(args.browser), result['binary_sha256'])
        result['passed'] = all(entry['passed'] for entry in result['checks'])
        result['qualified'] = result['passed'] and not args.observe_only
        result['phase'] = 'complete'
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        budget.close()
        signal.signal(signal.SIGTERM, previous)
        if 'browser' in locals():
            result['owned_browser_stopped'] = browser.process.poll() is not None
        result['elapsed'] = time.monotonic() - budget.started
        result['failures'] = [entry for entry in result['checks'] if not entry['passed']]
        n.write_json(args.output / 'result.json', result)
        print(json.dumps({key: result.get(key) for key in
            ('passed', 'qualified', 'observe_only', 'elapsed', 'phase', 'failures', 'error')}, indent=2))
    return 0 if result['passed'] or args.observe_only else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe-only', action='store_true')
    raise SystemExit(run(parser.parse_args()))
