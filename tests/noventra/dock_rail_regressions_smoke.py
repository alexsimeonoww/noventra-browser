"""Native compact-rail regressions: constrained spaces and extension reordering.

Runs only on an owned X11 or Wayland display with disposable profiles/extensions.
"""
import argparse
import ctypes as C
import json
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image
from dock_controls_smoke import Pointer
from extension_smoke import ROOT, request
from layout_test_support import LayoutBrowser, NativeWaylandPointer


class Input:
    def __init__(self, platform):
        self.platform = platform
        self.pointer = NativeWaylandPointer() if platform == 'wayland' else Pointer()

    def move(self, x, y):
        if self.platform == 'wayland':
            self.pointer.send(f'motion {int(x)} {int(y)} 1200 760')
        else:
            p = self.pointer
            p.xt.XTestFakeMotionEvent(p.d, -1, int(x), int(y), 0)
            p.x.XFlush(p.d)

    def button(self, down):
        if self.platform == 'wayland':
            self.pointer.send(f'button {int(down)}')
        else:
            p = self.pointer
            p.xt.XTestFakeButtonEvent(p.d, 1, int(down), 0)
            p.x.XFlush(p.d)

    def drag(self, start, end):
        self.move(*start)
        time.sleep(.2)
        self.button(True)
        for step in range(1, 31):
            self.move(*(a + (b - a) * step / 30 for a, b in zip(start, end)))
            time.sleep(.03)
        time.sleep(.2)
        self.button(False)
        time.sleep(.7)

    def capture(self, path):
        if self.platform == 'wayland':
            self.pointer.capture(path)
        else:
            subprocess.run(['import', '-window', 'root', str(path)], check=True)
        return Image.open(path).convert('RGB')

    def resize(self, width, height):
        if self.platform == 'wayland':
            # The owned Sway fixture normally tiles its single browser window.
            subprocess.run(['swaymsg', 'floating enable, resize set '
                            f'{width} {height}, move position 0 0'], check=True, stdout=subprocess.DEVNULL)
        else:
            p = self.pointer
            p.x.XGetInputFocus.argtypes = [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)]
            p.x.XResizeWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_uint, C.c_uint]
            win, revert = C.c_ulong(), C.c_int()
            p.x.XGetInputFocus(p.d, C.byref(win), C.byref(revert))
            p.x.XResizeWindow(p.d, win.value, width, height)
            p.x.XFlush(p.d)
        time.sleep(.8)

    def close(self):
        self.pointer.close()


def apply_edge(browser, edge, order):
    layout = browser.state()['layout']
    layout['items'].sort(key=lambda i: order.index(i['id']) if i['id'] in order else 100)
    for n, item in enumerate(layout['items']):
        item.update(edge='left' if item['id']=='spaces' and edge=='top' else edge, order=n, visible=item['id'] in order)
    assert browser.apply(layout)['status'] == 'ok'
    time.sleep(.4)


