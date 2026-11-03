"""Import owned cookie databases through actual setup and Settings callbacks.

Run through the private Xvfb wrapper. isolate() hides the personal HOME, session
bus, and network. Every source cookie, account token, certificate, and database
belongs to this fixture. Native import callbacks are observed, never replaced.
"""
import argparse
from contextlib import contextmanager, ExitStack, nullcontext
from dataclasses import dataclass
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import json
import os
from pathlib import Path
import secrets
import shlex
import sqlite3
import ssl
import subprocess
import threading
import time

from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from interface_polish_smoke import DEEP, native_key
from onboarding_import_smoke import gesture
from onboarding_managers_smoke import owned_browser, page_ready
import onboarding_native_browser_import_smoke as source_keys
from onboarding_finish_import_smoke import go_ready
from portable_transfer_smoke import focus_owned, isolate, protocol

HOST = 'owned-cookie-import.test'
ROOT = Path(__file__).resolve().parents[2]
COOKIE_COLUMNS = '''creation_utc INTEGER NOT NULL,host_key TEXT NOT NULL,
top_frame_site_key TEXT NOT NULL,name TEXT NOT NULL,value TEXT NOT NULL,
encrypted_value BLOB NOT NULL,path TEXT NOT NULL,expires_utc INTEGER NOT NULL,
is_secure INTEGER NOT NULL,is_httponly INTEGER NOT NULL,last_access_utc INTEGER NOT NULL,
has_expires INTEGER NOT NULL,is_persistent INTEGER NOT NULL,priority INTEGER NOT NULL,
samesite INTEGER NOT NULL,source_scheme INTEGER NOT NULL,source_port INTEGER NOT NULL,
last_update_utc INTEGER NOT NULL,source_type INTEGER NOT NULL,
has_cross_site_ancestor INTEGER NOT NULL'''
OBSERVE = '''(()=>{
 const state=window.ownedCookieImport={profiles:[],discoveryIds:[],imports:[],events:[]};
 const send=chrome.send.bind(chrome),reply=window.cr.webUIResponse,
       event=window.cr.webUIListenerCallback;
 chrome.send=(name,args=[])=>{
   if(name==='initializeImportDialog')state.discoveryIds.push(args[0]);
   if(name==='importData')state.imports.push(args);
   return send(name,args);
 };
 window.cr.webUIResponse=(id,success,value)=>{
   if(state.discoveryIds.includes(id))state.profiles=success?value:[];
   return reply(id,success,value);
 };
 window.cr.webUIListenerCallback=(name,...args)=>{
   if(name==='import-data-status-changed'||name==='import-cookies-result')
     state.events.push({name,value:args[0]});
   return event(name,...args);
 };
 return true;
})()'''
SETTINGS_OBSERVE = '''(async()=>{
 const {addWebUiListener}=await import('chrome://resources/js/cr.js');
 window.ownedCookieImport={events:[]};
 for(const name of ['import-data-status-changed','import-cookies-result'])
   addWebUiListener(name,value=>ownedCookieImport.events.push({name,value}));
 return true;
})()'''
DIALOG = "deep(document,'settings-import-data-dialog')"


def check(result, name, condition):
    assert condition, name
    result['checks'].append(name)


def chrome_time(seconds):
    return int((seconds + 11644473600) * 1000000)


@dataclass(frozen=True)
class Cookie:
    name: str
    value: str
    domain: str = HOST
    path: str = '/'
    secure: bool = False
    httponly: bool = True
    same_site: int = 1
    lifetime: int = 86400
    partition: str | None = ''
    encrypted: bool = True
    bad_host_hash: bool = False
    envelope: bytes = b'v10'
    container: str | None = ''
    persistent: bool = True


def chrome_row(cookie, version, key, http_port, https_port):
    now = time.time()
    plain = cookie.value.encode()
    if version >= 24:
        plain = hashlib.sha256((cookie.domain if not cookie.bad_host_hash else
                                'different-owned-host.test').encode()).digest() + plain
    encrypted = cookie.envelope + source_keys.aes_cbc(plain, key) if cookie.encrypted else b''
    return (chrome_time(now - 180), cookie.domain, cookie.partition, cookie.name,
            '' if cookie.encrypted else cookie.value, encrypted, cookie.path,
            chrome_time(now + cookie.lifetime) if cookie.persistent else 0,
            int(cookie.secure), int(cookie.httponly),
            chrome_time(now - 60), int(cookie.persistent), int(cookie.persistent), 1, cookie.same_site,
            2 if cookie.secure else 1, https_port if cookie.secure else http_port,
            chrome_time(now - 60), 0, int(bool(cookie.partition)))


