"""Apply encrypted workspace changes from an actual Work Profile.

Use the authenticated private Xvfb and private keyring wrappers with an exact
runtime hash. Two owned browser processes share an opaque loopback store. Real
Settings APIs create/join/sync; no worker callback or disk write is mocked.
All credentials, profiles and servers belong to this disposable fixture.
"""

from copy import deepcopy
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import traceback

import incognito_space_smoke as private
import pinned_tabs_smoke as p
import profile_spaces_smoke as profiles
import workspace_sync_smoke as sync


A_WORK = '88000000-0000-4000-8000-000000000001'
B_WORK = '88000000-0000-4000-8000-000000000002'
CATALOG_KEYS = ('noventra.browser.account_spaces',
                'noventra.browser.account_space_order',
                'noventra.browser.archived_account_spaces')
LAYOUT_KEY = 'noventra.browser.dock_layout'


class Budget(sync.Budget):
    def __init__(self):
        super().__init__()
        signal.setitimer(signal.ITIMER_REAL, 300)


def dotted(value, key):
    for part in key.split('.'):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value


def preferences(path):
    return json.loads((path / 'Preferences').read_text())


def seed(profile, identifier, name):
    p.prefs(profile, width=200)
    path = profile / 'Default/Preferences'
    value = json.loads(path.read_text())
    value['noventra'] = {'browser': {
        'account_spaces': {'': 'Personal', identifier: name},
        'account_space_order': ['', identifier],
        'archived_account_spaces': []}}
    path.write_text(json.dumps(value))


def work_directory(browser):
    state = json.loads((browser.profile / 'Local State').read_text())
    binding = state['noventra']['profile_spaces']['groups']['Default']['spaces'][B_WORK]
    directory = browser.profile / binding['directory']
    assert directory.resolve().parent == browser.profile.resolve()
    assert directory.is_dir() and not directory.is_symlink()
    return directory


def bind_work(browser, result):
    browser.call('noventraSpaces', 'switch', B_WORK, '')
    seen = set()

    def exact_profile():
        for page in p.pages(browser):
            context = profiles.context(browser, page)
            if context in seen or not page['url'].startswith(
                    ('chrome://newtab', 'chrome://new-tab-page', 'about:blank',
                     'chrome://settings/appearance')):
                continue
            seen.add(context)
            browser.protocol('Page.navigate', {'url': 'chrome://version/'}, page)
            actual_path = p.wait_for(lambda: browser.ev(
                'document.querySelector("#profile_path")?.innerText||""', page),
                browser.process, timeout=15)
            try:
                expected = work_directory(browser)
            except (KeyError, FileNotFoundError):
                continue
            if Path(actual_path).resolve() == expected.resolve():
                browser.child_directory = expected
                return page, actual_path

    page, actual_path = p.wait_for(exact_profile, browser.process, timeout=25)
    p.check(result, 'Work Settings belongs to its exact native Profile directory',
            Path(actual_path).resolve() == browser.child_directory.resolve(),
            {'actual': Path(actual_path).name, 'expected': browser.child_directory.name})
    private.goto(browser, page, 'chrome://settings/appearance')
    browser.settings = page
    p.wait_for(lambda: browser.ev('!!document.querySelector("settings-ui")?.shadowRoot'),
               browser.process, timeout=15)


def completed(browser, action, payload, result, label):
    sync.wait(lambda: not sync.status(browser)['syncing'], timeout=30,
              description=label + ' idle service')
    reply = browser.rpc('noventraWorkspaceSync', action, payload)
    if reply.get('status') == 'resolved':
        key = reply.get('value', {}).get('recovery_key')
        if key:
            browser.redactor.add(key)
    result.setdefault('operations', []).append({'label': label,
        'reply': browser.redactor.safe(reply)})
    p.check(result, label + ' native callback resolves', reply.get('status') == 'resolved',
            browser.redactor.safe(reply))
    status = reply['value']
    p.check(result, label + ' completes without pending/error/conflict state',
            status['configured'] and not status['syncing'] and not status['dirty'] and not status['conflict']
            and not status['error'] and status['state'] == 'synced'
            and bool(status['last_success']), browser.redactor.safe(status))
    return status


def expect_disk_ownership(browser, expected_names, expected_layout, result, label):
    root = preferences(browser.profile / 'Default')
    child = preferences(browser.child_directory)
    names = dotted(root, CATALOG_KEYS[0])
    p.check(result, label + ' catalog rename is durable in Personal',
            all(names.get(identifier) == name for identifier, name in expected_names.items()), names)
    p.check(result, label + ' catalog order/archive preferences exist in Personal',
            all(dotted(root, key) is not None for key in CATALOG_KEYS[1:]))
    layout = dotted(child, LAYOUT_KEY)
    p.check(result, label + ' selected Work layout is durable in Work',
            layout is not None and sync.portable(layout) == sync.portable(expected_layout))
    p.check(result, label + ' Work does not duplicate the Personal catalog',
            dotted(child, CATALOG_KEYS[0]) != names)
    state = browser.child_directory / 'Noventra Workspace Sync'
    p.check(result, label + ' encrypted service state belongs to Work', state.is_file())
    p.check(result, label + ' Personal has no workspace-sync enrollment',
            not (browser.profile / 'Default/Noventra Workspace Sync').exists())


