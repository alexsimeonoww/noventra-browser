"""Real browser imports are staged until Finish; use only an owned HOME/display.

All credentials, bookmark URLs, history and keyring contents are synthetic.
Native requests/callbacks are observed unchanged, never substituted.
"""
import argparse
from contextlib import nullcontext
import json
import os
from pathlib import Path
import shlex

from extension_smoke import request, wait_for
from onboarding_import_smoke import gesture
from onboarding_managers_smoke import owned_browser, page_ready
import onboarding_native_browser_import_smoke as passwords
import onboarding_history_import_smoke as history
from portable_transfer_smoke import isolate, write


def check(result, name, value):
    assert value, name
    result['checks'].append(name)


def first_launch_setup(browser, result):
    # Normal startup creates the genuine first-run tab itself. Do not call the
    # reentry helper: opening another setup tab leaves the original unfinished
    # and makes session restore faithfully reopen that untouched duplicate.
    setups = wait_for(lambda: [page for page in request(browser.port, '/json/list')
                              if page['type'] == 'page' and page['url'] == 'chrome://setup/'],
                      browser.process)
    check(result, 'Fresh normal startup opens exactly one genuine setup tab', len(setups) == 1)
    page = setups[0]
    page_ready(browser, page)
    check(result, 'Genuine first-launch setup starts at Welcome',
          browser.ev('document.querySelector("#step-title").innerText', page) == 'Make yourself at home.')
    result['first_launch_setup'] = {'id': page['id'], 'url': page['url'],
                                    'created_by_fixture': False}
    request(browser.port, '/json/activate/' + page['id'])
    return page


def current_step(browser, page):
    return browser.ev('document.querySelector(".steps [aria-current=step] span:last-child")?.textContent??""', page).strip()


def click_next(browser, page):
    before = current_step(browser, page)
    gesture(page, 'document.querySelector("#continue").click();true')
    wait_for(lambda: current_step(browser, page) != before, browser.process)


def stage(browser, page):
    page_ready(browser, page)
    browser.ev(history.OBSERVE, page)
    click_next(browser, page)
    wait_for(lambda: any(p['brand'] == 'Brave Origin' and p['profile'] == 'Origin local'
                        for p in passwords.cards(browser, page)), browser.process)
    passwords.choose(browser, page, 'Brave Origin', 'Origin local')
    # The baseline's separate Passwords step is handled below. This lets the
    # red run reach the same Finish action rather than fail on changed markup.
    if browser.ev('!!document.querySelector("#import-data-passwords")', page):
        assert browser.ev('document.querySelector("#import-data-passwords").checked', page)
    return page


def go_ready(browser, page, choose_search=False):
    chosen_search_id = None
    for _ in range(5):
        if current_step(browser, page) == 'Ready':
            return chosen_search_id
        if current_step(browser, page) == 'Passwords':
            passwords.choose(browser, page, 'Brave Origin', 'Origin local')
        if current_step(browser, page) == 'Search engine' and choose_search:
            wait_for(lambda: browser.ev('!!document.querySelector("#setup-search-engine")&&'
                                        '!document.querySelector("#setup-search-engine").disabled', page), browser.process)
            chosen_search_id = browser.ev('(()=>{const choice=document.querySelector('
                '"#setup-search-engine input:not(:disabled):not(:checked)");'
                'return choice?Number(choice.value):null})()', page)
            assert chosen_search_id is not None, 'Search offers no alternative native-supported engine'
            gesture(page, 'document.querySelector("[data-search-id=\\"' + str(chosen_search_id) + '\\"] input").click();true')
            assert browser.ev('document.querySelector("[data-search-id=\\"' + str(chosen_search_id) + '\\"] input").checked', page)
        click_next(browser, page)
    raise AssertionError('Setup did not reach Ready')


def bookmark_count(browser, model):
    return browser.ev('new Promise(r=>chrome.bookmarks.search({url:' +
                      json.dumps(passwords.BRAVE_BOOKMARK) + '},x=>r(x.length)))', model)


def complete_pref(profile):
    path = profile / 'Default/Preferences'
    return json.loads(path.read_text()).get('helium', {}).get('completed_onboarding', False)


