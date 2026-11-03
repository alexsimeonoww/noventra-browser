"""Bounded Settings integration smoke for a freshly packaged Noventra runtime.

Run through the existing private-display wrapper with --browser and --output.
Uses real preferences and color-scheme Mojo calls in a disposable profile.
This verifies Settings persistence, not compositor blur or native materials;
it never overrides platform support or injects replacement preference data.
"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

from extension_smoke import ROOT, request, wait_for
from layout_editor_smoke import FIND
from layout_test_support import LayoutBrowser, private_wayland_output
from dock_rail_regressions_smoke import Input
from native_ui_tree import NativeUITree


GLASS_PREF = 'noventra.browser.glass_enabled'


class Appearance:
    def __init__(self, browser):
        self.browser = browser
        request(browser.port, '/json/activate/' + browser.settings['id'])
        self.wait('return !!editor?.draft && !!root.querySelector("#noventraGlassAppearance")'
                  '?.pref && page.selectedColorSchemeMode_ !== undefined;')
        self.ui('root.querySelector("[data-section=customizeAppearance]").click();'
                'await page.updateComplete;return true;')

    def ui(self, code):
        return self.browser.ev('(async()=>{' + FIND + '''
          const page=deep(document,'settings-appearance-page');
          const index=deep(document,'settings-appearance-page-index');
          if (!page || !index) return false;
          const root=page.shadowRoot;
        ''' + code + '})()')

    def wait(self, code):
        return wait_for(lambda: self.ui(code), self.browser.process, timeout=10)

    def click(self, selector):
        if selector == '#noventraGlassAppearance' and self.ui(
                'return !!root.querySelector("#noventraSurfaceChoices");'):
            # The old toggle is now only a preference adapter. Exercise the
            # actual visible replacement instead of clicking a hidden control.
            target = 'solid' if self.ui(
                'return page.noventraGlassEnabledPref_.value;') else 'glass'
            self.native_click('input[name=noventraSurface][value="' + target + '"]')
            return
        self.ui('const el=root.querySelector(' + json.dumps(selector) + ');'
                'if(!el || el.disabled || !el.getClientRects().length)'
                'throw Error("Appearance control unavailable");'
                'el.click();await page.updateComplete;return true;')

    def native_click(self, selector):
        tree = NativeUITree(self.browser)
        inp = Input(self.browser.platform)
        try:
            request(self.browser.port, '/json/activate/' + self.browser.settings['id'])
            point = self.ui('const el=root.querySelector(' + json.dumps(selector) + ');'
                            'if(!el || el.disabled || !el.checkVisibility())'
                            'throw Error("Appearance control unavailable");'
                            'el.scrollIntoView({block:"center",behavior:"instant"});'
                            'const r=el.getBoundingClientRect();'
                            'return {x:r.x+r.width/2,y:r.y+r.height/2,w:innerWidth,h:innerHeight};')
            tree.read()
            views = [tree.rect(node) for node in tree.nodes.values()
                     if ' INVISIBLE ' not in node['line']
                     and 'class_name=ContentsContainerView ' in node['line']
                     and abs(node['bounds'][2] - point['w']) <= 2
                     and abs(node['bounds'][3] - point['h']) <= 2]
            assert len(views) == 1, (selector, point, views)
            view = views[0]
            assert 0 < point['x'] < view['w'] and 0 < point['y'] < view['h'], point
            inp.pointer.click(view['x'] + point['x'], view['y'] + point['y'])
            time.sleep(.15)
        finally:
            inp.close()
            request(self.browser.port, '/json/close/' + tree.target['id'])
            request(self.browser.port, '/json/activate/' + self.browser.settings['id'])

    def scheme(self, mode):
        self.click('input[name=noventraAppearance][value="' + str(mode) + '"]')
        self.assert_mode(mode)

    def assert_mode(self, mode):
        self.wait('return page.selectedColorSchemeMode_ === ' + str(mode)
                  + ' && root.querySelector("input[name=noventraAppearance]:checked")'
                  '.value === ' + json.dumps(str(mode)) + ';')
        assert self.ui('return root.querySelector("#noventraColorScheme").value;') == str(mode)

    def assert_glass(self, enabled):
        value = json.dumps(enabled)
        self.wait('return page.noventraGlassEnabledPref_?.value === ' + value
                  + ' && root.querySelector("#noventraGlassAppearance").checked === '
                  + value + ';')
        actual = self.browser.ev('new Promise(resolve=>chrome.settingsPrivate.getPref('
                                 + json.dumps(GLASS_PREF) + ',p=>resolve(p?.value)))')
        assert actual is enabled, ('Native preference mismatch', actual, enabled)
        has_styles = self.ui('return !!root.querySelector("#noventraSurfaceChoices");')
        expected = ['System', 'Light', 'Dark'] if has_styles or not enabled else ['System', 'Light glass', 'Dark glass']
        assert self.ui('return [...root.querySelectorAll(".appearance-choice-name")]'
                       '.map(e=>e.textContent.trim());') == expected
        assert self.ui('return root.querySelector("#noventraAppearanceChoices").dataset.glass;') == str(enabled).lower()
        if has_styles:
            assert self.ui('return root.querySelector("input[name=noventraSurface]:checked").value;') == ('glass' if enabled else 'solid')

    def key(self, key, code, virtual_key):
        steps = [{'method': 'Input.dispatchKeyEvent', 'params': {
            'type': event, 'key': key, 'code': code,
            'windowsVirtualKeyCode': virtual_key,
        }} for event in ('keyDown', 'keyUp')]
        subprocess.run(['node', str(ROOT / 'tests/noventra/layout_editor_input.mjs'),
                        self.browser.settings['webSocketDebuggerUrl'], json.dumps(steps)],
                       check=True, timeout=25)

    def capture(self, name):
        """Capture the owned display, including native chrome and Settings."""
        time.sleep(.25)
        target = self.browser.output / (name + '.png')
        if self.browser.platform == 'x11':
            subprocess.run(['import', '-window', 'root', str(target)],
                           check=True, timeout=8)
        else:
            output = private_wayland_output()
            subprocess.run(['grim', '-o', output['name'], str(target)],
                           check=True, timeout=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'browser': str(args.browser.resolve()), 'checks': [],
              'scope': 'Settings and preference persistence; native material rendering is not certified'}
    try:
        with tempfile.TemporaryDirectory(prefix='glass-profile-', dir=args.output) as tmp:
            profile = Path(tmp)
            with LayoutBrowser(args.browser, profile, args.output / 'initial',
                               ('--force-renderer-accessibility',)) as browser:
                ui = Appearance(browser)
                ui.assert_mode(0)
                ui.assert_glass(True)
                ui.capture('appearance-system')
                result['checks'].append('Fresh profile defaults to System and glass enabled')
                saved = browser.state()['layout']
                theme = ui.ui('return page.themeIdPref_.value;')
                assert ui.ui('return !!root.querySelector("cr-theme-color-picker") && '
                             '![...root.querySelectorAll("settings-toggle-button")]'
                             '.some(el=>el.prefKey==="helium.browser.native_frame_materials");')
                ui.ui('window.__glassEditor=editor;editor.selected="back";'
                      'const edge=editor.draft.current.items.find(i=>i.id==="back").edge;'
                      'editor.move(edge==="right"?"top":"right");'
                      'await editor.updateComplete;return true;')
                draft = ui.ui('return editor.draft.current;')
                assert ui.ui('return editor.dirty();'), 'Fixture must have an unsaved draft'
                for mode in (1, 2, 0):
                    ui.scheme(mode)
                    ui.capture('appearance-' + {0: 'system', 1: 'light', 2: 'dark'}[mode])
                ui.ui('root.querySelector("input[name=noventraAppearance]:checked").focus();return true;')
                ui.key('ArrowRight', 'ArrowRight', 39)
                ui.assert_mode(1)
                assert ui.ui('return root.activeElement?.matches("input[name=noventraAppearance]");')
                result['checks'].append('All schemes use the shared color mode; arrow keys move radio selection and focus')
                ui.click('#noventraGlassAppearance')
                ui.assert_glass(False)
                ui.assert_mode(1)
                ui.click('#noventraGlassAppearance')
                ui.assert_glass(True)
                ui.assert_mode(1)
                ui.click('#noventraGlassAppearance')
                ui.assert_glass(False)
                ui.scheme(2)
                ui.capture('appearance-solid-dark')
                result['checks'].append('Glass toggles immediately without changing the scheme; labels and previews adapt')
                query = 'Liquid glass' if ui.ui('return !!root.querySelector("#noventraSurfaceChoices");') else 'Glass appearance'
                found = ui.ui('return await index.searchContents(' + json.dumps(query) + ');')
                assert not found['canceled'] and found['matchCount'] > 0, found
                assert ui.ui('return !root.querySelector("#customizeAppearance").hidden;')
                ui.ui('await index.searchContents("");return true;')
                assert ui.ui('return editor===window.__glassEditor && editor.dirty();')
                assert ui.ui('return editor.draft.current;') == draft
                assert browser.state()['layout'] == saved
                assert ui.ui('return page.themeIdPref_.value;') == theme
                result['checks'].append('Appearance and search preserve the selected theme, unsaved layout and saved layout revision')
                # Discard only the fixture's draft before closing its owned browser.
                ui.ui('editor.shadowRoot.querySelector("#cancelLayout").click();return true;')
            with LayoutBrowser(args.browser, profile, args.output / 'solid-restart',
                               ('--force-renderer-accessibility',)) as browser:
                ui = Appearance(browser)
                ui.assert_mode(2)
                ui.assert_glass(False)
                assert browser.state()['layout'] == saved
                ui.scheme(0)
                ui.click('#noventraGlassAppearance')
                ui.assert_glass(True)
            with LayoutBrowser(args.browser, profile, args.output / 'glass-restart',
                               ('--force-renderer-accessibility',)) as browser:
                ui = Appearance(browser)
                ui.assert_mode(0)
                ui.assert_glass(True)
                assert browser.state()['layout'] == saved
                ui.capture('appearance-system-restarted')
            result['checks'].append('Dark/solid and System/glass persist across browser restarts without layout revision changes')
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
