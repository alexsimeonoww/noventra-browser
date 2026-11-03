"""Check optional password-manager setup in a disposable native browser.

Use the project's private Xvfb wrapper. The default mode never requests a store
installation. --install-bitwarden additionally contacts the real Chrome Web
Store, cancels its native permission prompt once, then accepts a second prompt
and requires the actual extension registry to report Bitwarden enabled.

--native-source-profile uses an owned-home mount to exercise the real browser
detector and bookmark importer without reading personal browser profiles. All
profiles and failure captures are retained. Normal quit uses Browser.close;
completion is read from disk after exit0 and checked again on normal restart.
"""
import argparse
from contextlib import contextmanager
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time

from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from interface_polish_smoke import native_key
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree
from onboarding_import_smoke import gesture
from workspace_management_smoke import protocol
from workspace_tools_smoke import native_text


BITWARDEN = 'nngceckbapebfimnlniiiahkandclblb'
MANAGERS = ('protonPass', 'bitwarden', 'onePassword', 'keePassXC', 'dashlane', 'icloudPasswords')
MANAGER_TITLES = ('Proton Pass', 'Bitwarden', '1Password', 'KeePassXC', 'Dashlane', 'iCloud Passwords')
MANAGER_EXTENSIONS = ('ghmbeldphafepmbegfdlkpapadhbakde', BITWARDEN,
                      'aeblfdkhhhdcdjpifhhbdiojplfjncoa', 'oboonakemofpalcgghocfoadofidjkkk',
                      'fdjamakpfbbddfjaooikfcpapjohcfmg', 'pejdijmoenmkgeppbflobdenhhabjlaj')
NTP = '''(()=>{const app=document.querySelector('ntp-app'),root=app?.shadowRoot;
 const home=root?.querySelector('#noventraHome');
 const input=root?.querySelector('#noventraSearch')?.shadowRoot
   ?.querySelector('cr-searchbox-input')?.shadowRoot?.querySelector('input');
 return {href:location.href,documentURL:document.URL,title:document.title,
 ready:document.readyState==='complete',app:!!app,homeVisible:!!home?.getClientRects().length,
 brand:root?.querySelector('#noventraBrand')?.innerText??null,
 searchVisible:!!input?.getClientRects().length,searchEnabled:!!input&&!input.disabled,
 setupPresent:!!document.querySelector('#noventra-onboarding')};})()'''
SEARCH_INPUT = '''document.querySelector('ntp-app').shadowRoot.querySelector('#noventraSearch')
 .shadowRoot.querySelector('cr-searchbox-input').shadowRoot.querySelector('input')'''
BUTTON_COLORS = '''(()=>{const rgba=color=>{const c=document.createElement('canvas');
 c.width=c.height=1;const x=c.getContext('2d');x.fillStyle=color;x.fillRect(0,0,1,1);
 return [...x.getImageData(0,0,1,1).data]};
 return [...document.querySelectorAll('[data-manager] button')].map(e=>{
 const s=getComputedStyle(e),ancestors=[];for(let p=e.parentElement;p;p=p.parentElement){
 const a=getComputedStyle(p);ancestors.push(rgba(a.backgroundColor))}
 return {id:e.id,disabled:e.disabled,foreground:s.color,background:s.backgroundColor,
 foregroundRGBA:rgba(s.color),backgroundRGBA:rgba(s.backgroundColor),opacity:s.opacity,
 ancestors};});})()'''