def default_search_id(browser):
    settings = request(browser.port, '/json/new?chrome://settings/search', 'PUT')
    try:
        wait_for(lambda: browser.ev('document.readyState==="complete"', settings), browser.process)
        return browser.ev('''(async()=>{const {sendWithPromise}=await import('chrome://resources/js/cr.js');
          const list=await sendWithPromise('getSearchEnginesList');
          return Object.values(list).flat().find(e=>e.default)?.id??null})()''', settings)
    finally:
        request(browser.port, '/json/close/' + settings['id'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=['finish-import', 'finish-system-key',
                                         'finish-failure', 'finish-skip', 'finish-defaults',
                                         'finish-policy'], default='finish-import')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.rtc_policy_diagnostic = False
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    assert passwords.sha(args.browser) == args.expected_binary
    assert passwords.sha(args.browser.parent / 'resources.pak') == args.expected_resources
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    result = {'passed': False, 'case': args.case, 'checks': [],
              'home_isolated': True, 'network_isolated_no_egress': True,
              'source_data': 'owned synthetic fixtures only',
              'binary_sha256': args.expected_binary,
              'resources_sha256': args.expected_resources,
              'fixture_sha256': passwords.sha(Path(__file__)),
              'native_callbacks': 'observed pass-through, never substituted'}
    config = Path.home() / 'owned-browser-config'
    profile = args.output / 'destination'
    chosen_search_id = None
    try:
        service = passwords.owned_secret_service(args, result) if args.case == 'finish-system-key' else nullcontext(None)
        with service as keyring:
            passwords.seed(config, keyring.secret if keyring else None,
                           keyring.brave_secret if keyring else None)
            if args.case == 'finish-failure':
                login = config / 'BraveSoftware/Brave-Origin/Default/Login Data'
                login.unlink()
                passwords.database(login,
                                   [('unsupported', 'https://owned-finish.invalid/', b'v20unsupported')])
            source_db = history.chrome_history(config / 'BraveSoftware/Brave-Origin/Default/History',
                                               [history.MAIN], history.WAL)
            try:
                before = {str(p.relative_to(config)): passwords.sha(p)
                          for p in config.rglob('*') if p.is_file()}
                os.environ['XDG_CONFIG_HOME'] = str(config)
                extension = history.controller_fixture(args.output)
                launcher = args.output / 'owned-finish-launcher'
                launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(args.browser)) +
                                    ' --password-store=basic --load-extension=' +
                                    shlex.quote(str(extension)) + ' "$@"\n')
                launcher.chmod(0o700)
                check(result, 'Destination starts without a seeded profile', not profile.exists())
                with owned_browser(launcher, profile, args.output / 'initial', result,
                                   'initial', normal_startup=True) as browser:
                    page = stage(browser, first_launch_setup(browser, result))
                    model = history.controller(browser)
                    check(result, 'Selecting a browser does not copy bookmarks or history',
                          bookmark_count(browser, model) == 0 and not history.history_urls(browser, model))
                    check(result, 'Selecting a browser does not copy passwords', passwords.matches(browser, {}))
                    request(browser.port, '/json/activate/' + page['id'])
                    initial_search_id = default_search_id(browser) if args.case == 'finish-defaults' else None
                    request(browser.port, '/json/activate/' + page['id'])
                    chosen_search_id = go_ready(browser, page, args.case == 'finish-defaults')
                    check(result, 'Continue stages all choices without native import requests',
                          browser.ev('ownedHistoryImport.imports.length===0', page))
                    check(result, 'Ready still has no imported destination data',
                          bookmark_count(browser, model) == 0 and not history.history_urls(browser, model)
                          and passwords.matches(browser, {}))
                    request(browser.port, '/json/activate/' + page['id'])
                    if args.case == 'finish-defaults':
                        check(result, 'Search stages an alternative native-supported engine without applying it before Finish',
                              chosen_search_id is not None and chosen_search_id != initial_search_id
                              and default_search_id(browser) == initial_search_id)
                        check(result, 'Ready summarizes the explicitly staged search choice',
                              browser.ev('document.querySelector("#search-engine-summary .summary-tag").innerText', page) == 'Selected')
                        request(browser.port, '/json/activate/' + page['id'])
                    if args.case == 'finish-skip':
                        gesture(page, 'document.querySelector("#skip-setup").click();true')
                        wait_for(lambda: browser.ev('location.href!=="chrome://setup/"', page), browser.process)
                        check(result, 'Skip completes setup without executing staged imports',
                              bookmark_count(browser, model) == 0 and not history.history_urls(browser, model)
                              and passwords.matches(browser, {}))
                    elif args.case == 'finish-policy':
                        settings = request(browser.port, '/json/new?chrome://settings/', 'PUT')
                        wait_for(lambda: browser.ev('typeof chrome.settingsPrivate==="object"', settings), browser.process)
                        assert browser.ev('new Promise(r=>chrome.settingsPrivate.setPref("credentials_enable_service",false,"",r))', settings)
                        request(browser.port, '/json/close/' + settings['id'])
                        with passwords.mandatory_password_policy(browser, result,
                                lambda name, value: check(result, name, value)):
                            request(browser.port, '/json/activate/' + page['id'])
                            gesture(page, 'document.querySelector("#continue").click();true')
                            wait_for(lambda: browser.ev('!!document.querySelector("#setup-import-error")&&!document.querySelector("#continue").disabled', page), browser.process, timeout=75)
                            check(result, 'Managed password policy rejects the final password job without finishing setup',
                                  browser.ev('location.href==="chrome://setup/"', page) and passwords.matches(browser, {}))
                            check(result, 'Managed password rejection preserves the already imported browser data',
                                  bookmark_count(browser, model) == 1 and
                                  {history.MAIN, history.WAL} <= set(history.history_urls(browser, model)))
                        request(browser.port, '/json/activate/' + page['id'])
                        gesture(page, 'document.querySelector("#continue").click();true')
                        wait_for(lambda: browser.ev('location.href!=="chrome://setup/"', page), browser.process, timeout=75)
                        expected = {'existing-user': passwords.SOURCE, 'brave-user': passwords.ADDED}
                        wait_for(lambda: passwords.matches(browser, expected), browser.process)
                        check(result, 'Retry after policy removal imports passwords and keeps the earlier bookmark unique',
                              passwords.matches(browser, expected) and bookmark_count(browser, model) == 1)
                    elif args.case == 'finish-failure':
                        gesture(page, 'document.querySelector("#continue").click();true')
                        wait_for(lambda: browser.ev('!!document.querySelector("#setup-import-error")', page), browser.process, timeout=75)
                        check(result, 'A failed password job leaves setup open with its error',
                              browser.ev('location.href==="chrome://setup/"', page))
                        check(result, 'Earlier successful bookmark/history job reaches the real models',
                              bookmark_count(browser, model) == 1 and
                              {history.MAIN, history.WAL} <= set(history.history_urls(browser, model)))
                        first = browser.ev('ownedHistoryImport.imports', page)
                        gesture(page, 'document.querySelector("#continue").click();true')
                        wait_for(lambda: browser.ev('ownedHistoryImport.imports.length', page) > len(first), browser.process)
                        wait_for(lambda: browser.ev('!!document.querySelector("#setup-import-error")&&!document.querySelector("#continue").disabled', page), browser.process, timeout=75)
                        calls = browser.ev('ownedHistoryImport.imports', page)
                        check(result, 'Retry does not repeat completed bookmark/history jobs',
                              sum(x[1]['import_dialog_bookmarks'] or x[1]['import_dialog_history'] for x in calls) == 1
                              and bookmark_count(browser, model) == 1)
                        check(result, 'Unsupported passwords never create destination credentials', passwords.matches(browser, {}))
                        result['native_import_requests'] = calls
                    else:
                        # A rapid repeat activation must not queue a second batch.
                        gesture(page, 'document.querySelector("#continue").click();document.querySelector("#continue")?.click();true')
                        wait_for(lambda: browser.ev('location.href!=="chrome://setup/"', page), browser.process, timeout=75)
                        wait_for(lambda: bookmark_count(browser, model) == 1, browser.process, timeout=10)
                        check(result, 'Finish imports the selected Brave bookmark into the real model', bookmark_count(browser, model) == 1)
                        wait_for(lambda: {history.MAIN, history.WAL} <= set(history.history_urls(browser, model)), browser.process)
                        check(result, 'Finish imports recent Brave WAL history into the real model',
                              {history.MAIN, history.WAL} <= set(history.history_urls(browser, model)))
                        expected = {'existing-user': passwords.SOURCE, 'brave-user': passwords.ADDED}
                        wait_for(lambda: passwords.matches(browser, expected), browser.process)
                        check(result, 'Finish imports selected Brave passwords through the native reader', passwords.matches(browser, expected))
                        if chosen_search_id is not None:
                            check(result, 'Explicit search choice is applied through the actual native handler',
                                  default_search_id(browser) == chosen_search_id)
                check(result, 'Completion preference matches the actual batch outcome',
                      complete_pref(profile) == (args.case != 'finish-failure'))
                with owned_browser(launcher, profile, args.output / 'restart', result,
                                   'restart', normal_startup=True) as browser:
                    model = history.controller(browser)
                    if args.case not in ('finish-skip', 'finish-failure'):
                        check(result, 'All selected data persists after normal restart',
                              bookmark_count(browser, model) == 1 and
                              {history.MAIN, history.WAL} <= set(history.history_urls(browser, model)) and
                              passwords.matches(browser, {'existing-user': passwords.SOURCE,
                                                          'brave-user': passwords.ADDED}))
                    check(result, 'Setup reappears only while the queued import is incomplete',
                          any('setup' in p['url'] for p in request(browser.port, '/json/list')) == (args.case == 'finish-failure'))
                    if chosen_search_id is not None:
                        check(result, 'The explicitly chosen search engine persists after restart',
                              default_search_id(browser) == chosen_search_id)
                check(result, 'Source files and sidecars remain unchanged',
                      {str(p.relative_to(config)): passwords.sha(p) for p in config.rglob('*') if p.is_file()} == before)
                if keyring:
                    check(result, 'Existing synthetic Chromium and Brave source keys are unchanged', keyring.unchanged())
                result['passed'] = True
            finally:
                source_db.close()
    finally:
        write(args.output / 'result.json', result)
    print(json.dumps({'passed': result['passed'], 'case': args.case, 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
