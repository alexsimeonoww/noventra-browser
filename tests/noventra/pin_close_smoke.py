"""Native pin close/unload/reopen regression on a private authenticated X11.

Run through work/ram-investigation/with-xvfb.py with exact candidate hashes and
a new --output. All profiles, pages and native input belong to this fixture.
Extension APIs inspect real tab state; pin closure and reopening use native
keys/buttons. A discarded page is never evaluated before native activation.
"""

import http.server
import json
from pathlib import Path
import re
import shutil
import tempfile
import threading
import time

import pinned_tabs_smoke as pin


QUERY = '''chrome.tabs.query({}).then(ts=>ts.map(t=>({
  id:t.id,url:t.url,title:t.title,pinned:t.pinned,discarded:t.discarded,
  active:t.active,windowId:t.windowId,splitViewId:t.splitViewId})))'''


def tabs(api):
    return api(QUERY)


def pinned_tab(api, url):
    matches = [value for value in tabs(api) if value['pinned'] and value['url'] == url]
    assert len(matches) == 1, (url, matches)
    return matches[0]


def unloaded(browser, api, url):
    # Inspect browser-owned tab metadata only. Connecting to the discarded
    # target to evaluate JS could load it and invalidate the memory assertion.
    try:
        return pin.wait_for(lambda: (value if (value := pinned_tab(api, url))['discarded']
                                     else None), browser.process, timeout=8)
    except TimeoutError as error:
        raise AssertionError({'pin_not_unloaded': pinned_tab(api, url)}) from error


def pin_node(browser, tree, name, *, selected=False):
    pattern = (r'class_name=(?:TabView|Tab) name=' + re.escape(name)
               + r'(?: - Pinned)?(?: - Inactive tab)? name_from=')

    def found():
        nodes = pin.native.visible(tree.read(), pattern)
        if selected:
            nodes = [node for node in nodes if 'selected=true' in node['line']]
        return nodes[0] if len(nodes) == 1 else None

    return pin.wait_for(found, browser.process, timeout=5)


def edit_pin(browser, tree, inp, result, output, page, title, *, name=None, home=None, save=False):
    pin.tab.activate(browser, tree, inp, page)
    bounds = pin.tab.bounds(tree, pin_node(browser, tree, title, selected=True))
    pin.tab.native_button(inp, 3, pin.tab.center(bounds))
    number = len(result.setdefault('menus', []))
    capture = 'pin-close-menu-' + str(number)
    pin.native.capture(tree, inp, output, capture)
    labels = pin.popup_labels(output, capture, ['Unpin', pin.EDIT], required=False)
    pin.check(result, 'saved pin exposes Unpin and Edit pinned tab separately from Close',
              'Unpin' in labels and pin.EDIT in labels, sorted(labels))
    result['menus'].append({'capture': capture, 'title': title, 'positions': labels})
    inp.pointer.click(*labels[pin.EDIT])
    initial = pin.dialog(browser, tree)
    if name is not None:
        pin.fill(tree, inp, pin.NAME, name)
    if home is not None:
        pin.fill(tree, inp, pin.HOME, home)
    pin.close_dialog(browser, tree, inp, save=save)
    return initial


def activate_pin(browser, tree, inp, name, url):
    node = pin_node(browser, tree, name)
    bounds = pin.tab.bounds(tree, node)
    # The saved shortcut's center is separate from its upper-right close target.
    inp.pointer.click(*pin.tab.center(bounds))
    return pin.selected_page(browser, url)


def pin_close_button(browser, tree, inp, name):
    node = pin_node(browser, tree, name)
    bounds = pin.tab.bounds(tree, node)
    inp.move(*pin.tab.center(bounds))

    def found():
        owner = pin_node(browser, tree, name)
        values = []
        for candidate in pin.native.visible(tree, r'class_name=TabCloseButton '):
            current = candidate
            while current and current != owner:
                current = tree.nodes.get(current['parent'])
            if current == owner:
                value = pin.tab.bounds(tree, candidate)
                if value['w'] > 0 and value['h'] > 0:
                    values.append(value)
        return values[0] if len(values) == 1 else None

    value = pin.wait_for(found, browser.process, timeout=5)
    assert pin.native.inside(value, pin.tab.box(tree, r'class_name=BrowserView ')), value
    return value