def file_hash(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def normal_close(browser):
    endpoint = request(browser.port, '/json/version')['webSocketDebuggerUrl']
    script = '''const s=new WebSocket(process.argv[1]);let sent=false,ack=false;
      const t=setTimeout(()=>process.exit(1),10000);
      s.onopen=()=>{sent=true;s.send(JSON.stringify({id:1,method:'Browser.close'}))};
      s.onmessage=e=>{const m=JSON.parse(e.data);if(m.id===1&&!m.error)ack=true};
      s.onclose=()=>{clearTimeout(t);process.stdout.write(JSON.stringify({sent,ack}));process.exit(sent?0:1)};'''
    observed = json.loads(subprocess.check_output(['node', '-e', script, endpoint],
                                                text=True, timeout=15))
    observed['exit_code'] = browser.process.wait(timeout=15)
    assert observed['sent'] and observed['exit_code'] == 0, observed
    return observed


@contextmanager
def owned_browser(browser_path, profile, output, result, phase, normal_startup=False):
    """Preserve every profile/failure; quit normally, with owned-group fallback."""
    output.mkdir(parents=True, exist_ok=True)
    browser = LayoutBrowser(browser_path, profile, output)
    failed = False
    try:
        if normal_startup or not (browser.profile / 'First Run').exists():
            # Fresh profiles and restarts both use genuine startup. Only the
            # browser may apply initial preferences and create its sentinel.
            # Deliberately omit --no-first-run and any explicit startup URL.
            active = browser.profile / 'DevToolsActivePort'
            active.unlink(missing_ok=True)
            env = os.environ.copy()
            env['NOVENTRA_CONFIG_HOME'] = str(browser.profile / 'test-config')
            browser.log = (output / 'browser.log').open('w')
            argv = [str(browser.browser), '--user-data-dir=' + str(browser.profile),
                    '--ozone-platform=x11', '--disable-gpu', '--window-size=1200,760',
                    '--no-default-browser-check', '--remote-debugging-address=127.0.0.1',
                    '--remote-debugging-port=0']
            (output / 'launch.json').write_text(json.dumps(argv, indent=2) + '\n')
            browser.process = subprocess.Popen(argv, env=env, stdout=browser.log,
                                               stderr=browser.log, start_new_session=True)
            wait_for(lambda: active.exists() and len(active.read_text().splitlines()) >= 2,
                     browser.process)
            browser.port = int(active.read_text().splitlines()[0])
            browser.settings = wait_for(lambda: next((p for p in request(browser.port, '/json/list')
                                                     if p['type'] == 'page'), None), browser.process)
            wait_for(lambda: browser.ev('document.readyState==="complete"'), browser.process)
            if not normal_startup:
                browser.fresh_setup_page = wait_for(lambda: next((page for page in
                    request(browser.port, '/json/list') if page['type'] == 'page' and
                    page['url'].startswith('chrome://setup/')), None), browser.process)
                browser.settings = request(browser.port, '/json/new?chrome://settings/appearance', 'PUT')
                wait_for(lambda: browser.ev('document.readyState==="complete" && '
                    '!!document.querySelector("settings-ui")?.shadowRoot'), browser.process)
        else:
            browser.__enter__()
        yield browser
    except BaseException as error:
        failed = True
        failure = {'error': repr(error), 'profile': str(profile)}
        if browser.process and browser.process.poll() is None:
            try:
                failure['targets'] = request(browser.port, '/json/list')
                failure['documents'] = []
                for page in failure['targets']:
                    if page['type'] != 'page':
                        continue
                    try:
                        failure['documents'].append({'id': page['id'],
                            'state': browser.ev(NTP, page),
                            'frames': protocol(browser, 'Page.getFrameTree', {}, page)})
                    except BaseException as observation_error:
                        failure['documents'].append({'id': page['id'], 'error': repr(observation_error)})
                inp = Input(browser.platform)
                try:
                    inp.capture(output / 'failure.png').close()
                finally:
                    inp.close()
            except BaseException as capture_error:
                failure['capture_error'] = repr(capture_error)
        (output / 'failure.json').write_text(json.dumps(failure, indent=2) + '\n')
        raise
    finally:
        if browser.process and browser.process.poll() is None:
            try:
                result.setdefault('normal_shutdowns', {})[phase] = normal_close(browser)
            except BaseException as close_error:
                result.setdefault('shutdown_errors', {})[phase] = repr(close_error)
                browser.__exit__(None, None, None)
                if not failed:
                    raise
        if hasattr(browser, 'log'):
            browser.log.close()


def wait_ntp(browser, page, result, label):
    def ready():
        state = browser.ev(NTP, page)
        return state if (state['href'] == 'chrome://new-tab-page/' and
                         state['documentURL'] == state['href'] and state['ready'] and
                         state['app'] and state['homeVisible'] and state['brand'] == 'noventra' and
                         state['searchVisible'] and state['searchEnabled'] and not state['setupPresent']) else None
    state = wait_for(ready, browser.process, timeout=15)
    frame = protocol(browser, 'Page.getFrameTree', {}, page)['frameTree']['frame']
    assert frame['url'] == 'chrome://new-tab-page/', frame
    state.update(target_id=page['id'], websocket=page['webSocketDebuggerUrl'], frame=frame)
    result.setdefault('ntp_identity', {})[label] = state
    return state


def exercise_ntp_search(browser, page, output, result):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            self.server.received.append(self.path)
            data = b'<!doctype html><title>Owned NTP Search destination</title><h1 id="destination">Owned Search reached</h1>'
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.received = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    inp = Input(browser.platform)
    try:
        request(browser.port, '/json/activate/' + page['id'])
        browser.ev('(' + SEARCH_INPUT + ').focus();true', page)
        destination = f'http://127.0.0.1:{server.server_port}/owned-ntp-search'
        native_text(inp, destination)
        observed = browser.ev('({value:(' + SEARCH_INPUT + ').value,focused:(' +
                              SEARCH_INPUT + ').matches(":focus")})', page)
        assert observed == {'value': destination, 'focused': True}, observed
        inp.capture(output / 'ntp-search-input.png').close()
        native_key(inp, 'Return')
        wait_for(lambda: browser.ev('location.href===' + json.dumps(destination) +
                                    '&&document.querySelector("#destination")?.innerText==="Owned Search reached"', page), browser.process)
        assert '/owned-ntp-search' in server.received, server.received
        result['native_ntp_search'] = {'target_id': page['id'], 'typed': observed,
                                      'destination': destination, 'requests': server.received}
        inp.capture(output / 'ntp-search-destination.png').close()
        result['checks'].append('Actual NTP Search accepts native typing and Enter and reaches the owned loopback destination')
    finally:
        inp.close()
        server.shutdown()
        server.server_close()


def settled_colors(browser, page):
    previous = None
    stable = 0

    def sample():
        nonlocal previous, stable
        value = browser.ev(BUTTON_COLORS, page)
        stable = stable + 1 if value == previous else 0
        previous = value
        return value if stable >= 2 else None

    colors = wait_for(sample, browser.process, timeout=5)

    def over(color, background):
        alpha = color[3] / 255
        return [color[i] * alpha + background[i] * (1 - alpha) for i in range(3)]

    def luminance(color):
        channels = [v / 255 for v in color]
        channels = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in channels]
        return sum(v * weight for v, weight in zip(channels, (.2126, .7152, .0722)))

    for button in colors:
        background = [255, 255, 255]
        for ancestor in reversed(button['ancestors']):
            background = over(ancestor, background)
        background = over(button['backgroundRGBA'], background)
        foreground = over(button['foregroundRGBA'], background)
        lights = [luminance(c) for c in (background, foreground)]
        button['effective_background_rgb'] = background
        button['contrast_ratio'] = (max(lights) + .05) / (min(lights) + .05)
        if not button['disabled']:
            assert button['opacity'] == '1' and button['contrast_ratio'] >= 4.5, button
    return colors


