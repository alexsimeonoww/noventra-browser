"""Prove actual Profile service ownership and retained pages in one window.

Run on an owned Xvfb or private Wayland display with exact runtime hashes. All browser
profiles, credentials, extension state and HTTP pages are disposable fixtures.
Space switching and new-tab entry use native input. Service APIs seed and read
real native stores rather than mocking profile isolation.
"""

import ctypes as C
import http.server
import json
import os
from pathlib import Path
import shutil
import threading

import incognito_space_smoke as private
import pinned_tabs_smoke as p
import space_capacity_resize_smoke as spaces
import incognito_space_visibility_smoke as visibility

WORK = '55000000-0000-4000-8000-000000000001'


class XErrorEvent(C.Structure):
    _fields_ = [('type', C.c_int), ('display', C.c_void_p),
                ('resourceid', C.c_ulong), ('serial', C.c_ulong),
                ('error_code', C.c_ubyte), ('request_code', C.c_ubyte),
                ('minor_code', C.c_ubyte)]


def admit(args, source):
    if os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1':
        return p.admit(args, source)
    import javascript_modal_wayland_smoke as wayland
    compositor = wayland.Compositor()
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.absolute()
    assert args.browser.parent == Path(os.environ['NOVENTRA_TEST_RUNTIME']).resolve(strict=True)
    assert args.output.is_relative_to(Path(os.environ['NOVENTRA_TEST_OUTPUT']).resolve(strict=True))
    assert p.sha(args.browser) == args.expected_binary
    assert p.sha(args.browser.parent / 'resources.pak') == args.expected_resources
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(source, args.output / 'executed-test.py')
    shutil.copyfile(__file__, args.output / 'executed-profiles.py')
    shutil.copyfile(wayland.__file__, args.output / 'executed-wayland.py')
    return {'passed': False, 'checks': [], 'normal_shutdowns': [],
            'runtime': str(args.browser), 'binary_sha256': args.expected_binary,
            'resources_sha256': args.expected_resources, 'test_sha256': p.sha(source),
            'platform': 'wayland', 'compositor_pid': compositor.pid}


def owned_windows(browser, inp):
    if inp.platform != 'wayland':
        x, display = inp.pointer.x, inp.pointer.d
        x.XSetErrorHandler.argtypes = [C.c_void_p]
        x.XSetErrorHandler.restype = C.c_void_p
        x.XSync.argtypes = [C.c_void_p, C.c_int]
        errors = []

        @C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(XErrorEvent))
        def record_error(_, event):
            value = event.contents
            errors.append((value.error_code, value.request_code, value.resourceid))
            return 0

        x.XSync(display, 0)
        previous = x.XSetErrorHandler(C.cast(record_error, C.c_void_p))
        try:
            # Temporary drag windows can disappear between XQueryTree and inspection.
            for _ in range(3):
                errors.clear()
                failed = None
                try:
                    windows = private.owned_windows(browser, inp)
                except AssertionError as error:
                    failed = error
                x.XSync(display, 0)
                assert all(error[0] == 3 for error in errors), errors
                if not errors:
                    if failed is not None:
                        raise failed
                    return windows
            raise AssertionError(('Window enumeration kept racing destruction', errors))
        finally:
            x.XSync(display, 0)
            x.XSetErrorHandler(previous)
    from javascript_modal_wayland_smoke import Compositor
    clients = json.loads(Compositor().command('j/clients'))
    return sorted(client['address'] for client in clients
                  if client['pid'] == browser.process.pid and client['mapped'])


class SpaceNativeTree(spaces.SpaceNativeTree):
    def __init__(self, browser, inp):
        if inp.platform != 'wayland':
            super().__init__(browser, inp)
            return
        windows = owned_windows(browser, inp)
        assert len(windows) == 1, windows
        self.inp, self.window = inp, windows[0]
        self.bound_browser_session = None
        private.NativeUITree.__init__(self, browser)

    def read(self):
        if self.inp.platform != 'wayland':
            return super().read()
        self.widget_bounds = None
        session = self.bound_browser_session
        if session is not None and session != self.host_session:
            self.bound_browser_session = self.host_session
            private.PositionedNativeUITree.read(self)
            host = p.native.one(self, r'class_name=BrowserView ')
            self.widget_bounds = private.NativeUITree.rect(self, host)
            self.bound_browser_session = session
        return private.PositionedNativeUITree.read(self)

    def rect(self, node):
        if self.inp.platform != 'wayland' or getattr(self, 'widget_bounds', None):
            return super().rect(node)
        rect = private.NativeUITree.rect(self, node)
        root = node
        while root and 'class_name=BrowserRootView ' not in root['line']:
            root = self.nodes.get(root['parent'])
        if root:
            rect['x'] -= root['bounds'][0]
            rect['y'] -= root['bounds'][1]
        return rect


