"""Create native mixed pin/ordinary splits without losing saved pins.

Run on an owned authenticated Xvfb display with exact runtime hashes. Cases
exercise each drag/menu direction in Personal or Work. Extension APIs observe
real tab state; split creation uses native mouse input and captured menu labels.
"""

import http.server
import json
from pathlib import Path
import re
import tempfile
import threading
import time

import incognito_space_smoke as private
import pin_close_smoke as close
import pinned_tabs_smoke as p
import profile_spaces_smoke as profiles
import space_capacity_resize_smoke as spaces


WORK = '66000000-0000-4000-8000-000000000001'
CASES = ('drag-pin-to-tab', 'drag-tab-to-pin', 'menu-pin-to-tab',
         'menu-tab-to-pin', 'closed-pin-menu', 'closed-pin-picker', 'cross-space-menu',
         'join-pin-to-split', 'grouped-join-cancel', 'grouped-join-accept',
         'native-rejoin', 'native-tearoff')


def tabs(api):
    return api('chrome.tabs.query({}).then(ts=>ts.map(t=>({'
               'id:t.id,windowId:t.windowId,url:t.url,title:t.title,index:t.index,pinned:t.pinned,'
               'discarded:t.discarded,splitViewId:t.splitViewId,groupId:t.groupId})))')


def tab_at(api, url):
    values = [tab for tab in tabs(api) if tab['url'] == url]
    assert len(values) == 1, values
    return values[0]


def record(result, name, condition, actual=None):
    result['checks'].append({'name': name, 'passed': bool(condition), 'actual': actual})


def pin_node(browser, tree, title):
    pattern = (r'class_name=(?:TabView|Tab) name=' + re.escape(title)
               + r'(?: - .*?)? name_from=')
    def unique():
        nodes = p.native.visible(tree.read(), pattern)
        return nodes[0] if len(nodes) == 1 else None

    return p.wait_for(unique, browser.process)


def switch_work(browser, tree, inp, api_url):
    before = {page['id'] for page in p.pages(browser)}
    profiles.choose(browser, tree, inp, 'Work')
    work = p.wait_for(lambda: next((page for page in p.pages(browser)
        if page['id'] not in before and page['url'].startswith(
            ('chrome://newtab', 'chrome://new-tab-page', 'about:blank'))), None), browser.process)
    tree.bind_browser(work)
    control = profiles.new_tab(browser, inp, api_url)
    return work, profiles.extension_api(browser, control)


def horizontal_profile(browser, inp):
    previous = browser.settings
    browser.settings = profiles.new_tab(browser, inp, 'chrome://settings/appearance')
    try:
        p.native.apply(browser, p.native.top_layout(browser.state()['layout'],
                                                   (*p.native.TOP_ORDER, 'spaces')))
    finally:
        browser.settings = previous


def pane_bounds(text):
    panes = {}
    container = None
    for line in text.splitlines():
        depth = len(line) - len(line.lstrip('+'))
        if container and depth <= container['depth']:
            container = None
        if 'class_name=ContentsContainerView ' in line and ' INVISIBLE ' not in line:
            match = re.search(r'\((-?\d+), (-?\d+)\)-\((\d+), (\d+)\)', line)
            assert match, line
            x, y, width, height = map(int, match.groups())
            container = {'depth': depth, 'x': x, 'y': y, 'w': width, 'h': height}
        elif container and ' rootWebArea ' in line and ' INVISIBLE ' not in line:
            match = re.search(r'\burl=(\S+)', line)
            if match:
                assert match[1] not in panes, ('Duplicate visible page', match[1])
                panes[match[1]] = {key: value for key, value in container.items() if key != 'depth'}
    return panes


def context_menu(browser, tree, inp, output, page, title, name, parent='Split with tab'):
    p.tab.activate(browser, tree, inp, page)
    bounds = p.tab.bounds(tree, pin_node(browser, tree, title))
    point = (bounds['x'] + min(8, bounds['w'] / 4), bounds['y'] + bounds['h'] / 2)
    p.tab.native_button(inp, 3, point)
    time.sleep(.3)
    p.native.capture(tree, inp, output, name + '-parent')
    point = p.popup_labels(output, name + '-parent', [parent])[parent]
    inp.move(*point)
    time.sleep(.7)
    p.native.capture(tree, inp, output, name + '-choices')