def cancel_beforeunload(browser, tree, inp, result, output, page, api, url):
    before = pin.state(browser, page)
    pin.tab.page_focus(tree, inp)
    pin.check(result, 'fixture pin has genuine native user activation',
              browser.ev('navigator.userActivation.hasBeenActive', page))
    browser.ev('window.onbeforeunload=e=>{e.preventDefault();e.returnValue="pending"};true', page)
    pin.native_key(inp, 'w', ('Control_L',))
    leave = r'class_name=MdTextButton name=Leave name_from='
    pin.wait_for(lambda: bool(pin.native.visible(tree.read(), leave))
                 and bool(pin.native.visible(tree, pin.CANCEL)), browser.process, timeout=6)
    pin.native.capture(tree, inp, output, 'pinned-beforeunload-cancel')
    pin.native_key(inp, 'Escape')
    pin.wait_for(lambda: not pin.native.visible(tree.read(), leave), browser.process, timeout=5)
    browser.ev('window.onbeforeunload=null;true', page)
    pin.check(result, 'Cancel preserves the exact loaded pin, draft and account',
              not pinned_tab(api, url)['discarded'] and pin.state(browser, page) == before)


def accept_beforeunload(browser, tree, inp, result, output, page, api, url):
    pin.tab.page_focus(tree, inp)
    browser.ev('window.onbeforeunload=e=>{e.preventDefault();e.returnValue="pending"};true', page)
    pin.native_key(inp, 'w', ('Control_L',))
    leave = r'class_name=MdTextButton name=Leave name_from='
    pin.wait_for(lambda: bool(pin.native.visible(tree.read(), leave)), browser.process, timeout=6)
    button = pin.native.one(tree, leave)
    pin.check(result, 'Leave is the native beforeunload dialog default action',
              ' DEFAULT ' in button['line'], button['line'])
    pin.native.capture(tree, inp, output, 'pinned-beforeunload-leave')
    # Modal widgets have their own RootView coordinate origin. Activate the
    # observed default action with native keyboard input.
    pin.native_key(inp, 'Return')
    closed = unloaded(browser, api, url)
    pin.check(result, 'accepting Leave unloads the page and retains its saved pin',
              closed['pinned'] and not closed['active'], closed)
    pin.wait_for(lambda: not pin.native.visible(tree.read(), leave), browser.process, timeout=5)


def split_close(browser, tree, inp, result, output, api, base, site_url):
    saved = activate_pin(browser, tree, inp, 'Saved site', site_url)
    partner_url = base + '/split-partner'
    partner = pin.open_page(browser, tree, inp, partner_url)
    pin.pin(browser, api, partner)
    pin.activate_settings(browser)
    accepted = browser.ev('new Promise(r=>chrome.settingsPrivate.setPref('
                          '"browser.split_view_drag_and_drop_enabled",true,"",r))')
    pin.check(result, 'native split drag is enabled for the two pinned sites', accepted)
    pin.tab.activate(browser, tree, inp, saved)
    source = pin.tab.bounds(tree, pin_node(browser, tree, 'pin split-partner'))
    content = pin.tab.box(tree, r'class_name=MultiContentsView ')
    start, end = pin.tab.center(source), pin.tab.center(content)
    inp.move(*start)
    inp.button(True)
    try:
        for step in range(1, 31):
            inp.move(*(a + (b - a) * step / 30 for a, b in zip(start, end)))
            time.sleep(.025)
        time.sleep(.6)
    finally:
        inp.button(False)

    def paired():
        left, right = pinned_tab(api, site_url), pinned_tab(api, partner_url)
        split = left.get('splitViewId', -1)
        return [left, right] if split >= 0 and right.get('splitViewId', -1) == split else None

    pair = pin.wait_for(paired, browser.process, timeout=6)
    pin.check(result, 'native drag creates one genuine split from both pinned sites',
              pair[0]['pinned'] and pair[1]['pinned'], pair)
    pin.request(browser.port, '/json/activate/' + partner['id'])
    pin.wait_for(lambda: pinned_tab(api, partner_url)['active'], browser.process, timeout=5)
    saved_before = pin.state(browser, saved)
    before_count = len(tabs(api))
    pin.native.capture(tree, inp, output, 'pinned-split-before-close')
    pin.native_key(inp, 'w', ('Control_L',))
    closed = unloaded(browser, api, partner_url)
    pin.check(result, 'Ctrl+W unloads only the active pinned split pane and preserves its live partner',
              closed['pinned'] and not pinned_tab(api, site_url)['discarded']
              and len(tabs(api)) == before_count and pin.state(browser, saved) == saved_before,
              {'closed': closed, 'partner': pinned_tab(api, site_url)})
    # Leave the original saved shortcut closed for the restart assertions.
    pin.tab.activate(browser, tree, inp, saved)
    pin.native_key(inp, 'w', ('Control_L',))
    unloaded(browser, api, site_url)