def run(args, result, redactor, browsers, servers):
    clients = {}
    for label, identifier, name in (('A', A_WORK, 'Remote Work'),
                                     ('B', B_WORK, 'Local Work')):
        profile = args.output / 'private' / ('profile-' + label)
        seed(profile, identifier, name)
        browser = sync.OwnedBrowser(args.browser, profile,
            args.output / 'private' / ('initial-' + label), redactor, label)
        browsers.append(browser)
        browser.__enter__()
        clients[label] = browser
    a, b = clients['A'], clients['B']
    p.check(result, 'Clients have independent processes and user-data directories',
            a.process.pid != b.process.pid and a.profile != b.profile)
    bind_work(b, result)
    order = (*p.native.TOP_ORDER, 'spaces')
    sync.saved_layout(a, order)
    sync.saved_layout(b, ('new-tab', *[item for item in order if item != 'new-tab']))
    p.check(result, 'Join must change the selected Work layout',
            sync.portable(a.state()['layout']) != sync.portable(b.state()['layout']))
    created = completed(a, 'create', {'origin': servers.origin('A'),
        'token': args.test_token}, result, 'Create remote vault')
    key = created['recovery_key']
    redactor.add(key)
    joined = completed(b, 'join', {'origin': servers.origin('B'),
        'token': args.test_token, 'vault_id': created['vault_id'],
        'recovery_key': key}, result, 'Join from Work')
    p.check(result, 'Work joined the same authenticated vault',
            joined['vault_id'] == created['vault_id'])
    remote = next(space for space in sync.spaces(b)
                  if space['name'] == 'Remote Work')
    expected_names = {B_WORK: 'Local Work', remote['id']: 'Remote Work'}
    expect_disk_ownership(b, expected_names, a.state()['layout'], result, 'Initial apply')
    sync.spaces(a, 'rename', A_WORK, 'Remote Work Renamed')
    sync.saved_layout(a, ('tabs', *[item for item in order if item != 'tabs']))
    uploaded = completed(a, 'sync', {}, result, 'Upload catalog and layout edits')
    completed(b, 'sync', {}, result, 'Apply catalog and layout edits from Work')
    sync.wait(lambda: (state if (state := sync.healthy(b))
                      and state['generation'] >= uploaded['generation'] else None),
              timeout=35, description='Work accepted edited remote generation')
    expected_names[remote['id']] = 'Remote Work Renamed'
    expected_layout = deepcopy(a.state()['layout'])
    expect_disk_ownership(b, expected_names, expected_layout, result, 'Updated apply')
    p.check(result, 'Work sees the remote renamed Space through the real catalog API',
            any(space['id'] == remote['id'] and space['name'] == 'Remote Work Renamed'
                for space in sync.spaces(b)))
    p.check(result, 'First Work browser exits normally', b.normal_quit() == 0)
    b.__exit__()
    restarted = sync.OwnedBrowser(args.browser, b.profile,
        args.output / 'private/restarted-B', redactor, 'B-restarted', restore=True)
    browsers.append(restarted)
    restarted.__enter__()
    bind_work(restarted, result)
    def restored_status():
        value = sync.status(restarted)
        samples = result.setdefault('restart_status_samples', [])
        if not samples or samples[-1] != value:
            samples.append(redactor.safe(value))
        return value if value['configured'] and not value['syncing'] and not value['dirty'] \
            and not value['error'] and not value['conflict'] and value['last_success'] else None

    sync.wait(restored_status, timeout=45,
              description='Work native sync configuration after restart')
    expect_disk_ownership(restarted, expected_names, expected_layout, result, 'Restart')
    completed(restarted, 'sync', {}, result, 'Sync from Work after restart')
    p.check(result, 'Restarted Work browser exits normally', restarted.normal_quit() == 0)
    p.check(result, 'Remote browser exits normally', a.normal_quit() == 0)


def main():
    args = p.cli(__doc__).parse_args()
    old_umask = os.umask(0o077)
    result = p.admit(args, Path(__file__))
    result['qualification'] = 'Two local processes with an owned private keyring and opaque loopback store; no physical-device claim'
    redactor, browsers, servers = sync.Redactor(), [], None
    budget = Budget()
    try:
        result['private_keyring'] = sync.attest_private_keyring()
        (args.output / 'private').mkdir(mode=0o700)
        for module in (sync, profiles, p, private):
            path = Path(module.__file__)
            shutil.copyfile(path, args.output / ('executed-' + path.name))
            result.setdefault('helper_sources', {})[str(path)] = p.sha(path)
        args.test_token = redactor.add(secrets.token_urlsafe(48))
        servers = sync.OpaqueServers(args.output / 'private/server', args.test_token)
        run(args, result, redactor, browsers, servers)
        result['passed'] = True
    except BaseException as error:
        result['error'] = redactor.text(repr(error))
        result['traceback'] = redactor.text(traceback.format_exc())
    finally:
        budget.cleanup()
        result['native_api_calls'] = [browser.calls for browser in browsers]
        cleanup_errors = []
        for browser in reversed(browsers):
            try:
                browser.__exit__()
            except BaseException as error:
                cleanup_errors.append(redactor.text(repr(error)))
        if servers:
            result['server_requests'] = {label: gate.events for label, gate in servers.gates.items()}
            try:
                servers.close()
            except BaseException as error:
                cleanup_errors.append(redactor.text(repr(error)))
        result['cleanup_errors'] = cleanup_errors
        result['owned_processes_stopped'] = all(browser.process is None
            or browser.process.poll() is not None for browser in browsers)
        if cleanup_errors or not result['owned_processes_stopped']:
            result['passed'] = False
        result['elapsed_seconds'] = budget.finish()
        sync.write_json(args.output / 'result.json', result, redactor)
        os.umask(old_umask)
    if not result['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