def bridge(browser, page, message):
    # Setup bundles its own cr module. Importing Chromium's separate copy would
    # collide with window.cr and its pending promises. Observe only this real
    # native response, forwarding every application response to its own bridge.
    return browser.ev('''new Promise((resolve,reject)=>{
      const id='noventra_fixture_'+crypto.randomUUID();
      const original=window.cr.webUIResponse;
      const restore=()=>{if(window.cr.webUIResponse===observe)
        window.cr.webUIResponse=original;clearTimeout(timer)};
      const observe=(callback,success,value)=>{
        if(callback!==id)return original(callback,success,value);
        restore();if(success)resolve(value);else reject(Error(String(value)));
      };
      const timer=setTimeout(()=>{restore();reject(Error('Native response timed out'))},10000);
      window.cr.webUIResponse=observe;
      try{chrome.send(MESSAGE,[id])}catch(error){restore();reject(error)}
    })'''.replace('MESSAGE', json.dumps(message)), page)


def services(preferences):
    return {key: value for key, value in preferences.items()
            if key.startswith('services.')}


def page_ready(browser, page):
    wait_for(lambda: browser.ev('document.readyState === "complete" && '
                                '!!document.querySelector("#step-title")', page),
             browser.process)


def reload_page(browser, page):
    before = browser.ev('performance.timeOrigin', page)
    browser.ev('location.reload();true', page)
    time.sleep(.4)
    page_ready(browser, page)
    assert browser.ev('performance.timeOrigin', page) != before, 'Reload did not replace the document'


def click(browser, page, selector, expected):
    browser.ev('document.querySelector(' + json.dumps(selector) + ').click();true', page)
    wait_for(lambda: browser.ev('document.querySelector("#step-title")?.innerText', page)
             == expected, browser.process)
    assert browser.ev('document.activeElement === '
                      'document.querySelector("#step-title")', page), 'Step heading lost focus'


def setup_page(browser):
    page = getattr(browser, 'fresh_setup_page', None)
    browser.fresh_setup_page = None
    if page is None:
        return request(browser.port, '/json/new?chrome://setup/', 'PUT')
    request(browser.port, '/json/activate/' + page['id'])
    return page


def open_managers(browser):
    page = setup_page(browser)
    page_ready(browser, page)
    assert browser.ev('document.querySelector("#step-title").innerText', page) == 'Make yourself at home.'
    click(browser, page, '#continue', 'Bring your browsing data.')
    assert browser.ev('!!document.querySelector("#bookmark-file")', page)
    assert browser.ev('!!document.querySelector("#password-import")', page)
    click(browser, page, '#continue', 'Keep your password manager.')
    wait_for(lambda: browser.ev('document.querySelector(".managers")?.getAttribute('
                                '"aria-busy") === "false"', page), browser.process)
    return page


def inspect_choices(browser, page):
    wait_for(lambda: browser.ev('Array.from(document.querySelectorAll('
                               '"[data-manager] .manager-icon img")).length === 6 && '
                               'Array.from(document.querySelectorAll("[data-manager] '
                               '.manager-icon img")).every(img => img.complete && '
                               'img.naturalWidth > 0)', page), browser.process)
    choices = browser.ev('Array.from(document.querySelectorAll("[data-manager]")).map('
                         'card=>({id:card.dataset.manager,title:card.querySelector("h3").innerText,'
                         'disabled:card.querySelector("button")?.disabled ?? null,'
                         'label:card.querySelector("button")?.getAttribute("aria-label") ?? null,'
                         'view:card.querySelector(".view-extension")?.href ?? null}))', page)
    assert [choice['id'] for choice in choices] == list(MANAGERS), choices
    assert [choice['title'] for choice in choices] == list(MANAGER_TITLES), choices
    assert all(choice['disabled'] is False and choice['view'] is None
               for choice in choices[:-1]), choices
    assert choices[-1]['disabled'] is None and choices[-1]['view'] == (
        'https://chromewebstore.google.com/detail/pejdijmoenmkgeppbflobdenhhabjlaj'), choices
    assert not browser.ev('!!document.querySelector("#install-icloudPasswords")', page), \
        'Linux must offer the extension listing without claiming iCloud integration'
    assert browser.ev('document.querySelector("[data-manager=icloudPasswords]").innerText.'
                      'includes("Requires macOS or Windows")', page)
    assert 'Continue without installing' in browser.ev('document.querySelector("#continue").innerText', page)
    assert browser.ev('Array.from(document.querySelectorAll("[data-manager] details, '
                      '.download-details")).every(details => !details.open)', page)
    visible_words = browser.ev('document.querySelector(".panel-content").innerText.trim().split(/\\s+/).length', page)
    assert visible_words <= 150, ('Manager step is no longer concise', visible_words)
    return choices