def spaces(browser_path, output, checks):
    with tempfile.TemporaryDirectory(prefix='spaces-', dir=output) as tmp:
        profile = Path(tmp)
        (profile / 'Default').mkdir()
        (profile / 'Default/Preferences').write_text(json.dumps({'vertical_tabs': {'collapsed_state': True}}))
        with LayoutBrowser(browser_path, profile, output) as b:
            inp = Input(b.platform)
            try:
                # Enough controls to require extra columns at 700x480. The tab
                # strip remains compact; the space switcher must adapt with it.
                controls = ['back', 'forward', 'reload', 'home', 'new-tab', 'menu',
                            'customize', 'profile', 'action:history', 'action:downloads',
                            'action:bookmarks', 'action:print', 'action:copy-url', 'action:save-page']
                apply_edge(b, 'left', ['tabs', *controls, 'address', 'spaces', 'extensions', 'action:settings'])
                page = request(b.port, '/json/new?about:blank', 'PUT')
                b.ev('document.body.innerHTML="<input value=kept>";window.railDraft=42;true', page)
                inp.pointer.click(600, 400)
                inp.resize(700, 480)
                geometry = b.ev('({w:outerWidth,h:outerHeight,inner:innerWidth})', page)
                if b.platform == 'x11':
                    assert geometry['w'] == 700 and geometry['h'] == 480, geometry
                else:
                    # Ozone reports fallback outer frame insets on private
                    # Wayland. Check the compositor's actual client allocation.
                    tree = json.loads(subprocess.check_output(['swaymsg', '-t', 'get_tree', '-r'], text=True))
                    def nodes(node):
                        yield node
                        for child in node.get('nodes', []) + node.get('floating_nodes', []):
                            yield from nodes(child)
                    client = next(n for n in nodes(tree) if n.get('app_id') == 'noventra-browser')
                    (output / 'compositor-geometry.json').write_text(json.dumps(client['window_rect'], indent=2))
                    assert client['window_rect']['width'] == 700 and client['window_rect']['height'] == 480, client['window_rect']
                rail_width = 700 - geometry['inner']
                assert rail_width >= 100, ('Fixture did not exercise a widened compact rail', geometry)
                shot = output / 'short-compact.png'
                inp.capture(shot)
                text = subprocess.check_output(['tesseract', str(shot), 'stdout'], stderr=subprocess.DEVNULL, text=True)
                assert 'Personal' not in text, ('Compact account-space control exposes a clipped expanded row', text)
                assert b.ev('window.railDraft===42 && document.querySelector("input").value==="kept"', page)
                checks.append('A short, crowded compact sidebar keeps account spaces compact and retains the page draft')
                inp.resize(1200, 760)
                rail_width = 1200 - b.ev('innerWidth', page)
                inp.drag((rail_width - 1, 50), (260, 50))
                expanded = output / 'spaces-expanded.png'
                inp.capture(expanded)
                text = subprocess.check_output(['tesseract', str(expanded), 'stdout'], stderr=subprocess.DEVNULL, text=True)
                assert 'Personal' in text, ('Expanding no longer restores labeled account spaces', text)
                checks.append('Expanding a crowded icon rail restores labeled account spaces')
            finally:
                inp.close()


def icon_order(shot, x, height, colors):
    found = []
    for name, expected in colors.items():
        pixels = [y for y in range(height) for column in range(max(0, x-20), min(shot.width, x+22))
                  if max(abs(a-b) for a, b in zip(shot.getpixel((column, y)), expected)) < 20]
        # Each fixture is an opaque 16x16 square. Allow small raster differences
        # while rejecting the old five-pixel sliver at the edge of the rail.
        assert len(pixels) >= 240, ('Pinned fixture icon is clipped or missing', name, len(pixels))
        found.append((sum(pixels)/len(pixels), name))
    return [name for _, name in sorted(found)]


