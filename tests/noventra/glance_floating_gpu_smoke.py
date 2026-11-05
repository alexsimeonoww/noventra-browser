"""One native Glance case on the owned 1200x760 Hyprland/GPU display.

Run through with-private-theme-blur.py. The wrapper supplies a fresh HOME,
no-egress network namespace, GPU and virtual input; no host profile or pointer
is used. This case deliberately does not resize Wayland windows through CDP.
"""
import argparse
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import threading
import time
import urllib.parse

from PIL import Image

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / 'tests/noventra').is_dir())
sys.path.insert(0, str(ROOT / 'tests/noventra'))
from compact_chrome_smoke import apply, one, visible
from extension_smoke import request, wait_for
from glance_workflow_smoke import popup_origin
from native_graphics_readiness import native_graphics_browser
from portable_transfer_smoke import sha, write
from sidebar_unified_smoke import close_normally

WORK = ROOT / 'work/glass-appearance-2026-10-24'
spec = importlib.util.spec_from_file_location('glance_gpu', WORK / 'gpu-optical-control.py')
gpu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu)
optical = gpu.control.optical
SOURCE_RGB = (32, 136, 224)
TARGET_RGB = (215, 231, 247)
ACTIONS = ('Close preview (Esc)', 'Open in tab without reloading',
           'Open in split without reloading')
STATE = '''({url:location.href,marker:window.marker,origin:performance.timeOrigin,
  draft:document.querySelector('#draft').value,
  body:getComputedStyle(document.body).backgroundColor})'''


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path)
        self.server.requests.append(self.path)
        source = path.path == '/source'
        rgb = SOURCE_RGB if source else TARGET_RGB
        target = (f'http://127.0.0.2:{self.server.server_port}/target?'
                  + urllib.parse.urlencode({'identity': 'long-address-' * 24,
                                            'seed': path.query}))
        title = 'Glance GPU source' if source else 'Long preview identity ' * 24
        body = f'''<!doctype html><meta charset="utf-8"><title>{title}</title>
          <style>html,body{{margin:0;min-height:100%;background:rgb{rgb};
          font:14px sans-serif}}input,a{{position:absolute;left:32px}}
          input{{top:32px}}a{{top:82px;padding:12px;width:230px}}</style>
          <input id="draft" value="retained draft">
          <a id="external" href="{target}">Open local preview</a>
          <script>window.marker=crypto.randomUUID()</script>'''.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Keyboard:
    """Use the existing persistent virtual-keyboard helper's evdev protocol."""
    def __init__(self):
        self.process = subprocess.Popen([os.environ['NOVENTRA_TEST_KEYBOARD']],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.reply('ready')

    def reply(self, expected='ok'):
        assert select.select([self.process.stdout], [], [], 4)[0], 'Keyboard timeout'
        assert self.process.stdout.readline().strip() == expected

    def send(self, code, down):
        self.process.stdin.write(f'key {code} {int(down)}\n')
        self.process.stdin.flush()
        self.reply()

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=4)


def page_view(browser, tree, page):
    size = browser.ev('({w:innerWidth,h:innerHeight})', page)
    tree.read()
    matches = []
    for parent in visible(tree, r'class_name=ContentsContainerView '):
        identifier = next(key for key, value in tree.nodes.items() if value is parent)
        for node in tree.nodes.values():
            if (node['parent'] == identifier and
                    re.search(r'class_name=(?:WebView|ContentsWebView) ', node['line']) and
                    node['bounds'][2:] == (size['w'], size['h'])):
                matches.append(tree.rect(node))
    assert len(matches) == 1, (size, matches)
    return matches[0]