def choose_split(browser, tree, inp, output, api, source, target, saved_target=None):
    source_title = browser.ev('document.title', source)
    target_tab = saved_target or tab_at(api, browser.ev('location.href', target))
    target_title = target_tab['title'] if saved_target else browser.ev('document.title', target)
    source_tab = tab_at(api, browser.ev('location.href', source))
    parent = 'Add tab to split' if source_tab.get('splitViewId', -1) >= 0 else 'Split with tab'
    number = getattr(browser, 'pin_split_menu_count', 0)
    browser.pin_split_menu_count = number + 1
    name = 'split-menu-' + str(number)
    context_menu(browser, tree, inp, output, source, source_title, name, parent)
    label = target_title + ' (' + str(target_tab['index'] + 1) + ')'
    point = p.popup_labels(output, name + '-choices', [label])[label]
    inp.pointer.click(*point)


def drag_split(browser, tree, inp, output, source, target, direction, layout, path):
    p.tab.activate(browser, tree, inp, target)
    source_title = browser.ev('document.title', source)
    row = p.tab.bounds(tree, pin_node(browser, tree, source_title))
    content = p.tab.box(tree, r'class_name=MultiContentsView ')
    start = (row['x'] + min(8, row['w'] / 4), row['y'] + row['h'] / 2)
    end = {
        'left': (content['x'] + content['w'] / 4, content['y'] + content['h'] / 2),
        'right': (content['x'] + content['w'] * .75, content['y'] + content['h'] / 2),
        'above': (content['x'] + content['w'] / 2, content['y'] + 24),
        'below': (content['x'] + content['w'] / 2, content['y'] + content['h'] - 24),
    }[direction]
    waypoint = None
    if layout == 'horizontal':
        waypoint = ((start[0] + 80, start[1]) if path == 'native'
                    else (start[0], content['y'] + 24))
    p.write(output / 'mixed-pin-drag.json', {
        'source': source_title, 'row': row, 'content': content,
        'start': start, 'waypoint': waypoint, 'end': end, 'direction': direction,
        'path': path})
    (output / 'mixed-pin-before-drag.txt').write_text(tree.read().text)
    inp.move(*start)
    inp.button(True)
    try:
        previous = start
        for destination in ([waypoint, end] if waypoint else [end]):
            for step in range(1, 31):
                inp.move(*(a + (b - a) * step / 30 for a, b in zip(previous, destination)))
                time.sleep(.025)
            previous = destination
        time.sleep(.6)
        p.native.capture(tree, inp, output, 'mixed-pin-drop-preview')
    finally:
        inp.button(False)