def capture_layouts(browser, page, output, result):
    request(browser.port, '/json/activate/' + page['id'])
    inp = Input(browser.platform)
    try:
        for mode, label in ((1, 'light'), (2, 'dark')):
            browser.ev('(async()=>{const m=await import('
                       '"chrome://resources/cr_components/customize_color_scheme_mode/'
                       'customize_color_scheme_mode.mojom-webui.js");'
                       'm.browserProxyFactory.getInstance().handler.setColorSchemeMode('
                       + str(mode) + ');return true;})()')
            wait_for(lambda: browser.ev('matchMedia("(prefers-color-scheme: dark)").matches', page)
                     == (mode == 2), browser.process)
            inp.move(1230, 780)
            result.setdefault('settled_button_colors', {})[label] = settled_colors(browser, page)
            for width, height, size in ((1200, 760, 'desktop'), (700, 600, 'narrow')):
                inp.resize(width, height)
                browser.ev('scrollTo(0,0);true', page)
                geometry = browser.ev('({width:innerWidth,height:innerHeight,'
                                      'scrollWidth:document.documentElement.scrollWidth,'
                                      'columns:getComputedStyle(document.querySelector(".managers")).gridTemplateColumns.split(" ").length,'
                                      'cards:Array.from(document.querySelectorAll("[data-manager]"))'
                                      '.map(el=>({left:el.getBoundingClientRect().left,'
                                      'right:el.getBoundingClientRect().right}))})', page)
                assert geometry['scrollWidth'] <= geometry['width'], geometry
                assert geometry['columns'] == (2 if geometry['width'] > 620 else 1), geometry
                if size == 'narrow':
                    assert geometry['width'] < 850, ('Narrow viewport was not exercised', geometry)
                assert all(card['left'] >= 0 and card['right'] <= geometry['width'] + 1
                           for card in geometry['cards']), geometry
                result.setdefault('viewports', []).append({'mode': label, 'size': size, **geometry})
                inp.capture(output / f'managers-{label}-{size}.png').close()
                browser.ev('document.querySelector("#continue").scrollIntoView('
                           '{block:"center"});true', page)
                assert browser.ev('(()=>{const r=document.querySelector("#continue").'
                                  'getBoundingClientRect();return r.top >= 0 && '
                                  'r.bottom <= innerHeight;})()', page)
                inp.capture(output / f'managers-{label}-{size}-continue.png').close()
        inp.resize(1200, 760)
        browser.ev('document.querySelector("#install-bitwarden").focus();true', page)
        native_key(inp, 'Tab')
        wait_for(lambda: browser.ev('document.activeElement === document.querySelector("[data-manager=bitwarden] summary")', page),
                 browser.process)
        native_key(inp, 'Tab')
        wait_for(lambda: browser.ev('document.activeElement?.id === "install-onePassword"', page),
                 browser.process)
        assert browser.ev('(()=>{const s=getComputedStyle(document.activeElement);'
                          'return document.activeElement.matches(":focus-visible") && '
                          's.outlineStyle !== "none" && parseFloat(s.outlineWidth) >= 2;})()', page)
        inp.capture(output / 'managers-keyboard-focus.png').close()
    finally:
        inp.close()


def skip_and_finish(browser_path, output, result):
    profile = output / 'skip-profile'
    with owned_browser(browser_path, profile, output, result, 'skip-finish') as browser:
        assert browser.platform == 'x11', 'This fixture qualifies private X11 only'
        page = open_managers(browser)
        baseline = bridge(browser, page, 'getPrefs')
        assert baseline['extensions.chrome_web_store'] is False, baseline
        before_extensions = bridge(browser, page, 'getExtensions')
        assert not any(identifier in before_extensions for identifier in MANAGER_EXTENSIONS)
        result['choices'] = inspect_choices(browser, page)
        result['checks'].append('Fresh setup offers six branded manager choices; Linux links to iCloud’s listing and explains its supported platforms')
        capture_layouts(browser, page, output, result)
        result['checks'].append('Desktop/narrow light and dark layouts fit; Continue is reachable and native Tab paints focus')

        click(browser, page, '#back', 'Bring your browsing data.')
        click(browser, page, '#continue', 'Keep your password manager.')
        reload_page(browser, page)
        wait_for(lambda: browser.ev('document.querySelector(".managers")?.getAttribute('
                                    '"aria-busy") === "false"', page), browser.process)
        assert browser.ev('document.querySelector("#step-title").innerText', page) == 'Keep your password manager.'
        inspect_choices(browser, page)
        assert bridge(browser, page, 'getExtensions') == before_extensions
        after_navigation = bridge(browser, page, 'getPrefs')
        assert after_navigation['extensions.chrome_web_store'] is False
        assert services(after_navigation) == services(baseline)
        result['checks'].append('Back/Continue and reload preserve the manager step without enabling downloads or installing extensions')

        browser.ev('document.querySelector("#skip-setup").click();true', page)
        wait_ntp(browser, page, result, 'after-skip')
        page = open_managers(browser)
        reopened = bridge(browser, page, 'getPrefs')
        assert reopened['completed_onboarding'] is True
        assert reopened['extensions.chrome_web_store'] is False
        assert services(reopened) == services(baseline)
        assert bridge(browser, page, 'getExtensions') == before_extensions
        result['checks'].append('Skip saves completion; reopening setup leaves downloads, services and installed extensions unchanged')

        click(browser, page, '#continue', 'Choose your search engine.')
        click(browser, page, '#continue', 'You’re ready to browse.')
        summary = browser.ev('document.querySelector(".panel-content").innerText', page)
        assert 'Password manager' in summary and 'Add an extension later.' in summary, summary
        assert 'installed.' not in summary, summary
        click(browser, page, '#back', 'Choose your search engine.')
        click(browser, page, '#back', 'Keep your password manager.')
        click(browser, page, '#continue', 'Choose your search engine.')
        click(browser, page, '#continue', 'You’re ready to browse.')
        browser.ev('document.querySelector("#continue").click();true', page)
        wait_ntp(browser, page, result, 'after-finish')
        exercise_ntp_search(browser, page, output, result)
        result['checks'].append('Continue without installing reaches an accurate Ready summary; Start browsing finishes setup')
    saved = json.loads((profile / 'Default/Preferences').read_text())['helium']
    assert saved['completed_onboarding'] is True
    assert saved.get('extensions', {}).get('chrome_web_store', False) is False
    result['checks'].append('Completed setup and the skipped-download choice persist to the native profile')
    sentinel = profile / 'First Run'
    pre_restart = {'completed_onboarding': saved['completed_onboarding'],
                   'chrome_web_store': saved.get('extensions', {}).get('chrome_web_store', False),
                   'preferences_sha256': file_hash(profile / 'Default/Preferences'),
                   'first_run_sentinel_exists': sentinel.is_file(),
                   'sentinel_sha256': file_hash(sentinel) if sentinel.is_file() else None,
                   'sentinel_mtime_ns': sentinel.stat().st_mtime_ns if sentinel.is_file() else None}
    result['pre_restart_profile_state'] = pre_restart
    (output / 'pre-restart-profile-state.json').write_text(json.dumps(pre_restart, indent=2) + '\n')
    assert pre_restart['first_run_sentinel_exists'], pre_restart
    assert pre_restart['completed_onboarding'] is True, pre_restart

    with owned_browser(browser_path, profile, output / 'normal-restart', result,
                       'normal-restart', normal_startup=True) as browser:
        restored_url = result['native_ntp_search']['destination']
        restored = wait_for(lambda: next((page for page in request(browser.port, '/json/list')
            if page['type'] == 'page' and page['url'] == restored_url), None), browser.process)
        result['restored_search_target'] = {'id': restored['id'], 'url': restored['url']}
        result['checks'].append('Default session restore reopens the last owned browsing URL after a normal restart')
        new_tab = request(browser.port, '/json/new?chrome://newtab/', 'PUT')
        wait_ntp(browser, new_tab, result, 'normal-restart')
        targets = request(browser.port, '/json/list')
        assert not any('setup' in page['url'] for page in targets), targets
        result['normal_restart_targets'] = targets
        result['checks'].append('Normal restart without --no-first-run keeps completed setup dismissed')
    assert result['normal_shutdowns']['skip-finish']['exit_code'] == 0
    assert result['normal_shutdowns']['normal-restart']['exit_code'] == 0
    saved = json.loads((profile / 'Default/Preferences').read_text())['helium']
    assert saved['completed_onboarding'] is True
    assert saved.get('extensions', {}).get('chrome_web_store', False) is False
    result['checks'].append('Setup completion and skipped downloads persist after Browser.close exit0 and a normal restart/close')


