"""Exercise the browser's shared Settings, home and native menu surfaces."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from dock_rail_regressions_smoke import Input
from extension_smoke import ROOT, request
from layout_test_support import LayoutBrowser

DEEP = '''function deep(root, selector) {
  const match=root.querySelector(selector); if(match) return match;
  for(const el of root.querySelectorAll('*')) if(el.shadowRoot) {
    const match=deep(el.shadowRoot,selector); if(match) return match;
  } return null;
}'''


def native_key(inp, key, modifiers=()):
    if inp.platform == 'wayland':
        aliases = {'Control_L': 'ctrl', 'Control_R': 'ctrl', 'Shift_L': 'shift',
                   'Shift_R': 'shift', 'Alt_L': 'alt', 'Alt_R': 'altgr',
                   'Super_L': 'logo', 'Super_R': 'logo'}
        modifiers = tuple(aliases.get(modifier, modifier) for modifier in modifiers)
        subprocess.run(['wtype', *[a for m in modifiers for a in ('-M', m)], '-k', key, *[a for m in reversed(modifiers) for a in ('-m', m)]], check=True)
    else:
        p = inp.pointer
        p.x.XStringToKeysym.argtypes = [C.c_char_p]
        p.x.XStringToKeysym.restype = C.c_ulong
        p.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
        p.x.XKeysymToKeycode.restype = C.c_uint
        p.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        mods = [p.x.XKeysymToKeycode(p.d, p.x.XStringToKeysym(m.encode())) for m in modifiers]
        for mod in mods:
            p.xt.XTestFakeKeyEvent(p.d, mod, 1, 0)
        code = p.x.XKeysymToKeycode(p.d, p.x.XStringToKeysym(key.encode()))
        assert code, key
        p.xt.XTestFakeKeyEvent(p.d, code, 1, 0)
        p.xt.XTestFakeKeyEvent(p.d, code, 0, 0)
        for mod in reversed(mods):
            p.xt.XTestFakeKeyEvent(p.d, mod, 0, 0)
        p.x.XFlush(p.d)
    time.sleep(.2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--capture-only', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'checks': [], 'settings': {}}
    try:
        with tempfile.TemporaryDirectory(dir=args.output) as tmp, LayoutBrowser(
                args.browser, Path(tmp), args.output) as b:
            inp = Input(b.platform)
            try:
                def ui(js):
                    return b.ev('(()=>{' + DEEP + js + '})()')

                def navigate(route):
                    b.ev('location.href=' + json.dumps('chrome://settings/' + route) + ';true')
                    time.sleep(1.1)

                for mode in (1, 2):
                    assert b.ev('(async()=>{const m=await import("chrome://resources/cr_components/customize_color_scheme_mode/customize_color_scheme_mode.mojom-webui.js");m.browserProxyFactory.getInstance().handler.setColorSchemeMode(' + str(mode) + ');return true})()')
                    time.sleep(.5)
                    for route in ['people', 'appearance', 'privacy', 'performance', 'search', 'defaultBrowser',
                                  'onStartup', 'languages', 'downloads', 'accessibility',
                                  'system', 'reset', 'help', 'content', 'security', 'fonts']:
                        navigate(route)
                        state = ui('''const shell=deep(document,'settings-ui');
                          const menu=deep(document,'settings-menu');
                          const link=menu.shadowRoot.querySelector('a[selected]');
                          const scroller=shell.shadowRoot.querySelector('#container');
                          const main=shell.shadowRoot.querySelector('#main').getBoundingClientRect();
                          const cards=[...function* walk(root){for(const e of root.querySelectorAll('*')) {
                            if(e.matches('settings-section,settings-subpage'))yield e;
                            if(e.shadowRoot)yield* walk(e.shadowRoot);
                          }}(document)].map(e=>e.matches('settings-subpage') ? e : e.shadowRoot.querySelector('#card'));
                          return {width:innerWidth,overflow:scroller.scrollWidth>scroller.clientWidth,
                            navVisible:!shell.narrow_,selected:link?.getAttribute('href'),
                            navHeight:link?.getBoundingClientRect().height,
                            cards:cards.filter(c=>c.getBoundingClientRect().width>0).map(c=>({
                              card:getComputedStyle(c).borderRadius,
                              contained:c.getBoundingClientRect().left>=main.left && c.getBoundingClientRect().right<=main.right+1}))};''')
                        result['settings'][f'{mode}-{route}'] = state
                        inp.capture(args.output / f'settings-{mode}-{route}.png')
                        if not args.capture_only:
                            assert not state['overflow'], (route, state)
                            assert all(c['contained'] for c in state['cards']), (route, state)
                            if route in ('content', 'security', 'fonts'):
                                assert state['cards'], ('No visible subpage checked', route, state)
                            assert state['navVisible'], ('Settings hides navigation in a normal sidebar window', state)
                            parent = {'content': 'privacy', 'security': 'privacy', 'fonts': 'appearance'}.get(route, route)
                            assert state['selected'] == '/' + parent, (route, state)
                            assert 28 <= state['navHeight'] <= 34, state
                        if route == 'privacy' and not args.capture_only:
                            assert ui('''const row=deep(document,'#heliumServicesLinkRow');
                              const icon=deep(row.shadowRoot,'cr-icon');
                              return getComputedStyle(icon,'::before').backgroundColor;''') == ('rgb(0, 0, 0)' if mode == 1 else 'rgb(255, 255, 255)')
                            assert ui("const icon=deep(deep(document,'#heliumServicesLinkRow').shadowRoot,'cr-icon');return getComputedStyle(icon.shadowRoot.querySelector('svg')).display;") == 'none', 'A second product image remains visible beside the approved mask'
                    if not args.capture_only:
                        result['checks'].append('Settings navigation stays visible, compact and selected across every primary page in ' + ('light' if mode == 1 else 'dark') + ' mode')

                navigate('downloads')
                before = b.ev('new Promise(r=>chrome.settingsPrivate.getPref("download.prompt_for_download",r))')
                ui('deep(document,"settings-toggle-button").click();return true;')
                time.sleep(.3)
                after = b.ev('new Promise(r=>chrome.settingsPrivate.getPref("download.prompt_for_download",r))')
                assert before['value'] != after['value'], (before, after)
                navigate('downloads')
                assert b.ev('new Promise(r=>chrome.settingsPrivate.getPref("download.prompt_for_download",r))')['value'] == after['value']
                result['checks'].append('A Settings toggle changes its real preference and survives navigation')

                home = request(b.port, '/json/new?chrome://newtab/', 'PUT')
                time.sleep(1.8)
                for mode in (1, 2):
                    assert b.ev('(async()=>{const m=await import("chrome://resources/cr_components/customize_color_scheme_mode/customize_color_scheme_mode.mojom-webui.js");m.browserProxyFactory.getInstance().handler.setColorSchemeMode(' + str(mode) + ');return true})()')
                    time.sleep(.6)
                    state = b.ev('''(()=>{const app=document.querySelector('ntp-app');
                      const search=app.shadowRoot.querySelector('ntp-searchbox');
                      const input=search.shadowRoot.querySelector('cr-searchbox-input');
                      return {page:getComputedStyle(app).getPropertyValue('--color-new-tab-page-background'),
                        field:getComputedStyle(search.shadowRoot.querySelector('#inputWrapper')).backgroundColor,
                        text:getComputedStyle(input.shadowRoot.querySelector('input')).color,
                        overflow:document.documentElement.scrollWidth>innerWidth};})()''', home)
                    result[f'home-{mode}'] = state
                    inp.capture(args.output / f'home-{mode}.png')
                    assert not state['overflow'], state
                if not args.capture_only:
                    assert result['home-1']['field'] != result['home-2']['field'], 'Home search ignores live theme changes'
                    steps = [
                        {'method': 'Emulation.setEmulatedMedia', 'params': {'features': [{'name': 'prefers-reduced-motion', 'value': 'reduce'}]}},
                        {'method': 'Runtime.evaluate', 'params': {'expression': '''(()=>{
                          const tiles=document.querySelector('ntp-app').shadowRoot.querySelector('cr-most-visited');
                          return matchMedia('(prefers-reduced-motion: reduce)').matches &&
                            [...tiles.shadowRoot.querySelectorAll('#container,#addShortcut,.tile')].every(e=>getComputedStyle(e).transitionDuration.split(',').every(d=>parseFloat(d)===0));
                        })()''', 'returnByValue': True}, 'assert': True},
                    ]
                    subprocess.run(['node', str(ROOT / 'tests/noventra/layout_editor_input.mjs'), home['webSocketDebuggerUrl'], json.dumps(steps)], check=True)
                    result['checks'].append('Shortcut movement and reveal stop under reduced-motion preferences')
                request(b.port, '/json/close/' + home['id'])
                result['checks'].append('Home stays inside the page and follows light and dark themes')

                navigate('appearance')
                inp.resize(700, 600)
                time.sleep(.4)
                state = ui('const s=deep(document,"settings-ui");const c=s.shadowRoot.querySelector("#container");return {narrow:s.narrow_,overflow:c.scrollWidth>c.clientWidth};')
                assert state['narrow'] and not state['overflow'], state
                ui('deep(document,"cr-toolbar").shadowRoot.querySelector("#menuButton").click();return true;')
                time.sleep(.3)
                inp.capture(args.output / 'settings-drawer.png')
                assert ui('return deep(document,"cr-drawer").open;')
                native_key(inp, 'Escape')
                result['checks'].append('Narrow Settings uses an accessible drawer and fits the window')
                inp.resize(1200, 760)
                navigate('privacy')
                # Resizing a floating Wayland window may leave the seat focused
                # on the dismissed drawer. Focus the owned page's empty area.
                inp.pointer.click(800, 600)
                # The browser's focused Settings toolbar is outside the page;
                # Alt+F reaches its native app menu through the normal accelerator.
                keyboard = None
                try:
                    if b.platform == 'wayland':
                        # Use the private US physical keymap for the real Alt+F
                        # accelerator; wtype assigns printable keys synthetic codes.
                        subprocess.run(['swaymsg', '[app_id="noventra-browser"] focus'],
                                       check=True, stdout=subprocess.DEVNULL)
                        keyboard = subprocess.Popen([os.environ['NOVENTRA_TEST_KEYBOARD']],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                        assert keyboard.stdout.readline().strip() == 'ready'
                        for code, down in ((56, 1), (33, 1), (33, 0), (56, 0)):
                            keyboard.stdin.write(f'key {code} {down}\n')
                            keyboard.stdin.flush()
                            assert keyboard.stdout.readline().strip() == 'ok'
                        time.sleep(.4)
                    else:
                        native_key(inp, 'f', ('Alt_L',))
                    inp.capture(args.output / 'browser-menu.png')
                    text = subprocess.check_output(['tesseract', str(args.output / 'browser-menu.png'), 'stdout'], stderr=subprocess.DEVNULL, text=True)
                finally:
                    if keyboard:
                        keyboard.stdin.close()
                        keyboard.wait(timeout=5)
                assert 'New tab' in text and 'New window' in text, text
                native_key(inp, 'Escape')
                result['checks'].append('Native browser menu remains reachable by keyboard')
                result['passed'] = not args.capture_only
            finally:
                inp.close()
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
