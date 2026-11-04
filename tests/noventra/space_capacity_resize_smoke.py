"""Keep two normal Space shortcuts plus Incognito visible while resizing.

Use the owned Xvfb/Fluxbox adapter with exact runtime hashes and a fresh output.
The default footer keeps BOTH Bookmarks and More, which constrain the Space
slot at the 144-DIP sidebar minimum. Every resize is a genuine native edge drag.
Cases: expanded fails promptly on a missing third shortcut; matrix also shrinks
through 320/200/160/144/48 and back; right repeats that matrix on the other side;
overflow retains active/private reachability among six ordinary Spaces.
"""

import argparse
import http.server
import json
import os
from pathlib import Path
import re
import threading
import time

import incognito_space_visibility_smoke as visibility

incognito = visibility.incognito
WORK = '55000000-0000-4000-8000-000000000001'
WIDTHS = {
    'expanded': (144, 160, 200, 320),
    'compact': (48,),
    'matrix': (320, 200, 160, 144, 48, 144),
    'right': (320, 200, 160, 144, 48, 144),
    'overflow': (320, 200, 160, 144, 48, 144),
}
DOT = r'class_name=AccountSpaceDotButton name=Workspace: '


class SpaceNativeTree(incognito.PositionedNativeUITree):
    def bind_browser(self, target):
        super().bind_browser(target)
        if not hasattr(self, 'host_session'):
            self.host_session = self.bound_browser_session

    def read(self):
        self.widget_bounds = None
        session = self.bound_browser_session
        if session is not None and session != self.host_session:
            # LayoutIncognitoSpaces places the retained widget at the regular
            # BrowserView's screen bounds. CDP window bounds intentionally route
            # to the host window, so use the native host view's actual inset.
            self.bound_browser_session = self.host_session
            super().read()
            host = visibility.one(self, r'class_name=BrowserView ')
            self.widget_bounds = incognito.PositionedNativeUITree.rect(self, host)
            self.bound_browser_session = session
        return super().read()

    def rect(self, node):
        if not getattr(self, 'widget_bounds', None):
            return super().rect(node)
        # The retained private widget is inset inside the regular host.
        rect = incognito.NativeUITree.rect(self, node)
        root = node
        while root and 'class_name=BrowserRootView ' not in root['line']:
            root = self.nodes.get(root['parent'])
        if root:
            rect['x'] += self.widget_bounds['x'] - root['bounds'][0]
            rect['y'] += self.widget_bounds['y'] - root['bounds'][1]
        return rect


def seed(profile, args):
    visibility.seed(profile)
    path = profile / 'Default/Preferences'
    prefs = json.loads(path.read_text())
    ordinary = (['', WORK] if args.case != 'overflow' else
                ['', *[f'55000000-0000-4000-8000-{n:012d}' for n in range(1, 6)]])
    prefs['noventra']['browser'].update(
        account_spaces={identifier: ('Personal' if not identifier else
                                    'Work' if identifier == WORK else 'Work ' + str(n))
                        for n, identifier in enumerate(ordinary)},
        account_space_order=ordinary)
    prefs['vertical_tabs'].update(collapsed_state=False,
                                  uncollapsed_width=max(144, WIDTHS[args.case][0]))
    prefs['helium']['browser']['vertical_right_aligned'] = args.case == 'right'
    incognito.write(path, prefs)
    return ordinary


def inside(inner, outer):
    return (inner['w'] > 0 and inner['h'] > 0
            and inner['x'] >= outer['x'] and inner['y'] >= outer['y']
            and inner['x'] + inner['w'] <= outer['x'] + outer['w']
            and inner['y'] + inner['h'] <= outer['y'] + outer['h'])


def overlap(a, b):
    return (a['x'] < b['x'] + b['w'] and b['x'] < a['x'] + a['w']
            and a['y'] < b['y'] + b['h'] and b['y'] < a['y'] + a['h'])


def dot_name(node):
    return re.search(r'name=Workspace: (.*?) name_from=', node['line'])[1]


def geometry(tree):
    tree.read()
    dock = visibility.sidebar(tree)
    footer = visibility.one(tree, r'class_name=AccountSpacesButton ')
    popup = visibility.one(tree, r'class_name=NoventraBookmarksView name=Bookmark popup ')
    menu = visibility.one(tree, r'class_name=BrowserAppMenuButton ')
    dots = visibility.visible(tree, DOT)
    return {'dock': dock, 'footer': tree.rect(footer), 'bookmarks': tree.rect(popup),
            'menu': tree.rect(menu),
            'dots': {dot_name(node): tree.rect(node) for node in dots},
            'selected': [dot_name(node) for node in dots if 'selected=true' in node['line']]}