def closing_flow(browser, tree, inp, result, args, base):
    output = args.output / 'native'
    api = pin.tab.controller(browser)
    site_url, home_url = base + '/saved', base + '/home'
    site = pin.open_page(browser, tree, inp, site_url)
    pin.seed_account(browser, site, 'personal')
    pin.pin(browser, api, site)
    edit_pin(browser, tree, inp, result, output, site, 'pin saved',
             name='Saved site', home=home_url, save=True)
    sibling = pin.open_page(browser, tree, inp, base + '/ordinary')
    sibling_before = pin.state(browser, sibling)
    pin.tab.activate(browser, tree, inp, site)
    before = pin.state(browser, site)
    before_count = len(tabs(api))
    pin.native_key(inp, 'w', ('Control_L',))
    closed = unloaded(browser, api, site_url)
    pin.check(result, 'one Ctrl+W unloads the site while retaining its pin and tab count',
              closed['pinned'] and not closed['active'] and len(tabs(api)) == before_count, closed)
    pin_node(browser, tree, 'Saved site')
    pin.native.capture(tree, inp, output, 'closed-shortcut-remains')

    reopened = activate_pin(browser, tree, inp, 'Saved site', site_url)
    after = pin.state(browser, reopened)
    pin.check(result, 'clicking the closed shortcut loads a fresh real document at its saved current URL',
              after['url'] == site_url and after['token'] != before['token']
              and after['timeOrigin'] != before['timeOrigin'] and after['discarded'], after)
    pin.check(result, 'reopened pin retains its original account and custom home/name',
              pin.account_is(after, 'personal')
              and edit_pin(browser, tree, inp, result, output, reopened, 'Saved site')
              == {'name': 'Saved site', 'home': home_url})
    cancel_beforeunload(browser, tree, inp, result, output, reopened, api, site_url)
    accept_beforeunload(browser, tree, inp, result, output, reopened, api, site_url)
    reopened = activate_pin(browser, tree, inp, 'Saved site', site_url)

    pin.tab.activate(browser, tree, inp, sibling)
    close_bounds = pin_close_button(browser, tree, inp, 'Saved site')
    pin.native.capture(tree, inp, output, 'native-pin-close-button')
    inp.pointer.click(*pin.tab.center(close_bounds))
    closed = unloaded(browser, api, site_url)
    pin.check(result, 'native pin X unloads a background page without changing the active document',
              not closed['active'] and len(tabs(api)) == before_count
              and pin.state(browser, sibling) == sibling_before, close_bounds)
    split_close(browser, tree, inp, result, output, api, base, site_url)

    # Work has no diagnostic/control tabs. Remove only the ordinary tabs that
    # native creation added there, leaving a single pin in that Space.
    pin.activate_settings(browser)
    before_work = {value['id'] for value in tabs(api)}
    browser.call('noventraSpaces', 'create', '', 'Work')
    pin.switch(browser, tree, inp, 'Work')
    work_url = base + '/work-only'
    work = pin.open_page(browser, tree, inp, work_url)
    pin.seed_account(browser, work, 'work')
    work_id = pin.pin(browser, api, work)
    work_ordinary = [value['id'] for value in tabs(api)
                     if value['id'] not in before_work and value['id'] != work_id]
    if work_ordinary:
        api('chrome.tabs.remove(' + json.dumps(work_ordinary) + ')')
    pin.tab.activate(browser, tree, inp, work)
    before_only = {value['id'] for value in tabs(api)}
    pin.native_key(inp, 'w', ('Control_L',))
    unloaded(browser, api, work_url)
    current = pin.wait_for(lambda: next((value for value in tabs(api)
                         if value['active'] and value['id'] not in before_only
                         and not value['pinned']), None), browser.process, timeout=8)
    pin.check(result, 'closing the only pin in a Space creates a usable ordinary New Tab',
              current['url'].startswith(('chrome://newtab', 'chrome://new-tab-page'))
              and len(tabs(api)) == len(before_only) + 1, current)
    witness = pin.open_page(browser, tree, inp, base + '/work-witness')
    pin.check(result, 'replacement New Tab inherits the Work account rather than Personal',
              pin.account_is(pin.state(browser, witness), 'work'))
    pin.check(result, 'only-pin handling leaves the Personal pin unloaded',
              pinned_tab(api, site_url)['discarded'])
    pin.native.capture(tree, inp, output, 'only-pin-new-tab')
    result['restart'] = [
        {'url': site_url, 'name': 'Saved site', 'home': home_url, 'owner': 'personal'},
        {'url': work_url, 'name': 'pin work-only', 'home': work_url, 'owner': 'work'},
    ]


