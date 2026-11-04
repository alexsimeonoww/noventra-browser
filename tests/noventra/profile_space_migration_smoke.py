"""Cold legacy-Space migration and native session retention on owned Xvfb.

Supply the immutable current and candidate browser hashes. The current browser
seeds only a disposable profile. Two candidate launches reuse that profile and
the same loopback origin. Native clicks switch Spaces and restore their saved
tabs; real storage and tab APIs inspect the result. No personal data is opened.
"""

from copy import copy
import http.server
import json
from pathlib import Path
import shutil
import threading
import time
import urllib.parse

import incognito_space_smoke as private
import pin_split_smoke as split
import pinned_tabs_smoke as p
import profile_spaces_smoke as profiles
import space_capacity_resize_smoke as spaces


WORK = '77000000-0000-4000-8000-000000000001'
STORAGE_SEED = '''(async()=>{
  const owner=OWNER;
  await fetch('/set?owner='+owner);
  localStorage.setItem('owner',owner);
  sessionStorage.setItem('migration-draft',owner+' original session');
  const db=await new Promise((resolve,reject)=>{
    const request=indexedDB.open('migration-fixture',1);
    request.onupgradeneeded=()=>request.result.createObjectStore('owners');
    request.onsuccess=()=>resolve(request.result);
    request.onerror=()=>reject(request.error);
  });
  await new Promise((resolve,reject)=>{
    const tx=db.transaction('owners','readwrite');
    tx.objectStore('owners').put(owner,'owner');
    tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error);
    tx.onabort=()=>reject(tx.error);
  });
  db.close();
  const cache=await caches.open('migration-fixture');
  await cache.put('/cached-owner',new Response(owner));
  await navigator.serviceWorker.register('/migration-worker.js');
  await navigator.serviceWorker.ready;
  return true;
})()'''
STORAGE_STATE = '''(async()=>{
  const databases=await indexedDB.databases();
  let owner=null;
  if(databases.some(db=>db.name==='migration-fixture')){
    const db=await new Promise((resolve,reject)=>{
      const request=indexedDB.open('migration-fixture');
      request.onsuccess=()=>resolve(request.result);
      request.onerror=()=>reject(request.error);
    });
    owner=await new Promise((resolve,reject)=>{
      const request=db.transaction('owners').objectStore('owners').get('owner');
      request.onsuccess=()=>resolve(request.result??null);
      request.onerror=()=>reject(request.error);
    });
    db.close();
  }
  const response=await caches.match('/cached-owner');
  return {cookie:await(await fetch('/cookie')).text(),scriptCookie:document.cookie,
    local:localStorage.getItem('owner'),indexed:owner,
    cached:response?await response.text():null,
    worker:!!await navigator.serviceWorker.getRegistration('/'),
    session:sessionStorage.getItem('migration-draft')};
})()'''
SESSION_SEED = '''(async()=>{
  const owner=OWNER;
  await fetch('/session?owner='+owner);
  sessionStorage.setItem('migration-draft',owner+' retained session');
  return true;
})()'''


class Handler(p.Handler):
    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == '/migration-worker.js':
            data = ("self.addEventListener('install',event=>event.waitUntil(self.skipWaiting()));"
                    "self.addEventListener('activate',event=>event.waitUntil(self.clients.claim()));").encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/javascript')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif parsed.path == '/session':
            owner = urllib.parse.parse_qs(parsed.query)['owner'][0]
            self.send_response(200)
            self.send_header('Set-Cookie', 'session-owner=' + owner
                             + '; HttpOnly; SameSite=Lax; Path=/')
            self.send_header('Content-Length', '0')
            self.end_headers()
        else:
            super().do_GET()


def inventory(folder):
    files = {}
    for path in sorted(folder.rglob('*')):
        assert not path.is_symlink(), path
        if path.is_file():
            files[str(path.relative_to(folder))] = p.sha(path)
        else:
            assert path.is_dir(), path
    return files


def storage(browser, page):
    return browser.ev(STORAGE_STATE, page)


def seed(browser, page, owner):
    browser.ev(STORAGE_SEED.replace('OWNER', json.dumps(owner)), page)


def session_seed(browser, page, owner):
    browser.ev(SESSION_SEED.replace('OWNER', json.dumps(owner)), page)