def permission_target_from_evidence(tsv, node, nodes, window, browser_pid, profile):
    """Validate native identity and return the unique observed screen-label center."""
    import csv
    import io
    import re
    assert re.search(r'button .*name=Add extension name_from=', node['line']), node
    assert ' INVISIBLE ' not in node['line'] and 'restriction=disabled' not in node['line'], node
    chain, seen, current = [], set(), node
    while current is not None:
        identifier = next(key for key, value in nodes.items() if value is current)
        assert identifier not in seen, ('Cyclic AX parent chain', chain)
        seen.add(identifier)
        chain.append(current)
        assert current['parent'] is None or current['parent'] in nodes, current
        current = nodes.get(current['parent'])
    assert 'alertDialog ' in chain[-1]['line'] and 'name=Add "Bitwarden Password Manager"? name_from=' in chain[-1]['line'], chain
    assert window['pid'] == browser_pid and profile in window['wm_class'], window
    assert window['focused'] and window['map_state'] == 2, window
    words = [row for row in csv.DictReader(io.StringIO(tsv), delimiter='\t', quoting=csv.QUOTE_NONE)
             if row['text'].strip() and float(row['conf']) >= 40]
    boxes = []
    for first, second in zip(words, words[1:]):
        same_line = all(first[key] == second[key] for key in ('page_num', 'block_num', 'par_num', 'line_num'))
        adjacent = int(second['word_num']) == int(first['word_num']) + 1
        if same_line and adjacent and first['text'] == 'Add' and second['text'] == 'extension':
            left = min(int(row['left']) for row in (first, second))
            top = min(int(row['top']) for row in (first, second))
            right = max(int(row['left']) + int(row['width']) for row in (first, second))
            bottom = max(int(row['top']) + int(row['height']) for row in (first, second))
            boxes.append([left, top, right-left, bottom-top])
    assert len(boxes) == 1, ('Expected one visible Add extension label', boxes)
    bx, by, bw, bh = boxes[0]
    left, top, width, height = window['bounds']
    assert left <= bx and top <= by and bx+bw <= left+width and by+bh <= top+height, (boxes, window)
    assert 0 < bw <= node['bounds'][2] and 0 < bh <= node['bounds'][3], (boxes, node)
    return {'derived_screen_point': [int(bx+bw/2), int(by+bh/2)], 'visible_label_box': boxes[0],
            'modal_offset_chain': chain, 'window': window,
            'source': 'Observed unique screen-coordinate label; no AX/modal offset assumed'}