def restart_flow(browser, tree, inp, result, args):
    api = pin.tab.controller(browser)
    for expected in result['restart']:
        matches = [page for page in pin.pages(browser) if page['url'] == expected['url']]
        pin.check(result, 'closed pin survives normal shutdown/restart: ' + expected['name'],
                  len(matches) == 1, [page['url'] for page in pin.pages(browser)])
        page = matches[0]
        pin.tab.activate(browser, tree, inp, page)
        pin.ready(browser, page, expected['url'])
        value = pin.state(browser, page)
        pin.check(result, 'restored pin keeps its account, name, current URL and home',
                  pinned_tab(api, expected['url'])['pinned']
                  and pin.account_is(value, expected['owner'])
                  and edit_pin(browser, tree, inp, result, args.output / 'restart-native',
                               page, expected['name'])
                  == {'name': expected['name'] if expected['name'] == 'Saved site' else '',
                      'home': expected['home']}, {'expected': expected, 'actual': value})
    pin.native.capture(tree, inp, args.output / 'restart-native', 'restored-closed-pins')


def main():
    parser = pin.cli(__doc__)
    parser.add_argument('--layout', choices=['sidebar', 'horizontal'], default='sidebar')
    args = parser.parse_args()
    result = pin.admit(args, Path(__file__))
    result['layout'] = args.layout
    for module in (pin, pin.tab):
        source = Path(module.__file__)
        shutil.copyfile(source, args.output / ('executed-' + source.name))
        result.setdefault('helper_sources', {})[str(source)] = pin.sha(source)
    budget = pin.Budget()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), pin.Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with tempfile.TemporaryDirectory(prefix='owned-pin-close-profile-', dir=args.output) as tmp:
            profile = Path(tmp)
            pin.prefs(profile)
            extension = pin.extension_fixture(profile)
            flags = (*pin.native.FLAGS, '--load-extension=' + str(extension.resolve()))
            with pin.session(args, profile, args.output / 'native', result, budget, flags) as (browser, tree, inp):
                pin.activate_settings(browser)
                if args.layout == 'horizontal':
                    pin.native.apply(browser, pin.native.top_layout(browser.state()['layout'],
                                                                    (*pin.native.TOP_ORDER, 'spaces')))
                closing_flow(browser, tree, inp, result, args, base)
            with pin.session(args, profile, args.output / 'restart-native', result, budget,
                             (*flags, '--restore-last-session')) as (browser, tree, inp):
                pin.activate_settings(browser)
                restart_flow(browser, tree, inp, result, args)
            pin.check(result, 'all owned browser sessions close normally',
                      all(row['exit'] == 0 for row in result['normal_shutdowns']), result['normal_shutdowns'])
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        pin.write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