def storage_is(value, owner, *, retained=False):
    cookies = set(value['cookie'].split('; ')) if value['cookie'] else set()
    expected = {'owner=' + owner}
    if retained:
        expected.add('session-owner=' + owner)
    return (cookies == expected and value['scriptCookie'] == ''
            and value['local'] == value['indexed'] == value['cached'] == owner
            and value['worker'] and value['session'] == owner
            + (' retained session' if retained else ' original session'))


def one_page(browser, url):
    values = [page for page in p.pages(browser) if page['url'] == url]
    assert len(values) == 1, {'expected': url, 'targets': p.pages(browser)}
    return values[0]


def activate(browser, tree, inp, page):
    p.request(browser.port, '/json/activate/' + page['id'])
    p.wait_for(lambda: browser.ev('document.visibilityState==="visible"', page),
               browser.process, timeout=8)
    tree.bind_browser(page)
    p.ready(browser, page)
    p.native_key(inp, 'Escape')


def controller(browser, inp, url):
    return profiles.extension_api(browser, profiles.new_tab(browser, inp, url))


def tab_state(api, urls):
    return api('chrome.tabs.query({}).then(tabs=>tabs.filter(t=>'
               + json.dumps(urls) + '.includes(t.url)).map(t=>({url:t.url,'
               'pinned:t.pinned,split:t.splitViewId,group:t.groupId,index:t.index})))')


def assert_saved_tabs(result, name, api, base):
    urls = [base + '/' + path for path in ('work-pin', 'work-a', 'work-b')]
    tabs = tab_state(api, urls)
    by_url = {tab['url']: tab for tab in tabs}
    p.check(result, name + ': each legacy Work tab exists exactly once',
            len(tabs) == len(by_url) == 3, tabs)
    p.check(result, name + ': the legacy pin is still pinned', by_url[urls[0]]['pinned'])
    first, second = [by_url[url] for url in urls[1:]]
    p.check(result, name + ': native split membership survives',
            first.get('split', -1) >= 0 and first['split'] == second.get('split'), tabs)
    p.check(result, name + ': native group membership and tab order survive',
            first.get('group', -1) >= 0 and first['group'] == second.get('group')
            and [tab['url'] for tab in sorted(tabs, key=lambda tab: tab['index'])]
            == result.setdefault('legacy_work_order', [tab['url'] for tab in sorted(
                tabs, key=lambda tab: tab['index'])]), tabs)
    title = api('chrome.tabGroups.get(' + str(first['group']) + ').then(group=>group.title)')
    p.check(result, name + ': native group title survives', title == 'Legacy grouped split', title)


def create_tree(browser, inp, page):
    tree = spaces.SpaceNativeTree(browser, inp)
    tree.bind_browser(page)
    return tree


def legacy(args, profile, result, budget, flags, base):
    old = copy(args)
    old.browser = args.baseline_browser
    with p.session(old, profile, args.output / 'legacy', result, budget, flags) as (browser, _, inp):
        private.apply_native_test_layout(browser.port, edge='left',
            visible=['tabs', 'spaces', 'new-tab', 'address', 'action:bookmarks', 'menu'])
        personal = profiles.new_tab(browser, inp, base + '/personal-pin')
        p.ready(browser, personal)
        tree = create_tree(browser, inp, personal)
        api = p.tab.controller(browser)
        control_url = p.wait_for(lambda: next((page['url'] for page in p.pages(browser)
            if page['url'].endswith('/control.html')), None), browser.process)
        p.pin(browser, api, personal)
        seed(browser, personal, 'personal')
        api('chrome.bookmarks.create({title:"Legacy shared bookmark",url:'
            + json.dumps(base + '/personal-pin') + '})')
        spaces.click_space(browser, tree, inp, 'Work')
        work = profiles.new_tab(browser, inp, base + '/work-pin')
        p.ready(browser, work)
        seed(browser, work, 'work')
        p.pin(browser, api, work)
        ordinary = profiles.new_tab(browser, inp, base + '/work-a')
        partner = profiles.new_tab(browser, inp, base + '/work-b')
        tree.bind_browser(work)
        split.choose_split(browser, tree, inp, args.output / 'legacy', api, ordinary, partner)
        p.wait_for(lambda: (tabs if len(tabs := tab_state(api,
            [base + '/work-a', base + '/work-b'])) == 2
            and all(tab.get('split', -1) >= 0 for tab in tabs)
            and tabs[0]['split'] == tabs[1]['split'] else None), browser.process)
        identifiers = api('chrome.tabs.query({}).then(ts=>ts.filter(t=>'
            + json.dumps([base + '/work-a', base + '/work-b'])
            + '.includes(t.url)).map(t=>t.id))')
        group = api('chrome.tabs.group({tabIds:' + json.dumps(identifiers) + '})')
        api('chrome.tabGroups.update(' + str(group) + ',{title:"Legacy grouped split"})')
        p.check(result, 'Legacy Work owns independent same-origin website storage',
                storage_is(storage(browser, work), 'work'))
        p.check(result, 'Legacy fixture shares one native BrowserContext',
                profiles.context(browser, personal) == profiles.context(browser, work))
        assert_saved_tabs(result, 'Legacy baseline', api, base)
        spaces.click_space(browser, tree, inp, 'Personal')
        activate(browser, tree, inp, personal)
        p.check(result, 'Legacy Personal website storage remains separate',
                storage_is(storage(browser, personal), 'personal'))
        p.native.capture(tree, inp, args.output / 'legacy', 'seeded-personal')
        return control_url