def new_tab(browser, inp, url):
    before = {page['id'] for page in p.pages(browser)}
    p.native_key(inp, 't', ('Control_L',))
    page = p.wait_for(lambda: next((page for page in p.pages(browser)
        if page['id'] not in before), None), browser.process)
    browser.ev('location.href=' + json.dumps(url) + ';true', page)
    p.wait_for(lambda: browser.ev('location.href===' + json.dumps(url)
        + '&&document.readyState==="complete"', page), browser.process)
    return page


def extension_api(browser, page):
    p.wait_for(lambda: browser.ev('!!globalThis.chrome?.runtime?.id', page), browser.process)
    return lambda expression: browser.ev('(async()=>{const value=await('
        + expression + ');return value??null})()', page)


def history(api, url):
    return api('chrome.history.search({text:' + json.dumps(url)
               + ',startTime:0,maxResults:100}).then(items=>items.map(i=>i.url))')


def bookmarks(api, title):
    return api('chrome.bookmarks.search({title:' + json.dumps(title)
               + '}).then(items=>items.map(i=>({title:i.title,url:i.url})))')


def passwords(browser, inp, username=None):
    page = new_tab(browser, inp, 'chrome://password-manager/passwords')
    p.wait_for(lambda: browser.ev('!!chrome.passwordsPrivate', page), browser.process)
    if username:
        options = {'url': 'https://fixture.example/', 'username': username,
                   'password': 'Disposable-test-password-42', 'note': '',
                   'useAccountStore': False}
        browser.ev('chrome.passwordsPrivate.addPassword('
                   + json.dumps(options) + ').then(()=>true)', page)
    value = browser.ev('chrome.passwordsPrivate.getSavedPasswordList()'
                       '.then(items=>items.map(i=>i.username))', page)
    return value


def download(browser, inp, api, folder, url, name):
    folder.mkdir(parents=True, exist_ok=True)
    settings = new_tab(browser, inp, 'chrome://settings/downloads')
    configured = browser.ev('new Promise(r=>chrome.settingsPrivate.getPref('
        '"download.default_directory",p=>r(p.value)))', settings)
    assert Path(configured).resolve() == folder.resolve(), configured
    identifier = api('chrome.downloads.download({url:' + json.dumps(url)
                     + ',filename:' + json.dumps(name) + ',saveAs:false})')
    p.wait_for(lambda: api('chrome.downloads.search({id:' + str(identifier)
        + '}).then(items=>items[0]?.state)') == 'complete', browser.process)
    value = api('chrome.downloads.search({id:' + str(identifier) + '}).then(items=>items[0])')
    file = Path(value['filename']).resolve()
    assert file.is_relative_to(folder.resolve()) and file.is_file(), value


def choose(browser, tree, inp, name):
    node = p.native.one(tree.read(), spaces.DOT + name + r' name_from=')
    inp.pointer.click(*tree.center(node))


def context(browser, page):
    return private.cdp(browser, 'Target.getTargetInfo',
                       {'targetId': page['id']})['targetInfo']['browserContextId']


def document_witness(browser, page):
    return browser.ev('({url:location.href,token:window.pinToken,'
                      'timeOrigin:performance.timeOrigin,'
                      'draft:document.querySelector("#draft")?.value})', page)


def retained_document(browser, tree, inp, page, expected, result, name):
    witness = document_witness(browser, page)
    p.check(result, name + ' retains its document before activation',
            witness == {key: expected[key] for key in witness}, witness)
    p.tab.activate(browser, tree, inp, page)
    p.check(result, name + ' retains its complete website state after activation',
            p.state(browser, page) == expected)