def resize(browser, tree, inp, target, side, result):
    tree.read()
    dock = visibility.sidebar(tree)
    if dock['w'] != target:
        # The whole dock edge forwards to the native resize delegate, including
        # beside its footer. Avoid assuming the ordinary TabStrip fills it.
        x = dock['x'] + dock['w'] - 2 if side == 'left' else dock['x'] + 1
        y = dock['y'] + dock['h'] - 12
        end = x + (target - dock['w']) * (1 if side == 'left' else -1)
        result.setdefault('resize_drags', []).append(
            {'from': dock, 'target': target, 'start': [x, y], 'end': [end, y],
             'widget_bounds': tree.widget_bounds})
        inp.move(x, y)
        inp.button(True)
        try:
            for step in range(1, 13):
                inp.move(x + (end - x) * step / 12, y)
                time.sleep(.025)
        finally:
            inp.button(False)
    incognito.wait_for(lambda: visibility.sidebar(tree.read())['w'] == target,
                       browser.process, timeout=8)
    incognito.check(result, f'{side}: native resize reaches {target} DIP without crashing',
                    browser.process.poll() is None and visibility.sidebar(tree)['w'] == target)


def check_shortcuts(browser, tree, inp, result, output, selected, width, ordinary_count):
    value = geometry(tree)
    value.update(selected_space=selected, requested_width=width)
    result.setdefault('measurements', []).append(value)
    names = set(value['dots'])
    incognito.capture(tree, inp, output,
                      f'{selected.lower()}-{len(result["measurements"]):02d}-{width}')
    label = f'{selected} at {width} DIP'
    incognito.check(result, label + ': retains visible Bookmarks and More footer controls',
                    inside(value['bookmarks'], value['dock'])
                    and inside(value['menu'], value['dock'])
                    and value['bookmarks']['w'] >= 24 and value['menu']['w'] >= 24)
    if ordinary_count == 2:
        incognito.check(result, label + ': all three Space shortcuts remain visible',
                        names == {'Personal', 'Work', 'Incognito'})
    else:
        incognito.check(result, label + ': overflow retains the active and private shortcuts',
                        selected in names and 'Incognito' in names and len(names) >= 3)
    incognito.check(result, label + ': every visible shortcut is inside the measured footer',
                    all(inside(rect, value['footer']) and inside(rect, value['dock'])
                        for rect in value['dots'].values()))
    rects = list(value['dots'].values())
    incognito.check(result, label + ': shortcuts and adjacent footer tools do not overlap',
                    not any(overlap(a, b) for n, a in enumerate(rects) for b in rects[n + 1:])
                    and all(not overlap(rect, tool) for rect in rects
                            for tool in (value['bookmarks'], value['menu'])))
    incognito.check(result, label + ': the selected Space is marked correctly',
                    value['selected'] == [selected])


def click_space(browser, tree, inp, name):
    node = visibility.one(tree, DOT + re.escape(name) + r' name_from=')
    inp.pointer.click(*tree.center(node))
    incognito.wait_for(lambda: any('selected=true' in node['line']
        for node in visibility.visible(tree.read(), DOT + re.escape(name) + r' name_from=')),
        browser.process, timeout=8)