def extensions(browser_path, output, checks):
    colors = {'Alpha': (230, 30, 40), 'Bravo': (20, 190, 60), 'Charlie': (30, 80, 230)}
    with tempfile.TemporaryDirectory(prefix='extensions-', dir=output) as tmp:
        base = Path(tmp)
        fixtures = []
        for name, color in colors.items():
            fixture = base / name
            shutil.copytree(ROOT / 'tests/fixtures/extension-smoke', fixture)
            manifest = json.loads((fixture / 'manifest.json').read_text())
            manifest['name'] = 'Rail drag ' + name
            manifest['action']['default_icon'] = {'16': 'icon.png'}
            Image.new('RGB', (16, 16), color).save(fixture / 'icon.png')
            (fixture / 'manifest.json').write_text(json.dumps(manifest))
            fixtures.append(fixture)
        with LayoutBrowser(browser_path, base / 'profile', output,
                           ['--load-extension=' + ','.join(str(p) for p in fixtures)]) as b:
            inp = Input(b.platform)
            try:
                ids = {m['label'].removeprefix('Rail drag '): m['id'].split(':', 1)[1]
                       for c in b.state()['capabilities'] for m in c.get('members', [])
                       if m['label'].startswith('Rail drag ')}
                assert set(ids) == set(colors), ids
                manager = request(b.port, '/json/new?chrome://extensions', 'PUT')
                time.sleep(.8)
                b.ev('(async()=>{const infos=await chrome.developerPrivate.getExtensionsInfo({includeDisabled:true,includeTerminated:true});'
                     'for(const i of infos)if(i.pinnedToToolbar)await chrome.developerPrivate.updateExtensionConfiguration({extensionId:i.id,pinnedToToolbar:false});'
                     'for(const id of ' + json.dumps([ids[n] for n in colors]) + ')await chrome.developerPrivate.updateExtensionConfiguration({extensionId:id,pinnedToToolbar:true});return true;})()', manager)
                request(b.port, '/json/close/' + manager['id'])
                # Exercise drag ordering independently of closing-tab reparenting.
                # That animation race has its own no-delay regression below.
                time.sleep(.6)
                apply_edge(b, 'left', ['extensions', 'tabs'])
                page = request(b.port, '/json/new?about:blank', 'PUT')
                inp.pointer.click(600, 400)
                rail_width = 1200 - b.ev('innerWidth', page)
                # Account for the X11 frame pixel and grab inside the resize
                # area's 8-DIP hit target, rather than the content boundary.
                inp.drag((rail_width - 4, 300), (45, 300))
                compact_width = 1200 - b.ev('innerWidth', page)
                inp.capture(output / 'extensions-collapse.png')
                assert 40 <= compact_width <= 48, ('Compact rail did not collapse', rail_width, compact_width)
                for edge, x in [('left', 20), ('right', 1180)]:
                    if edge == 'right':
                        apply_edge(b, edge, ['extensions', 'tabs'])
                        request(b.port, '/json/activate/' + page['id'])
                    before = inp.capture(output / (edge + '-extension-before.png'))
                    assert icon_order(before, x, 130, colors) == ['Alpha', 'Bravo', 'Charlie']
                    checks.append('Three pinned extension icons fit fully inside the ' + edge + ' icon rail')
                    inp.drag((x, 14), (x, 85))
                    after = inp.capture(output / (edge + '-extension-after.png'))
                    assert icon_order(after, x, 130, colors) == ['Bravo', 'Charlie', 'Alpha'], 'Dragging down the vertical icon stack did not reorder pinned extensions'
                    checks.append('Native downward dragging reorders pinned extensions on the ' + edge)
                    inp.drag((x, 70), (x, 8))
                    restored = inp.capture(output / (edge + '-extension-restored.png'))
                    assert icon_order(restored, x, 130, colors) == ['Alpha', 'Bravo', 'Charlie'], 'Dragging up the vertical icon stack did not restore extension order'
                    checks.append('Native upward dragging restores pinned extension order on the ' + edge)
            finally:
                inp.close()


def closing(browser_path, output, checks):
    with tempfile.TemporaryDirectory(prefix='closing-', dir=output) as tmp:
        with LayoutBrowser(browser_path, Path(tmp), output) as b:
            page = request(b.port, '/json/new?about:blank', 'PUT')
            b.ev('window.retainedDraft=42;document.body.innerHTML="<input value=kept>";true', page)
            for edge in next(c['edges'] for c in b.state()['capabilities'] if c['id']=='tabs'):
                layout = b.state()['layout']
                for n, item in enumerate(layout['items']):
                    item.update(edge='left' if item['id']=='spaces' and edge=='top' else edge, order=n, visible=item['id'] in ('tabs', 'address'))
                doomed = request(b.port, '/json/new?about:blank', 'PUT')
                time.sleep(.15)
                request(b.port, '/json/close/' + doomed['id'])
                # Apply while the tab view is animating closed. Its backing tab
                # is already gone, but the closing view is still being painted.
                assert b.apply(layout)['status'] == 'ok'
                time.sleep(.4)
                assert b.process.poll() is None, 'Reparenting a closing tab crashed the browser'
                assert b.ev('window.retainedDraft===42 && document.querySelector("input").value==="kept"', page)
                checks.append('Closing a tab then applying the ' + edge + ' layout preserves the browser and retained draft')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('spaces', 'extensions', 'closing', 'all'), default='all')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'checks': []}
    try:
        for name, case in [('spaces', spaces), ('extensions', extensions), ('closing', closing)]:
            if args.case in ('all', name):
                folder = args.output / name
                folder.mkdir(exist_ok=True)
                case(args.browser, folder, result['checks'])
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
