"""Import synthetic Linux browser passwords through the real setup/native stack.

Run via the private Xvfb wrapper. The additional namespace hides the user's
HOME and has no network egress/session bus. All databases and passwords below
are owned fixtures. Never use a personal source profile or keyring here.

--case system-key additionally owns a private session bus and foreground
Secret Service, and requires real native v11 imports using distinct synthetic
Chromium and Brave keys.
--case system-key-preflight checks that isolated harness without launching a
browser. It is not acceptance of native v11 import.
"""
import argparse
from contextlib import contextmanager, nullcontext
import ctypes as C
from ctypes.util import find_library
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import select
import shlex
import signal
import sqlite3
import stat
import subprocess
import sys
import time
from types import SimpleNamespace

from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from onboarding_import_smoke import gesture
from onboarding_managers_smoke import owned_browser, page_ready, click, setup_page
from portable_transfer_smoke import isolate, evaluate, open_manager

ORIGIN = 'https://owned-password-import.invalid/'
EXISTING = 'Owned destination password 2026!'
SOURCE = 'Owned source password 2026!'
ADDED = 'Owned newly imported password 2026!'
BOOKMARK = 'https://owned-bookmark-import.invalid/'
BRAVE_BOOKMARK = 'https://owned-brave-bookmark-import.invalid/'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def aes_cbc(data, key, decrypt=False):
    """Keep synthetic system keys/passwords out of process arguments and logs."""
    library = C.CDLL(find_library('crypto'))
    library.EVP_CIPHER_CTX_new.restype = C.c_void_p
    library.EVP_CIPHER_CTX_free.argtypes = [C.c_void_p]
    library.EVP_aes_128_cbc.restype = C.c_void_p
    operation = 'Decrypt' if decrypt else 'Encrypt'
    start = getattr(library, 'EVP_' + operation + 'Init_ex')
    update = getattr(library, 'EVP_' + operation + 'Update')
    finish = getattr(library, 'EVP_' + operation + 'Final_ex')
    start.argtypes = [C.c_void_p] * 5
    update.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_int), C.c_void_p, C.c_int]
    finish.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_int)]
    context = library.EVP_CIPHER_CTX_new()
    assert context and len(key) == 16, 'Owned cipher initialization failed'
    output = C.create_string_buffer(len(data) + 16)
    written, final = C.c_int(), C.c_int()
    try:
        assert start(context, library.EVP_aes_128_cbc(), None,
                     C.c_char_p(key), C.c_char_p(b' ' * 16)) == 1
        assert update(context, output, C.byref(written), C.c_char_p(data), len(data)) == 1
        if finish(context, C.byref(output, written.value), C.byref(final)) != 1:
            assert decrypt, 'Owned cipher encryption failed'
            return None
        return output.raw[:written.value + final.value]
    finally:
        library.EVP_CIPHER_CTX_free(context)


def system_key(secret):
    # Exact persisted Linux v11 derivation used by FreedesktopSecretKeyProvider.
    return hashlib.pbkdf2_hmac('sha1', secret, b'saltysalt', 1, 16)


def cipher(password, tag=b'v10', system_secret=None):
    if system_secret is not None:
        return tag + aes_cbc(password.encode(), system_key(system_secret))
    key = system_key(b'peanuts')
    encrypted = subprocess.run(['openssl', 'enc', '-aes-128-cbc', '-K', key.hex(),
                                '-iv', (b' ' * 16).hex()], input=password.encode(),
                               capture_output=True, check=True).stdout
    return tag + encrypted


