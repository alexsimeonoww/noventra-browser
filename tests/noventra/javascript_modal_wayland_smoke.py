"""Visible close confirmations on zero-origin and shifted Wayland outputs.

Run inside work/grid-mirroring-theme-2026-10-31/with-private-theme-blur.py with
exact runtime hashes and a new --output. The wrapper owns the HOME, network,
compositor and native input helpers. No personal browser or desktop is used.
"""

import argparse
import csv
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import re
import select
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time

from PIL import Image

from compact_chrome_smoke import Budget, TOP_ORDER, apply, top_layout
from extension_smoke import request, wait_for
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree
from sidebar_unified_smoke import close_normally


ORIGINS = (('origin', 0, 0), ('shifted', 4137, -2462))
LEAVE = r'class_name=MdTextButton name=Leave name_from='
CANCEL = r'class_name=MdTextButton name=Cancel name_from='
STATE = '''({url:location.href,token:window.modalToken,
  origin:performance.timeOrigin,draft:document.querySelector('#draft').value})'''


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def check(result, name, passed, actual=None):
    row = {'name': name, 'passed': bool(passed), 'actual': actual}
    result['checks'].append(row)
    assert passed, row


class Compositor:
    def __init__(self):
        assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') == '1'
        assert os.environ.get('NOVENTRA_TEST_COMPOSITOR') == 'hyprland'
        assert os.environ.get('NOVENTRA_TEST_CONTROL') == '/work'
        assert os.environ.get('HOME') == '/tmp/nv-private/home'
        runtime = Path(os.environ['XDG_RUNTIME_DIR']).resolve(strict=True)
        assert str(runtime).startswith('/tmp/nv-')
        signature = os.environ['HYPRLAND_INSTANCE_SIGNATURE']
        assert signature and Path(signature).name == signature
        self.endpoint = runtime/'hypr'/signature/'.socket.sock'
        assert self.endpoint == Path(os.environ['NOVENTRA_TEST_COMPOSITOR_IPC'])
        info = self.endpoint.stat()
        assert stat.S_ISSOCK(info.st_mode) and info.st_uid == os.getuid()
        self.pid = int(os.environ['NOVENTRA_TEST_COMPOSITOR_PID'])
        assert self.pid > 1
        self.set_origin(0, 0)

    def command(self, command):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(4)
            connection.connect(str(self.endpoint))
            pid, uid, _ = struct.unpack('3i', connection.getsockopt(
                socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            assert pid == self.pid and uid == os.getuid(), 'Unowned compositor IPC'
            connection.sendall(command.encode())
            data = bytearray()
            deadline = time.monotonic() + 4
            while True:
                assert time.monotonic() < deadline, 'Compositor reply deadline'
                block = connection.recv(65536)
                if not block:
                    break
                data.extend(block)
                assert len(data) <= 1024 * 1024
        return data.decode()

    def set_origin(self, x, y):
        command = ('/eval hl.monitor({output="WAYLAND-1",mode="1200x760@60",'
                   f'position="{x}x{y}",scale=1}})')
        assert self.command(command).strip() == 'ok'
        deadline = time.monotonic() + 4
        while True:
            monitors = json.loads(self.command('j/monitors'))
            assert len(monitors) == 1, monitors
            monitor = monitors[0]
            if all(monitor.get(k) == value for k, value in {
                'name': 'WAYLAND-1', 'width': 1200, 'height': 760,
                'x': x, 'y': y, 'scale': 1}.items()):
                return monitor
            assert time.monotonic() < deadline, monitors
            time.sleep(.1)


class NativeInput:
    def __init__(self):
        self.helpers = []
        try:
            for kind in ('pointer', 'keyboard'):
                binary = Path(os.environ['NOVENTRA_TEST_' + kind.upper()])
                assert binary == Path('/work/test-' + kind) and binary.is_file()
                helper = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, text=True, start_new_session=True)
                self.helpers.append(helper)
                setattr(self, kind, helper)
            self.reply(self.keyboard, 'ready')
        except BaseException:
            self.close()
            raise

    @staticmethod
    def reply(helper, expected='ok'):
        assert select.select([helper.stdout], [], [], 4)[0], 'Native helper deadline'
        assert helper.stdout.readline().strip() == expected

    def send(self, helper, command):
        helper.stdin.write(command + '\n')
        helper.stdin.flush()
        self.reply(helper)

    def key(self, code, modifiers=()):
        for modifier in modifiers:
            self.send(self.keyboard, f'key {modifier} 1')
        self.send(self.keyboard, f'key {code} 1')
        self.send(self.keyboard, f'key {code} 0')
        for modifier in reversed(modifiers):
            self.send(self.keyboard, f'key {modifier} 0')
        time.sleep(.2)

    def click(self, x, y):
        assert 0 <= x < 1200 and 0 <= y < 760
        for command in (f'motion {int(x)} {int(y)} 1200 760', 'button 1', 'button 0'):
            self.send(self.pointer, command)
        time.sleep(.3)

    @staticmethod
    def capture(path):
        subprocess.run(['grim', '-o', 'WAYLAND-1', str(path)],
                       check=True, timeout=6)

    def close(self):
        codes = []
        for helper in reversed(self.helpers):
            try:
                helper.stdin.close()
            except BrokenPipeError:
                pass
            try:
                helper.wait(timeout=4)
            except subprocess.TimeoutExpired:
                helper.kill()
                helper.wait(timeout=4)
            codes.append(helper.returncode)
        return codes


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        name = self.path.strip('/')
        if name not in ('source', 'sibling'):
            self.send_error(404)
            return
        body = f'''<!doctype html><meta charset="utf-8"><title>Modal {name}</title>
          <style>body{{background:#eef3f8;font:16px sans-serif}}</style>
          <h1>Modal {name}</h1><input id="draft" value="retained {name} draft">
          <script>window.modalToken=crypto.randomUUID()</script>'''.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def pages(browser):
    return [page for page in request(browser.port, '/json/list') if page['type'] == 'page']


def open_page(browser, url):
    page = request(browser.port, '/json/new?' + url, 'PUT')
    wait_for(lambda: browser.ev('document.readyState==="complete"&&'
        '!!document.querySelector("#draft")', page), browser.process, timeout=8)
    return page


def buttons(tree):
    return {name: [node['line'] for node in tree.nodes.values()
                   if re.search(pattern, node['line']) and ' INVISIBLE ' not in node['line']]
            for name, pattern in (('Leave', LEAVE), ('Cancel', CANCEL))}


def painted_buttons(path, tree):
    with Image.open(path) as image:
        assert image.size == (1200, 760), image.size
        pixels = image.convert('RGB')
    bright = sum(min(pixel) > 235 for pixel in pixels.crop(
        (200, 0, 1000, 220)).get_flattened_data())
    ocr = subprocess.run(['tesseract', str(path), 'stdout', '--psm', '11', 'tsv'],
                         capture_output=True, text=True, check=True, timeout=10)
    path.with_name(path.stem + '-ocr.tsv').write_text(ocr.stdout)
    words = []
    for row in csv.DictReader(io.StringIO(ocr.stdout), delimiter='\t', quoting=csv.QUOTE_NONE):
        text = re.sub(r'[^A-Za-z]', '', row.get('text', ''))
        if text in ('Leave', 'Cancel'):
            words.append({'text': text, **{key: int(row[key])
                         for key in ('left', 'top', 'width', 'height')}})
    pairs = [(cancel, leave) for cancel in words for leave in words
             if cancel['text'] == 'Cancel' and leave['text'] == 'Leave'
             and abs(cancel['top'] + cancel['height']/2 -
                     leave['top'] - leave['height']/2) <= 20]
    crop_reads = []
    if not pairs:
        controls = {name: [node for node in tree.nodes.values()
                          if re.search(pattern, node['line']) and ' INVISIBLE ' not in node['line']]
                    for name, pattern in (('Leave', LEAVE), ('Cancel', CANCEL))}
        assert all(len(nodes) == 1 for nodes in controls.values()), controls
        cancel_rect = tree.rect(controls['Cancel'][0])
        leave_rect = tree.rect(controls['Leave'][0])
        for index, cancel in enumerate([word for word in words if word['text'] == 'Cancel']):
            center_x = cancel['left'] + cancel['width']/2 + (
                leave_rect['x'] + leave_rect['w']/2 - cancel_rect['x'] - cancel_rect['w']/2)
            center_y = cancel['top'] + cancel['height']/2 + (
                leave_rect['y'] + leave_rect['h']/2 - cancel_rect['y'] - cancel_rect['h']/2)
            box = (int(center_x - leave_rect['w']/2 - 4),
                   int(center_y - leave_rect['h']/2 - 4),
                   int(center_x + leave_rect['w']/2 + 4),
                   int(center_y + leave_rect['h']/2 + 4))
            if box[0] < 0 or box[1] < 0 or box[2] > 1200 or box[3] > 760:
                continue
            crop_path = path.with_name(path.stem + f'-leave-{index}-ocr.png')
            crop = pixels.crop(box)
            crop.resize((crop.width*4, crop.height*4), Image.Resampling.LANCZOS).save(crop_path)
            read = subprocess.run(['tesseract', str(crop_path), 'stdout', '--psm', '7', 'tsv'],
                                  capture_output=True, text=True, check=True, timeout=10)
            crop_path.with_suffix('.tsv').write_text(read.stdout)
            crop_reads.append({'box': box, 'capture': crop_path.name})
            for word in csv.DictReader(io.StringIO(read.stdout), delimiter='\t', quoting=csv.QUOTE_NONE):
                if re.sub(r'[^A-Za-z]', '', word.get('text', '')) != 'Leave':
                    continue
                leave = {'text': 'Leave', 'left': box[0] + int(word['left'])/4,
                         'top': box[1] + int(word['top'])/4,
                         'width': int(word['width'])/4, 'height': int(word['height'])/4}
                words.append(leave)
                pairs.append((cancel, leave))
    pixels.close()
    return {'painted_labels': words, 'bright_dialog_pixels': bright,
            'button_pairs': pairs, 'leave_crop_reads': crop_reads}


def capture_modal(browser, tree, native, folder, label, result):
    wait_for(lambda: all(buttons(tree.read()).values()), browser.process, timeout=6)
    path = folder/(label + '.png')
    (folder/(label + '-tree.txt')).write_text(tree.text)
    native.capture(path)
    measurement = {'native_buttons': buttons(tree), **painted_buttons(path, tree)}
    result.setdefault('dialogs', []).append({'capture': label, **measurement})
    check(result, label + ': Leave and Cancel are painted inside the viewport',
          bool(measurement['button_pairs']), measurement)
    check(result, label + ': Leave is the native default action',
          any(' DEFAULT ' in line for line in measurement['native_buttons']['Leave']))
    return measurement['button_pairs'][0]


def switch_away_and_back(browser, source, native, result, label):
    native.key(15, (29,))
    wait_for(lambda: browser.ev('document.visibilityState==="hidden"', source),
             browser.process, timeout=5)
    native.key(15, (29, 42))
    wait_for(lambda: browser.ev('document.visibilityState==="visible"', source),
             browser.process, timeout=5)
    check(result, label + ': native tab switching responds after the modal closes',
          browser.process.poll() is None)


def run_case(args, compositor, native, server, result, name, x, y, budget):
    folder = args.output/name
    folder.mkdir()
    case = {'name': name, 'origin': [x, y], 'checks': []}
    result['cases'].append(case)
    case['monitor'] = compositor.set_origin(x, y)
    with tempfile.TemporaryDirectory(prefix='owned-modal-', dir=folder) as temporary:
        profile = Path(temporary)
        (profile/'Default').mkdir()
        write(profile/'Default/Preferences', {
            'helium': {'browser': {'layout': 2, 'zen_mode': True,
                'zen_mode_sidebar_pinned': True, 'zen_mode_top_chrome_pinned': True}},
            'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 160}})
        flags = ('--force-renderer-accessibility', '--force-device-scale-factor=1',
                 '--touch-ui=disabled', '--lang=en-US', '--disable-background-networking')
        with LayoutBrowser(args.browser, profile, folder/'browser', flags) as browser:
            source = None
            try:
                if args.layout == 'horizontal':
                    apply(browser, top_layout(browser.state()['layout'], (*TOP_ORDER, 'spaces')))
                tree = NativeUITree(browser)
                base = 'http://127.0.0.1:' + str(server.server_port)
                sibling = open_page(browser, base + '/sibling')
                source = open_page(browser, base + '/source')
                sibling_before = browser.ev(STATE, sibling)
                request(browser.port, '/json/activate/' + source['id'])
                native.click(600, 250)
                native.key(30)
                check(case, 'fixture has genuine native user activation',
                      browser.ev('navigator.userActivation.hasBeenActive', source))
                before = browser.ev(STATE, source)
                before_tabs = {page['id'] for page in pages(browser)}
                browser.ev('window.modalBeforeUnloadCount=0;window.onbeforeunload=e=>{'
                           '++window.modalBeforeUnloadCount;e.preventDefault();'
                           'e.returnValue="pending"};true', source)
                native.capture(folder/'before-close.png')
                for label, action in (('cancel', 'cancel'), ('escape', 'escape'), ('leave', 'leave')):
                    native.key(17, (29,))
                    cancel, _ = capture_modal(browser, tree, native, folder, label, case)
                    if action == 'cancel':
                        native.click(cancel['left'] + cancel['width']/2,
                                     cancel['top'] + cancel['height']/2)
                    else:
                        native.key(1 if action == 'escape' else 28)
                    wait_for(lambda: not any(buttons(tree.read()).values()),
                             browser.process, timeout=5)
                    check(case, label + ': sibling document and draft survive',
                          browser.ev(STATE, sibling) == sibling_before)
                    if action != 'leave':
                        check(case, label + ': original document and draft survive',
                              browser.ev(STATE, source) == before)
                        switch_away_and_back(browser, source, native, case, label)
                    else:
                        wait_for(lambda: source['id'] not in {p['id'] for p in pages(browser)},
                                 browser.process, timeout=6)
                        check(case, 'Leave closes only the intended target',
                              {page['id'] for page in pages(browser)} == before_tabs - {source['id']}
                              and browser.process.poll() is None)
                        request(browser.port, '/json/activate/' + sibling['id'])
                        switch_away_and_back(browser, sibling, native, case, label)
                native.capture(folder/'after-leave.png')
                result['normal_shutdowns'].append({'case': name, 'exit': close_normally(browser)})
                case['passed'] = True
            except BaseException as error:
                case['error'] = repr(error)
                budget.arm(min(time.monotonic() + 20, budget.deadline))
                try:
                    native.capture(folder/'failure.png')
                except Exception as capture_error:
                    case['capture_error'] = repr(capture_error)
                if browser.process.poll() is None:
                    try:
                        native.key(1)
                        if source and any(page['id'] == source['id'] for page in pages(browser)):
                            browser.ev('window.onbeforeunload=null;true', source)
                    except Exception as cleanup_error:
                        case['modal_cleanup_error'] = repr(cleanup_error)
                raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--layout', choices=('sidebar', 'horizontal'), default='sidebar')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.absolute()
    assert args.browser.parent == Path(os.environ['NOVENTRA_TEST_RUNTIME']).resolve(strict=True)
    assert args.output.is_relative_to(Path(os.environ['NOVENTRA_TEST_OUTPUT']).resolve(strict=True))
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent/'resources.pak') == args.expected_resources
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(__file__, args.output/'executed-test.py')
    result = {'passed': False, 'checks': [], 'cases': [], 'normal_shutdowns': [],
              'layout': args.layout, 'browser': str(args.browser),
              'binary_sha256': args.expected_binary,
              'resources_sha256': args.expected_resources, 'fixture_sha256': sha(__file__)}
    budget = Budget()
    compositor = native = server = thread = None
    try:
        compositor = Compositor()
        assert not any(error.strip() for error in json.loads(compositor.command('j/configerrors')))
        assert compositor.command('/dismissnotify -1').strip() == 'ok'
        native = NativeInput()
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        for name, x, y in ORIGINS:
            run_case(args, compositor, native, server, result, name, x, y, budget)
        check(result, 'exact browser and resources remain unchanged',
              sha(args.browser) == args.expected_binary
              and sha(args.browser.parent/'resources.pak') == args.expected_resources)
        check(result, 'executed fixture remains unchanged', sha(__file__) == result['fixture_sha256'])
        check(result, 'owned browser sessions exit normally',
              len(result['normal_shutdowns']) == len(ORIGINS)
              and all(row['exit'] == 0 for row in result['normal_shutdowns']))
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        budget.arm(min(time.monotonic() + 20, budget.deadline))
        if compositor:
            try:
                result['restored_monitor'] = compositor.set_origin(0, 0)
            except Exception as error:
                result['restore_error'] = repr(error)
                result['passed'] = False
        if native:
            try:
                result['native_helper_exit_codes'] = native.close()
                if any(result['native_helper_exit_codes']):
                    result['passed'] = False
            except Exception as error:
                result['native_cleanup_error'] = repr(error)
                result['passed'] = False
        if server:
            try:
                server.shutdown()
                server.server_close()
                if thread:
                    thread.join(timeout=2)
            except Exception as error:
                result['server_cleanup_error'] = repr(error)
                result['passed'] = False
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        write(args.output/'result.json', result)
        print(json.dumps({'passed': result['passed'], 'cases': len(result['cases']),
                          'checks': sum(len(case['checks']) for case in result['cases'])
                                    + len(result['checks'])}), flush=True)
    assert result['passed'], result


if __name__ == '__main__':
    main()