def permission_screen_point(browser, tree, node, inp, screenshot):
    import ctypes as C
    import re

    class Attributes(C.Structure):
        _fields_ = [(name, C.c_int) for name in ('x', 'y', 'width', 'height', 'border_width', 'depth')] + [
            ('visual', C.c_void_p), ('root', C.c_ulong), ('c_class', C.c_int),
            ('bit_gravity', C.c_int), ('win_gravity', C.c_int), ('backing_store', C.c_int),
            ('backing_planes', C.c_ulong), ('backing_pixel', C.c_ulong), ('save_under', C.c_int),
            ('colormap', C.c_ulong), ('map_installed', C.c_int), ('map_state', C.c_int),
            ('all_event_masks', C.c_long), ('your_event_mask', C.c_long),
            ('do_not_propagate_mask', C.c_long), ('override_redirect', C.c_int), ('screen', C.c_void_p)]

    pointer = inp.pointer
    x = pointer.x
    x.XGetInputFocus.argtypes = [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)]
    xid, revert = C.c_ulong(), C.c_int()
    x.XGetInputFocus(pointer.d, C.byref(xid), C.byref(revert))
    x.XGetWindowAttributes.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(Attributes)]
    attributes = Attributes()
    assert x.XGetWindowAttributes(pointer.d, xid.value, C.byref(attributes)), xid.value
    x.XTranslateCoordinates.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_int, C.c_int,
        C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_ulong)]
    left, top, child = C.c_int(), C.c_int(), C.c_ulong()
    assert x.XTranslateCoordinates(pointer.d, xid.value, attributes.root, 0, 0,
        C.byref(left), C.byref(top), C.byref(child))
    identity = subprocess.check_output(['xprop', '-id', str(xid.value), '_NET_WM_PID', 'WM_CLASS'],
                                       text=True, timeout=5)
    pid = re.search(r'_NET_WM_PID\(CARDINAL\) = (\d+)', identity)
    window = {'xid': xid.value, 'pid': int(pid[1]) if pid else None, 'wm_class': identity,
        'focused': True, 'map_state': attributes.map_state,
        'bounds': [left.value, top.value, attributes.width, attributes.height]}
    screenshot.with_suffix('.window.json').write_text(json.dumps(window, indent=2) + '\n')
    raw_tsv = subprocess.check_output(['tesseract', str(screenshot), 'stdout', '--psm', '11', 'tsv'],
                                      stderr=subprocess.DEVNULL, timeout=10)
    screenshot.with_suffix('.ocr.tsv').write_bytes(raw_tsv)
    target = permission_target_from_evidence(raw_tsv.decode('utf-8'), node, tree.nodes,
                                            window, browser.process.pid, str(browser.profile))
    screenshot.with_suffix('.click-target.json').write_text(json.dumps(target, indent=2) + '\n')
    return target['derived_screen_point']


def install_bitwarden(browser_path, output, result):
    output.mkdir()
    with owned_browser(browser_path, output / 'profile', output, result, 'live-store') as browser:
        assert browser.platform == 'x11', 'Native prompt automation is private X11 only'
        page = open_managers(browser)
        baseline = bridge(browser, page, 'getPrefs')
        assert baseline['extensions.chrome_web_store'] is False
        tree = NativeUITree(browser)

        def capture_native_tree(label):
            tree.read()
            (output / f'{label}-raw-tree.txt').write_text(tree.text)
            (output / f'{label}-parsed-nodes.json').write_text(json.dumps(tree.nodes, indent=2) + '\n')
            data = browser.ev('(()=>{const x=new XMLHttpRequest();x.open("GET",'
                '"targets-data.json",false);x.send();return JSON.parse(x.responseText)})()', tree.target)
            (output / f'{label}-targets-data.json').write_text(json.dumps(data, indent=2) + '\n')
            return {'native': data['native'], 'node_count': len(tree.nodes), 'browsers': data['browsers']}

        capture_native_tree('native-accessibility-before')
        browser.ev('chrome.send("setGlobalFlag",[{flagName:"native",enabled:true}]);true', tree.target)

        def native_tree_ready():
            state = capture_native_tree('native-accessibility-ready')
            return state if state['native'] and state['node_count'] > 1 else None

        result['native_accessibility'] = wait_for(native_tree_ready, browser.process, timeout=10)
        request(browser.port, '/json/activate/' + page['id'])
        inp = Input(browser.platform)
        try:
            for accept in (False, True):
                gesture(page, 'document.querySelector("#install-bitwarden").click();true')
                wait_for(lambda: bridge(browser, page, 'getPrefs')['extensions.chrome_web_store'] is True,
                         browser.process)
                assert browser.ev('document.querySelector("#continue").disabled && '
                                  'document.querySelector("#back").disabled && '
                                  'document.querySelector("#skip-setup").disabled', page)

                def prompt_or_failure():
                    capture_native_tree('native-permission-' + ('accept' if accept else 'cancel') + '-observed')
                    try:
                        node = tree.find(r'button .*name=Add extension name_from=')
                        if accept and 'restriction=disabled' in node['line']:
                            return None
                        return {'button': node}
                    except StopIteration:
                        message = browser.ev('document.querySelector('
                                             '"[data-manager=bitwarden] [role=status]")?.innerText || ""', page)
                        return {'failure': message} if message else None

                prompt = wait_for(prompt_or_failure, browser.process, timeout=120)
                assert 'button' in prompt, ('No native permission prompt', prompt)
                suffix = 'accept' if accept else 'cancel'
                (output / f'native-permission-{suffix}.txt').write_text(tree.text)
                inp.capture(output / f'native-permission-{suffix}.png').close()
                if accept:
                    point = permission_screen_point(browser, tree, prompt['button'], inp,
                        output / 'native-permission-accept.png')
                    inp.pointer.click(*point)
                    wait_for(lambda: bridge(browser, page, 'getExtensions').get(BITWARDEN) is True,
                             browser.process, timeout=120)
                    request(browser.port, '/json/activate/' + page['id'])
                    wait_for(lambda: browser.ev('document.querySelector("#install-bitwarden").'
                                                'innerText.includes("Installed")', page), browser.process)
                    assert browser.ev('document.querySelector("#install-bitwarden").disabled', page)
                    result['checks'].append('Accepting the real native store permission prompt installs and enables the official Bitwarden ID')
                else:
                    native_key(inp, 'Escape')
                    wait_for(lambda: browser.ev('!document.querySelector("#continue").disabled', page),
                             browser.process)
                    assert BITWARDEN not in bridge(browser, page, 'getExtensions')
                    assert 'canceled' in browser.ev('document.querySelector('
                                                   '"[data-manager=bitwarden] [role=status]").innerText.toLowerCase()', page)
                    result['checks'].append('Canceling the real native permission prompt keeps Bitwarden uninstalled and navigation usable')
                assert services(bridge(browser, page, 'getPrefs')) == services(baseline)

            reload_page(browser, page)
            wait_for(lambda: browser.ev('document.querySelector("#install-bitwarden")?.'
                                        'innerText.includes("Installed")', page), browser.process)
            assert browser.ev('document.querySelector("#install-bitwarden").disabled', page)
            click(browser, page, '#continue', 'Choose your search engine.')
            click(browser, page, '#continue', 'You’re ready to browse.')
            assert 'Bitwarden installed.' in browser.ev('document.querySelector(".panel-content").innerText', page)
            reload_page(browser, page)
            wait_for(lambda: 'Bitwarden installed.' in browser.ev(
                'document.querySelector(".panel-content").innerText', page), browser.process)
            result['checks'].append('Reload reads actual installed state; Ready reports Bitwarden while service preferences stay unchanged')
            inp.capture(output / 'installed-ready.png').close()
            browser.ev('document.querySelector("#continue").click();true', page)
            wait_ntp(browser, page, result, 'installed-finish')
        finally:
            inp.close()


