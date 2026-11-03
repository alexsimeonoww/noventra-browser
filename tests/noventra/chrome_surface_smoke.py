"""Bounded native chrome geometry, paint and input on a private display.

Run with the existing Pillow venv and work/ram-investigation/with-xvfb.py:
  work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    work/ram-investigation/with-xvfb.py \
    work/mixed-sites-baseline-2026-10-11/venv/bin/python -B \
    tests/noventra/chrome_surface_smoke.py --browser /absolute/candidate \
    --output work/checks/chrome-surface-new

Use a NEW output directory. --observe-only collects the same target failures
without a failing exit status; it never qualifies a runtime. Native input tests
the transparent 3px above/below the 22px tab fill, a full 16px local favicon,
Ctrl+L and retained unsaved input/documents. Both sides and light/dark solid
surfaces are covered at 1200x760, scale 1. Expanded rail width remains 220px.
The existing private Wayland wrappers also work: geometry/input are asserted,
while screenshots/pixel observations are retained for compositor inspection.
Fresh-profile glass stays enabled; X11 naturally uses the solid fallback.
The shared fixture bounds work to 200s and evidence/owned cleanup to 40s more.
Glass/compositor strokes, touch UI, RTL and other platforms are not qualified.
"""

import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import threading
import time

from PIL import Image, ImageChops

import compact_chrome_smoke as n
from glass_appearance_smoke import Appearance


COLORS = {'alpha': (224, 54, 105), 'bravo': (24, 151, 95)}
ORDER = ('back', 'forward', 'reload', 'address', 'tabs', 'spaces',
         'new-tab', 'extensions', 'menu')
DRAFT = '''({token:window.surfaceToken,origin:performance.timeOrigin,
  value:document.querySelector('#draft')?.value,url:location.href})'''


class Keyboard:
    """Keep the private Wayland seat/keymap stable throughout the input checks."""
    CODES = {'Escape': 1, 'Control_L': 29, 'a': 30, 'b': 48, 'd': 32,
             'f': 33, 'h': 35, 'l': 38, 'o': 24, 'p': 25, 'r': 19,
             't': 20, 'v': 47}

    def __init__(self, inp):
        self.inp = inp
        self.process = None

    def __enter__(self):
        if self.inp.platform == 'wayland':
            assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') == '1'
            self.process = subprocess.Popen([os.environ['NOVENTRA_TEST_KEYBOARD']],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                assert self.process.stdout.readline().strip() == 'ready'
            except BaseException:
                self.__exit__()
                raise
        return self

    def key(self, name, control=False):
        if not self.process:
            return n.native_key(self.inp, name, ('Control_L',) if control else ())
        def send(key, down):
            self.process.stdin.write(f'key {self.CODES[key]} {down}\n')
            self.process.stdin.flush()
            assert self.process.stdout.readline().strip() == 'ok'
        if control:
            send('Control_L', 1)
        send(name, 1)
        send(name, 0)
        if control:
            send('Control_L', 0)
        time.sleep(.2)

    def type(self, text):
        for char in text:
            self.key(char)

    def __exit__(self, *_):
        if self.process:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


@contextmanager
def local_pages():
    """Serve only two owned documents and their opaque 16x16 test favicons."""
    icons = {}
    for key, color in COLORS.items():
        with Image.new('RGB', (16, 16), color) as icon, io.BytesIO() as buffer:
            icon.save(buffer, format='PNG')
            icons[key] = buffer.getvalue()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            key = self.path.removeprefix('/').removesuffix('.png')
            if key not in COLORS:
                self.send_error(404)
                return
            if self.path.endswith('.png'):
                data, mime = icons[key], 'image/png'
            else:
                data = (f'<!doctype html><title>Surface {key}</title>'
                        f'<link rel="icon" href="/{key}.png" sizes="16x16">'
                        '<style>body{margin:32px;background:#eef3f8;font:16px sans-serif}'
                        'input{display:block;width:280px;height:36px;margin-top:12px}</style>'
                        '<label for="draft">Unsaved chrome surface fixture</label><input id="draft">'
                        '<script>window.surfaceToken=crypto.randomUUID()</script>').encode()
                mime = 'text/html; charset=utf-8'
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}'
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)


def check(result, case, name, actual, expected):
    result['checks'].append(dict(case=case, name=name, actual=actual,
                                 expected=expected, passed=actual == expected))


