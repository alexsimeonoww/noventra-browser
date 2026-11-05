"""Exercise Settings Make default in an owned HOME and XDG configuration."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

from dock_rail_regressions_smoke import Input
from extension_smoke import ROOT, request, wait_for
from interface_polish_smoke import native_key
from layout_editor_smoke import FIND
from layout_test_support import LayoutBrowser


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def normal_quit(browser):
    version = request(browser.port, '/json/version')
    subprocess.run(['node', str(ROOT / 'experiments/rust-gui/compare-cdp.mjs'),
                    version['webSocketDebuggerUrl'], 'Browser.close', '{}'],
                   check=True, capture_output=True, text=True, timeout=15)
    assert browser.process.wait(timeout=20) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--browser-env', default='omarchy-launch-browser')
    parser.add_argument('--observe-only', action='store_true')
    args = parser.parse_args()
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'binary_sha256': args.expected_binary,
              'resources_sha256': args.expected_resources, 'checks': []}
    home = args.output.resolve() / 'home'
    data = home / 'data'
    applications = data / 'applications'
    applications.mkdir(parents=True)
    launcher = home / 'AppRun'
    launcher.write_text('#!/bin/sh\nexit 0\n')
    launcher.chmod(0o755)
    spec = importlib.util.spec_from_file_location('installer', ROOT / 'noventra/package/installer.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    desktop = applications / 'noventra-browser.desktop'
    desktop.write_bytes(installer.desktop_contents(ROOT / 'noventra/package/noventra-browser.desktop', launcher, home / 'icon.png'))
    (applications / 'prior-browser.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=Prior browser\nExec=/usr/bin/true %U\n'
        'MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;\n')
    for name in ('config', 'other-config', 'other-data'):
        (home / name).mkdir()
    (home / 'config/mimeapps.list').write_text(
        '[Default Applications]\ntext/html=prior-browser.desktop\n'
        'x-scheme-handler/http=prior-browser.desktop\nx-scheme-handler/https=prior-browser.desktop\n')
    (home / 'config/hyprland-mimeapps.list').write_text(
        '[Default Applications]\nx-scheme-handler/http=omarchy-browser.desktop\n'
        'x-scheme-handler/https=omarchy-browser.desktop\n')
    (applications / 'omarchy-browser.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=Omarchy browser dispatcher\n'
        'Exec=omarchy-launch-browser %U\nMimeType=x-scheme-handler/http;x-scheme-handler/https;\n')
    os.environ.update(HOME=str(home), XDG_CONFIG_HOME=str(home / 'config'),
                      XDG_DATA_HOME=str(data), XDG_CONFIG_DIRS=str(home / 'other-config'),
                      XDG_DATA_DIRS=str(home / 'other-data'), XDG_CURRENT_DESKTOP='Hyprland',
                      BROWSER=args.browser_env)
    os.environ.pop('NOVENTRA_DESKTOP', None)

    def handlers():
        return {mime: subprocess.check_output(
            ['env', '-u', 'BROWSER', 'XDG_CURRENT_DESKTOP=', 'xdg-mime', 'query', 'default', mime],
            text=True).strip() for mime in ('text/html', 'x-scheme-handler/http', 'x-scheme-handler/https')}

    def state(browser, page):
        return browser.ev('''(async()=>{const {sendWithPromise}=await import(
            'chrome://resources/js/cr.js');return await sendWithPromise('requestDefaultBrowserState');})()''', page)

    try:
        before = handlers()
        assert set(before.values()) == {'prior-browser.desktop'}, before
        profile = home / 'profile'
        profile.mkdir()
        with LayoutBrowser(args.browser, profile, args.output / 'first') as browser:
            page = request(browser.port, '/json/new?chrome://settings/defaultBrowser', 'PUT')
            request(browser.port, '/json/activate/' + page['id'])
            wait_for(lambda: browser.ev('''(()=>{''' + FIND + '''
                return !!deep(document,'settings-default-browser-page')?.shadowRoot?.querySelector('cr-button');})()''', page), browser.process)
            result['initial_state'] = state(browser, page)
            assert result['initial_state']['canBeDefault'] and not result['initial_state']['isDefault']
            browser.ev('''(()=>{''' + FIND + '''
                deep(document,'settings-default-browser-page').shadowRoot.querySelector('cr-button').focus();return true;})()''', page)
            inp = Input(browser.platform)
            try:
                native_key(inp, 'Return')
            finally:
                inp.close()
            wait_for(lambda: state(browser, page)['isDefault'], browser.process, timeout=20)
            result['after_handlers'] = handlers()
            assert set(result['after_handlers'].values()) == {'noventra-browser.desktop'}
            shown = browser.ev('''(()=>{''' + FIND + '''
                const root=deep(document,'settings-default-browser-page').shadowRoot;
                const row=root.querySelector('#isDefault');return !!row&&!row.hidden&&row.getBoundingClientRect().height>0;})()''', page)
            assert shown, 'Settings does not show Noventra as default'
            result['checks'].append('Make default changes owned html/http/https handlers and updates Settings')
            normal_quit(browser)
        result['first_exit'] = browser.process.returncode
        assert result['first_exit'] == 0
        with LayoutBrowser(args.browser, profile, args.output / 'restart') as browser:
            page = request(browser.port, '/json/new?chrome://settings/defaultBrowser', 'PUT')
            wait_for(lambda: browser.ev('!!document.querySelector("settings-ui")?.shadowRoot', page), browser.process)
            wait_for(lambda: state(browser, page)['isDefault'], browser.process, timeout=15)
            result['checks'].append('Noventra remains default after a normal browser restart')
            normal_quit(browser)
        result['restart_exit'] = browser.process.returncode
        assert result['restart_exit'] == 0
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        if not args.observe_only:
            raise
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