def candidate(args, profile, result, budget, flags, base, control_url, *, restart=False):
    phase = 'restart' if restart else 'migrated'
    with p.session(args, profile, args.output / phase, result, budget,
                   (*flags, '--restore-last-session')) as (browser, _, inp):
        personal = p.wait_for(lambda: next((page for page in p.pages(browser)
            if page['url'] == base + '/personal-pin'), None), browser.process)
        tree = create_tree(browser, inp, personal)
        activate(browser, tree, inp, personal)
        windows = private.owned_windows(browser, inp)
        p.check(result, phase + ': exactly one physical browser window', len(windows) == 1, windows)
        personal_api = controller(browser, inp, control_url)
        activate(browser, tree, inp, personal)
        p.check(result, phase + ': Personal keeps its website storage and session namespace',
                storage_is(storage(browser, personal), 'personal'))
        p.check(result, phase + ': old shared bookmarks remain in Personal',
                len(profiles.bookmarks(personal_api, 'Legacy shared bookmark')) == 1)
        profiles.choose(browser, tree, inp, 'Work')
        work = p.wait_for(lambda: next((page for page in p.pages(browser)
            if page['url'] == base + '/work-pin'), None), browser.process)
        activate(browser, tree, inp, work)
        work_api = controller(browser, inp, control_url)
        activate(browser, tree, inp, work)
        p.check(result, phase + ': Work owns a different actual BrowserContext',
                profiles.context(browser, work) != profiles.context(browser, personal))
        p.check(result, phase + ': Work shares the original physical window',
                private.owned_windows(browser, inp) == windows)
        actual = storage(browser, work)
        p.check(result, phase + ': retained partition cookies, DOM, IDB, cache, worker and session survive',
                storage_is(actual, 'work', retained=restart), actual)
        p.check(result, phase + ': migrated Work does not inherit the old shared library',
                profiles.bookmarks(work_api, 'Legacy shared bookmark') == [])
        assert_saved_tabs(result, phase, work_api, base)
        if not restart:
            session_seed(browser, work, 'work')
            rows = browser.call('noventraSpaces', 'create', '', 'Fresh')
            fresh_id = next(row['id'] for row in rows if row['name'] == 'Fresh')
            result['fresh_space'] = fresh_id
        before = p.state(browser, work)
        previous = {page['id'] for page in p.pages(browser)}
        profiles.choose(browser, tree, inp, 'Fresh')
        if restart:
            fresh = p.wait_for(lambda: next((page for page in p.pages(browser)
                if page['url'] == base + '/fresh-pin'), None), browser.process)
            activate(browser, tree, inp, fresh)
        else:
            fresh = p.wait_for(lambda: next((page for page in p.pages(browser)
                if page['id'] not in previous and page['url'].startswith(
                    ('chrome://newtab', 'chrome://new-tab-page', 'about:blank'))), None), browser.process)
            browser.ev('location.href=' + json.dumps(base + '/fresh-pin') + ';true', fresh)
            p.ready(browser, fresh)
            tree.bind_browser(fresh)
            actual = storage(browser, fresh)
            p.check(result, 'New default-partition Profile starts without retained website data',
                    actual == {'cookie': '', 'scriptCookie': '', 'local': None,
                               'indexed': None, 'cached': None, 'worker': False, 'session': None}, actual)
            seed(browser, fresh, 'fresh')
            session_seed(browser, fresh, 'fresh')
            fresh_api = controller(browser, inp, control_url)
            p.pin(browser, fresh_api, fresh)
        p.check(result, phase + ': Fresh owns a third actual BrowserContext',
                profiles.context(browser, fresh) not in {
                    profiles.context(browser, personal), profiles.context(browser, work)})
        actual = storage(browser, fresh)
        p.check(result, phase + ': fresh Profile retains session cookie and sessionStorage',
                storage_is(actual, 'fresh', retained=True), actual)
        profiles.choose(browser, tree, inp, 'Work')
        activate(browser, tree, inp, work)
        tree.bind_browser(work)
        p.check(result, phase + ': switching retains the exact loaded Work document',
                p.state(browser, work) == before)
        p.native.capture(tree, inp, args.output / phase, 'retained-work')