def chromium_database(path, cookies, *, version=24, secret=b'peanuts',
                      http_port=80, https_port=443, wal_cookie=None, nullable_isolation=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    database = sqlite3.connect(path)
    database.execute('PRAGMA journal_mode=WAL')
    database.execute('PRAGMA wal_autocheckpoint=0')
    database.execute('CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY,value LONGVARCHAR)')
    database.executemany('INSERT INTO meta VALUES (?,?)', [('version', str(version)),
                                                          ('last_compatible_version', str(version))])
    columns = COOKIE_COLUMNS.replace('top_frame_site_key TEXT NOT NULL', 'top_frame_site_key TEXT') if nullable_isolation else COOKIE_COLUMNS
    database.execute('CREATE TABLE cookies(' + columns + ')')
    key = source_keys.system_key(secret)
    for cookie in cookies:
        database.execute('INSERT INTO cookies VALUES (' + ','.join('?' for _ in range(20)) + ')',
                         chrome_row(cookie, version, key, http_port, https_port))
    database.commit()
    database.execute('PRAGMA wal_checkpoint(TRUNCATE)')
    if wal_cookie:
        database.execute('INSERT INTO cookies VALUES (' + ','.join('?' for _ in range(20)) + ')',
                         chrome_row(wal_cookie, version, key, http_port, https_port))
        database.commit()
        assert Path(str(path) + '-wal').stat().st_size > 0
    return database


def firefox_database(path, cookies, version, *, nullable_isolation=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    database = sqlite3.connect(path)
    database.execute('PRAGMA journal_mode=WAL')
    database.execute('PRAGMA wal_autocheckpoint=0')
    database.execute('PRAGMA user_version=' + str(version))
    database.execute(f'''CREATE TABLE moz_cookies(id INTEGER PRIMARY KEY,
      originAttributes TEXT{'' if nullable_isolation else ' NOT NULL'} DEFAULT '',name TEXT,value TEXT,host TEXT,
      path TEXT,expiry INTEGER,lastAccessed INTEGER,creationTime INTEGER,
      isSecure INTEGER,isHttpOnly INTEGER,inBrowserElement INTEGER DEFAULT 0,
      sameSite INTEGER DEFAULT 0,isPartitionedAttributeSet INTEGER DEFAULT 0,
      schemeMap INTEGER DEFAULT 0)''')
    now = time.time()
    for cookie in cookies:
        expiry = int((now + cookie.lifetime) * (1000 if version == 16 else 1))
        database.execute('''INSERT INTO moz_cookies(originAttributes,name,value,host,path,
          expiry,lastAccessed,creationTime,isSecure,isHttpOnly,inBrowserElement,sameSite,
          isPartitionedAttributeSet,schemeMap) VALUES (?,?,?,?,?,?,?,?,?,?,0,?,?,?)''',
          (cookie.container, cookie.name, cookie.value, cookie.domain, cookie.path, expiry,
           int((now - 60) * 1000000), int((now - 180) * 1000000), int(cookie.secure),
           int(cookie.httponly), cookie.same_site, int(bool(cookie.partition)),
           2 if cookie.secure else 1))
    database.commit()
    return database


class Endpoint(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        cookies = SimpleCookie()
        cookies.load(self.headers.get('Cookie', ''))
        if self.path == '/seed-existing':
            self.send_response(200)
            self.send_header('Set-Cookie', 'owned_existing=' + self.server.tokens['destination']
                             + '; HttpOnly; SameSite=Lax; Path=/; Max-Age=86400')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path.startswith('/probe') or self.path.startswith('/account/probe'):
            state = {name: name in cookies and cookies[name].value == value
                     for name, value in self.server.tokens.items() if name.startswith('owned_')}
            state['destination_kept'] = ('owned_existing' in cookies and
                                         cookies['owned_existing'].value == self.server.tokens['destination'])
            payload = json.dumps(state).encode()
            kind = 'application/json'
        else:
            payload = b'<!doctype html><title>Owned cookie session proof</title><h1>Owned cookie session proof</h1>'
            kind = 'text/html'
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


@contextmanager
def endpoints(output, tokens):
    certificate = output / 'owned-certificate.pem'
    private = output / 'owned-certificate-key.pem'
    subprocess.run(['/usr/bin/openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(private), '-out', str(certificate), '-days', '1',
                    '-subj', '/CN=' + HOST, '-addext',
                    'subjectAltName=DNS:' + HOST + ',DNS:sub.' + HOST],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    private.chmod(0o600)
    http = ThreadingHTTPServer(('127.0.0.1', 0), Endpoint)
    https = ThreadingHTTPServer(('127.0.0.1', 0), Endpoint)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, private)
    https.socket = context.wrap_socket(https.socket, server_side=True)
    threads = []
    for server in (http, https):
        server.daemon_threads = True
        server.tokens = tokens
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        threads.append(thread)
    try:
        yield {'http': f'http://{HOST}:{http.server_port}',
               'https': f'https://{HOST}:{https.server_port}',
               'sub': f'http://sub.{HOST}:{http.server_port}',
               'http_port': http.server_port, 'https_port': https.server_port}
    finally:
        for server in (http, https):
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=5)


def seed(config, firefox, stack, tokens, sites, keyring=None, cancellation=False):
    chromium = config / 'chromium'
    profiles = [('Default', 'Owned cookie sessions', 24),
                ('Profile 1', 'Owned legacy cookies', 23),
                ('Profile 2', 'Owned invalid host hash', 24),
                ('Profile 3', 'Owned unsupported encryption', 24),
                ('Profile 4', 'Owned unknown cookie schema', 25),
                ('Profile 6', 'Owned unknown cookie isolation', 24)]
    cache = {}
    for directory, title, version in profiles:
        profile = chromium / directory
        profile.mkdir(parents=True)
        (profile / 'Preferences').write_text('{}\n')
        cache[directory] = {'name': title}
        rows = [Cookie('owned_auth', tokens['owned_auth'])]
        wal = None
        if directory == 'Default':
            rows += [Cookie('owned_secure', tokens['owned_secure'], secure=True, same_site=0),
                     Cookie('owned_domain', tokens['owned_domain'], domain='.' + HOST, same_site=2),
                     Cookie('owned_path', tokens['owned_path'], path='/account'),
                     Cookie('owned_existing', tokens['owned_existing']),
                     Cookie('owned_session', tokens['owned_session'], persistent=False),
                     Cookie('owned_plaintext', tokens['owned_plaintext'], encrypted=False),
                     Cookie('owned_expired', tokens['owned_expired'], lifetime=-3600),
                     Cookie('owned_partitioned', tokens['owned_partitioned'], partition='https://owned-top-frame.test')]
            if cancellation:
                rows += [Cookie('owned_many_' + str(i), secrets.token_urlsafe(12)) for i in range(5000)]
            wal = Cookie('owned_wal', tokens['owned_wal'])
        elif directory == 'Profile 1':
            rows = [Cookie('owned_legacy', tokens['owned_legacy'])]
        elif directory == 'Profile 2':
            rows += [Cookie('owned_bad_hash', secrets.token_urlsafe(24), bad_host_hash=True)]
        elif directory == 'Profile 3':
            rows += [Cookie('owned_bad_envelope', secrets.token_urlsafe(24), envelope=b'v20')]
        elif directory == 'Profile 6':
            rows = [Cookie('owned_null_chromium', tokens['owned_null_chromium'], partition=None)]
        database = chromium_database(profile / 'Network/Cookies', rows, version=version,
                                      http_port=sites['http_port'], https_port=sites['https_port'], wal_cookie=wal,
                                      nullable_isolation=directory == 'Profile 6')
        stack.callback(database.close)
    (chromium / 'Local State').write_text(json.dumps({'profile': {'info_cache': cache}}))
    brave = config / 'BraveSoftware/Brave-Origin'
    brave_profile = brave / 'Default'
    brave_profile.mkdir(parents=True)
    (brave_profile / 'Preferences').write_text('{}\n')
    (brave / 'Local State').write_text(json.dumps({'profile': {'info_cache': {
        'Default': {'name': 'Owned Brave cookie sessions'}}}}))
    if keyring:
        database = chromium_database(brave_profile / 'Network/Cookies',
            [Cookie('owned_brave', tokens['owned_brave'], envelope=b'v11')], secret=keyring.brave_secret,
            http_port=sites['http_port'], https_port=sites['https_port'])
        stack.callback(database.close)
        extra = chromium / 'Profile 5'
        extra.mkdir()
        (extra / 'Preferences').write_text('{}\n')
        cache['Profile 5'] = {'name': 'Owned Chromium system-key cookies'}
        (chromium / 'Local State').write_text(json.dumps({'profile': {'info_cache': cache}}))
        database = chromium_database(extra / 'Network/Cookies',
            [Cookie('owned_system', tokens['owned_system'], envelope=b'v11')], secret=keyring.secret,
            http_port=sites['http_port'], https_port=sites['https_port'])
        stack.callback(database.close)
    firefox.mkdir(parents=True)
    application = firefox / 'owned-application'
    application.mkdir()
    (application / 'application.ini').write_text('[App]\nName=Firefox\n')
    lines = []
    for version in (15, 16):
        profile = firefox / ('Profiles/owned-cookies-' + str(version))
        profile.mkdir(parents=True)
        (profile / '.parentlock').touch()
        (profile / 'compatibility.ini').write_text('[Compatibility]\nLastVersion=156.0_20261103000000\nLastAppDir=' + str(application) + '\n')
        (profile / 'prefs.js').write_text('user_pref("browser.startup.homepage", "about:blank");\n')
        rows = [Cookie('owned_firefox_' + str(version), tokens['owned_firefox_' + str(version)]),
                Cookie('owned_firefox_expired_' + str(version), secrets.token_urlsafe(12), lifetime=-3600),
                Cookie('owned_firefox_container_' + str(version), secrets.token_urlsafe(12), container='^userContextId=1'),
                Cookie('owned_firefox_partition_' + str(version), secrets.token_urlsafe(12), partition='https://owned-top-frame.test'),
                Cookie('owned_null_firefox_' + str(version), tokens['owned_null_firefox_' + str(version)], container=None)]
        database = firefox_database(profile / 'cookies.sqlite', rows, version, nullable_isolation=True)
        stack.callback(database.close)
        lines.append(f'[Profile{version - 15}]\nName=Owned Firefox cookies {version}\nIsRelative=1\nPath=Profiles/owned-cookies-{version}\nDefault={int(version == 15)}\n')
    (firefox / 'profiles.ini').write_text('\n'.join(lines))


def source_hashes(config, firefox):
    return {str(path): source_keys.sha(path) for root in (config / 'chromium',
            config / 'BraveSoftware', firefox) for path in root.rglob('*') if path.is_file()}


def goto(browser, page, url):
    protocol(browser, 'Page.navigate', {'url': url}, page)
    wait_for(lambda: browser.ev('location.href===' + json.dumps(url) +
             '&&document.readyState==="complete"', page), browser.process)
    return page


def page(browser, url):
    target = request(browser.port, '/json/new?about:blank', 'PUT')
    return goto(browser, target, url)


def probe(browser, target, path='/probe'):
    return browser.ev('(async()=>await(await fetch(' + json.dumps(path) +
                      ',{cache:"no-store",credentials:"include"})).json())()', target)


def cookies(browser, target, sites):
    return protocol(browser, 'Network.getCookies', {'urls': [
        sites['http'] + '/account/', sites['https'] + '/account/']}, target)['cookies']


def setup_data(browser):
    target = wait_for(lambda: next((page for page in request(browser.port, '/json/list')
        if page['type'] == 'page' and page['url'] == 'chrome://setup/'), None), browser.process)
    request(browser.port, '/json/activate/' + target['id'])
    page_ready(browser, target)
    browser.ev(OBSERVE, target)
    gesture(target, 'document.querySelector("#continue").click();true')
    wait_for(lambda: browser.ev('!!document.querySelector("#refresh-import-profiles")&&'
        'document.querySelector(".panel").getAttribute("aria-busy")==="false"', target), browser.process)
    source_keys.choose(browser, target, 'Chromium', 'Owned cookie sessions')
    wait_for(lambda: browser.ev('!!document.querySelector("#import-data-cookies")', target), browser.process)
    return target


def cookie_only_setup(browser, target, result, enabled):
    check(result, 'Cookie import defaults off when a browser is selected',
          browser.ev('!document.querySelector("#import-data-cookies").checked', target))
    for kind in ('bookmarks', 'history', 'passwords'):
        gesture(target, '(()=>{const e=document.querySelector("#import-data-' + kind + '");'
                'if(e?.checked)e.click();return true})()')
    if enabled:
        gesture(target, 'document.querySelector("#import-data-cookies").click();true')
    check(result, 'Only explicitly selected cookies are staged', browser.ev(
        'document.querySelector("#import-data-cookies").checked===' + str(enabled).lower(), target))


def capture(browser, target, name):
    request(browser.port, '/json/activate/' + target['id'])
    owned_input = Input(browser.platform)
    try:
        owned_input.capture(browser.output / name).close()
    finally:
        owned_input.close()


def finish_setup(browser, target, result):
    go_ready(browser, target)
    check(result, 'Continue does not import cookies before Finish',
          browser.ev('ownedCookieImport.imports.length===0', target))
    gesture(target, 'document.querySelector("#continue").click();true')
    wait_for(lambda: browser.ev('location.href!=="chrome://setup/"', target), browser.process, timeout=90)


def settings_page(browser, target=None):
    target = goto(browser, target, 'chrome://settings/importData') if target else page(browser, 'chrome://settings/importData')
    wait_for(lambda: browser.ev('!!document.querySelector("settings-ui")?.shadowRoot', target), browser.process)
    wait_for(lambda: browser.ev('(()=>{' + DEEP + ';return !!' + DIALOG +
        '?.shadowRoot?.querySelector("#importDialogCookies")})()', target), browser.process)
    browser.ev(SETTINGS_OBSERVE, target)
    return target


def dialog_eval(browser, target, body):
    return browser.ev('(()=>{' + DEEP + ';const dialog=' + DIALOG +
                      ';const root=dialog.shadowRoot;' + body + '})()', target)


def select_settings(browser, target, brand, label):
    expression = '(()=>{' + DEEP + ';const d=' + DIALOG + ';const select=d.shadowRoot.querySelector("#browserSelect");'
    expression += ('const i=d.browserProfiles_.findIndex(p=>p.name===' + json.dumps(brand)
                   + '&&p.profileName===' + json.dumps(label) + ');if(i<0)return false;')
    expression += 'select.selectedIndex=i;select.dispatchEvent(new Event("change",{bubbles:true}));return true})()'
    assert browser.ev(expression, target), ('Owned cookie source not discovered: ' +
        json.dumps(dialog_eval(browser, target, 'return dialog.browserProfiles_.map(p=>({name:p.name,profileName:p.profileName}))')))
    dialog_eval(browser, target, 'return dialog.updateComplete')
    browser.ev('new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(()=>r(true))))', target)
    wait_for(lambda: dialog_eval(browser, target, 'return !root.querySelector("#importDialogCookies").hidden'), browser.process)
    assert dialog_eval(browser, target, 'return !root.querySelector("#importDialogCookies").checked'), 'Settings cookie import must default off'
    for identity in ('importDialogHistory', 'importDialogBookmarks', 'importDialogSavedPasswords',
                     'importDialogSearchEngine', 'importDialogAutofillFormData', 'importDialogExtensions'):
        gesture(target, '(()=>{' + DEEP + ';const e=' + DIALOG +
                '.shadowRoot.querySelector(' + json.dumps('#' + identity) + ');if(e?.checked&&!e.hidden)e.shadowRoot.querySelector("#checkbox").click();return true})()')
    browser.ev('new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(()=>r(true))))', target)
    assert dialog_eval(browser, target, 'return root.querySelector("#import").disabled'), ('Import must be disabled before the explicit cookie choice: ' +
        json.dumps(dialog_eval(browser, target, 'return [...root.querySelectorAll("settings-checkbox")].map(e=>({id:e.id,hidden:e.hidden,checked:e.checked,attribute:e.hasAttribute("checked")}))')))
    gesture(target, '(()=>{' + DEEP + ';' + DIALOG +
            '.shadowRoot.querySelector("#importDialogCookies").click();return true})()')
    wait_for(lambda: dialog_eval(browser, target, 'return !root.querySelector("#import").disabled'), browser.process)


def begin_settings(browser, target, *, complete=True):
    before = browser.ev('ownedCookieImport.events.length', target)
    gesture(target, '(()=>{' + DEEP + ';' + DIALOG +
            '.shadowRoot.querySelector("#import").click();return true})()')
    if not complete:
        wait_for(lambda: browser.ev('ownedCookieImport.events.slice(' + str(before) +
            ').some(e=>e.name==="import-data-status-changed"&&e.value==="inProgress")', target), browser.process)
        return None
    wait_for(lambda: browser.ev('ownedCookieImport.events.slice(' + str(before) +
        ').some(e=>e.name==="import-data-status-changed"&&["succeeded","failed"].includes(e.value))', target), browser.process, timeout=90)
    events = browser.ev('ownedCookieImport.events.slice(' + str(before) + ')', target)
    final = next(e['value'] for e in reversed(events) if e['name'] == 'import-data-status-changed')
    summaries = [e['value'] for e in events if e['name'] == 'import-cookies-result']
    if final == 'succeeded':
        assert summaries, 'Native cookie import did not report a summary before success'
        assert next(i for i, e in enumerate(events) if e['name'] == 'import-cookies-result') < next(
            i for i, e in enumerate(events) if e['name'] == 'import-data-status-changed' and e['value'] == 'succeeded'), 'Cookie summary must precede completion'
    return final, summaries[-1] if summaries else None


def import_settings(browser, brand, label, target=None):
    target = settings_page(browser, target)
    select_settings(browser, target, brand, label)
    capture(browser, target, 'cookie-choice-' + '-'.join(label.lower().split()) + '.png')
    return target, begin_settings(browser, target)


def assert_attributes(browser, target, result, sites):
    rows = {row['name']: row for row in cookies(browser, target, sites)}
    check(result, 'Native cookie store retains Secure and HttpOnly restrictions',
          rows['owned_secure']['secure'] and rows['owned_secure']['httpOnly'])
    check(result, 'Native cookie store preserves SameSite None, Lax, and Strict',
          rows['owned_secure']['sameSite'] == 'None' and rows['owned_auth']['sameSite'] == 'Lax'
          and rows['owned_domain']['sameSite'] == 'Strict')
    check(result, 'Cookie host-only, domain, and path scopes survive import',
          rows['owned_auth']['domain'] == HOST and rows['owned_domain']['domain'] == '.' + HOST
          and rows['owned_path']['path'] == '/account')
    check(result, 'Imported persistent cookie retains its bounded expiry',
          time.time() + 86000 < rows['owned_auth']['expires'] < time.time() + 86500)
    check(result, 'A source session cookie remains a session cookie',
          rows['owned_session']['session'] and rows['owned_session']['expires'] <= 0)
    check(result, 'Expired and partitioned source cookies are not imported',
          'owned_expired' not in rows and 'owned_partitioned' not in rows)


def basic(args, launcher, profile, result, sites):
    with owned_browser(launcher, profile / 'no-opt-in', args.output / 'no-opt-in', result,
                       'no-opt-in', normal_startup=True) as browser:
        target = setup_data(browser)
        cookie_only_setup(browser, target, result, False)
        finish_setup(browser, target, result)
        witness = page(browser, sites['http'] + '/')
        check(result, 'Finishing setup without cookie consent does not copy a session',
              not probe(browser, witness)['owned_auth'])
    actual = profile / 'with-opt-in'
    with owned_browser(launcher, actual, args.output / 'initial', result,
                       'initial', normal_startup=True) as browser:
        target = setup_data(browser)
        cookie_only_setup(browser, target, result, True)
        capture(browser, target, 'onboarding-cookie-opt-in.png')
        witness = page(browser, sites['http'] + '/')
        browser.ev('fetch("/seed-existing",{credentials:"include"}).then(()=>true)', witness)
        check(result, 'Owned endpoint requires a sign-in before the cookie import',
              not probe(browser, witness)['owned_auth'])
        request(browser.port, '/json/activate/' + target['id'])
        finish_setup(browser, target, result)
        state = probe(browser, witness)
        check(result, 'Finish makes an actual outgoing request signed in without a new login', state['owned_auth'])
        check(result, 'The importer reads the source WAL and preserves an existing destination identity',
              state['owned_wal'] and state['destination_kept'])
        check(result, 'Plaintext and non-persistent source sessions transfer without changing their scope',
              state['owned_plaintext'] and state['owned_session'])
        check(result, 'Path-restricted cookie is omitted outside its path', not state['owned_path'])
        check(result, 'Path-restricted cookie is sent to its original path', probe(browser, witness, '/account/probe')['owned_path'])
        secure = page(browser, sites['https'] + '/')
        check(result, 'A real HTTPS request sends the imported secure session', probe(browser, secure)['owned_secure'])
        subdomain = page(browser, sites['sub'] + '/')
        child = probe(browser, subdomain)
        check(result, 'A subdomain receives the domain cookie and not the host-only session',
              child['owned_domain'] and not child['owned_auth'])
        check(result, 'Imported HttpOnly sessions remain inaccessible to page JavaScript',
              browser.ev('document.cookie===""', witness))
        assert_attributes(browser, witness, result, sites)
        _, outcome = import_settings(browser, 'Chromium', 'Owned legacy cookies')
        check(result, 'Legacy v10 cookies without a host hash import through Settings',
              outcome[0] == 'succeeded' and outcome[1]['imported'] == 1 and probe(browser, witness)['owned_legacy'])
    with owned_browser(launcher, actual, args.output / 'restart', result,
                       'restart', normal_startup=True) as browser:
        witness = page(browser, sites['http'] + '/')
        state = probe(browser, witness)
        check(result, 'Persistent imported sign-in and kept destination survive normal restart',
              state['owned_auth'] and state['owned_wal'] and state['destination_kept'] and state['owned_legacy'])


def other_cases(args, launcher, profile, result, sites, keyring):
    with owned_browser(launcher, profile, args.output / 'initial', result,
                       'initial', normal_startup=True) as browser:
        witness = page(browser, sites['http'] + '/')
        if args.case == 'system-key':
            for brand, label, name in [('Chromium', 'Owned Chromium system-key cookies', 'owned_system'),
                                        ('Brave Origin', 'Owned Brave cookie sessions', 'owned_brave')]:
                _, outcome = import_settings(browser, brand, label)
                check(result, 'Native v11 cookie import decrypts the owned ' + brand + ' key',
                      outcome[0] == 'succeeded' and outcome[1]['imported'] == 1 and probe(browser, witness)[name])
            check(result, 'Owned Chromium and Brave key identities remain unchanged', keyring.unchanged())
        elif args.case == 'firefox':
            for version in (15, 16):
                _, outcome = import_settings(browser, 'Mozilla Firefox', 'Owned Firefox cookies ' + str(version))
                check(result, 'Firefox schema ' + str(version) + ' imports a real signed-in session with the right expiry units',
                      outcome[0] == 'succeeded' and outcome[1]['imported'] == 1
                      and outcome[1]['skipped'] == 4 and probe(browser, witness)['owned_firefox_' + str(version)])
                check(result, 'Firefox schema ' + str(version) + ' does not remove unknown NULL isolation',
                      not probe(browser, witness)['owned_null_firefox_' + str(version)])
                row = next(row for row in cookies(browser, witness, sites) if row['name'] == 'owned_firefox_' + str(version))
                check(result, 'Firefox schema ' + str(version) + ' expiry remains approximately one day',
                      time.time() + 86000 < row['expires'] < time.time() + 86500)
        elif args.case == 'failures':
            for label in ('Owned invalid host hash', 'Owned unsupported encryption', 'Owned unknown cookie schema'):
                _, outcome = import_settings(browser, 'Chromium', label)
                check(result, 'Native cookie preflight rejects ' + label + ' without partial destination writes',
                      outcome[0] == 'failed' and not probe(browser, witness)['owned_auth'])
            _, outcome = import_settings(browser, 'Chromium', 'Owned unknown cookie isolation')
            check(result, 'Chromium does not remove unknown NULL isolation to import a cookie',
                  outcome[0] == 'succeeded' and outcome[1]['imported'] == 0
                  and outcome[1]['skipped'] == 1 and not probe(browser, witness)['owned_null_chromium'])
        elif args.case == 'isolation':
            settings = settings_page(browser)
            browser.settings = settings
            spaces = browser.call('noventraSpaces', 'create', '', 'Work')
            work = next(space['id'] for space in spaces if space['name'] == 'Work')
            before = {target['id'] for target in request(browser.port, '/json/list')}
            browser.call('noventraSpaces', 'switch', work, '')
            work_target = wait_for(lambda: next((target for target in request(browser.port, '/json/list')
                if target['type'] == 'page' and target['id'] not in before), None), browser.process)
            work_target, outcome = import_settings(browser, 'Chromium', 'Owned cookie sessions', work_target)
            check(result, 'Work Settings reports a completed cookie import', outcome[0] == 'succeeded')
            goto(browser, work_target, sites['http'] + '/')
            check(result, 'Work receives its imported signed-in session', probe(browser, work_target)['owned_auth'])
            check(result, 'Importing into Work does not sign Personal in', not probe(browser, witness)['owned_auth'])
            browser.settings = settings
            spaces = browser.call('noventraSpaces', 'create', '', 'Other')
            other = next(space['id'] for space in spaces if space['name'] == 'Other')
            before = {target['id'] for target in request(browser.port, '/json/list')}
            browser.call('noventraSpaces', 'switch', other, '')
            other_target = wait_for(lambda: next((target for target in request(browser.port, '/json/list')
                if target['type'] == 'page' and target['id'] not in before), None), browser.process)
            goto(browser, other_target, sites['http'] + '/')
            check(result, 'A different regular Space receives none of Work\'s imported cookies',
                  not probe(browser, other_target)['owned_auth'])
            request(browser.port, '/json/activate/' + other_target['id'])
            inp = Input(browser.platform)
            try:
                before = {target['id'] for target in request(browser.port, '/json/list')}
                focus_owned(browser, inp)
                native_key(inp, 'n', ('Control_L', 'Shift_L'))
                private = wait_for(lambda: next((target for target in request(browser.port, '/json/list')
                    if target['type'] == 'page' and target['id'] not in before), None), browser.process)
                goto(browser, private, sites['http'] + '/')
                private_context = protocol(browser, 'Target.getTargetInfo',
                                           {'targetId': private['id']}, private)['targetInfo'].get('browserContextId')
                regular_context = protocol(browser, 'Target.getTargetInfo',
                                           {'targetId': witness['id']}, witness)['targetInfo'].get('browserContextId')
                check(result, 'The native Incognito shortcut creates a distinct private browser context',
                      bool(private_context) and private_context != regular_context)
                check(result, 'A genuine Incognito Space receives no regular imported session',
                      not probe(browser, private)['owned_auth'])
            finally:
                inp.close()
        elif args.case == 'cancellation':
            target = settings_page(browser)
            select_settings(browser, target, 'Chromium', 'Owned cookie sessions')
            begin_settings(browser, target, complete=False)
            wait_for(lambda: browser.ev('ownedCookieImport.events.some(e=>e.name==="import-cookies-result"&&e.value.imported>0)', target), browser.process)
            check(result, 'Cancellation closes a still-running import after a real cookie batch was acknowledged',
                  browser.ev('ownedCookieImport.events.filter(e=>e.name==="import-data-status-changed").at(-1)?.value', target) == 'inProgress')
            request(browser.port, '/json/close/' + target['id'])
            check(result, 'Closing an active import page leaves the native browser usable',
                  browser.process.poll() is None and probe(browser, witness) is not None)
            _, outcome = import_settings(browser, 'Chromium', 'Owned legacy cookies')
            check(result, 'A new native cookie import completes after the previous import was cancelled',
                  outcome[0] == 'succeeded' and probe(browser, witness)['owned_legacy'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['basic', 'system-key', 'firefox', 'isolation', 'failures', 'cancellation',
                                         'fixture-preflight'], default='basic')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.rtc_policy_diagnostic = False
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    assert source_keys.sha(args.browser) == args.expected_binary
    assert source_keys.sha(args.browser.parent / 'resources.pak') == args.expected_resources
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    result = {'passed': False, 'case': args.case, 'checks': [],
              'home_isolated': True, 'network_isolated_no_egress': True,
              'source_data': 'owned synthetic fixtures only',
              'binary_sha256': args.expected_binary, 'resources_sha256': args.expected_resources,
              'fixture_sha256': source_keys.sha(Path(__file__)),
              'native_callbacks': 'observed pass-through, never substituted'}
    tokens = {name: secrets.token_urlsafe(24) for name in ('owned_auth', 'owned_secure', 'owned_domain',
              'owned_path', 'owned_existing', 'owned_session', 'owned_plaintext',
              'owned_expired', 'owned_partitioned', 'owned_wal', 'owned_legacy',
              'owned_system', 'owned_brave', 'owned_firefox_15', 'owned_firefox_16',
              'owned_null_chromium', 'owned_null_firefox_15', 'owned_null_firefox_16', 'destination')}
    config = Path.home() / 'owned-cookie-config'
    firefox = Path.home() / '.mozilla/firefox'
    profile = args.output / 'destination'
    try:
        service = source_keys.owned_secret_service(args, result) if args.case == 'system-key' else nullcontext(None)
        with service as keyring, endpoints(args.output, tokens) as sites, ExitStack() as stack:
            seed(config, firefox, stack, tokens, sites, keyring, args.case == 'cancellation')
            before = source_hashes(config, firefox)
            os.environ['XDG_CONFIG_HOME'] = str(config)
            launcher = args.output / 'owned-cookie-launcher'
            flags = ['--password-store=basic', '--ignore-certificate-errors',
                     '--host-resolver-rules=MAP ' + HOST + ' 127.0.0.1,MAP *.' + HOST + ' 127.0.0.1']
            launcher.write_text('#!/bin/sh\nexec ' + shlex.join([str(args.browser), *flags]) + ' "$@"\n')
            launcher.chmod(0o700)
            if args.case == 'fixture-preflight':
                database = sqlite3.connect((config / 'chromium/Default/Network/Cookies').as_uri() + '?mode=ro&immutable=1', uri=True)
                try:
                    row = database.execute('SELECT host_key,encrypted_value FROM cookies WHERE name="owned_auth"').fetchone()
                    plaintext = source_keys.aes_cbc(row[1][3:], source_keys.system_key(b'peanuts'), decrypt=True)
                    check(result, 'Owned modern ciphertext contains the exact host hash and synthetic value',
                          plaintext == hashlib.sha256(row[0].encode()).digest() + tokens['owned_auth'].encode())
                finally:
                    database.close()
                check(result, 'Fixture preflight does not create a native destination profile', not profile.exists())
            elif args.case == 'basic':
                basic(args, launcher, profile, result, sites)
            else:
                other_cases(args, launcher, profile, result, sites, keyring)
            check(result, 'Every source database, WAL, sidecar, and profile file stays byte-identical',
                  before == source_hashes(config, firefox))
            result['passed'] = True
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'case': args.case, 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