def native_window_flow(browser, tree, inp, result, args, api, pinned, ordinary, before):
    assert args.layout == 'horizontal' and inp.platform == 'x11'
    windows = profiles.owned_windows(browser, inp)
    original = tabs(api)
    pin_url = browser.ev('location.href', pinned)
    pin_id = tab_at(api, pin_url)['id']
    original_window = tab_at(api, pin_url)['windowId']
    p.tab.activate(browser, tree, inp, ordinary)
    row = p.tab.bounds(tree, pin_node(browser, tree, 'pin pinned'))
    target = p.tab.bounds(tree, pin_node(browser, tree, 'pin ordinary'))
    content = p.tab.box(tree, r'class_name=MultiContentsView ')
    start = (row['x'] + 8, row['y'] + row['h'] / 2)
    middle = (content['x'] + content['w'] / 4, content['y'] + content['h'] * .65)
    end = p.tab.center(target) if args.case == 'native-rejoin' else (1000, 790)
    p.write(args.output / 'native/window-gesture.json',
            {'start': start, 'middle': middle, 'end': end, 'original_tabs': original})
    inp.move(*start)
    inp.button(True)
    try:
        previous = start
        for destination in ((start[0] + 80, start[1]), middle):
            for step in range(1, 31):
                inp.move(*(a + (b - a) * step / 30 for a, b in zip(previous, destination)))
                time.sleep(.025)
            previous = destination
        transient = p.wait_for(lambda: (value if len(value := profiles.owned_windows(browser, inp)) > len(windows)
                                       else None), browser.process, timeout=6)
        record(result, 'Native gesture creates a real temporary OS host', len(transient) == len(windows) + 1)
        for step in range(1, 31):
            inp.move(*(a + (b - a) * step / 30 for a, b in zip(middle, end)))
            time.sleep(.025)
        time.sleep(.4)
        p.native.capture(tree, inp, args.output / 'native', 'window-gesture-preview')
    finally:
        inp.button(False)
    if args.case == 'native-rejoin':
        p.write(args.output / 'native/window-gesture-after-tabs.json', tabs(api))
        p.wait_for(lambda: profiles.owned_windows(browser, inp) == windows, browser.process, timeout=8)
        record(result, 'Rejoining the original tab strip removes the temporary host',
               profiles.owned_windows(browser, inp) == windows)
        record(result, 'Rejoining retains every original tab identity',
               {tab['id'] for tab in tabs(api)} == {tab['id'] for tab in original})
    else:
        p.wait_for(lambda: tab_at(api, pin_url)['windowId'] != original_window,
                   browser.process, timeout=8)
        permanent = profiles.owned_windows(browser, inp)
        record(result, 'Tearing off retains exactly one new OS host', len(permanent) == len(windows) + 1)
        record(result, 'Tearing off retains all original tabs and the pin',
               {tab['id'] for tab in tabs(api)} == {tab['id'] for tab in original}
               and tab_at(api, pin_url)['pinned'])
        new_window = tab_at(api, pin_url)['windowId']
        api('chrome.tabs.update(' + str(pin_id) + ',{pinned:false})')
        p.request(browser.port, '/json/activate/' + pinned['id'])
        private.activate_x_window(inp, next(iter(set(permanent) - set(windows))), browser.process)
        p.native_key(inp, 'w', ('Control_L',))
        p.wait_for(lambda: not any(tab['id'] == pin_id for tab in tabs(api))
                   and any(tab['windowId'] == new_window and tab['url'].startswith('chrome://newtab')
                           for tab in tabs(api)), browser.process, timeout=8)
        record(result, 'A permanent tearoff Space keeps its normal last-tab replacement',
               profiles.owned_windows(browser, inp) == permanent)
    if args.case == 'native-rejoin':
        witness = profiles.document_witness(browser, pinned)
        record(result, 'Native rejoin retains the dragged document and draft',
               witness == {key: before['pin'][key] for key in witness})
    witness = profiles.document_witness(browser, ordinary)
    record(result, 'Native window changes retain the original partner document and draft',
           witness == {key: before['ordinary'][key] for key in witness})
    p.native.capture(tree, inp, args.output / 'native', args.case + '-final')


