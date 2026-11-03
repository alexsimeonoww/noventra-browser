"""Import owned bookmark/history fixtures through the actual native browser stack.

Run through the private Xvfb wrapper. isolate() hides the real HOME and network;
all source databases, visits, extension code and profiles here are synthetic.
Native callbacks are observed without substituting a request or response.

native-history is a narrow backend regression usable on the old build: it sends
an actual native HISTORY request and requires both main-DB and WAL-only rows.
ui-history requires the new data checkboxes and covers combined/history-only
imports, Chrome/Firefox WALs, failures, partial-category reporting and restart.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import shlex
import sqlite3
import time

from dock_rail_regressions_smoke import Input

from extension_smoke import request, wait_for
from onboarding_import_smoke import gesture
from onboarding_managers_smoke import owned_browser, page_ready, setup_page
from portable_transfer_smoke import isolate

BASE = 'https://owned-history-import.invalid/'
MAIN = BASE + 'chrome-main'
WAL = BASE + 'chrome-wal'
BOOKMARK = BASE + 'chrome-bookmark'
EXISTING = BASE + 'existing'
EXISTING_BOOKMARK = BASE + 'existing-bookmark'
FIREFOX_MAIN = BASE + 'firefox-main'
FIREFOX_WAL = BASE + 'firefox-wal'
FIREFOX_BOOKMARK = BASE + 'firefox-bookmark'
ONLY_HISTORY = BASE + 'history-only'
PARTIAL_HISTORY = BASE + 'partial-history'
BAD_QUERY_BOOKMARK = BASE + 'must-not-copy-query-failure'
BAD_DATABASE_BOOKMARK = BASE + 'must-not-copy-corrupt-failure'
WINDOWS_EPOCH_VISIT = int((time.time() + 11644473600) * 1000000)
UNIX_VISIT = int(time.time() * 1000000)

OBSERVE = '''(()=>{
 const state=window.ownedHistoryImport={profiles:[],discoveryIds:[],imports:[],statuses:[]};
 const send=chrome.send.bind(chrome),reply=window.cr.webUIResponse,
       event=window.cr.webUIListenerCallback;
 chrome.send=(name,args=[])=>{
   if(name==='initializeImportDialog')state.discoveryIds.push(args[0]);
   if(name==='importData')state.imports.push(args);
   return send(name,args);
 };
 window.cr.webUIResponse=(id,success,value)=>{
   if(state.discoveryIds.includes(id)){state.profiles=success?value:[];state.discoveryError=!success}
   return reply(id,success,value);
 };
 window.cr.webUIListenerCallback=(name,...args)=>{
   if(name==='import-data-status-changed')state.statuses.push(args[0]);
   return event(name,...args);
 };
 return true;
})()'''


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_bytes(config, firefox):
    return {str(p): sha(p) for root in [config, firefox] for p in root.rglob('*') if p.is_file()}


def bookmarks(path, url):
    path.write_text(json.dumps({'version': 1, 'roots': {
        'bookmark_bar': {'type': 'folder', 'name': 'Bookmarks bar', 'children': [
            {'type': 'url', 'name': 'Owned history fixture bookmark', 'url': url,
             'date_added': str(WINDOWS_EPOCH_VISIT)}]},
        'other': {'type': 'folder', 'name': 'Other bookmarks', 'children': []}}}))


def chrome_history(path, urls, wal_url=None):
    db = sqlite3.connect(path)
    db.execute('PRAGMA journal_mode=WAL')
    db.execute('PRAGMA wal_autocheckpoint=0')
    db.executescript('''CREATE TABLE urls (id INTEGER PRIMARY KEY,url TEXT,title TEXT,
        hidden INTEGER,typed_count INTEGER,visit_count INTEGER);
      CREATE TABLE visits (id INTEGER PRIMARY KEY,url INTEGER,visit_time INTEGER,
        transition INTEGER);''')

    def add(url):
        identity = db.execute('SELECT COUNT(*)+1 FROM urls').fetchone()[0]
        db.execute('INSERT INTO urls VALUES (?,?,?,0,1,1)',
                   (identity, url, 'Owned imported visit'))
        db.execute('INSERT INTO visits VALUES (?,?,?,?)',
                   (identity, identity, WINDOWS_EPOCH_VISIT, 0x20000001))
        db.commit()

    for url in urls:
        add(url)
    db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
    if wal_url:
        add(wal_url)
        assert Path(str(path) + '-wal').stat().st_size > 0
    return db


def firefox_places(path):
    db = sqlite3.connect(path)
    db.execute('PRAGMA journal_mode=WAL')
    db.execute('PRAGMA wal_autocheckpoint=0')
    db.executescript('''CREATE TABLE moz_places (id INTEGER PRIMARY KEY,url TEXT,
        title TEXT,visit_count INTEGER,hidden INTEGER,typed INTEGER,favicon_id INTEGER);
      CREATE TABLE moz_historyvisits (id INTEGER PRIMARY KEY,place_id INTEGER,
        visit_date INTEGER,visit_type INTEGER);
      CREATE TABLE moz_bookmarks (id INTEGER PRIMARY KEY,type INTEGER,parent INTEGER,
        position INTEGER,fk INTEGER,title TEXT,dateAdded INTEGER,keyword_id INTEGER,guid TEXT);
      CREATE TABLE moz_keywords (id INTEGER PRIMARY KEY,keyword TEXT);
      CREATE TABLE moz_items_annos (id INTEGER PRIMARY KEY,item_id INTEGER,anno_attribute_id INTEGER);
      CREATE TABLE moz_anno_attributes (id INTEGER PRIMARY KEY,name TEXT);
      CREATE TABLE moz_favicons (id INTEGER PRIMARY KEY,url TEXT,data BLOB);''')
    for identity, guid, name in [(1, 'menu________', 'Bookmarks menu'),
                                  (2, 'toolbar_____', 'Bookmarks toolbar'),
                                  (3, 'unfiled_____', 'Other bookmarks')]:
        db.execute('INSERT INTO moz_bookmarks VALUES (?,2,0,?,NULL,?,?,NULL,?)',
                   (identity, identity, name, WINDOWS_EPOCH_VISIT, guid))
    db.execute('INSERT INTO moz_places VALUES (1,?,?,1,0,1,NULL)',
               (FIREFOX_MAIN, 'Owned Firefox main visit'))
    db.execute('INSERT INTO moz_historyvisits VALUES (1,1,?,2)',
               (UNIX_VISIT,))
    db.execute('INSERT INTO moz_places VALUES (3,?,?,0,0,0,NULL)',
               (FIREFOX_BOOKMARK, 'Owned Firefox bookmark'))
    db.execute('INSERT INTO moz_bookmarks VALUES (4,1,2,0,3,?,?,NULL,?)',
               ('Owned Firefox bookmark', WINDOWS_EPOCH_VISIT, 'owned-bookmark'))
    db.commit()
    db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
    db.execute('INSERT INTO moz_places VALUES (2,?,?,1,0,1,NULL)',
               (FIREFOX_WAL, 'Owned Firefox WAL visit'))
    db.execute('INSERT INTO moz_historyvisits VALUES (2,2,?,2)',
               (UNIX_VISIT + 1000000,))
    db.commit()
    assert Path(str(path) + '-wal').stat().st_size > 0
    return db


def seed(config, firefox, stack):
    chromium = config / 'chromium'
    names = [('Default', 'Owned combined'), ('Profile 1', 'Owned invalid query'),
             ('Profile 2', 'Owned corrupt database'), ('Profile 3', 'Owned partial import'),
             ('Profile 4', 'Owned history only')]
    cache = {}
    for directory, label in names:
        profile = chromium / directory
        profile.mkdir(parents=True)
        (profile / 'Preferences').write_text('{}\n')
        cache[directory] = {'name': label}
    (chromium / 'Local State').write_text(json.dumps({'profile': {'info_cache': cache}}))
    bookmarks(chromium / 'Default/Bookmarks', BOOKMARK)
    db = chrome_history(chromium / 'Default/History', [MAIN], WAL)
    # Keep the live WAL until source-preservation checks and restart finish.
    stack.callback(db.close)
    db = sqlite3.connect(chromium / 'Profile 1/History')
    try:
        db.execute('CREATE TABLE urls (id INTEGER PRIMARY KEY,url TEXT)')
        db.execute('INSERT INTO urls VALUES (1,?)', (BASE + 'invalid-query',))
        db.commit()
    finally:
        db.close()
    bookmarks(chromium / 'Profile 1/Bookmarks', BAD_QUERY_BOOKMARK)
    (chromium / 'Profile 2/History').write_bytes(b'Owned malformed SQLite database\n' * 256)
    bookmarks(chromium / 'Profile 2/Bookmarks', BAD_DATABASE_BOOKMARK)
    db = chrome_history(chromium / 'Profile 3/History', [PARTIAL_HISTORY])
    stack.callback(db.close)
    (chromium / 'Profile 3/Bookmarks').write_text('{owned invalid bookmarks')
    db = chrome_history(chromium / 'Profile 4/History', [ONLY_HISTORY])
    stack.callback(db.close)
    firefox.mkdir(parents=True)
    profile = firefox / 'Profiles/owned-history'
    profile.mkdir(parents=True)
    # Firefox leaves this empty lock file in an existing profile. Chromium's
    # inherited import guard acquires it before reading any Firefox data.
    (profile / '.parentlock').touch()
    app = firefox / 'owned-application'
    app.mkdir()
    (app / 'application.ini').write_text('[App]\nName=Firefox\n')
    (profile / 'compatibility.ini').write_text(
        '[Compatibility]\nLastVersion=156.0_20261101000000\nLastAppDir=' + str(app) + '\n')
    (profile / 'prefs.js').write_text('user_pref("browser.startup.homepage", "about:blank");\n')
    (firefox / 'profiles.ini').write_text('[Profile0]\nName=Owned Firefox history\nIsRelative=1\nPath=Profiles/owned-history\nDefault=1\n')
    db = firefox_places(profile / 'places.sqlite')
    stack.callback(db.close)


def controller_fixture(output):
    root = output / 'history-controller'
    root.mkdir()
    (root / 'manifest.json').write_text(json.dumps({
        'manifest_version': 3, 'name': 'Owned history import model witness',
        'version': '1.0', 'permissions': ['history', 'bookmarks'],
        'background': {'service_worker': 'history-worker.js'}}))
    (root / 'history-worker.js').write_text('chrome.runtime.onInstalled.addListener(()=>{});')
    (root / 'control.html').write_text('<title>Owned history model witness</title>')
    return root


def controller(browser):
    worker = wait_for(lambda: next((t for t in request(browser.port, '/json/list')
        if t['type'] == 'service_worker' and t['url'].endswith('/history-worker.js')), None), browser.process)
    page = request(browser.port, '/json/new?' + worker['url'].rsplit('/', 1)[0] + '/control.html', 'PUT')
    wait_for(lambda: browser.ev('typeof chrome.history?.search === "function" && typeof chrome.bookmarks?.search === "function"', page), browser.process)
    return page


def history_urls(browser, model):
    return browser.ev('''(async()=>{const rows=await chrome.history.search({text:BASE,startTime:0,maxResults:1000});
      return [...new Set(rows.map(r=>r.url))].sort()})()'''.replace('BASE', json.dumps(BASE)), model)


def bookmark_urls(browser, model):
    return browser.ev('''(async()=>{const rows=await chrome.bookmarks.search({query:BASE});
      return rows.map(r=>r.url).filter(Boolean).sort()})()'''.replace('BASE', json.dumps(BASE)), model)


def setup_import_page(browser):
    page = setup_page(browser)
    page_ready(browser, page)
    browser.ev(OBSERVE, page)
    gesture(page, 'document.querySelector("#continue").click();true')
    wait_for(lambda: browser.ev('!!document.querySelector("#bookmark-file")', page), browser.process)
    if browser.ev('!!document.querySelector("#find-import-profiles")', page):
        gesture(page, 'document.querySelector("#find-import-profiles").click();true')
    wait_for(lambda: browser.ev('ownedHistoryImport.profiles.some(p=>p.name==="Chromium"&&p.profileName==="Owned combined")', page), browser.process)
    wait_for(lambda: browser.ev('document.querySelector(".panel").getAttribute("aria-busy")==="false"', page), browser.process)
    return page


def source(browser, page, name, profile):
    return browser.ev('ownedHistoryImport.profiles.find(p=>p.name===' + json.dumps(name) + '&&p.profileName===' + json.dumps(profile) + ')??null', page)


def choose(browser, page, profile):
    assert profile and (profile['favorites'] or profile['history'])
    request(browser.port, '/json/activate/' + page['id'])
    gesture(page, 'document.querySelector(".profile-card input[value=\\"' + str(profile['id']) + '\\"]").click();true')
    wait_for(lambda: browser.ev('document.querySelector(".profile-card input[value=\\"' + str(profile['id']) + '\\"]").checked', page), browser.process)


def begin_ui_import(browser, page):
    previous = browser.ev('ownedHistoryImport.statuses.length', page)
    gesture(page, 'document.querySelector("#import-bookmarks").click();true')
    wait_for(lambda: browser.ev('ownedHistoryImport.statuses.slice(' + str(previous) + ').some(s=>s==="succeeded"||s==="failed")', page), browser.process, timeout=75)
    return browser.ev('ownedHistoryImport.statuses.at(-1)', page)


def capture(browser, output):
    owned_input = Input(browser.platform)
    try:
        owned_input.capture(output).close()
    finally:
        owned_input.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['native-history', 'ui-history'], default='ui-history')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    args.rtc_policy_diagnostic = False
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    config, firefox = Path.home() / 'owned-history-config', Path.home() / '.mozilla/firefox'
    profile = args.output / 'destination'
    result = {'passed': False, 'case': args.case, 'checks': [], 'home_isolated': True,
              'network_isolated_no_egress': True, 'source_data': 'owned synthetic fixtures only',
              'binary_sha256': args.expected_binary, 'resources_sha256': args.expected_resources,
              'fixture_sha256': sha(Path(__file__)), 'native_callbacks': 'observed pass-through, never substituted'}

    def check(name, value):
        assert value, name
        result['checks'].append(name)

    try:
        with ExitStack() as stack:
            seed(config, firefox, stack)
            before = source_bytes(config, firefox)
            (args.output / 'source-before.json').write_text(json.dumps(before, indent=2) + '\n')
            os.environ['XDG_CONFIG_HOME'] = str(config)
            extension = controller_fixture(args.output)
            launcher = args.output / 'owned-history-launcher'
            launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(args.browser)) + ' --password-store=basic --load-extension=' + shlex.quote(str(extension)) + ' "$@"\n')
            launcher.chmod(0o700)
            try:
                with owned_browser(launcher, profile, args.output / 'initial', result, 'initial', normal_startup=True) as browser:
                    assert Path('/proc', str(browser.process.pid), 'exe').resolve() == args.browser
                    page = setup_import_page(browser)
                    result['detected_profiles'] = browser.ev('ownedHistoryImport.profiles', page)
                    model = controller(browser)
                    browser.ev('(async()=>{await chrome.history.addUrl({url:' + json.dumps(EXISTING) + '});await chrome.bookmarks.create({title:"Owned existing bookmark",url:' + json.dumps(EXISTING_BOOKMARK) + '});return true})()', model)
                    wait_for(lambda: EXISTING in history_urls(browser, model), browser.process)
                    combined = source(browser, page, 'Chromium', 'Owned combined')
                    check('Actual native detector advertises both bookmark and history capabilities', combined and combined['favorites'] and combined['history'])
                    if args.case == 'native-history':
                        previous = browser.ev('ownedHistoryImport.statuses.length', page)
                        types = {'import_dialog_bookmarks': False, 'import_dialog_history': True,
                                 'import_dialog_saved_passwords': False, 'import_dialog_search_engine': False,
                                 'import_dialog_autofill_form_data': False, 'import_dialog_extensions': False}
                        gesture(page, 'chrome.send("importData",[' + str(combined['index']) + ',' + json.dumps(types) + ']);true')
                        wait_for(lambda: browser.ev('ownedHistoryImport.statuses.slice(' + str(previous) + ').some(s=>s==="succeeded"||s==="failed")', page), browser.process, timeout=75)
                        check('Native history request reports successful completion', browser.ev('ownedHistoryImport.statuses.at(-1)==="succeeded"', page))
                        wait_for(lambda: MAIN in history_urls(browser, model), browser.process)
                        check('Native history import includes the committed WAL-only visit', WAL in history_urls(browser, model))
                    else:
                        choose(browser, page, combined)
                        check('Browser-data UI defaults to the two native-supported categories', browser.ev('document.querySelector("#import-data-bookmarks").checked&&document.querySelector("#import-data-history").checked', page))
                        capture(browser, args.output / 'browser-data-combined.png')
                        check('Combined bookmark/history import completes through actual native APIs', begin_ui_import(browser, page) == 'succeeded')
                        wait_for(lambda: {MAIN, WAL, EXISTING} <= set(history_urls(browser, model)), browser.process)
                        wait_for(lambda: {BOOKMARK, EXISTING_BOOKMARK} <= set(bookmark_urls(browser, model)), browser.process)
                        check('Actual destination model includes Chrome main/WAL visits and bookmarks', {MAIN, WAL, EXISTING} <= set(history_urls(browser, model)) and {BOOKMARK, EXISTING_BOOKMARK} <= set(bookmark_urls(browser, model)))
                        only = source(browser, page, 'Chromium', 'Owned history only')
                        choose(browser, page, only)
                        check('History-only source is selectable without offering nonexistent bookmarks', browser.ev('document.querySelector("#import-data-bookmarks").disabled&&!document.querySelector("#import-data-bookmarks").checked&&document.querySelector("#import-data-history").checked', page))
                        check('History-only native import finishes', begin_ui_import(browser, page) == 'succeeded')
                        wait_for(lambda: ONLY_HISTORY in history_urls(browser, model), browser.process)
                        # The native Firefox picker omits a redundant profile
                        # label when this is its only detected Firefox profile.
                        firefox_source = source(browser, page, 'Mozilla Firefox', '')
                        check('Exactly one owned Firefox source was detected',
                              sum(p['name'] == 'Mozilla Firefox' for p in result['detected_profiles']) == 1)
                        check('Owned Firefox profile exposes its real native categories', firefox_source and firefox_source['history'] and firefox_source['favorites'])
                        choose(browser, page, firefox_source)
                        check('Firefox bookmark/history native import completes', begin_ui_import(browser, page) == 'succeeded')
                        wait_for(lambda: {FIREFOX_MAIN, FIREFOX_WAL} <= set(history_urls(browser, model)), browser.process)
                        wait_for(lambda: FIREFOX_BOOKMARK in bookmark_urls(browser, model), browser.process)
                        check('Firefox committed WAL history and bookmarks reach actual models', {FIREFOX_MAIN, FIREFOX_WAL} <= set(history_urls(browser, model)) and FIREFOX_BOOKMARK in bookmark_urls(browser, model))
                        good_history = history_urls(browser, model)
                        good_bookmarks = bookmark_urls(browser, model)
                        for label in ['Owned invalid query', 'Owned corrupt database']:
                            choose(browser, page, source(browser, page, 'Chromium', label))
                            check(label + ' fails instead of claiming successful import', begin_ui_import(browser, page) == 'failed')
                            check(label + ' writes no partial history or other selected categories', history_urls(browser, model) == good_history and bookmark_urls(browser, model) == good_bookmarks)
                        choose(browser, page, source(browser, page, 'Chromium', 'Owned partial import'))
                        check('Malformed bookmarks fail even after the selected history was copied', begin_ui_import(browser, page) == 'failed')
                        wait_for(lambda: PARTIAL_HISTORY in history_urls(browser, model), browser.process)
                        check('Partial-category failure does not claim all data was copied', browser.ev('!!document.querySelector(".import-message .error")&&document.querySelector(".import-message").innerText.includes("Some data may already")', page))
                        for selector in ['#continue', '#continue', '#continue']:
                            gesture(page, 'document.querySelector(' + json.dumps(selector) + ').click();true')
                            wait_for(lambda: browser.ev('document.querySelector(".panel").getAttribute("aria-busy")==="false"', page), browser.process)
                        check('Ready names incomplete browser data truthfully', browser.ev('document.querySelector("#data-import-summary").innerText.includes("Check needed")', page))
                        capture(browser, args.output / 'browser-data-partial-summary.png')
                        gesture(page, 'document.querySelector("#continue").click();true')
                        wait_for(lambda: browser.ev('location.href!=="chrome://setup/"', page), browser.process)
                    expected_history, expected_bookmarks = history_urls(browser, model), bookmark_urls(browser, model)
                with owned_browser(launcher, profile, args.output / 'restart', result, 'restart', normal_startup=True) as browser:
                    model = controller(browser)
                    check('Actual history and bookmarks persist after normal browser restart', history_urls(browser, model) == expected_history and bookmark_urls(browser, model) == expected_bookmarks)
                    if args.case == 'ui-history':
                        check('Completed optional setup does not reopen on restart', not any('setup' in p['url'] for p in request(browser.port, '/json/list')))
                check('Source databases and sidecars remain byte-identical with no new source files', source_bytes(config, firefox) == before)
                result['passed'] = True
            finally:
                after = source_bytes(config, firefox)
                (args.output / 'source-after.json').write_text(json.dumps(after, indent=2) + '\n')
                result['source_files_unchanged'] = after == before
                result['source_byte_changes'] = [
                    {'path': path, 'before': before.get(path), 'after': after.get(path)}
                    for path in sorted(before.keys() | after.keys())
                    if before.get(path) != after.get(path)]
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'case': args.case, 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
