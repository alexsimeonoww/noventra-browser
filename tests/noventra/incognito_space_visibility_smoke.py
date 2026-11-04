"""Retain a native Incognito shortcut through regular-space switches.

Run on the private Xvfb wrapper with exact runtime hashes and a fresh output.
Both cases seed six ordinary spaces. Incognito is created through an actual
browser-menu click; compact checks both regular and private 48-DIP rails.
No personal profile, desktop input, or external network is used.
"""

import argparse
import http.server
import json
from pathlib import Path
import re
import threading
import time

import incognito_space_smoke as incognito
from sidebar_footer_smoke import popup_labels


IDENTIFIERS = ['', *[f'54000000-0000-4000-8000-{index:012d}'
                     for index in range(1, 6)]]
STATE = '''({token:window.visibilityToken,origin:performance.timeOrigin,
            draft:document.querySelector('#draft').value})'''


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        data = b'''<!doctype html><meta charset="utf-8"><title>Space visibility witness</title>
          <label>Unsaved draft <input id="draft"></label>
          <script>window.visibilityToken=crypto.randomUUID();</script>'''
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def visible(tree, pattern):
    return [node for node in tree.nodes.values()
            if ' INVISIBLE ' not in node['line'] and re.search(pattern, node['line'])
            and node['bounds'][2] > 0 and node['bounds'][3] > 0]


def one(tree, pattern):
    nodes = visible(tree.read(), pattern)
    assert len(nodes) == 1, (pattern, [node['line'] for node in nodes])
    return nodes[0]


def sidebar(tree):
    region = one(tree, r'class_name=VerticalTabStripRegionView ')
    node = region
    while node and 'class_name=NoventraDockView ' not in node['line']:
        node = tree.nodes.get(node['parent'])
    assert node, region
    return tree.rect(node)


def collapse_sidebar(browser, tree, inp):
    dock = sidebar(tree)
    if dock['w'] != 48:
        x, y = dock['x'] + dock['w'] - 2, dock['y'] + dock['h'] - 12
        inp.move(x, y)
        inp.button(True)
        try:
            inp.move(x + 48 - dock['w'], y)
            time.sleep(.3)
        finally:
            inp.button(False)
    incognito.wait_for(lambda: sidebar(tree)['w'] == 48, browser.process)


def create_from_menu(browser, tree, inp, output):
    before = {page['id'] for page in incognito.pages(browser)}
    button = one(tree, r'class_name=BrowserAppMenuButton ')
    inp.pointer.click(*tree.center(button))
    time.sleep(.4)
    inp.capture(output / 'create-menu.png').close()
    positions = popup_labels(output, 'create-menu', ['New Incognito window'])
    incognito.write(output / 'create-menu.json', {'positions': positions})
    inp.pointer.click(*positions['New Incognito window'])
    return incognito.wait_for(lambda: next((page for page in incognito.pages(browser)
        if page['id'] not in before), None), browser.process)


def seed(profile):
    (profile / 'Default').mkdir(parents=True)
    incognito.write(profile / 'Default/Preferences', {
        'helium': {'browser': {'layout': 2, 'vertical_right_aligned': False,
            'zen_mode': True, 'zen_mode_sidebar_pinned': True,
            'zen_mode_top_chrome_pinned': True}},
        'vertical_tabs': {'collapsed_state': False, 'uncollapsed_width': 144},
        'noventra': {'browser': {
            'account_spaces': {identifier: 'Ordinary ' + str(index) if identifier
                               else 'Personal'
                               for index, identifier in enumerate(IDENTIFIERS)},
            'account_space_order': IDENTIFIERS,
            'archived_account_spaces': [],
        }},
    })


