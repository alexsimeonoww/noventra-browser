"""Native vertical-tab selection/foreground/corner contrast for seven presets.

Use the private Xvfb wrapper, pinned binary/resources and a fresh output. The
owned transfer namespace hides the host HOME and has no network egress. Theme
choices go through the actual Settings controls. Bounds come from TabView AX;
measurements come from native screenshots, never CSS or simulated tab paint.
Software X11 covers fallback-tab optics; desktop glass transmission is separate.
"""
import argparse
import ctypes as C
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/noventra/inline_footer'))
from sidebar_common import click_dom, ui
from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree
from interface_polish_smoke import native_key
from portable_transfer_smoke import focus_owned, isolate, protocol
from sidebar_inline_address_smoke import screen_rect
from sidebar_unified_smoke import close_normally

PRESETS = ('white-glass', 'black-glass', 'solid-white', 'solid-black',
           'forest', 'ocean', 'sand')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def luminance(rgb):
    values = [value / 255 for value in rgb]
    channels = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4
                for v in values]
    return sum(v * weight for v, weight in zip(channels, (.2126, .7152, .0722)))


def ratio(a, b):
    left, right = sorted((luminance(a), luminance(b)))
    return (right + .05) / (left + .05)


def delta(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def pixels(image, box):
    left, top, right, bottom = map(round, box)
    assert 0 <= left < right <= image.width and 0 <= top < bottom <= image.height, box
    return [image.getpixel((x, y)) for y in range(top, bottom) for x in range(left, right)]


def median(values):
    return tuple(round(statistics.median(c)) for c in zip(*values))


def one(tree, pattern):
    nodes = [node for node in tree.nodes.values()
             if ' INVISIBLE ' not in node['line'] and re.search(pattern, node['line'])]
    assert len(nodes) == 1, (pattern, [n['line'] for n in nodes])
    return nodes[0]


def tab_rect(tree, title):
    row = screen_rect(tree, one(tree, r'class_name=TabView name=' + re.escape(title) + r' name_from='))
    assert row['w'] >= 180 and 30 <= row['h'] <= 48, ('Owned expanded tab required', row)
    return row


def samples(image, row):
    x, y, w, h = (row[k] for k in ('x', 'y', 'w', 'h'))
    # The title lives near the leading icon. The trailing close target occupies
    # the last24px. These measured empty interior pixels touch neither control.
    fill = median(pixels(image, (x + w * .69, y + h / 2 - 2,
                                 x + w * .79, y + h / 2 + 2)))
    background = median(pixels(image, (x + w * .69, y,
                                       x + w * .79, y + 1)))
    return fill, background


def foreground(image, row, fill, output, name):
    x, y, w, h = (row[k] for k in ('x', 'y', 'w', 'h'))
    box = (x + 30, y + 6, x + min(w - 34, 138), y + h - 6)
    colours = pixels(image, box)
    ratios = sorted(ratio(colour, fill) for colour in colours)
    # Use the actual strongest glyph pixels, not antialiased edge averages.
    # Requiring several readable pixels and OCR ties the ratio to real text.
    text_ratio = ratios[max(0, len(ratios) - 12)]
    crop = image.crop(tuple(map(round, box)))
    crop = crop.resize((crop.width * 4, crop.height * 4), Image.Resampling.NEAREST)
    crop.save(output / (name + '-title.png'))
    text = subprocess.check_output(
        ['tesseract', str(output / (name + '-title.png')), 'stdout', '--psm', '7'],
        text=True, stderr=subprocess.DEVNULL, timeout=10).strip()
    icon_colours = pixels(image, (x + 7, y + h / 2 - 8,
                                   x + 25, y + h / 2 + 8))
    icon_ratios = sorted(ratio(colour, fill) for colour in icon_colours)
    return {'text_contrast': text_ratio, 'ocr': text,
            'icon_contrast': icon_ratios[max(0, len(icon_ratios) - 10)]}


def corners(image, row, fill, background):
    x, y, w = (row[k] for k in ('x', 'y', 'w'))
    separation = delta(fill, background)
    threshold = max(6, separation * .4)
    edges = []
    for offset in (3, 4, 7):
        found = [dx for dx in range(20)
                 if delta(image.getpixel((x + dx, y + offset)), background) >= threshold]
        edges.append(min(found) if found else None)
    # Paint starts2px inside a TabView. A compact8px curve reaches the first
    #7/5/3columns on these three scanlines; a16px pill needs about10/7/4.
    compact = separation >= 12 and all(value is not None and value <= limit
                                      for value, limit in zip(edges, (7, 5, 3)))
    return {'left_insets_at_y_3_4_7': edges, 'compact_8px_curve': compact,
            'paint_separation': separation, 'threshold': threshold}


class WindowAttributes(C.Structure):
    _fields_ = [(name, C.c_int) for name in ('x', 'y', 'width', 'height', 'border_width', 'depth')] + [
        ('visual', C.c_void_p), ('root', C.c_ulong), ('klass', C.c_int),
        ('bit_gravity', C.c_int), ('win_gravity', C.c_int), ('backing_store', C.c_int),
        ('backing_planes', C.c_ulong), ('backing_pixel', C.c_ulong), ('save_under', C.c_int),
        ('colormap', C.c_ulong), ('map_installed', C.c_int), ('map_state', C.c_int),
        ('all_event_masks', C.c_long), ('your_event_mask', C.c_long),
        ('do_not_propagate_mask', C.c_long), ('override_redirect', C.c_int), ('screen', C.c_void_p)]


def focus_main(browser, inp):
    # A dismissed omnibox suggestion is an unmapped same-PID X11 window.
    # Focus the mapped main window, never the last-created child popup.
    x, d = inp.pointer.x, inp.pointer.d
    x.XDefaultRootWindow.argtypes=[C.c_void_p];x.XDefaultRootWindow.restype=C.c_ulong
    x.XInternAtom.argtypes=[C.c_void_p,C.c_char_p,C.c_int];x.XInternAtom.restype=C.c_ulong
    x.XQueryTree.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_ulong),C.POINTER(C.POINTER(C.c_ulong)),C.POINTER(C.c_uint)]
    x.XGetWindowProperty.argtypes=[C.c_void_p,C.c_ulong,C.c_ulong,C.c_long,C.c_long,C.c_int,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),C.POINTER(C.c_ulong),C.POINTER(C.c_ulong),C.POINTER(C.POINTER(C.c_ubyte))]
    x.XGetWindowAttributes.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(WindowAttributes)]
    x.XSetInputFocus.argtypes=[C.c_void_p,C.c_ulong,C.c_int,C.c_ulong]
    x.XRaiseWindow.argtypes=[C.c_void_p,C.c_ulong]
    root,parent,children,count=C.c_ulong(),C.c_ulong(),C.POINTER(C.c_ulong)(),C.c_uint()
    assert x.XQueryTree(d,x.XDefaultRootWindow(d),C.byref(root),C.byref(parent),C.byref(children),C.byref(count))
    atom=x.XInternAtom(d,b'_NET_WM_PID',0);found=[]
    try:
        for i in range(count.value):
            actual,fmt,total,after=C.c_ulong(),C.c_int(),C.c_ulong(),C.c_ulong();data=C.POINTER(C.c_ubyte)()
            x.XGetWindowProperty(d,children[i],atom,0,1,0,0,C.byref(actual),C.byref(fmt),C.byref(total),C.byref(after),C.byref(data))
            try:
                if not total.value or C.cast(data,C.POINTER(C.c_ulong))[0]!=browser.process.pid:continue
                attrs=WindowAttributes();assert x.XGetWindowAttributes(d,children[i],C.byref(attrs))
                if attrs.map_state==2 and not attrs.override_redirect:
                    found.append((attrs.width*attrs.height,children[i]))
            finally:
                if data:x.XFree(data)
    finally:x.XFree(children)
    assert found,'No mapped owned native browser window'
    main=max(found)[1];x.XRaiseWindow(d,main);x.XSetInputFocus(d,main,2,0);x.XFlush(d)