def near(a, b, tolerance=2):
    return max(abs(x - y) for x, y in zip(a, b)) <= tolerance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    args = parser.parse_args()
    assert os.environ.get('NOVENTRA_TEST_CONTROL') == '/work'
    assert os.environ.get('NOVENTRA_TEST_COMPOSITOR') == 'hyprland'
    assert os.environ.get('HOME') == '/tmp/nv-private/home'
    assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') == '1'
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    args.output.mkdir(exist_ok=False)
    result = {'passed': False, 'checks': [], 'states': [], 'scope': __doc__,
              'binary_sha256': args.expected_binary,
              'resources_sha256': args.expected_resources, 'test_sha256': sha(__file__)}

    def check(name, passed, actual=None):
        row = {'name': name, 'passed': bool(passed), 'actual': actual}
        result['checks'].append(row)
        assert passed, row

    pointer = keyboard = tree = None
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    server.requests = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        # The nested compositor is intentionally launched directly by its
        # isolated wrapper. Its startup warning can cover top-corner probes.
        # Clear only the owned compositor's notification overlay after independently
        # requiring an empty configuration-error list. Product rendering and
        # all optical assertions remain unchanged.
        ipc_spec = importlib.util.spec_from_file_location('glance_private_ipc',
            ROOT / 'work/grid-mirroring-theme-2026-10-31/with-private-theme-blur.py')
        ipc_api = importlib.util.module_from_spec(ipc_spec)
        ipc_spec.loader.exec_module(ipc_api)
        ipc = Path(os.environ['NOVENTRA_TEST_COMPOSITOR_IPC'])
        owner = int(os.environ['NOVENTRA_TEST_COMPOSITOR_PID'])
        assert ipc.is_relative_to(Path(os.environ['XDG_RUNTIME_DIR']) / 'hypr')
        ipc_api.verified_peer(ipc, owner)
        errors = json.loads(ipc_api.ipc_command(ipc, owner, 'j/configerrors'))
        assert not [error for error in errors if error.strip()], errors
        pointer = optical.NativeWaylandPointer()
        keyboard = Keyboard()
        profile = args.output / 'profile'
        (profile / 'Default').mkdir(parents=True)
        write(profile / 'Default/Preferences', {
            'noventra': {'browser': {'glass_enabled': True, 'link_preview': True,
                                     'link_preview_trigger': 0}},
            'browser': {'theme': {'color_scheme2': 2, 'is_grayscale2': True}},
            'helium': {'browser': {'layout': 2, 'zen_mode': True,
                'zen_mode_sidebar_pinned': True, 'zen_mode_top_chrome_pinned': True}},
            'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 180}})
        with native_graphics_browser(gpu, args.browser, profile, args.output / 'browser',
                ['--force-renderer-accessibility', '--force-device-scale-factor=1',
                 '--touch-ui=disabled', '--lang=en-US', '--ui-toolkit=gtk']) as browser:
            apply(browser, browser.state()['presets']['minimal-left'])
            tree = optical.NativeUITree(browser)

            def capture(label):
                path = args.output / (label + '.png')
                pointer.capture(path)
                tree.read()
                (args.output / (label + '-tree.txt')).write_text(tree.text)
                image = Image.open(path).convert('RGB')
                assert image.size == (1200, 760), image.size
                return image

            def targets():
                return [p for p in request(browser.port, '/json/list') if p['type'] == 'page']

            for label, action in zip(('close', 'expand', 'split'), ACTIONS, strict=True):
                url = f'http://127.0.0.1:{server.server_port}/source?seed={label}'
                source = request(browser.port, '/json/new?' + url, 'PUT')
                wait_for(lambda: browser.ev('document.readyState==="complete"&&!!document.querySelector("#draft")', source), browser.process)
                request(browser.port, '/json/activate/' + source['id'])
                browser.ev('document.querySelector("#draft").value=' + json.dumps(label + ' source') + ';true', source)
                source_state = browser.ev(STATE, source)
                source_box = page_view(browser, tree, source)
                point = browser.ev('(()=>{const r=document.querySelector("#external").getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2,url:document.querySelector("#external").href}})()', source)
                pointer.send('motion 1100 700 1200 760')
                if label == 'close':
                    pointer.capture(args.output / 'compositor-startup-notification.png')
                    errors = json.loads(ipc_api.ipc_command(ipc, owner, 'j/configerrors'))
                    assert not [error for error in errors if error.strip()], errors
                    reply = ipc_api.ipc_command(ipc, owner, '/dismissnotify')
                    write(args.output / 'owned-compositor-overlay-reset.json',
                          {'socket': str(ipc), 'verified_peer_pid': owner,
                           'configuration_errors': errors, 'command': '/dismissnotify',
                           'reply': reply, 'product_configuration_changed': False})
                    check('owned compositor startup notification is dismissed without configuration errors',
                          reply.strip() == 'ok', reply)
                    # Wait for the real notification fade, then require the
                    # uniform source's top reference to be optically uncovered.
                    deadline = time.monotonic() + 4
                    clear = False
                    while time.monotonic() < deadline:
                        probe = args.output / 'uncovered-source-reference.png'
                        pointer.capture(probe)
                        with Image.open(probe) as shot:
                            reference = shot.getpixel((source_box['x'] + source_box['w'] - 100,
                                                       source_box['y'] + 14))[:3]
                        if near(reference, SOURCE_RGB):
                            clear = True
                            break
                        time.sleep(.1)
                    check('top source reference is optically clear before preview measurements',
                          clear, reference)
                time.sleep(.35)
                with capture(label + '-source') as before:
                    baseline = before.getpixel((source_box['x'] + source_box['w'] // 2,
                                                source_box['y'] + source_box['h'] // 2))
                check(label + ': source fixture starts with its opaque RGB', near(baseline, SOURCE_RGB), baseline)
                keyboard.send(56, True)  # Linux evdev KEY_LEFTALT.
                try:
                    pointer.click(source_box['x'] + point['x'], source_box['y'] + point['y'])
                finally:
                    keyboard.send(56, False)

                preview = wait_for(lambda: next((p for p in targets() if p['url'] == point['url']), None), browser.process, timeout=8)
                wait_for(lambda: browser.ev('document.readyState==="complete"&&!!document.querySelector("#draft")', preview), browser.process)
                browser.ev('document.querySelector("#draft").value=' + json.dumps(label + ' preview') + ';true', preview)
                state = browser.ev(STATE, preview)
                pointer.send('motion 1100 700 1200 760')
                time.sleep(.4)
                with capture(label + '-preview') as image:
                    root = one(tree, r'class_name=RootView name=Link preview name_from=')
                    origin = popup_origin(tree, source_box, source_box)
                    page = tree.rect(one(tree, r'class_name=\S+ name=Preview page name_from='))
                    controls = tree.rect(one(tree, r'class_name=\S+ name=Preview controls name_from='))
                    check(label + ': page fills the headerless popup height', page['x'] == page['y'] == 0 and page['h'] == root['bounds'][3], page)
                    check(label + ': three floating actions occupy a transparent 56x144 strip', controls['w'] == 56 and controls['h'] == 144 and controls['x'] == page['w'] and controls['y'] == 15, controls)
                    check(label + ': preview has no navigation-arrow buttons', not visible(tree, r'class_name=ImageButton name=(?:Back|Forward) in preview name_from='))
                    inner = browser.ev('({w:innerWidth,h:innerHeight})', preview)
                    check(label + ': website has only the 1px frame inset', inner == {'w': page['w'] - 2, 'h': page['h'] - 2}, inner)
                    preview_root = one(tree, r'class_name=PreviewTab ')
                    preview_id = next(key for key, node in tree.nodes.items() if node is preview_root)
                    def in_preview(node):
                        while node and node['parent'] is not None:
                            if node['parent'] == preview_id:
                                return True
                            node = tree.nodes.get(node['parent'])
                        return False
                    labels = [n['line'] for n in tree.nodes.values()
                              if ' INVISIBLE ' not in n['line'] and
                              re.search(r'class_name=Label(?: |$)', n['line']) and in_preview(n)]
                    check(label + ': long title/address creates no clipped header labels', not labels, labels)
                    buttons = {}
                    for index, name in enumerate(ACTIONS):
                        rect = tree.rect(one(tree, r'class_name=ImageButton name=' + re.escape(name) + r' name_from='))
                        buttons[name] = rect
                        check(label + ': ' + name + ' has a full native target', rect == {'x': controls['x'] + 12, 'y': controls['y'] + 12 + 44 * index, 'w': 32, 'h': 32}, rect)
                    def pixel(x, y):
                        return image.getpixel((origin['x'] + x, origin['y'] + y))
                    outside_x = root['bounds'][2] + 7
                    for index, name in enumerate(ACTIONS):
                        rect = buttons[name]
                        check(label + ': ' + name + ' has a round transparent corner', near(pixel(rect['x'] + 1, rect['y'] + 1), pixel(outside_x, rect['y'] + 1)))
                        if index:
                            gap_y = rect['y'] - 6
                            check(label + ': source remains visible between separate round buttons', near(pixel(rect['x'] + 16, gap_y), pixel(outside_x, gap_y)))
                    references = []
                    for y in (controls['h'] // 2, page['h'] // 2):
                        outside = pixel(outside_x, y)
                        gutter = [pixel(page['w'] + x, y) for x in (2, 3, 4, 5)]
                        references.append({'y': y, 'outside': outside, 'gutter': gutter})
                        check(label + ': gutter reveals source beside and below controls at y=' + str(y), all(near(value, outside) for value in gutter), references[-1])
                    # Radius12 frame and native WebView radius11 must both expose
                    # the underlying flat source at all four page corners.
                    check(label + ': top corner reference is unobstructed by compositor overlays',
                          near(pixel(outside_x, 2), pixel(outside_x, page['h'] // 2)),
                          {'top': pixel(outside_x, 2), 'middle': pixel(outside_x, page['h'] // 2)})
                    corners = [(2, 2), (page['w'] - 3, 2),
                               (2, page['h'] - 3), (page['w'] - 3, page['h'] - 3)]
                    corner_pixels = [{'point': xy, 'pixel': pixel(*xy),
                                      'source': pixel(outside_x, xy[1])} for xy in corners]
                    check(label + ': GPU page corners remain rounded rather than rectangular', all(near(v['pixel'], v['source']) for v in corner_pixels), corner_pixels)
                    cap_corner = pixel(controls['x'] + 1, controls['y'] + 1)
                    check(label + ': floating control area has a transparent corner', near(cap_corner, pixel(outside_x, controls['y'] + 1)), cap_corner)
                    check(label + ': page paint reaches both edges without a title bar', near(pixel(page['w'] // 2, 2), TARGET_RGB) and near(pixel(page['w'] // 2, page['h'] - 3), TARGET_RGB))
                    result['states'].append({'label': label, 'source': source_box,
                        'origin': origin, 'page': page, 'controls': controls,
                        'inner': inner, 'gutter': references, 'corners': corner_pixels})
                check(label + ': opening preview preserves source identity and draft', browser.ev(STATE, source) == source_state)
                button = buttons[action]
                pointer.click(origin['x'] + button['x'] + 16,
                              origin['y'] + button['y'] + 16)
                wait_for(lambda: not visible(tree.read(), r'class_name=PreviewTab '), browser.process, timeout=6)
                if label == 'close':
                    wait_for(lambda: not any(p['id'] == preview['id'] for p in targets()), browser.process, timeout=6)
                    check('Close preserves source document, draft and body RGB', browser.ev(STATE, source) == source_state)
                    with capture('close-restored') as restored:
                        value = restored.getpixel((source_box['x'] + source_box['w'] // 2,
                                                  source_box['y'] + source_box['h'] // 2))
                    check('Close removes preview tint from ordinary source pixels', near(value, baseline, 1), value)
                else:
                    check(label + ': promotes the same target and document without reloading', any(p['id'] == preview['id'] for p in targets()) and browser.ev(STATE, preview) == state)
                    check(label + ': retains the original source document and RGB', browser.ev(STATE, source) == source_state)
                    if label == 'split':
                        wait_for(lambda: len(visible(tree.read(), r'class_name=ContentsContainerView ')) == 2, browser.process, timeout=6)
                        check('Split displays both retained documents', all(browser.ev('document.visibilityState==="visible"', page) for page in (source, preview)))
                    with capture(label + '-promoted'):
                        pass
            result['normal_close_exit'] = close_normally(browser)
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        if pointer:
            try:
                pointer.capture(args.output / 'failure.png')
            except BaseException as capture_error:
                result['capture_error'] = repr(capture_error)
        raise
    finally:
        for obj in (keyboard, pointer):
            if obj:
                obj.close()
        server.shutdown()
        server.server_close()
        thread.join(timeout=4)
        result['requests'] = server.requests
        write(args.output / 'result.json', result)
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