def flow(args, result):
    profile = args.output / 'profile'
    ordinary = seed(profile, args)
    side = 'right' if args.case == 'right' else 'left'
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), visibility.Handler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with incognito.LayoutBrowser(args.browser, profile, args.output / 'browser',
                                      incognito.FLAGS) as browser:
            inp = incognito.Input(browser.platform)
            tree = None
            try:
                # Keeping bookmarks is essential: at 144 DIP the two36-DIP
                # footer tools leave only72 DIP for the three Space targets.
                incognito.apply_native_test_layout(browser.port, edge=side,
                    visible=['tabs', 'spaces', 'new-tab', 'address', 'action:bookmarks', 'menu'])
                tree = SpaceNativeTree(browser, inp)
                regular = incognito.request(browser.port, '/json/new?' + base + '/regular', 'PUT')
                incognito.wait_for(lambda: browser.ev('!!window.visibilityToken', regular), browser.process)
                incognito.request(browser.port, '/json/activate/' + regular['id'])
                tree.bind_browser(regular)
                windows = incognito.owned_windows(browser, inp)
                incognito.check(result, 'Fixture starts with exactly the requested normal Spaces in one OS window',
                    len(windows) == 1 and len(browser.call('noventraSpaces', 'list', '', '')) == len(ordinary))
                incognito.activate_x_window(inp, windows[0], browser.process)
                retained_regular = browser.ev(visibility.STATE, regular)
                private = visibility.create_from_menu(browser, tree, inp, args.output)
                incognito.goto(browser, private, base + '/private')
                browser.ev('document.querySelector("#draft").value="keep private draft";true', private)
                retained_private = browser.ev(visibility.STATE, private)
                tree.bind_browser(private)
                incognito.check(result, 'Native browser menu adds one genuine private context in the same OS window',
                    incognito.cdp(browser, 'Target.getTargetInfo', {'targetId': private['id']})['targetInfo']['browserContextId'] !=
                    incognito.cdp(browser, 'Target.getTargetInfo', {'targetId': regular['id']})['targetInfo']['browserContextId']
                    and incognito.owned_windows(browser, inp) == windows)
                for width in WIDTHS[args.case]:
                    tree.bind_browser(private)
                    resize(browser, tree, inp, width, side, result)
                    check_shortcuts(browser, tree, inp, result, args.output, 'Incognito', width, len(ordinary))
                    if len(ordinary) == 2:
                        click_space(browser, tree, inp, 'Work')
                        tree.bind_browser(regular)
                        incognito.check(result, f'Work at {width} DIP: native third shortcut activates its own Space',
                                        geometry(tree)['selected'] == ['Work'])
                    click_space(browser, tree, inp, 'Personal')
                    incognito.wait_for(lambda: browser.ev('document.visibilityState==="visible"', regular), browser.process)
                    tree.bind_browser(regular)
                    resize(browser, tree, inp, width, side, result)
                    check_shortcuts(browser, tree, inp, result, args.output, 'Personal', width, len(ordinary))
                    incognito.check(result, f'Personal at {width} DIP: resize/switch preserves its exact document',
                                    browser.ev(visibility.STATE, regular) == retained_regular)
                    click_space(browser, tree, inp, 'Incognito')
                    incognito.wait_for(lambda: browser.ev('document.visibilityState==="visible"', private), browser.process)
                    tree.bind_browser(private)
                    incognito.check(result, f'Incognito at {width} DIP: native return preserves document and draft',
                                    browser.ev(visibility.STATE, private) == retained_private
                                    and incognito.owned_windows(browser, inp) == windows)
                incognito.native_key(inp, 'w', ('Control_L',))
                incognito.wait_for(lambda: all(page['id'] != private['id'] for page in incognito.pages(browser))
                    and browser.ev('document.visibilityState==="visible"', regular), browser.process)
                tree.bind_browser(regular)
                incognito.capture(tree, inp, args.output, 'last-private-tab-closed')
                incognito.check(result, 'Closing the private Space removes only its temporary shortcut',
                    not visibility.visible(tree, DOT + r'Incognito name_from=')
                    and len(browser.call('noventraSpaces', 'list', '', '')) == len(ordinary))
            except BaseException:
                result['browser_exit_at_failure'] = browser.process.poll()
                if tree and browser.process.poll() is None:
                    try:
                        incognito.capture(tree, inp, args.output, 'failure')
                    except BaseException as error:
                        result['capture_error'] = repr(error)
                raise
            finally:
                inp.close()
                if browser.process.poll() is None:
                    result.setdefault('normal_shutdowns', []).append(incognito.normal_close(browser))
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=tuple(WIDTHS), default='matrix')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.browser, args.output = args.browser.resolve(strict=True), args.output.resolve()
    args.rtc_policy_diagnostic = False
    assert incognito.sha(args.browser) == args.expected_binary
    assert incognito.sha(args.browser.parent / 'resources.pak') == args.expected_resources
    owner = os.environ.get('NOVENTRA_BENCH_XVFB_PID', '')
    assert owner.isdigit() and Path('/proc', owner, 'comm').read_text().strip() == 'Xvfb'
    assert Path(os.environ['XAUTHORITY']).resolve().is_relative_to(incognito.ROOT / 'work/ram-investigation')
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