def row(tree, key):
    return n.one(tree, r'class_name=TabView name=Surface ' + key + r' name_from=')


def geometry(tree):
    tree.read()
    tabstrip = n.one(tree, r'class_name=VerticalTabStripRegionView ')
    back = n.one(tree, n.BUTTONS['back'])
    return {
        'window': tree.rect(n.one(tree, r'class_name=BrowserView ')),
        'content': tree.rect(n.one(tree, r'class_name=MultiContentsView ')),
        'rail': tree.rect(tree.nodes[tabstrip['parent']]),
        'top': tree.rect(tree.nodes[back['parent']]),
        'address': tree.rect(n.one(tree, r'class_name=LocationBarView ')),
        'tabs': {key: tree.rect(row(tree, key)) for key in COLORS},
    }


def capture(tree, inp, output, name):
    tree.read()
    (output / (name + '.txt')).write_text(tree.text)
    return inp.capture(output / (name + '.png'))


def page_focus(tree, inp):
    content = tree.rect(n.one(tree.read(), r'class_name=MultiContentsView '))
    inp.pointer.click(content['x'] + content['w'] / 2,
                      content['y'] + content['h'] / 2)


def clear_hover(tree, inp):
    content = tree.rect(n.one(tree.read(), r'class_name=MultiContentsView '))
    inp.move(content['x'] + content['w'] / 2, content['y'] + content['h'] / 2)
    time.sleep(.3)


def click_row(browser, tree, inp, pages, key, offset):
    rect = tree.rect(row(tree.read(), key))
    point = (rect['x'] + rect['w'] / 2, rect['y'] + offset)
    inp.pointer.click(*point)
    n.wait_for(lambda: browser.ev(n.JS['visible'], pages[key][0]),
               browser.process, timeout=5)
    assert all(not browser.ev(n.JS['visible'], target)
               for other, (target, _) in pages.items() if other != key)
    return point


def bbox(points):
    if not points:
        return None
    xs, ys = zip(*points)
    return {'x': min(xs), 'y': min(ys),
            'w': max(xs) - min(xs) + 1, 'h': max(ys) - min(ys) + 1}


def paint_difference(first, second, rect):
    box = (rect['x'], rect['y'], rect['x'] + rect['w'], rect['y'] + rect['h'])
    with first.crop(box) as a, second.crop(box) as b, ImageChops.difference(a, b) as diff:
        return bbox([(x, y) for y in range(rect['h']) for x in range(rect['w'])
                     if max(diff.getpixel((x, y))) > 2])


def favicon_pixels(shot, rect, color):
    points = [(x, y) for y in range(rect['h']) for x in range(rect['w'])
              if shot.getpixel((rect['x'] + x, rect['y'] + y)) == color]
    return {'bounds': bbox(points), 'pixels': len(points)}


