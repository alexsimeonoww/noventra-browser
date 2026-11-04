"""Check pin size, loaded-only Close and favicon color on an owned display."""

import http.server
from io import BytesIO
import json
from pathlib import Path
import tempfile
import threading
import time

from PIL import Image

import pin_close_smoke as close
import pinned_tabs_smoke as p
import profile_spaces_smoke as profiles


class Handler(p.Handler):
    def do_GET(self):
        if self.path == '/icon.png':
            icon = Image.new('RGB', (16, 16), (224, 25, 55))
            for x in range(8):
                for y in range(16):
                    icon.putpixel((x, y), (25, 90, 224))
            stream = BytesIO()
            icon.save(stream, format='PNG')
            data, kind = stream.getvalue(), 'image/png'
        elif self.path in ('/cookie', '/set'):
            return super().do_GET()
        else:
            title = 'pin ' + self.path.strip('/')
            data = ('<!doctype html><meta charset="utf-8"><title>' + title
                    + '</title><link rel="icon" href="/icon.png">'
                    '<input id="draft" value="original">'
                    '<script>window.pinToken=crypto.randomUUID();</script>').encode()
            kind = 'text/html; charset=utf-8'
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)


def owned_children(tree, owner, pattern):
    result = []
    for node in p.native.visible(tree, pattern):
        current = node
        while current and current != owner:
            current = tree.nodes.get(current['parent'])
        if current == owner:
            result.append(node)
    return result


def inspect(browser, tree, inp, name, output):
    owner = close.pin_node(browser, tree, name)
    tile = p.tab.bounds(tree, owner)
    inp.move(*p.tab.center(tile))
    time.sleep(.5)
    owner = close.pin_node(browser, tree, name)
    buttons = owned_children(tree, owner, r'class_name=TabCloseButton ')
    with inp.capture(output) as shot:
        crop = shot.crop((tile['x'], tile['y'], tile['x'] + tile['w'], tile['y'] + tile['h']))
        colored = sum(max(pixel[:3]) - min(pixel[:3]) > 70
                      for pixel in crop.convert('RGB').getdata())
    return {'tile': tile, 'close_count': len(buttons),
            'colored_pixels': colored}


def main():
    parser = p.cli(__doc__)
    parser.add_argument('--layout', choices=('sidebar', 'horizontal'), default='sidebar')
    parser.add_argument('--observe-only', action='store_true')
    parser.add_argument('--density', choices=('comfortable', 'compact'), default='comfortable')
    args = parser.parse_args()
    result = profiles.admit(args, Path(__file__))
    result.update(layout=args.layout, observe_only=args.observe_only, density=args.density)
    budget = p.Budget()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with tempfile.TemporaryDirectory(prefix='owned-pin-visual-', dir=args.output) as tmp:
            profile = Path(tmp)
            p.prefs(profile, width=144)
            pref = profile / 'Default/Preferences'
            value = json.loads(pref.read_text())
            value['noventra'] = {'browser': {'tab_density': int(args.density == 'comfortable')}}
            pref.write_text(json.dumps(value))
            extension = p.extension_fixture(profile)
            flags = (*p.native.FLAGS, '--load-extension=' + str(extension.resolve()))
            with p.session(args, profile, args.output / 'native', result, budget, flags) as (browser, tree, inp):
                if args.layout == 'horizontal':
                    p.native.apply(browser, p.native.top_layout(browser.state()['layout'],
                                                               (*p.native.TOP_ORDER, 'spaces')))
                api = p.tab.controller(browser)
                site = p.open_page(browser, tree, inp, base + '/red')
                p.pin(browser, api, site)
                other = p.open_page(browser, tree, inp, base + '/other')
                before = p.state(browser, other)
                loaded = inspect(browser, tree, inp, 'pin red', args.output / 'loaded.png')
                point = p.tab.center(loaded['tile'])
                inp.pointer.click(*point)
                time.sleep(.3)
                metadata = close.pinned_tab(api, base + '/red')
                center_activates = metadata['active'] and not metadata['discarded']
                if not center_activates:
                    site = close.activate_pin(browser, tree, inp, 'pin red', base + '/red')
                    p.ready(browser, site, base + '/red')
                p.tab.activate(browser, tree, inp, site)
                p.native_key(inp, 'w', ('Control_L',))
                close.unloaded(browser, api, base + '/red')
                closed = inspect(browser, tree, inp, 'pin red', args.output / 'closed.png')
                reopened = close.activate_pin(browser, tree, inp, 'pin red', base + '/red')
                p.ready(browser, reopened, base + '/red')
                p.tab.activate(browser, tree, inp, other)
                opened_again = inspect(browser, tree, inp, 'pin red', args.output / 'reopened.png')
                owner = close.pin_node(browser, tree, 'pin red')
                button = owned_children(tree, owner, r'class_name=TabCloseButton ')
                assert len(button) == 1, button
                inp.pointer.click(*p.tab.center(p.tab.bounds(tree, button[0])))
                closed_by_x = close.unloaded(browser, api, base + '/red')
                result['measurements'] = {'loaded': loaded, 'closed': closed, 'reopened': opened_again}
                checks = [
                    ('Loaded pin center activates its page and preserves the pin', center_activates),
                    ('Loaded pin exposes Close on hover', loaded['close_count'] == 1),
                    ('Closed pin hides Close even on hover', closed['close_count'] == 0),
                    ('Closed pin retains its colored favicon', closed['colored_pixels'] >= 100),
                    ('Reopening pin restores its Close button', opened_again['close_count'] == 1),
                    ('Clicking the pin X unloads the page and retains its shortcut',
                     closed_by_x['pinned'] and closed_by_x['discarded']),
                    ('Pin changes retain the other loaded document', p.state(browser, other) == before),
                ]
                if args.layout == 'sidebar':
                    size = 36 if args.density == 'comfortable' else 32
                    checks.append((args.density + ' pin fits a ' + str(size) + '-DIP square',
                                   loaded['tile']['w'] == loaded['tile']['h'] == size))
                for name, valid in checks:
                    result['checks'].append({'name': name, 'passed': bool(valid)})
                if not args.observe_only:
                    assert all(row['passed'] for row in result['checks']), result['checks']
            assert all(row['exit'] == 0 for row in result['normal_shutdowns'])
        result['passed'] = all(row['passed'] for row in result['checks'])
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        budget.close()
        p.write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