def focused_pid(inp):
    x, display = inp.pointer.x, inp.pointer.d
    x.XGetInputFocus.argtypes = [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)]
    window, revert = C.c_ulong(), C.c_int()
    x.XGetInputFocus(display, C.byref(window), C.byref(revert))
    atom = x.XInternAtom(display, b'_NET_WM_PID', 0)
    for _ in range(12):
        actual, fmt, count, after = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
        data = C.POINTER(C.c_ubyte)()
        x.XGetWindowProperty(display, window, atom, 0, 1, 0, 0,
            C.byref(actual), C.byref(fmt), C.byref(count), C.byref(after), C.byref(data))
        try:
            if count.value:
                return C.cast(data, C.POINTER(C.c_ulong))[0]
        finally:
            if data: x.XFree(data)
        root, parent, children, total = C.c_ulong(), C.c_ulong(), C.POINTER(C.c_ulong)(), C.c_uint()
        if not x.XQueryTree(display, window, C.byref(root), C.byref(parent), C.byref(children), C.byref(total)):
            break
        if children: x.XFree(children)
        if parent.value == window.value or not parent.value: break
        window = parent
    return None


def choose_theme(browser, tree, inp, result, preset):
    click_dom(browser, tree, inp, result, '[data-section="customizeAppearance"]', 'page')
    click_dom(browser, tree, inp, result,
              'input[name=noventraThemePreset][value=' + json.dumps(preset) + ']', 'page')
    wait_for(lambda: ui(browser, '''const page=deep(document,'settings-appearance-page');
      return !page.noventraAppearancePending_ && page.noventraAppearancePresets_
        .some(p=>p.id===PRESET && page.isNoventraAppearanceSelected_(p));'''
        .replace('PRESET', json.dumps(preset))), browser.process, timeout=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['theme-contrast'], default='theme-contrast')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    args.rtc_policy_diagnostic = False
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    result = {'passed': False, 'checks': [], 'states': [], 'case': args.case,
              'binary_sha256': args.expected_binary, 'resources_sha256': args.expected_resources,
              'fixture_sha256': sha(__file__), 'home_isolated': True,
              'network_isolated_no_egress': True, 'normal_shutdowns': [],
              'scope': 'Native software-X11 fallback selection/corner/text/icon optics; no glass transmission claim',
              'helpers': {str(ROOT / 'tests/noventra' / name): sha(ROOT / 'tests/noventra' / name)
                          for name in ('layout_test_support.py', 'native_ui_tree.py',
                                       'dock_rail_regressions_smoke.py', 'portable_transfer_smoke.py',
                                       'sidebar_inline_address_smoke.py', 'sidebar_unified_smoke.py',
                                       'inline_footer/sidebar_common.py')}}

    def check(preset, focus, name, value, actual):
        result['checks'].append({'preset': preset, 'window': focus, 'name': name,
                                 'passed': bool(value), 'actual': actual})

    pointer = None
    try:
        profile = args.output / 'profile'
        (profile / 'Default').mkdir(parents=True)
        (profile / 'Default/Preferences').write_text(json.dumps({
            'helium': {'completed_onboarding': True, 'browser': {'layout': 2,
                'zen_mode': True, 'zen_mode_sidebar_pinned': True,
                'zen_mode_top_chrome_pinned': True}},
            'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 232}}))
        flags = ['--force-renderer-accessibility', '--force-device-scale-factor=1', '--ui-toolkit=gtk']
        with ExitStack() as stack:
            browser = stack.enter_context(LayoutBrowser(args.browser, profile, args.output / 'browser', flags))
            pointer = Input(browser.platform)
            tree = NativeUITree(browser)
            state = browser.state()
            layout = state['presets']['minimal-left']
            layout['revision'] = state['layout']['revision']
            assert browser.apply(layout)['status'] == 'ok'
            passive = request(browser.port, '/json/new?about:blank', 'PUT')
            browser.ev('document.title="Passive";window.themeTabWitness="passive";true', passive)
            active = request(browser.port, '/json/new?about:blank', 'PUT')
            browser.ev('document.title="Active";window.themeTabWitness="active";true', active)
            other = None
            for focus in ('active', 'inactive'):
                if focus == 'inactive':
                    other = stack.enter_context(LayoutBrowser(
                        args.browser, args.output / 'focus-profile', args.output / 'focus-browser',
                        [*flags, '--window-position=850,590', '--window-size=400,180']))
                    other_tree = NativeUITree(other)
                    other_tree.read()
                    local_content = other_tree.rect(one(other_tree, r'class_name=MultiContentsView '))
                    # Native AX stores this widget's contents in local coordinates.
                    # Read and position the actual owned window through Chrome's native
                    # window boundary before mapping that rectangle into screenshot space.
                    target_window = protocol(other, 'Browser.getWindowForTarget',
                                             {'targetId': other.settings['id']})
                    protocol(other, 'Browser.setWindowBounds', {
                        'windowId': target_window['windowId'],
                        'bounds': {'left': 850, 'top': 590, 'width': 400, 'height': 180}})
                    def located():
                        bounds = protocol(other, 'Browser.getWindowBounds',
                                          {'windowId': target_window['windowId']})['bounds']
                        return bounds if bounds['left'] >= 800 and bounds['top'] >= 500 else None
                    window_bounds = wait_for(located, other.process, timeout=5)
                    other_content = {**local_content,
                                     'x': local_content['x'] + window_bounds['left'],
                                     'y': local_content['y'] + window_bounds['top']}
                    result['focus_window_geometry'] = {'local_content': local_content,
                        'native_window': window_bounds, 'screen_content': other_content}
                    assert other_content['x'] >= 800, ('Owned focus window must not overlap sidebar', other_content)
                    focus_point = (min(1250, other_content['x'] + other_content['w'] / 2),
                                   min(780, other_content['y'] + other_content['h'] / 2))
                for preset in PRESETS:
                    focus_main(browser, pointer)
                    choose_theme(browser, tree, pointer, result, preset)
                    request(browser.port, '/json/activate/' + active['id'])
                    focus_main(browser, pointer)
                    native_key(pointer, 'Escape')
                    pointer.pointer.click(800, 600)
                    pointer.capture(args.output / (preset + '-' + focus + '-before-focus-check.png')).close()
                    wait_for(lambda: focused_pid(pointer) == browser.process.pid,
                             browser.process, timeout=5)
                    assert browser.ev('document.visibilityState==="visible"', active), 'Selected page must be visible' 
                    if focus == 'inactive':
                        request(other.port, '/json/activate/' + other.settings['id'])
                        focus_main(other, pointer)
                        pointer.pointer.click(*focus_point)
                        wait_for(lambda: focused_pid(pointer) == other.process.pid, browser.process, timeout=5)
                    pointer.move(700, 700)
                    time.sleep(.35)
                    tree.read()
                    assert any('class_name=BrowserView ' in n['line'] for n in tree.nodes.values())
                    selected, ordinary = tab_rect(tree, 'Active'), tab_rect(tree, 'Passive')
                    path = args.output / (preset + '-' + focus + '.png')
                    image = pointer.capture(path)
                    fill, bg = samples(image, selected)
                    inactive_fill, _ = samples(image, ordinary)
                    active_fg = foreground(image, selected, fill, args.output, preset + '-' + focus + '-active')
                    passive_fg = foreground(image, ordinary, inactive_fill, args.output, preset + '-' + focus + '-passive')
                    curve = corners(image, selected, fill, bg)
                    row = {'preset': preset, 'window': focus, 'active_bounds': selected,
                           'inactive_bounds': ordinary, 'fill': fill, 'background': bg,
                           'inactive_fill': inactive_fill, 'active_foreground': active_fg,
                           'inactive_foreground': passive_fg, 'corners': curve,
                           'active_fill_delta': delta(fill, bg), 'selection_delta': delta(fill, inactive_fill)}
                    result['states'].append(row)
                    (args.output / (preset + '-' + focus + '-tree.txt')).write_text(tree.text)
                    check(preset, focus, 'Active tab fill separates from its real sidebar',
                          row['active_fill_delta'] >= 12 and row['selection_delta'] >= 12,
                          {'fill_delta': row['active_fill_delta'], 'selection_delta': row['selection_delta']})
                    check(preset, focus, 'Compact active corner paint fits the8px shape', curve['compact_8px_curve'], curve)
                    for name, fg in [('active', active_fg), ('inactive', passive_fg)]:
                        wanted = 'active' if name == 'active' else 'passive'
                        actual = re.sub('[^a-z]', '', fg['ocr'].lower())
                        check(preset, focus, name + ' tab title is actually readable',
                              wanted in actual and fg['text_contrast'] >= 4.5, fg)
                        check(preset, focus, name + ' tab icon remains visible', fg['icon_contrast'] >= 3, fg)
                    image.close()
            assert browser.ev('window.themeTabWitness', active) == 'active'
            assert browser.ev('window.themeTabWitness', passive) == 'passive'
            result['normal_shutdowns'].append({'focus_window': close_normally(other)})
            result['normal_shutdowns'].append({'theme_window': close_normally(browser)})
        result['passed'] = all(row['passed'] for row in result['checks'])
        assert result['passed'], 'Native tab contrast/corner failures are recorded in result.json'
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        if pointer:
            pointer.close()
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'checks': len(result['checks']), 'states': len(result['states'])}))

if __name__ == '__main__':
    main()