def flow(browser, tree, inp, result, args, base):
    personal_api = p.tab.controller(browser)
    api = personal_api
    api_url = p.wait_for(lambda: next((page['url'] for page in p.pages(browser)
        if page['url'].endswith('/control.html')), None), browser.process)
    personal_control = next(page for page in p.pages(browser) if page['url'] == api_url)
    personal_context = profiles.context(browser, personal_control)
    output = args.output / 'native'
    if args.space == 'Work' and args.case != 'cross-space-menu':
        work, api = switch_work(browser, tree, inp, api_url)
        if args.layout == 'horizontal':
            horizontal_profile(browser, inp)
        record(result, 'Work uses its expected native Profile context',
               (profiles.context(browser, work) != personal_context) == (args.spaces_mode != 'legacy'))
    if args.case.startswith('drag'):
        settings = profiles.new_tab(browser, inp, 'chrome://settings/appearance')
        p.wait_for(lambda: browser.ev('!!chrome.settingsPrivate', settings), browser.process)
        enabled = browser.ev('new Promise(r=>chrome.settingsPrivate.setPref('
            '"browser.split_view_drag_and_drop_enabled",true,"",r))', settings)
        assert enabled
    pin_url, ordinary_url = base + '/pinned', base + '/ordinary'
    pinned = p.open_page(browser, tree, inp, pin_url)
    p.seed_account(browser, pinned, args.space.lower())
    p.pin(browser, api, pinned)
    pin_api = api
    if args.case == 'cross-space-menu':
        work, api = switch_work(browser, tree, inp, api_url)
        if args.layout == 'horizontal':
            horizontal_profile(browser, inp)
        record(result, 'Cross-Space fixture uses its expected native Profile contexts',
               (profiles.context(browser, work) != personal_context) == (args.spaces_mode != 'legacy'))
    ordinary = p.open_page(browser, tree, inp, ordinary_url)
    expected_view = ('HorizontalTabStripRegionView(?:Old)?' if args.layout == 'horizontal'
                     else 'VerticalTabStripRegionView')
    record(result, 'Selected Profile uses the requested native tab layout',
           bool(p.native.visible(tree.read(), 'class_name=' + expected_view + ' ')))
    partner = None
    group_id = None
    original_split = None
    if args.case == 'join-pin-to-split' or args.case.startswith('grouped-join-'):
        partner = p.open_page(browser, tree, inp, base + '/partner')
        choose_split(browser, tree, inp, output, api, ordinary, partner)
        p.wait_for(lambda: tab_at(api, ordinary_url).get('splitViewId', -1) >= 0,
                   browser.process, timeout=6)
        original_split = tab_at(api, ordinary_url)['splitViewId']
        if args.case.startswith('grouped-join-'):
            identifiers = [tab_at(api, ordinary_url)['id'], tab_at(api, base + '/partner')['id']]
            group_id = api('chrome.tabs.group({tabIds:' + json.dumps(identifiers) + '})')
            api('chrome.tabGroups.update(' + str(group_id) + ',{title:"Keep grouped pages"})')
            time.sleep(.5)

    def all_ids():
        return {tab['id'] for owner in (api, pin_api) for tab in tabs(owner)}

    if args.case == 'closed-pin-picker':
        identities = all_ids()
        loaded_id = tab_at(api, pin_url)['id']
        partner_witness = profiles.document_witness(browser, ordinary)
        p.tab.activate(browser, tree, inp, pinned)
        p.native_key(inp, 'w', ('Control_L',))
        unloaded = close.unloaded(browser, api, pin_url)
        record(result, 'Split picker starts with a saved unloaded pin',
               unloaded['pinned'] and unloaded['discarded'])
        saved_identities = all_ids()
        record(result, 'Unloading retains every other tab identity',
               saved_identities == (identities - {loaded_id}) | {unloaded['id']})
        choose_split(browser, tree, inp, output, api, ordinary, None,
                     saved_target=tab_at(api, pin_url))

        def loaded_split():
            pair = [tab_at(api, pin_url), tab_at(api, ordinary_url)]
            split = pair[0].get('splitViewId', -1)
            return pair if split >= 0 and split == pair[1].get('splitViewId', -1) \
                and not pair[0]['discarded'] else None

        pair = p.wait_for(loaded_split, browser.process, timeout=8)
        record(result, 'The ordinary tab picker opens its closed pin directly into a split',
               all(tab['pinned'] for tab in pair), pair)
        record(result, 'Splitting a closed pin retains every saved tab identity',
               all_ids() == saved_identities,
               {'before': sorted(saved_identities), 'after': sorted(all_ids())})
        reopened = p.wait_for(lambda: next((page for page in p.pages(browser)
            if page['url'] == pin_url), None), browser.process)
        p.ready(browser, reopened, pin_url)
        tree.read()
        panes = pane_bounds(tree.text)
        record(result, 'The reopened pin and partner render in separate native panes',
               pin_url in panes and ordinary_url in panes, panes)
        record(result, 'Opening the saved pin into a split retains the partner document and draft',
               profiles.document_witness(browser, ordinary) == partner_witness)
        p.native.capture(tree, inp, output, 'closed-pin-picker-final')
        return

    if args.case == 'closed-pin-menu':
        p.tab.activate(browser, tree, inp, pinned)
        p.native_key(inp, 'w', ('Control_L',))
        unloaded = close.unloaded(browser, api, pin_url)
        record(result, 'Closing before split retains the saved unloaded pin', unloaded['pinned'])
        pinned = close.activate_pin(browser, tree, inp, 'pin pinned', pin_url)
        p.ready(browser, pinned, pin_url)
    before_ids = all_ids()
    before_windows = profiles.owned_windows(browser, inp) if args.drag_path == 'native' else None
    if before_windows is not None:
        p.write(output / 'native-drag-before-tabs.json', tabs(api))
    before = {'pin': p.state(browser, pinned), 'ordinary': p.state(browser, ordinary)}
    if partner:
        before['partner'] = p.state(browser, partner)
    if args.case in ('native-rejoin', 'native-tearoff'):
        native_window_flow(browser, tree, inp, result, args, api, pinned, ordinary, before)
        return
    if args.case == 'cross-space-menu':
        context_menu(browser, tree, inp, output, ordinary, 'pin ordinary', 'foreign-pin')
        labels = p.popup_labels(output, 'foreign-pin-choices', ['New tab', 'pin pinned'], required=False)
        record(result, 'Work split picker opens and excludes the Personal pin',
               'New tab' in labels and 'pin pinned' not in labels, sorted(labels))
        p.native_key(inp, 'Escape')
        p.native_key(inp, 'Escape')
        pair = [tab_at(pin_api, pin_url), tab_at(api, ordinary_url)]
        record(result, 'Cross-Space picker leaves both tabs unsplit',
               all(tab.get('splitViewId', -1) < 0 for tab in pair), pair)
    else:
        source, target = ((pinned, ordinary) if args.case.endswith('pin-to-tab')
                          else (ordinary, pinned))
        if args.case.startswith('drag'):
            drag_split(browser, tree, inp, output, source, target, args.direction,
                       args.layout, args.drag_path)
        else:
            choose_split(browser, tree, inp, output, api, source, target)
        if args.case.startswith('grouped-join-'):
            cancel = r'class_name=MdTextButton name=Cancel name_from='
            try:
                p.wait_for(lambda: bool(p.native.visible(tree.read(), cancel)),
                           browser.process, timeout=4)
                confirmation = True
            except TimeoutError:
                confirmation = False
            record(result, 'Removing the saved group requires its native confirmation', confirmation)
            if confirmation:
                p.native.capture(tree, inp, output, 'saved-group-confirmation')
                if args.case.endswith('cancel'):
                    p.native_key(inp, 'Escape')
                else:
                    button = p.native.one(tree.read(),
                        r'class_name=MdTextButton name=Delete [Gg]roup name_from=')
                    assert ' DEFAULT ' in button['line'], button
                    p.native_key(inp, 'Return')
                p.wait_for(lambda: not p.native.visible(tree.read(), cancel), browser.process)

        if args.case == 'grouped-join-cancel':
            pair = [tab_at(api, pin_url), tab_at(api, ordinary_url)]
            member = tab_at(api, base + '/partner')
            record(result, 'Cancel preserves the saved pin and original grouped split',
                   pair[0]['pinned'] and pair[0].get('splitViewId', -1) < 0
                   and pair[1]['splitViewId'] == member['splitViewId'] == original_split
                   and pair[1]['groupId'] == member['groupId'] == group_id)
            record(result, 'Cancel retains every loaded document and draft',
                   p.state(browser, pinned) == before['pin']
                   and p.state(browser, ordinary) == before['ordinary']
                   and p.state(browser, partner) == before['partner'])
            record(result, 'Cancel retains the exact original tab identities',
                   all_ids() == before_ids)
            p.native.capture(tree, inp, output, 'saved-group-cancelled')
            return

        def joined():
            values = [tab_at(api, pin_url), tab_at(api, ordinary_url)]
            split = values[0].get('splitViewId', -1)
            return values if split >= 0 and values[1].get('splitViewId', -1) == split else None

        try:
            pair = p.wait_for(joined, browser.process, timeout=6)
        except TimeoutError:
            pair = [tab_at(api, pin_url), tab_at(api, ordinary_url)]
        split = pair[0].get('splitViewId', -1)
        formed = split >= 0 and pair[1].get('splitViewId', -1) == split
        record(result, args.case + ' creates a same-Space split', formed, pair)
        record(result, 'Split creation preserves the saved pin', pair[0]['pinned'], pair[0])
        record(result, 'The resulting split has a consistent pinned state',
               formed and all(tab['pinned'] for tab in pair), pair)
        if partner:
            joined_partner = tab_at(api, base + '/partner')
            record(result, 'Adding the pin retains all three split members',
                   formed and joined_partner.get('splitViewId', -1) == split
                   and joined_partner['pinned'], joined_partner)
        if formed:
            states = [browser.ev('({visible:document.visibilityState,width:innerWidth,height:innerHeight})', page)
                      for page in (pinned, ordinary)]
            tree.read()
            panes = pane_bounds(tree.text)
            required = (pin_url, ordinary_url, *((base + '/partner',) if partner else ()))
            record(result, 'Both real pages are visible in separate split panes',
                   all(state['visible'] == 'visible' and state['width'] > 100 and state['height'] > 100
                       for state in states) and all(url in panes for url in required), {'states': states, 'panes': panes})
            if args.case.startswith('drag') and all(url in panes for url in required):
                source_url, target_url = (browser.ev('location.href', page) for page in (source, target))
                first, second = panes[source_url], panes[target_url]
                horizontal = args.direction in ('left', 'right')
                axis, dimension = ('x', 'w') if horizontal else ('y', 'h')
                source_first = args.direction in ('left', 'above')
                ordered = (first[axis] + first[dimension] <= second[axis] if source_first
                           else second[axis] + second[dimension] <= first[axis])
                record(result, 'Dragged page occupies the requested ' + args.direction + ' pane',
                       ordered, {'direction': args.direction, 'source': first, 'target': second})
                source_tab, target_tab = tab_at(api, source_url), tab_at(api, target_url)
                record(result, 'Tab order agrees with rendered pane order',
                       (source_tab['index'] < target_tab['index']) == source_first,
                       {'source': source_tab, 'target': target_tab})
    if before_windows is not None:
        try:
            p.wait_for(lambda: all_ids() == before_ids
                       and profiles.owned_windows(browser, inp) == before_windows,
                       browser.process, timeout=8)
        except TimeoutError:
            pass
        record(result, 'Native drag closes its temporary OS windows',
               profiles.owned_windows(browser, inp) == before_windows,
               {'before': before_windows, 'after': profiles.owned_windows(browser, inp)})
        p.write(output / 'native-drag-final-tabs.json', tabs(api))
    record(result, 'Splitting retains exactly the original tab identities',
           all_ids() == before_ids, {'before': sorted(before_ids), 'after': sorted(all_ids())})
    record(result, 'Splitting retains both loaded documents, drafts and Space storage',
           p.state(browser, pinned) == before['pin'] and p.state(browser, ordinary) == before['ordinary'])
    if partner:
        record(result, 'Adding the pin retains the original split partner document',
               p.state(browser, partner) == before['partner'])
    p.native.capture(tree, inp, output, 'mixed-pin-final')