def flow(args, result):
    profile = args.output / 'profile'
    seed(profile)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with incognito.LayoutBrowser(args.browser, profile, args.output / 'browser',
                                      incognito.FLAGS) as browser:
            inp = incognito.Input(browser.platform)
            tree = None
            try:
                incognito.apply_native_test_layout(browser.port, edge='left',
                    visible=['tabs', 'spaces', 'new-tab', 'address', 'menu'])
                # Create the sole diagnostics page before the regular witness,
                # so its later reads cannot change the regular-space MRU tab.
                tree = incognito.PositionedNativeUITree(browser, inp)
                regular = incognito.request(browser.port, '/json/new?' + base + '/regular', 'PUT')
                incognito.wait_for(lambda: browser.ev('!!window.visibilityToken', regular), browser.process)
                incognito.request(browser.port, '/json/activate/' + regular['id'])
                windows = incognito.owned_windows(browser, inp)
                incognito.check(result, 'Starts in one native window with six ordinary spaces',
                    len(windows) == 1 and len(browser.call('noventraSpaces', 'list', '', '')) == 6)
                incognito.activate_x_window(inp, windows[0], browser.process)
                tree.bind_browser(regular)
                if args.case == 'compact':
                    collapse_sidebar(browser, tree, inp)
                    incognito.check(result, 'Regular sidebar is 48 DIP before native menu creation',
                                    sidebar(tree)['w'] == 48)
                private = create_from_menu(browser, tree, inp, args.output)
                incognito.goto(browser, private, base + '/private')
                browser.ev('document.querySelector("#draft").value="keep private draft";true', private)
                retained = browser.ev(STATE, private)
                tree.bind_browser(private)
                incognito.check(result, 'Native menu creates a genuine private context in the same OS window',
                    incognito.cdp(browser, 'Target.getTargetInfo', {'targetId': private['id']})['targetInfo']['browserContextId'] !=
                    incognito.cdp(browser, 'Target.getTargetInfo', {'targetId': regular['id']})['targetInfo']['browserContextId']
                    and incognito.owned_windows(browser, inp) == windows)
                if args.case == 'compact':
                    collapse_sidebar(browser, tree, inp)
                    incognito.check(result, 'Private sidebar independently measures 48 DIP',
                                    sidebar(tree)['w'] == 48)
                    incognito.native_key(inp, '1', ('Control_L', 'Shift_L'))
                else:
                    tree.read()
                    incognito.check(result, 'Expanded sidebar exercises ordinary-space overflow',
                        len(visible(tree, r'class_name=AccountSpaceDotButton name=Workspace: ')) < 7)
                    incognito.select_dot(browser, tree, inp, 'Personal')
                incognito.wait_for(lambda: browser.ev('document.visibilityState==="visible"', regular),
                                    browser.process)
                tree.bind_browser(regular)
                if args.case == 'compact':
                    incognito.check(result, 'Returning Personal still measures 48 DIP before shortcut assertion',
                                    sidebar(tree)['w'] == 48)
                incognito.capture(tree, inp, args.output, 'regular-private-shortcut')
                nodes = visible(tree, r'class_name=AccountSpaceDotButton name=Workspace: Incognito name_from=')
                rect = tree.rect(nodes[0]) if len(nodes) == 1 else None
                dock = sidebar(tree)
                incognito.check(result, 'Inactive Incognito shortcut stays visible and clickable in the sidebar',
                    rect is not None and rect['x'] >= dock['x'] and rect['y'] >= dock['y']
                    and rect['x'] + rect['w'] <= dock['x'] + dock['w']
                    and rect['y'] + rect['h'] <= dock['y'] + dock['h'])
                inp.pointer.click(rect['x'] + rect['w'] / 2, rect['y'] + rect['h'] / 2)
                incognito.wait_for(lambda: browser.ev('document.visibilityState==="visible"', private),
                                    browser.process)
                tree.bind_browser(private)
                incognito.check(result, 'Native shortcut click returns to the exact private document and draft',
                    browser.ev(STATE, private) == retained and incognito.owned_windows(browser, inp) == windows)
                incognito.native_key(inp, 'w', ('Control_L',))
                incognito.wait_for(lambda: all(page['id'] != private['id'] for page in incognito.pages(browser))
                    and browser.ev('document.visibilityState==="visible"', regular), browser.process)
                tree.bind_browser(regular)
                incognito.capture(tree, inp, args.output, 'last-private-tab-closed')
                incognito.check(result, 'Closing the last private tab removes its shortcut and preserves Personal',
                    not re.search(r'class_name=AccountSpaceDotButton name=Workspace: Incognito name_from=', tree.text)
                    and incognito.owned_windows(browser, inp) == windows)
            except BaseException:
                if tree and browser.process.poll() is None:
                    incognito.capture(tree, inp, args.output, 'failure')
                raise
            finally:
                inp.close()
                if browser.process.poll() is None:
                    result.setdefault('normal_shutdowns', []).append(incognito.normal_close(browser))
    finally:
        server.shutdown()
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['expanded', 'compact'], default='expanded')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.browser, args.output = args.browser.resolve(strict=True), args.output.resolve()
    args.rtc_policy_diagnostic = False
    assert incognito.sha(args.browser) == args.expected_binary
    assert incognito.sha(args.browser.parent / 'resources.pak') == args.expected_resources
    incognito.isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    result = {'passed': False, 'case': args.case, 'checks': [],
        'binary_sha256': args.expected_binary, 'resources_sha256': args.expected_resources,
        'fixture_sha256': incognito.sha(Path(__file__)), 'home_isolated': True,
        'network_isolated_no_egress': True}
    try:
        flow(args, result)
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        incognito.write(args.output / 'result.json', result)
    print(json.dumps({'passed': True, 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