def main():
    parser = p.cli(__doc__)
    parser.add_argument('--baseline-browser', type=Path, required=True)
    parser.add_argument('--baseline-expected-binary', required=True)
    parser.add_argument('--baseline-expected-resources', required=True)
    args = parser.parse_args()
    result = p.admit(args, Path(__file__))
    args.baseline_browser = args.baseline_browser.resolve(strict=True)
    assert p.sha(args.baseline_browser) == args.baseline_expected_binary
    assert p.sha(args.baseline_browser.parent / 'resources.pak') == args.baseline_expected_resources
    assert args.baseline_browser != args.browser
    result['baseline'] = {'runtime': str(args.baseline_browser),
        'binary_sha256': args.baseline_expected_binary,
        'resources_sha256': args.baseline_expected_resources}
    for module in (p, private, split, profiles, spaces):
        source = Path(module.__file__)
        shutil.copyfile(source, args.output / ('executed-' + source.name))
        result.setdefault('helper_sources', {})[str(source)] = p.sha(source)
    budget = p.Budget()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        profile = args.output / 'profile'
        p.prefs(profile, width=200)
        path = profile / 'Default/Preferences'
        prefs = json.loads(path.read_text())
        prefs.update(noventra={'browser': {'account_spaces': {'': 'Personal', WORK: 'Work'},
            'account_space_order': ['', WORK]}}, session={'restore_on_startup': 1})
        path.write_text(json.dumps(prefs))
        fixture = p.extension_fixture(profile)
        path = fixture / 'manifest.json'
        manifest = json.loads(path.read_text())
        manifest['permissions'].append('tabGroups')
        path.write_text(json.dumps(manifest))
        flags = (*p.native.FLAGS, '--load-extension=' + str(fixture.resolve()))
        control_url = legacy(args, profile, result, budget, flags, base)
        original = inventory(profile / 'Default')
        p.write(args.output / 'legacy-profile-inventory.json', original)
        candidate(args, profile, result, budget, flags, base, control_url)
        recovery = profile / 'NoventraSpaceRecovery/Default'
        snapshot = recovery / 'profile'
        journal = json.loads((recovery / 'migration.json').read_text())
        p.check(result, 'Migration commits its durable journal', journal['state'] == 'committed')
        p.check(result, 'Recovery snapshot exactly preserves the cold original Profile',
                inventory(snapshot) == original)
        markers = [(path, json.loads(path.read_text()))
            for path in profile.glob('*/NoventraSpaceMigration.json')]
        matches = [(path, marker) for path, marker in markers if marker.get('space') == WORK]
        p.check(result, 'One native child owns the legacy Work partition', len(matches) == 1,
                [{'path': str(path), 'marker': marker} for path, marker in markers])
        marker_path, marker = matches[0]
        relative = Path(marker['partition'])
        assert not relative.is_absolute() and '..' not in relative.parts
        p.check(result, 'Original legacy Work partition remains byte-for-byte intact',
                inventory(profile / 'Default' / relative) == inventory(snapshot / relative))
        p.check(result, 'Migrated child keeps the legacy partition at its original relative path',
                (marker_path.parent / relative).is_dir())
        snapshot_before_restart = inventory(snapshot)
        candidate(args, profile, result, budget, flags, base, control_url, restart=True)
        p.check(result, 'Candidate restart never rewrites the immutable recovery snapshot',
                inventory(snapshot) == snapshot_before_restart)
        p.check(result, 'Candidate restart retains the original legacy Work partition',
                inventory(profile / 'Default' / relative) == inventory(snapshot / relative))
        p.check(result, 'All three owned browser launches close normally',
                len(result['normal_shutdowns']) == 3
                and all(item['exit'] == 0 for item in result['normal_shutdowns']),
                result['normal_shutdowns'])
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        p.write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