def main():
    parser = p.cli(__doc__)
    parser.add_argument('--case', choices=CASES, required=True)
    parser.add_argument('--space', choices=('Personal', 'Work'), default='Personal')
    parser.add_argument('--layout', choices=('sidebar', 'horizontal'), default='sidebar')
    parser.add_argument('--direction', choices=('left', 'right', 'above', 'below'), default='right')
    parser.add_argument('--drag-path', choices=('page', 'native'), default='page')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--profile-spaces', dest='spaces_mode', action='store_const', const='profile')
    modes.add_argument('--legacy-spaces', dest='spaces_mode', action='store_const', const='legacy')
    parser.set_defaults(spaces_mode='default')
    parser.add_argument('--observe-only', action='store_true')
    args = parser.parse_args()
    result = profiles.admit(args, Path(__file__))
    result.update(case=args.case, space=args.space, layout=args.layout, direction=args.direction,
                  spaces_mode=args.spaces_mode, observe_only=args.observe_only)
    budget = p.Budget()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), p.Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        with tempfile.TemporaryDirectory(prefix='owned-pin-split-', dir=args.output) as tmp:
            profile = Path(tmp)
            p.prefs(profile, width=200)
            path = profile / 'Default/Preferences'
            value = json.loads(path.read_text())
            value['noventra'] = {'browser': {'account_spaces': {'': 'Personal', WORK: 'Work'},
                                           'account_space_order': ['', WORK]}}
            path.write_text(json.dumps(value))
            extension = p.extension_fixture(profile)
            manifest = json.loads((extension / 'manifest.json').read_text())
            manifest['permissions'].append('tabGroups')
            (extension / 'manifest.json').write_text(json.dumps(manifest))
            flags = (*p.native.FLAGS, '--load-extension=' + str(extension.resolve()))
            if args.spaces_mode != 'default':
                flags += (('--noventra-profile-spaces' if args.spaces_mode == 'profile'
                           else '--disable-noventra-profile-spaces'),)
            with p.session(args, profile, args.output / 'native', result, budget, flags) as (browser, tree, inp):
                if args.layout == 'horizontal':
                    p.native.apply(browser, p.native.top_layout(browser.state()['layout'],
                                                               (*p.native.TOP_ORDER, 'spaces')))
                tree = profiles.SpaceNativeTree(browser, inp)
                tree.bind_browser(browser.settings)
                flow(browser, tree, inp, result, args, base)
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