def main():
    args = p.cli(__doc__).parse_args()
    result = admit(args, Path(__file__))
    budget = p.Budget()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), p.Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    base = 'http://127.0.0.1:' + str(server.server_port)
    try:
        profile = args.output / 'profile'
        p.prefs(profile, width=200)
        pref_path = profile / 'Default/Preferences'
        pref = json.loads(pref_path.read_text())
        pref['noventra'] = {'browser': {
            'account_spaces': {'': 'Personal', WORK: 'Work'},
            'account_space_order': ['', WORK]}}
        downloads = profile / 'fixture-downloads'
        downloads.mkdir()
        pref['download'] = {'default_directory': str(downloads.resolve()),
                            'prompt_for_download': False}
        pref_path.write_text(json.dumps(pref))
        config = profile / 'fixture-xdg-config'
        config.mkdir()
        (config / 'user-dirs.dirs').write_text('XDG_DOWNLOAD_DIR="'
            + str(downloads.resolve()) + '"\n')
        os.environ['XDG_CONFIG_HOME'] = str(config.resolve())
        fixture = p.extension_fixture(profile)
        manifest = json.loads((fixture / 'manifest.json').read_text())
        manifest['permissions'] += ['history', 'storage', 'management', 'downloads']
        (fixture / 'manifest.json').write_text(json.dumps(manifest))
        dummy = profile / 'independent-extension'
        dummy.mkdir()
        (dummy / 'manifest.json').write_text(json.dumps({
            'manifest_version': 3, 'name': 'Independent extension fixture',
            'version': '1.0'}))
        flags = (*p.native.FLAGS,
                 '--load-extension=' + str(fixture.resolve()) + ',' + str(dummy.resolve()))
        with p.session(args, profile, args.output / 'native', result, budget, flags) as (browser, _, inp):
            private.apply_native_test_layout(browser.port, edge='left',
                visible=['tabs', 'spaces', 'new-tab', 'address', 'action:bookmarks', 'menu'])
            personal = new_tab(browser, inp, base + '/personal')
            p.ready(browser, personal)
            tree = SpaceNativeTree(browser, inp)
            tree.bind_browser(personal)
            windows = owned_windows(browser, inp)
            p.check(result, 'Personal starts in one real OS window', len(windows) == 1)
            p.seed_account(browser, personal, 'personal')
            personal_before = p.state(browser, personal)
            personal_api = p.tab.controller(browser)
            api_url = p.wait_for(lambda: next((page['url'] for page in p.pages(browser)
                if page['url'].endswith('/control.html')), None), browser.process)
            personal_api('chrome.bookmarks.create({title:"Personal only",url:'
                         + json.dumps(base + '/personal') + '})')
            personal_api('chrome.storage.local.set({owner:"personal"})')
            dummy_id = personal_api('chrome.management.getAll().then(items=>items.find('
                'i=>i.name==="Independent extension fixture").id)')
            personal_api('chrome.management.setEnabled(' + json.dumps(dummy_id) + ',false)')
            download(browser, inp, personal_api, downloads,
                     base + '/personal-download', 'personal.txt')
            p.check(result, 'Personal native password store accepts its fixture',
                    'personal-fixture' in passwords(browser, inp, 'personal-fixture'))
            p.wait_for(lambda: base + '/personal' in history(personal_api, base + '/personal'), browser.process)
            before_work = {page['id'] for page in p.pages(browser)}
            choose(browser, tree, inp, 'Work')
            work = p.wait_for(lambda: next((page for page in p.pages(browser)
                if page['id'] not in before_work
                and page['url'].startswith(('chrome://newtab', 'chrome://new-tab-page', 'about:blank'))), None), browser.process)
            browser.ev('location.href=' + json.dumps(base + '/work') + ';true', work)
            p.ready(browser, work, base + '/work')
            p.native_key(inp, 'Escape')
            tree.bind_browser(work)
            p.check(result, 'Work owns a different native BrowserContext', context(browser, work) != context(browser, personal))
            p.check(result, 'Work shares the same physical OS window', owned_windows(browser, inp) == windows)
            p.check(result, 'Work starts without Personal website data',
                    p.state(browser, work)['cookie'] == '' and p.state(browser, work)['owner'] is None)
            p.seed_account(browser, work, 'work')
            work_before = p.state(browser, work)
            work_control = new_tab(browser, inp, api_url)
            work_api = extension_api(browser, work_control)
            p.check(result, 'Work history excludes Personal visits', base + '/personal' not in history(work_api, base + '/personal'))
            p.check(result, 'Work bookmarks exclude Personal bookmarks', bookmarks(work_api, 'Personal only') == [])
            p.check(result, 'Extension storage is separate in Work', work_api('chrome.storage.local.get("owner")') == {})
            p.check(result, 'Work extensions do not inherit Personal disabled state',
                    work_api('chrome.management.get(' + json.dumps(dummy_id) + ').then(i=>i.enabled)'))
            p.check(result, 'Work download list excludes Personal downloads',
                    work_api('chrome.downloads.search({}).then(items=>items.length)') == 0)
            p.check(result, 'Work passwords exclude Personal credentials', passwords(browser, inp) == [])
            work_api('chrome.bookmarks.create({title:"Work only",url:' + json.dumps(base + '/work') + '})')
            work_api('chrome.storage.local.set({owner:"work"})')
            p.check(result, 'Work native password store accepts only its fixture',
                    passwords(browser, inp, 'work-fixture') == ['work-fixture'])
            p.wait_for(lambda: base + '/work' in history(work_api, base + '/work'), browser.process)
            download(browser, inp, work_api, downloads,
                     base + '/work-download', 'work.txt')
            choose(browser, tree, inp, 'Personal')
            tree.bind_browser(personal)
            retained_document(browser, tree, inp, personal, personal_before, result, 'Personal')
            p.check(result, 'Personal history excludes Work visits', base + '/work' not in history(personal_api, base + '/work'))
            p.check(result, 'Personal bookmarks exclude Work bookmarks', bookmarks(personal_api, 'Work only') == [])
            p.check(result, 'Personal extension state survives Work changes', personal_api('chrome.storage.local.get("owner")') == {'owner': 'personal'})
            p.check(result, 'Personal passwords remain independent', passwords(browser, inp) == ['personal-fixture'])
            p.check(result, 'Personal download list retains only its own download',
                    personal_api('chrome.downloads.search({}).then(items=>items.map(i=>i.url))') == [base + '/personal-download'])
            choose(browser, tree, inp, 'Work')
            tree.bind_browser(work)
            retained_document(browser, tree, inp, work, work_before, result, 'Work')
            p.check(result, 'Both normal Space shortcuts remain available',
                    {spaces.dot_name(node) for node in p.native.visible(tree.read(), spaces.DOT)} == {'Personal', 'Work'})
            private.capture(tree, inp, args.output, 'separate-work-profile')
            secret = visibility.create_from_menu(browser, tree, inp, args.output)
            private.goto(browser, secret, base + '/private')
            tree.bind_browser(secret)
            shortcuts = {spaces.dot_name(node) for node in p.native.visible(tree.read(), spaces.DOT)}
            p.check(result, 'Personal, Work and Incognito are all visible in the private Space',
                    shortcuts == {'Personal', 'Work', 'Incognito'}, sorted(shortcuts))
            p.check(result, 'Incognito is a separate context with an empty website jar',
                    context(browser, secret) != context(browser, work)
                    and p.state(browser, secret)['cookie'] == '')
            p.seed_account(browser, secret, 'private')
            secret_before = p.state(browser, secret)
            choose(browser, tree, inp, 'Personal')
            tree.bind_browser(personal)
            p.check(result, 'Incognito stays available when switching to Personal',
                    {spaces.dot_name(node) for node in p.native.visible(tree.read(), spaces.DOT)}
                    == {'Personal', 'Work', 'Incognito'})
            choose(browser, tree, inp, 'Incognito')
            tree.bind_browser(secret)
            p.check(result, 'Switching back retains the private loaded document and website data',
                    p.state(browser, secret) == secret_before)
            p.check(result, 'Ordinary Incognito visits stay out of Work history',
                    base + '/private' not in history(work_api, base + '/private'))
            p.check(result, 'Ordinary Incognito visits stay out of Personal history',
                    base + '/private' not in history(personal_api, base + '/private'))
            p.native_key(inp, 'd', ('Control_L',))
            p.native_key(inp, 'Escape')
            p.wait_for(lambda: bookmarks(work_api, 'pin private'), browser.process)
            p.check(result, 'An explicit private bookmark belongs to Work only',
                    bookmarks(personal_api, 'pin private') == [])
            p.native_key(inp, 'w', ('Control_L',))
            p.wait_for(lambda: all(page['id'] != secret['id'] for page in p.pages(browser)), browser.process)
            tree.bind_browser(work)
            p.wait_for(lambda: {spaces.dot_name(node) for node in p.native.visible(tree.read(), spaces.DOT)}
                       == {'Personal', 'Work'}, browser.process)
            p.check(result, 'Closing private tabs returns to Work without changing its website data',
                    p.state(browser, work) == work_before)
        p.check(result, 'Browser closes normally with both Profiles', all(row['exit'] == 0 for row in result['normal_shutdowns']))
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)
        budget.close()
        p.write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