def native_source_profile(browser_path, output, result):
    """Use the actual detector/importer against an owned Chrome profile."""
    output.mkdir()
    source = Path.home() / '.config/google-chrome'
    bookmarks = source / 'Default/Bookmarks'
    expected = 'https://noventra-fixture.invalid/native-profile'
    original = bookmarks.read_bytes()
    with owned_browser(browser_path, output / 'profile', output, result, 'native-source-profile') as browser:
        page = setup_page(browser)
        page_ready(browser, page)
        click(browser, page, '#continue', 'Bring your browsing data.')
        assert browser.ev('!document.querySelector("#continue").disabled', page)
        wait_for(lambda: browser.ev('document.querySelector(".panel").getAttribute("aria-busy") === "false" && '
                                    '!!document.querySelector("#refresh-import-profiles")', page), browser.process)
        wait_for(lambda: browser.ev('document.querySelectorAll(".profile-card").length > 0', page), browser.process)
        cards = browser.ev('Array.from(document.querySelectorAll(".profile-card")).map(c=>({text:c.innerText,id:c.querySelector("input").value}))', page)
        assert len(cards) == 1 and 'Noventra owned Chrome source' in cards[0]['text'], cards
        result['detected_source_cards'] = cards
        inp = Input(browser.platform)
        try:
            request(browser.port, '/json/activate/' + page['id'])
            inp.capture(output / 'detected-source.png').close()
            browser.ev('document.querySelector(".profile-card input").click();true', page)
            assert browser.ev('!!document.querySelector("#queued-import-note")&&!document.querySelector("#import-bookmarks")', page)
            gesture(page, 'document.querySelector("#refresh-import-profiles").click();true')
            wait_for(lambda: browser.ev('document.querySelector(".panel").getAttribute("aria-busy") === "false"', page), browser.process)
            refreshed = browser.ev('Array.from(document.querySelectorAll(".profile-card")).map(c=>({id:c.querySelector("input").value,selected:c.querySelector("input").checked}))', page)
            assert refreshed == [{'id': cards[0]['id'], 'selected': True}], refreshed
            inp.capture(output / 'native-profile-staged.png').close()
            click(browser, page, '#continue', 'Keep your password manager.')
            click(browser, page, '#continue', 'Choose your search engine.')
            click(browser, page, '#continue', 'You’re ready to browse.')
            gesture(page, 'document.querySelector("#continue").click();true')
            wait_for(lambda: 'setup' not in browser.ev('location.href', page), browser.process)
            manager = request(browser.port, '/json/new?chrome://bookmarks/', 'PUT')
            wait_for(lambda: browser.ev('typeof chrome.bookmarks === "object"', manager), browser.process)
            entries = browser.ev('new Promise(r=>chrome.bookmarks.search({url:' + json.dumps(expected) + '},r))', manager)
            assert any(entry['url'] == expected for entry in entries), entries
            result['native_import_entries'] = entries
            inp.capture(output / 'native-profile-imported.png').close()
        finally:
            inp.close()
    assert bookmarks.read_bytes() == original, 'Source browser bookmarks changed'
    stored = json.loads((output / 'profile/Default/Bookmarks').read_text())

    def contains_url(value):
        if isinstance(value, dict):
            return value.get('url') == expected or any(contains_url(v) for v in value.values())
        return isinstance(value, list) and any(contains_url(v) for v in value)

    assert contains_url(stored), 'Native source import did not persist after normal close'
    result['checks'].append('Entering Data detects the owned profile, keeps its staged stable ID on refresh, imports only at Finish, persists the bookmark, and leaves the source unchanged')