def surface_pixels(shot, g):
    top, field = g['top'], g['address']
    background = shot.getpixel((top['x'] + 2, top['y'] + top['h'] // 2))
    # Clear of both the centered idle URL and the left-aligned focused text.
    # The same patch must not accidentally measure the blue text selection.
    start = field['w'] * 3 // 4
    patch = [shot.getpixel((field['x'] + x, field['y'] + y))
             for y in range(7, 21) for x in range(start, start + 16)]
    color, count = Counter(patch).most_common(1)[0]
    edges = [(top['x'] + x, top['y'] + y)
             for x in (2, top['w'] // 2, top['w'] - 2)
             for y in (0, 1, top['h'] - 2, top['h'] - 1)]
    delta = lambda pixel: max(abs(a - b) for a, b in zip(pixel, background))
    rim = [shot.getpixel((field['x'] + x, field['y'] + y))
           for x in range(start, start + 16) for y in (0, field['h'] - 1)]
    return {'toolbar': background, 'address_fill': color,
            'uniform_samples': count, 'sample_count': len(patch),
            'address_delta': delta(color),
            'address_rim_min_delta': min(delta(pixel) for pixel in rim),
            'dock_edge_max_delta': max(delta(shot.getpixel(point)) for point in edges)}


def retained(browser, pages):
    after = {key: browser.ev(DRAFT, target) for key, (target, _) in pages.items()}
    assert after == {key: before for key, (_, before) in pages.items()}, after
    return after


def run_case(browser, tree, inp, keys, pages, output, side, scheme, result):
    case = side + '-' + scheme
    n.activate(browser, tree, inp, pages['alpha'][0])
    clear_hover(tree, inp)
    g = geometry(tree)
    record = result['cases'][case] = {'geometry': g}
    solid = browser.platform == 'x11'
    check(result, case, 'window size', [g['window']['w'], g['window']['h']], [1200, 760])
    check(result, case, 'collapsed rail width', g['rail']['w'], 34)
    check(result, case, 'top row height', g['top']['h'], 34)
    check(result, case, 'page width', g['content']['w'], 1166)
    check(result, case, 'page leading edge', g['content']['x'], 34 if side == 'left' else 0)
    check(result, case, 'native address height', g['address']['h'], 28)
    for key, rect in g['tabs'].items():
        check(result, case, key + ' native tab size', [rect['w'], rect['h']], [22, 28])
        check(result, case, key + ' rail padding', rect['x'] - g['rail']['x'], 6)

    with capture(tree, inp, output, case + '-idle-alpha') as first:
        record['idle_pixels'] = surface_pixels(first, g)
        if solid:
            check(result, case, 'idle omnibox blends with dock', record['idle_pixels']['address_delta'] <= 2, True)
            check(result, case, 'solid dock has no edge stroke', record['idle_pixels']['dock_edge_max_delta'] <= 2, True)
        record['favicons'] = {}
        for key, rect in g['tabs'].items():
            icon = record['favicons'][key] = favicon_pixels(first, rect, COLORS[key])
            if solid:
                check(result, case, key + ' favicon bounds', icon['bounds'], {'x': 3, 'y': 6, 'w': 16, 'h': 16})
                check(result, case, key + ' full favicon painted', icon['pixels'], 256)
        inp.move(*tree.center(row(tree, 'bravo')))
        time.sleep(.35)
        with capture(tree, inp, output, case + '-hover-bravo') as hover:
            record['hover_paint'] = paint_difference(first, hover, g['tabs']['bravo'])
        if solid:
            check(result, case, 'hover fill bounds', record['hover_paint'], {'x': 0, 'y': 3, 'w': 22, 'h': 22})
        record['top_hit'] = click_row(browser, tree, inp, pages, 'bravo', 1)
        clear_hover(tree, inp)
        with capture(tree, inp, output, case + '-idle-bravo') as second:
            record['selected_paint'] = paint_difference(first, second, g['tabs']['alpha'])
        if solid:
            check(result, case, 'selected fill bounds', record['selected_paint'], {'x': 0, 'y': 3, 'w': 22, 'h': 22})

    record['bottom_hit'] = click_row(browser, tree, inp, pages, 'alpha', 26)
    record['retained_after_edge_clicks'] = retained(browser, pages)
    clear_hover(tree, inp)
    keys.key('l', control=True)
    focused = geometry(tree)
    field = n.one(tree, r'class_name=OmniboxViewViews ')
    selected = re.search(r'sel_start=(\d+) sel_end=(\d+)', field['line'])
    # The native plain-HTTP formatter omits the scheme, including on Ctrl+L.
    # Test the entire rendered address, not the longer location.href string.
    displayed_url = pages['alpha'][1]['url'].removeprefix('http://')
    check(result, case, 'Ctrl+L selects native URL', sorted(map(int, selected.groups())) if selected else None,
          [0, len(displayed_url)])
    n.check_field(tree, displayed_url)
    check(result, case, 'address focus preserves geometry', focused, g)
    with capture(tree, inp, output, case + '-focused') as shot:
        record['focused_pixels'] = surface_pixels(shot, focused)
    if solid:
        # Light editing fill and the solid toolbar are both white. The native
        # focus outline, sampled away from the text, identifies its surface.
        check(result, case, 'focused omnibox keeps visible outline', record['focused_pixels']['address_rim_min_delta'] >= 6, True)
    keys.type('draft')
    n.check_field(tree, 'draft')
    with capture(tree, inp, output, case + '-editing'):
        pass
    # Keep autocomplete open: clearing focus first could hide a hit-target bug.
    record['editing_tab_hit'] = click_row(browser, tree, inp, pages, 'bravo', 1)
    record['retained_after_address_edit'] = retained(browser, pages)
    click_row(browser, tree, inp, pages, 'alpha', 26)
    keys.key('l', control=True)
    keys.key('Escape')
    keys.key('Escape')
    page_focus(tree, inp)
    record['native_input_passed'] = True


def resize_rail(tree, inp, side, width):
    g = geometry(tree)
    handle = n.one(tree, r'class_name=ResizeArea name=Vertical Tab Strip Resize Handle ')
    start = tree.center(handle)
    delta = (width - g['rail']['w']) * (1 if side == 'left' else -1)
    inp.drag(start, (start[0] + delta, start[1]))
    return geometry(tree)


def run(args):
    args.browser = args.browser.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'executed-test.py').write_bytes(Path(__file__).read_bytes())
    result = {'passed': False, 'qualified': False, 'observe_only': args.observe_only,
              'browser': str(args.browser), 'binary_sha256': sha256(args.browser),
              'source_sha256': sha256(Path(__file__)), 'checks': [], 'cases': {},
              'scope': 'Private scale-1 light/dark chrome geometry/input; pixel assertions only on solid X11, no glass/touch/RTL/desktop qualification'}
    budget = n.Budget()
    def cancelled(*_):
        raise RuntimeError('Chrome surface smoke cancelled')
    previous = signal.signal(signal.SIGTERM, cancelled)
    try:
        with local_pages() as base, tempfile.TemporaryDirectory(prefix='profile-', dir=args.output) as tmp:
            profile = Path(tmp)
            (profile / 'Default').mkdir()
            (profile / 'Default/Preferences').write_text(json.dumps({
                'vertical_tabs': {'collapsed_state': True, 'uncollapsed_width': 220}}))
            with n.session(args, profile, args.output, n.FLAGS, result, budget) as (browser, tree, inp), Keyboard(inp) as keys:
                result['platform'] = browser.platform
                result['pixel_assertions'] = browser.platform == 'x11'
                result['owned_browser_pid'] = browser.process.pid
                appearance = Appearance(browser)
                appearance.assert_glass(True)
                pages = {}
                for key in COLORS:
                    target = n.request(browser.port, '/json/new?' + base + '/' + key, 'PUT')
                    n.wait_for(lambda: browser.ev('document.readyState==="complete" && !!window.surfaceToken', target),
                               browser.process, timeout=8)
                    n.activate(browser, tree, inp, target)
                    content = tree.rect(n.one(tree, r'class_name=MultiContentsView '))
                    field = browser.ev('(()=>{const r=document.querySelector("#draft").getBoundingClientRect();'
                                       'return {x:r.x+r.width/2,y:r.y+r.height/2}})()', target)
                    inp.pointer.click(content['x'] + field['x'], content['y'] + field['y'])
                    keys.type(key)
                    before = browser.ev(DRAFT, target)
                    assert before['value'] == key and before['token'], before
                    pages[key] = (target, before)
                layout = n.top_layout(browser.state()['layout'], ORDER)
                for side in ('left', 'right'):
                    for item in layout['items']:
                        if item['id'] in ('tabs', 'spaces'):
                            item['edge'] = side
                    n.apply(browser, layout)
                    for scheme, mode in (('light', 1), ('dark', 2)):
                        appearance.scheme(mode)
                        run_case(browser, tree, inp, keys, pages, args.output, side, scheme, result)
                    expanded = resize_rail(tree, inp, side, 220)
                    result['cases'][side + '-expanded'] = expanded
                    check(result, side, 'expanded rail width', expanded['rail']['w'], 220)
                    check(result, side, 'expanded page width', expanded['content']['w'], 980)
                    with capture(tree, inp, args.output, side + '-expanded'):
                        pass
                    collapsed = resize_rail(tree, inp, side, 34)
                    check(result, side, 'drag returns to collapsed width', collapsed['rail']['w'], 34)
                    retained(browser, pages)
                result['final_layout'] = browser.state()['layout']
            result['owned_browser_stopped'] = browser.process.poll() is not None
            prefs = json.loads((profile / 'Default/Preferences').read_text())['vertical_tabs']
            result['saved_sidebar'] = prefs
            check(result, 'saved', 'expanded width retained', prefs['uncollapsed_width'], 220)
            check(result, 'saved', 'collapsed state retained', prefs['collapsed_state'], True)
        check(result, 'runtime', 'binary unchanged', sha256(args.browser), result['binary_sha256'])
        result['passed'] = all(entry['passed'] for entry in result['checks'])
        result['qualified'] = result['passed'] and not args.observe_only
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
                          ('passed', 'qualified', 'observe_only', 'elapsed', 'failures', 'error')}, indent=2))
    return 0 if result['passed'] or args.observe_only else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe-only', action='store_true')
    raise SystemExit(run(parser.parse_args()))