@contextmanager
def owned_secret_service(args, result):
    """Never connect to a host bus, activate services, or use a personal home."""
    assert args.inside_owned_home
    assert os.environ.get('NOVENTRA_TRANSFER_OWNED_HOME') == str(args.output.parent)
    assert os.environ.get('DBUS_SESSION_BUS_ADDRESS') == 'unix:path=/tmp/noventra-no-session-bus'
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    assert runtime == args.output.parent / 'runtime' and runtime.stat().st_mode & 0o077 == 0
    # isolate() bind-mounts its owned namespace-tmp over /tmp. A short path also
    # avoids sockaddr_un's limit when the retained result directory is long.
    work = Path('/tmp/noventra-system-key')
    work.mkdir(mode=0o700)
    control = work / 'control'
    control.mkdir(mode=0o700)
    data_home = Path.home() / 'owned-keyring-data'
    data_home.mkdir(mode=0o700)
    bus_socket = work / 'bus'
    expected_address = 'unix:path=' + str(bus_socket)
    configuration = work / 'session.conf'
    # No service directories/helpers: the bus cannot auto-activate untracked
    # processes. Both services below are explicitly owned foreground children.
    configuration.write_text('<busconfig><type>session</type><listen>' + expected_address +
        '</listen><auth>EXTERNAL</auth><policy context="default"><allow user="' +
        str(os.getuid()) + '"/><allow own="*"/><allow send_destination="*"/>' +
        '<allow receive_sender="*"/></policy></busconfig>\n')
    configuration.chmod(0o600)
    names = ['DBUS_SESSION_BUS_ADDRESS', 'DBUS_SYSTEM_BUS_ADDRESS',
             'DBUS_STARTER_ADDRESS', 'DBUS_STARTER_BUS_TYPE',
             'GNOME_KEYRING_CONTROL', 'XDG_DATA_HOME', 'XDG_CURRENT_DESKTOP', 'DESKTOP_SESSION']
    old_environment = {name: os.environ.get(name) for name in names}
    processes = []
    secret = secrets.token_urlsafe(32).encode()
    # Distinct source identities must not accidentally decrypt one another.
    # Choose a definite padding failure instead of relying on random chance.
    for _ in range(8):
        brave_secret = secrets.token_urlsafe(32).encode()
        if brave_secret != secret and aes_cbc(
                cipher(ADDED, b'v11', brave_secret)[3:], system_key(secret),
                decrypt=True) is None:
            break
    else:
        raise AssertionError('Owned distinct Brave key could not be prepared')
    source_keys = {'chromium': secret, 'brave': brave_secret}
    unlock = ('Owned keyring fixture ' + secrets.token_urlsafe(32)).encode()
    cleanup = result['owned_keyring_cleanup'] = {'activation_disabled': True,
                                               'foreground_processes_reaped': False}

    def command(argv, input=None, allowed=(0,)):
        assert os.environ.get('DBUS_SESSION_BUS_ADDRESS', '').split(',')[0] == expected_address
        try:
            run = subprocess.run(argv, input=input, capture_output=True, timeout=12)
        except subprocess.TimeoutExpired:
            raise RuntimeError('Owned Secret Service command timed out; output omitted') from None
        if run.returncode not in allowed:
            raise RuntimeError('Owned Secret Service command failed; output omitted')
        return run

    def bus_call(method, *values):
        return command(['/usr/bin/gdbus', 'call', '--session', '--dest',
                        'org.freedesktop.secrets', '--object-path',
                        '/org/freedesktop/secrets', '--method',
                        'org.freedesktop.Secret.Service.' + method, *values]).stdout

    def items(attributes='{}'):
        # SearchItems exposes only item object paths, never secret content.
        return bus_call('SearchItems', attributes)

    try:
        bus = subprocess.Popen(['/usr/bin/dbus-daemon', '--config-file=' + str(configuration),
                                '--nofork', '--nopidfile', '--nosyslog', '--print-address'],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               start_new_session=True)
        processes.append(bus)
        assert select.select([bus.stdout], [], [], 10)[0], 'Owned session bus did not start'
        address = bus.stdout.readline().decode().strip()
        assert address.split(',')[0] == expected_address and bus.poll() is None
        assert stat.S_ISSOCK(bus_socket.stat().st_mode) and bus_socket.stat().st_uid == os.getuid()
        bus_socket.chmod(0o600)
        os.environ.update(DBUS_SESSION_BUS_ADDRESS=address,
                          DBUS_SYSTEM_BUS_ADDRESS='unix:path=/tmp/noventra-no-system-bus',
                          GNOME_KEYRING_CONTROL=str(control), XDG_DATA_HOME=str(data_home),
                          XDG_CURRENT_DESKTOP='GNOME', DESKTOP_SESSION='gnome')
        os.environ.pop('DBUS_STARTER_ADDRESS', None)
        os.environ.pop('DBUS_STARTER_BUS_TYPE', None)
        keyring = subprocess.Popen(['/usr/bin/gnome-keyring-daemon', '--foreground', '--unlock',
                                    '--components=secrets', '--control-directory=' + str(control)],
                                   stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, start_new_session=True)
        processes.append(keyring)
        keyring.stdin.write(unlock)
        keyring.stdin.close()
        deadline = time.monotonic() + 12
        while True:
            assert keyring.poll() is None, 'Owned keyring exited before readiness'
            owned = command(['/usr/bin/gdbus', 'call', '--session', '--dest',
                             'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                             '--method', 'org.freedesktop.DBus.NameHasOwner',
                             'org.freedesktop.secrets']).stdout
            if b'true' in owned:
                break
            assert time.monotonic() < deadline, 'Owned keyring did not own its bus name'
            time.sleep(0.05)
        owner_pid = command(['/usr/bin/gdbus', 'call', '--session', '--dest',
                             'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                             '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID',
                             'org.freedesktop.secrets']).stdout
        assert re.search(rb'uint32 (\d+)', owner_pid)[1] == str(keyring.pid).encode(), 'Unexpected keyring bus owner'
        assert b'/org/freedesktop/secrets/' not in items(), 'Owned keyring must start empty'
        for application, source_secret in source_keys.items():
            command(['/usr/bin/secret-tool', 'store',
                     '--label=Owned browser import fixture',
                     'application', application], input=source_secret)
        assert b"objectpath '/'" not in bus_call('ReadAlias', 'default'), 'Owned default collection missing'

        def item_paths(response):
            return frozenset(re.findall(
                rb'/org/freedesktop/secrets/collection/[A-Za-z0-9_/]+', response))

        original_items = item_paths(items())
        matching_items = {application: item_paths(items(
            "{'application': '" + application + "'}")) for application in source_keys}
        assert len(original_items) == 2 and all(len(v) == 1 for v in matching_items.values())
        assert set.union(*(set(v) for v in matching_items.values())) == original_items
        assert matching_items['chromium'].isdisjoint(matching_items['brave'])

        def unchanged():
            for application, source_secret in source_keys.items():
                current = command(['/usr/bin/secret-tool', 'lookup',
                                   'application', application]).stdout
                if (current.rstrip(b'\n') != source_secret or item_paths(items(
                        "{'application': '" + application + "'}")) != matching_items[application]):
                    return False
            return (item_paths(items()) == original_items and
                    all(process.poll() is None for process in processes) and
                    all(not Path('/proc', str(p.pid), 'task', str(p.pid), 'children').read_text().strip()
                        for p in processes) and
                    stat.S_ISSOCK(bus_socket.stat().st_mode))

        assert unchanged(), 'Owned source key verification failed'
        result['private_secret_service'] = {
            'session_bus_socket_owned': True, 'foreground_keyring_name_owned': True,
            'applications': list(source_keys), 'source_keys': 'two synthetic owned items only',
            'distinct_source_keys': True, 'desktop_selection': 'GNOME',
            'default_collection_available': True}
        yield SimpleNamespace(secret=secret, brave_secret=brave_secret,
                              unchanged=unchanged)
    finally:
        # No auto-activation is configured. Foreground bus/keyring children are
        # reaped explicitly; isolate() intentionally has no PID namespace.
        exit_codes = []
        for process in reversed(processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
            exit_codes.append(process.wait(timeout=1))
            if process.stdout:
                process.stdout.close()
        def group_gone(process):
            try:
                os.killpg(process.pid, 0)
                return False
            except ProcessLookupError:
                return True

        for process in processes:
            if not group_gone(process):
                os.killpg(process.pid, signal.SIGKILL)
        deadline = time.monotonic() + 5
        while not all(group_gone(p) for p in processes) and time.monotonic() < deadline:
            time.sleep(0.05)
        cleanup.update(foreground_processes_reaped=all(p.poll() is not None for p in processes),
                       foreground_exit_codes=exit_codes,
                       owned_process_groups_gone=all(group_gone(p) for p in processes),
                       session_bus_socket_removed=not bus_socket.exists())
        for name, value in old_environment.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        assert cleanup['foreground_processes_reaped'] and cleanup['owned_process_groups_gone'], 'Owned daemon cleanup incomplete'


def database(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as connection:
        connection.execute('CREATE TABLE logins(origin_url TEXT, action_url TEXT, username_element TEXT, username_value TEXT, password_element TEXT, password_value BLOB, signon_realm TEXT, scheme INTEGER, blacklisted_by_user INTEGER, federation_url TEXT)')
        for username, origin, encrypted in rows:
            connection.execute('INSERT INTO logins VALUES(?,?,?,?,?,?,?,?,?,?)',
                               (origin, origin, '', username, '', encrypted, origin, 0, 0, ''))
    path.chmod(0o600)


def seed(config, system_secret=None, brave_secret=None):
    assert bool(system_secret) == bool(brave_secret)
    encrypt = (lambda password: cipher(password, b'v11', system_secret)) if system_secret else cipher
    helium = config / 'net.imput.helium'
    for name in ['Default', 'Profile 1', 'Profile 2', 'Profile 3', 'Profile 4']:
        path = helium / name
        path.mkdir(parents=True)
        (path / 'Preferences').write_text('{}\n')
    # Helium has no Local State: exercise real directory fallback.
    database(helium / 'Default/Login Data', [('existing-user', ORIGIN, encrypt(SOURCE)),
                                             ('new-user', ORIGIN, encrypt(ADDED))])
    database(helium / 'Profile 1/Login Data For Account',
             [('account-user', 'https://owned-account-import.invalid/', encrypt(ADDED))])
    unavailable = cipher(SOURCE, b'v11')
    if system_secret:
        # A real source key is available, but this profile was encrypted with
        # another owned key. Choose a definite PKCS#7 failure for the fixture.
        wrong_secret = b'Owned different source key'
        for attempt in range(8):
            unavailable = cipher(SOURCE, b'v11', wrong_secret)
            if aes_cbc(unavailable[3:], system_key(system_secret), decrypt=True) is None:
                break
            wrong_secret += b'!'
        else:
            raise AssertionError('Owned wrong-key fixture could not be prepared')
    database(helium / 'Profile 2/Login Data',
             [('locked-user', 'https://owned-locked-import.invalid/', unavailable)])
    database(helium / 'Profile 3/Login Data',
             [('partial-user', 'https://owned-partial-import.invalid/', encrypt(ADDED)),
              ('unsupported-user', 'https://owned-unsupported-import.invalid/', b'v20unsupported')])
    chromium = config / 'chromium'
    (chromium / 'Profile 2').mkdir(parents=True)
    # A stale nonempty cache must not hide the actual Profile 2 directory.
    (chromium / 'Local State').write_text(json.dumps({'profile': {'info_cache': {'Deleted': {'name': 'Deleted fixture'}}}}))
    (chromium / 'Profile 2/Preferences').write_text('{}\n')
    bookmarks = {'version': 1, 'roots': {'bookmark_bar': {'type': 'folder', 'name': 'Bookmarks bar',
        'children': [{'type': 'url', 'name': 'Owned direct import bookmark', 'url': BOOKMARK, 'date_added': '13400000000000000'}]},
        'other': {'type': 'folder', 'name': 'Other bookmarks', 'children': []}}}
    (chromium / 'Profile 2/Bookmarks').write_text(json.dumps(bookmarks))
    # Match the installed Brave Origin root instead of assuming Brave-Browser.
    brave = config / 'BraveSoftware/Brave-Origin'
    for name in ['Default', 'Profile 1']:
        (brave / name).mkdir(parents=True)
        (brave / name / 'Preferences').write_text('{}\n')
    (brave / 'Local State').write_text(json.dumps({'profile': {'info_cache': {
        'Default': {'name': 'Origin local'}, 'Profile 1': {'name': 'Origin account'}}}}))
    brave_encrypt = (lambda password: cipher(password, b'v11', brave_secret)) if brave_secret else cipher
    database(brave / 'Default/Login Data', [
        ('existing-user', ORIGIN, brave_encrypt(SOURCE)),
        ('brave-user', 'https://owned-brave-import.invalid/', brave_encrypt(ADDED))])
    database(brave / 'Profile 1/Login Data For Account', [
        ('brave-account-user', 'https://owned-brave-account-import.invalid/', brave_encrypt(ADDED))])
    brave_bookmarks = json.loads(json.dumps(bookmarks))
    brave_bookmarks['roots']['bookmark_bar']['children'][0].update(
        name='Owned Brave Origin bookmark', url=BRAVE_BOOKMARK)
    (brave / 'Default/Bookmarks').write_text(json.dumps(brave_bookmarks))
    if brave_secret:
        assert aes_cbc(brave_encrypt(ADDED)[3:], system_key(system_secret), decrypt=True) is None
    return {str(path.relative_to(config)): sha(path) for path in config.rglob('*') if path.is_file()}


def cards(browser, page):
    return browser.ev('Array.from(document.querySelectorAll(".profile-card")).map(c=>({id:Number(c.querySelector("input").value),brand:c.querySelector(".source-name strong").innerText,profile:c.querySelector(".source-name span").innerText,disabled:c.querySelector("input").disabled,imported:c.classList.contains("imported"),text:c.innerText}))', page)


def choose(browser, page, brand, profile):
    row = next(row for row in cards(browser, page) if row['brand'] == brand and row['profile'] == profile)
    assert not row['disabled'], row
    gesture(page, 'document.querySelector(".profile-card input[value=\\"' + str(row['id']) + '\\"]").click();true')
    return row['id']


def matches(browser, expected):
    manager = open_manager(browser)
    # Evaluate comparisons inside the owned page; only booleans leave it.
    return evaluate(browser, '''(async()=>{const wanted=EXPECTED;
      const rows=await chrome.passwordsPrivate.getSavedPasswordList();
      if(rows.length!==Object.keys(wanted).length)return false;
      for(const [username,password] of Object.entries(wanted)){
        const row=rows.find(r=>r.username===username);if(!row)return false;
        if(await chrome.passwordsPrivate.requestPlaintextPassword(row.id,
          chrome.passwordsPrivate.PlaintextReason.VIEW)!==password)return false;
      }return true})()'''.replace('EXPECTED', json.dumps(expected)), manager)


@contextmanager
def mandatory_password_policy(browser, result, check):
    """Apply/revert a real native local-test policy in this owned browser only."""
    settings = request(browser.port, '/json/new?chrome://settings/', 'PUT')
    policy = request(browser.port, '/json/new?chrome://policy/test', 'PUT')
    requested = False
    state = result['mandatory_password_policy'] = {
        'rule': {'namespace': 'chrome', 'name': 'PasswordManagerEnabled',
                 'value': False, 'level': 1, 'scope': 1, 'source': 2},
        'applied_enforced': False, 'reverted_to_ordinary_false': False}

    def pref_is(enforced):
        condition = 'p.enforcement === "ENFORCED"' if enforced else 'p.enforcement === undefined'
        return evaluate(browser, '''new Promise(resolve=>{
          chrome.settingsPrivate.getPref("credentials_enable_service",p=>
            resolve(!!p && p.value===false && CONDITION));
        })'''.replace('CONDITION', condition), settings)

    try:
        wait_for(lambda: browser.ev('typeof chrome.settingsPrivate === "object"', settings), browser.process)
        wait_for(lambda: browser.ev('document.readyState === "complete"', policy), browser.process)
        supported = evaluate(browser, '''(async()=>{
          const {loadTimeData}=await import('chrome://resources/js/load_time_data.js');
          return loadTimeData.getString('acceptedPaths').split('|').includes('/test');
        })()''', policy)
        state['native_test_route_supported'] = supported
        check('Native local-policy test route is available; unsupported channels fail', supported)
        check('Saving prompts start as ordinary false before managed-policy test', pref_is(False))
        requested = True
        applied = evaluate(browser, '''(async()=>{
          const {sendWithPromise}=await import('chrome://resources/js/cr.js');
          return sendWithPromise('setLocalTestPolicies',POLICIES,'');
        })()'''.replace('POLICIES', json.dumps(json.dumps([state['rule']]))), policy)
        check('Native policy handler accepts mandatory PasswordManagerEnabled=false', applied is True)
        wait_for(lambda: pref_is(True), browser.process)
        state['applied_enforced'] = True
        check('Real settingsPrivate pref is false with ENFORCED policy metadata', pref_is(True))
        yield state
    finally:
        try:
            if requested:
                evaluate(browser, '''(()=>{
                  chrome.send('revertLocalTestPolicies');return true;
                })()''', policy)
                wait_for(lambda: pref_is(False), browser.process)
                state['reverted_to_ordinary_false'] = True
                check('Native policy revert restores ordinary false without enabling saving prompts', pref_is(False))
        finally:
            request(browser.port, '/json/close/' + policy['id'])
            request(browser.port, '/json/close/' + settings['id'])


def native_import(args, launcher, profile, config, before, result, check, keyring):
    expected = {'existing-user': EXISTING}
    with owned_browser(launcher, profile, args.output / 'initial', result, 'initial', normal_startup=True) as browser:
        assert Path('/proc', str(browser.process.pid), 'exe').resolve() == args.browser
        page = setup_page(browser)
        page_ready(browser, page)
        click(browser, page, '#continue', 'Bring your browsing data.')
        # Opening the import step starts discovery without an extra Find click.
        wait_for(lambda: browser.ev('document.querySelector(".panel").getAttribute("aria-busy")==="false" && !!document.querySelector("#refresh-import-profiles")', page), browser.process)
        check('Opening the import step automatically completes native browser discovery', len(cards(browser, page)) == 8)
        found = cards(browser, page)
        result['discovered_profiles'] = [{k: row[k] for k in ['brand', 'profile', 'id']} for row in found]
        check('Native detection finds all8 owned profiles through XDG, Helium, Brave Origin and stale/missing cache', len(found) == 8 and sum(row['brand'] == 'Helium' for row in found) == 5 and sum(row['brand'] == 'Brave Origin' for row in found) == 2)
        check('Password-only and empty profiles are visible without invented bookmarks', all(row['disabled'] for row in found if row['brand'] == 'Helium') and 'No saved bookmarks' in next(row['text'] for row in found if row['brand'] == 'Helium'))
        choose(browser, page, 'Chromium', 'Profile 2')
        gesture(page, 'document.querySelector("#import-bookmarks").click();true')
        wait_for(lambda: browser.ev('!!document.querySelector(".import-message .success")', page), browser.process)
        bookmark_manager = request(browser.port, '/json/new?chrome://bookmarks/', 'PUT')
        wait_for(lambda: browser.ev('typeof chrome.bookmarks === "object"', bookmark_manager), browser.process)
        check('Native profile bookmark import writes the actual destination model', browser.ev('new Promise(r=>chrome.bookmarks.search({url:' + json.dumps(BOOKMARK) + '},x=>r(x.length===1)))', bookmark_manager))
        request(browser.port, '/json/activate/' + page['id'])
        choose(browser, page, 'Brave Origin', 'Origin local')
        gesture(page, 'document.querySelector("#import-bookmarks").click();true')
        wait_for(lambda: browser.ev('!!document.querySelector(".import-message .success")', page), browser.process)
        check('Brave Origin native bookmark import writes the actual destination model', browser.ev('new Promise(r=>chrome.bookmarks.search({url:' + json.dumps(BRAVE_BOOKMARK) + '},x=>r(x.length===1)))', bookmark_manager))
        request(browser.port, '/json/close/' + bookmark_manager['id'])
        manager = open_manager(browser)
        evaluate(browser, '(async()=>{await chrome.passwordsPrivate.addPassword(' + json.dumps({'url': ORIGIN, 'username': 'existing-user', 'password': EXISTING, 'note': 'Keep owned destination note', 'useAccountStore': False}) + ');return true})()', manager)
        wait_for(lambda: matches(browser, expected), browser.process)
        request(browser.port, '/json/activate/' + page['id'])
        click(browser, page, '#continue', 'Your passwords, on this device.')
        check('Password step reuses native discovery and enables only supported profiles', len(cards(browser, page)) == 8 and all(not row['disabled'] for row in cards(browser, page) if row['brand'] == 'Brave Origin' or (row['brand'] == 'Helium' and row['profile'] == 'Default')))
        with mandatory_password_policy(browser, result, check) as managed:
            request(browser.port, '/json/activate/' + page['id'])
            choose(browser, page, 'Helium', 'Default')
            gesture(page, 'document.querySelector("#import-passwords").click();true')
            wait_for(lambda: browser.ev('!!document.querySelector(".import-message .error")', page), browser.process, timeout=75)
            rejected = browser.ev('!!document.querySelector(".import-message .error")', page)
            managed['valid_import_rejected'] = rejected
            check('Mandatory PasswordManagerEnabled=false rejects a valid native source import', rejected)
            unchanged = matches(browser, expected)
            managed['existing_credentials_unchanged'] = unchanged
            check('Managed import rejection leaves existing destination credentials unchanged', unchanged)
            check('Managed rejection leaves the source available for an ordinary retry', not next(
                row['imported'] for row in cards(browser, page)
                if row['brand'] == 'Helium' and row['profile'] == 'Default'))
            request(browser.port, '/json/activate/' + page['id'])
            choose(browser, page, 'Brave Origin', 'Origin local')
            gesture(page, 'document.querySelector("#import-passwords").click();true')
            wait_for(lambda: browser.ev('!!document.querySelector(".import-message .error")', page), browser.process, timeout=75)
            check('Mandatory password-manager policy also rejects a valid Brave Origin source without writes', matches(browser, expected))
        request(browser.port, '/json/activate/' + page['id'])
        imported = []
        for brand, name, username in [
                ('Helium', 'Default', 'new-user'), ('Helium', 'Profile 1', 'account-user'),
                ('Brave Origin', 'Origin local', 'brave-user'),
                ('Brave Origin', 'Origin account', 'brave-account-user')]:
            identity = choose(browser, page, brand, name)
            gesture(page, 'document.querySelector("#import-passwords").click();true')
            wait_for(lambda: browser.ev('!!document.querySelector(".import-message .success")', page), browser.process, timeout=75)
            expected[username] = ADDED
            wait_for(lambda: matches(browser, expected), browser.process)
            imported.append(identity)
            check('Direct native import decrypts ' + brand + ' ' + name + ' and preserves the destination conflict', matches(browser, expected))
            request(browser.port, '/json/activate/' + page['id'])
        for name in ['Profile 2', 'Profile 3']:
            choose(browser, page, 'Helium', name)
            gesture(page, 'document.querySelector("#import-passwords").click();true')
            wait_for(lambda: browser.ev('!!document.querySelector(".import-message .error")', page), browser.process, timeout=75)
            check('Unavailable key/unsupported encrypted batch fails with no destination writes: ' + name, matches(browser, expected))
            request(browser.port, '/json/activate/' + page['id'])
        gesture(page, 'document.querySelector("#refresh-password-profiles").click();true')
        wait_for(lambda: browser.ev('document.querySelector(".panel").getAttribute("aria-busy")==="false"', page), browser.process)
        check('Refresh preserves completed password identities independently of bookmarks', {row['id'] for row in cards(browser, page) if row['imported']} == set(imported))
        check('Empty source profile cannot start password import', next(row['disabled'] for row in cards(browser, page) if row['brand'] == 'Helium' and row['profile'] == 'Profile 4'))
        inp = Input(browser.platform)
        try:
            request(browser.port, '/json/activate/' + page['id'])
            inp.capture(args.output / 'direct-password-results.png').close()
            inp.resize(700, 600)
            check('Password profile choices fit a narrow viewport', browser.ev('document.documentElement.scrollWidth<=innerWidth', page))
            inp.capture(args.output / 'direct-password-narrow.png').close()
            inp.resize(1200, 760)
        finally:
            inp.close()
        click(browser, page, '#continue', 'Keep your password manager.')
        click(browser, page, '#continue', 'You’re ready to browse.')
        check('Ready summary reports both completed categories', browser.ev('document.querySelector(".panel-content").innerText.includes("Password import finished.") && document.querySelector(".panel-content").innerText.includes("Bookmark import finished.")', page))
        gesture(page, 'document.querySelector("#continue").click();true')
        wait_for(lambda: browser.ev('location.href!=="chrome://setup/"', page), browser.process)
    with owned_browser(launcher, profile, args.output / 'restart', result, 'restart', normal_startup=True) as browser:
        check('Imported credentials persist after normal restart and existing password stays unchanged', matches(browser, expected))
        check('Completed onboarding does not reopen on restart', not any('setup' in row['url'] for row in request(browser.port, '/json/list')))
        settings = request(browser.port, '/json/new?chrome://settings/', 'PUT')
        wait_for(lambda: browser.ev('typeof chrome.settingsPrivate === "object"', settings), browser.process)
        check('Manual import leaves saving prompts disabled', browser.ev('new Promise(r=>chrome.settingsPrivate.getPref("credentials_enable_service",p=>r(p.value===false)))', settings))
        request(browser.port, '/json/close/' + settings['id'])
    after = {str(path.relative_to(config)): sha(path) for path in config.rglob('*') if path.is_file()}
    check('Every source file remains byte-identical and no source files/keys are created', before == after)
    if keyring:
        check('Real native v11 imports use and preserve distinct owned Chromium and Brave keys', keyring.unchanged())
        result['os_key_success_scope'] = 'real native v11 through the owned private Secret Service'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['native-import', 'system-key', 'system-key-preflight'],
                        default='native-import')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    args.rtc_policy_diagnostic = False
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    config = Path.home() / 'owned-browser-config'
    assert not config.exists()
    profile = args.output / 'destination-profile'
    system_case = args.case != 'native-import'
    result = {'passed': False, 'case': args.case, 'checks': [], 'home_isolated': True,
              'network_isolated_no_egress': True, 'source_passwords': 'synthetic fixtures only',
              'binary_sha256': args.expected_binary, 'resources_sha256': args.expected_resources,
              'fixture_sha256': sha(Path(__file__)), 'browser_launched': False,
              'os_key_success_scope': 'real native v10; successful v11 and read-only OS lookup covered by separate native unit tests'}
    if system_case:
        result['os_key_success_scope'] = 'owned keyring harness only; native v11 import not run'

    def check(name, condition):
        assert condition, name
        result['checks'].append(name)

    try:
        service = owned_secret_service(args, result) if system_case else nullcontext(None)
        with service as keyring:
            before = seed(config, keyring.secret if keyring else None,
                          keyring.brave_secret if keyring else None)
            os.environ['XDG_CONFIG_HOME'] = str(config)
            if args.case == 'system-key-preflight':
                check('Owned Secret Service returns both seeded synthetic Chromium and Brave keys unchanged', keyring.unchanged())
                check('In-process CBC agrees with independent legacy OpenSSL ciphertext',
                      cipher(SOURCE, b'v10', b'peanuts') == cipher(SOURCE))
                for name in ['Default/Login Data', 'Profile 1/Login Data For Account']:
                    path = config / 'net.imput.helium' / name
                    with sqlite3.connect(path.as_uri() + '?mode=ro&immutable=1', uri=True) as connection:
                        rows = connection.execute('SELECT password_value FROM logins').fetchall()
                    check('Owned source rows use v11 and the exact system-derived key: ' + name,
                          bool(rows) and all(row[0].startswith(b'v11') and aes_cbc(
                              row[0][3:], system_key(keyring.secret), decrypt=True) in
                              [SOURCE.encode(), ADDED.encode()] for row in rows))
                for name in ['Default/Login Data', 'Profile 1/Login Data For Account']:
                    path = config / 'BraveSoftware/Brave-Origin' / name
                    with sqlite3.connect(path.as_uri() + '?mode=ro&immutable=1', uri=True) as connection:
                        rows = connection.execute('SELECT password_value FROM logins').fetchall()
                    check('Owned Brave Origin rows use v11 and the distinct Brave key: ' + name,
                          bool(rows) and all(row[0].startswith(b'v11') and aes_cbc(
                              row[0][3:], system_key(keyring.brave_secret), decrypt=True) in
                              [SOURCE.encode(), ADDED.encode()] for row in rows))
                check('Owned Brave ciphertext cannot decrypt using the Chromium key', aes_cbc(
                    cipher(ADDED, b'v11', keyring.brave_secret)[3:],
                    system_key(keyring.secret), decrypt=True) is None)
                path = config / 'net.imput.helium/Profile 2/Login Data'
                with sqlite3.connect(path.as_uri() + '?mode=ro&immutable=1', uri=True) as connection:
                    encrypted = connection.execute('SELECT password_value FROM logins').fetchone()[0]
                check('Owned wrong-key source has a definite decryption failure',
                      aes_cbc(encrypted[3:], system_key(keyring.secret), decrypt=True) is None)
                check('Keyring preflight never starts a browser or creates a destination profile', not profile.exists())
                check('Keyring preflight leaves both source keys and item identities unchanged', keyring.unchanged())
                after = {str(path.relative_to(config)): sha(path) for path in config.rglob('*') if path.is_file()}
                check('Keyring preflight leaves all source files unchanged', before == after)
            else:
                # Destination selects basic storage even while the source uses
                # an owned Secret Service; no personal keyring is reachable.
                launcher = args.output / 'owned-basic-launcher'
                launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(args.browser)) + ' --password-store=basic "$@"\n')
                launcher.chmod(0o700)
                result['browser_launched'] = True
                native_import(args, launcher, profile, config, before, result, check, keyring)
            result['passed'] = True
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'case': args.case, 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
