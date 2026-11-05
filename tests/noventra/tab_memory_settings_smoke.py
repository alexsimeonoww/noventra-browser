"""Verify discoverable tab-memory controls and their persisted native preference."""

import argparse
import base64
import json
from pathlib import Path
import tempfile

from extension_smoke import wait_for
from layout_editor_smoke import FIND
from layout_test_support import LayoutBrowser
from portable_transfer_smoke import normal_close, protocol


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'checks': []}
    try:
        with tempfile.TemporaryDirectory(prefix='profile-', dir=args.output) as tmp:
            profile = Path(tmp)
            with LayoutBrowser(args.browser, profile, args.output / 'first') as browser:
                def ui(code):
                    return browser.ev('(async()=>{' + FIND + '''
                      const page=deep(document,'settings-appearance-page');
                      const root=page?.shadowRoot;
                    ''' + code + '})()')

                ui('''const {Router,routes}=await import('chrome://settings/settings.js');
                  Router.getInstance().navigateTo(routes.APPEARANCE,
                    new URLSearchParams({section:'browsing'}));return true;''')
                wait_for(lambda: ui('return !!root?.querySelector("#hoverCardMemoryUsageToggle");'), browser.process)
                assert ui('''const toggle=root.querySelector('#hoverCardMemoryUsageToggle');
                  return !root.querySelector('#noventraAdvancedBrowsing').open &&
                    toggle.checkVisibility() && !toggle.closest('details');'''), 'Tab memory setting is hidden inside advanced options'
                result['checks'].append('Tab memory control is visible without expanding advanced options')
                assert wait_for(lambda: ui('''const toggle=root.querySelector('#hoverCardMemoryUsageToggle');
                  const description=toggle.shadowRoot.querySelector('#sub-label-text');
                  return description?.checkVisibility() && description.textContent.includes('Task Manager');'''), browser.process)
                result['checks'].append('Preview controls explain estimates and the detailed Task Manager')
                ui('root.querySelector("#hoverCardMemoryUsageToggle").scrollIntoView({block:"center"});return true;')
                shot = protocol(browser, 'Page.captureScreenshot',
                                {'format': 'png', 'captureBeyondViewport': False}, browser.settings)
                (args.output / 'tab-previews.png').write_bytes(base64.b64decode(shot['data']))
                for enabled in (True, False, True):
                    browser.ev('''new Promise(resolve=>chrome.settingsPrivate.setPref(
                      'browser.hovercard.memory_usage_enabled', ''' + str(not enabled).lower() + ',resolve))')
                    wait_for(lambda: ui('return root.querySelector("#hoverCardMemoryUsageToggle").checked === ' + str(not enabled).lower() + ';'), browser.process)
                    ui('root.querySelector("#hoverCardMemoryUsageToggle").click();return true;')
                    assert wait_for(lambda: browser.ev('''new Promise(resolve=>chrome.settingsPrivate.getPref(
                      'browser.hovercard.memory_usage_enabled',p=>resolve(p.value === ''' + str(enabled).lower() + ')))'), browser.process)
                result['checks'].append('The real toggle updates the browser memory preference in both directions')
                assert normal_close(browser) == 0
            with LayoutBrowser(args.browser, profile, args.output / 'restart') as browser:
                assert browser.ev('''new Promise(resolve=>chrome.settingsPrivate.getPref(
                  'browser.hovercard.memory_usage_enabled',p=>resolve(p.value)))''')
                result['checks'].append('The enabled memory preference survives normal browser restart')
                assert normal_close(browser) == 0
            result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