def source_namespace(args, browser_path):
    """Hide personal browser profiles behind an owned home mount; keep HOME intact."""
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    home = output / 'source-home'
    source = home / '.config/google-chrome'
    (source / 'Default').mkdir(parents=True)
    state = {'profile': {'info_cache': {'Default': {'name': 'Noventra owned Chrome source'}}}}
    (source / 'Local State').write_text(json.dumps(state) + '\n')
    bookmarks = {'version': 1, 'roots': {'bookmark_bar': {'type': 'folder', 'name': 'Bookmarks bar',
        'children': [{'type': 'url', 'name': 'Noventra owned source bookmark',
                      'url': 'https://noventra-fixture.invalid/native-profile', 'date_added': '13400000000000000'}]},
        'other': {'type': 'folder', 'name': 'Other bookmarks', 'children': []}}}
    (source / 'Default/Bookmarks').write_text(json.dumps(bookmarks) + '\n')
    temporary = output / 'namespace-tmp'
    temporary.mkdir()
    executable = shutil.which('bwrap')
    if not executable:
        raise RuntimeError('bwrap is required to isolate native browser detection; no host-profile fallback')
    argv = [executable, '--die-with-parent', '--ro-bind', '/', '/',
            '--bind', str(output), str(output), '--bind', str(home), str(Path.home()),
            '--bind', str(temporary), '/tmp', '--ro-bind', '/tmp/.X11-unix', '/tmp/.X11-unix',
            '--dev-bind', '/dev', '/dev', '--proc', '/proc']
    # The venv can point through uv's unversioned alias before reaching this
    # resolved interpreter. Preserve those runtime-only aliases as well.
    base_python = Path(sys.base_prefix).resolve().parent
    if base_python.is_relative_to(Path.home()):
        argv.extend(['--ro-bind', str(base_python), str(base_python)])
    argv.extend(['--chdir', str(Path(__file__).resolve().parents[2]), '--', sys.executable, '-B',
                 str(Path(__file__).resolve()), '--browser', str(browser_path),
                 '--output', str(output), '--native-source-profile', '--source-namespace-child'])
    if args.install_bitwarden:
        argv.append('--install-bitwarden')
    if args.expected_binary_sha256:
        argv.extend(['--expected-binary-sha256', args.expected_binary_sha256])
    env = os.environ.copy()
    env['PATH'] = '/usr/bin:/bin:' + env.get('PATH', '')
    env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=/tmp/noventra-no-session-bus'
    record = {'argv': argv, 'home_environment_unchanged': env.get('HOME') == os.environ.get('HOME'),
              'home_mount': {'source': str(home), 'destination': str(Path.home())},
              'fixture_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'browser_sha256': file_hash(browser_path),
              'source_files': {str(p.relative_to(home)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in (source / 'Local State', source / 'Default/Bookmarks')}}
    (output / 'namespace-launch.json').write_text(json.dumps(record, indent=2) + '\n')
    completed = subprocess.run(argv, env=env, timeout=630)
    (output / 'namespace-result.json').write_text(json.dumps({'exit_code': completed.returncode,
        'profile_and_evidence_preserved': True}, indent=2) + '\n')
    return completed.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--install-bitwarden', action='store_true',
                        help='Also contact the live store and accept installation in a disposable profile')
    parser.add_argument('--native-source-profile', action='store_true',
                        help='Also test native profile detection/import in an isolated owned-home mount (requires bwrap)')
    parser.add_argument('--source-namespace-child', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--expected-binary-sha256', help='Require the exact packaged candidate digest')
    args = parser.parse_args()
    browser_path = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    if args.native_source_profile and not args.source_namespace_child:
        raise SystemExit(source_namespace(args, browser_path))
    if args.source_namespace_child:
        assert args.native_source_profile and Path.home().samefile(args.output / 'source-home'), 'Owned home namespace is required'
        assert (args.output / 'namespace-launch.json').is_file()
    else:
        args.output.mkdir(parents=True, exist_ok=False)
    with browser_path.open('rb') as binary:
        browser_hash = hashlib.file_digest(binary, 'sha256').hexdigest()
    if args.expected_binary_sha256:
        assert browser_hash == args.expected_binary_sha256, (browser_hash, args.expected_binary_sha256)
    os.environ['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=/tmp/noventra-no-session-bus'
    os.environ['GDK_BACKEND'] = 'x11'
    result = {'passed': False, 'checks': [], 'browser': str(browser_path),
              'browser_sha256': browser_hash,
              'resources_sha256': hashlib.sha256((browser_path.parent / 'resources.pak').read_bytes()).hexdigest(),
              'fixture_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'live_store_install_requested': args.install_bitwarden,
              'native_source_profile_requested': args.native_source_profile,
              'profiles_preserved_on_failure': True,
              'scope': 'Disposable profiles on private X11; no personal browser data or accounts'}

    def bounded(*_):
        raise TimeoutError('Manager acceptance exceeded its 540-second work budget')

    signal.signal(signal.SIGALRM, bounded)
    signal.signal(signal.SIGTERM, bounded)
    signal.alarm(540)
    try:
        if args.native_source_profile:
            native_source_profile(browser_path, args.output / 'native-source', result)
        skip_and_finish(browser_path, args.output, result)
        if args.install_bitwarden:
            install_bitwarden(browser_path, args.output / 'live-store', result)
        else:
            result['limits'] = ['Live store downloads, native installation/cancellation and installed-extension state were not exercised']
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        signal.alarm(0)
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
