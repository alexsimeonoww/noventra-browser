"""Settings card containment and interrupted collapse recovery in a real browser.

Run through the private Xvfb wrapper with exact runtime hashes. Uses an owned
home and a no-egress network namespace. All language and preference changes
belong to its disposable profile.
"""
import argparse
import base64
import json
from pathlib import Path
import time

from extension_smoke import request, wait_for
from layout_editor_smoke import FIND
from layout_test_support import LayoutBrowser
from portable_transfer_smoke import isolate, normal_close, protocol, sha, write


ROUTES = ('languages', 'downloads', 'performance', 'privacy', 'system',
          'search', 'defaultBrowser', 'onStartup', 'accessibility', 'reset',
          'privacy/noise', 'privacy/services', 'cookies', 'security', 'content',
          'content/protectedContent')
GEOMETRY = '''(()=>{FIND
const visible=e=>e.checkVisibility({visibilityProperty:true,opacityProperty:true});
const all=[];const visit=root=>{for(const e of root.querySelectorAll('*')){
  all.push(e);if(e.shadowRoot)visit(e.shadowRoot)}};visit(document);
const bounds=e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height}};
const ui=deep(document,'settings-ui').shadowRoot;
const container=ui.querySelector('#container'),viewport=container.getBoundingClientRect();
const cards=all.filter(e=>visible(e)&&e.getBoundingClientRect().height>0&&
  (e.localName==='settings-subpage'||e.id==='card'&&e.getRootNode().host?.localName==='settings-section'));
const cardSet=new Set(cards);
const owner=e=>{for(let p=e;p;p=p.assignedSlot||p.parentElement||p.getRootNode().host){
  if(cardSet.has(p))return p}return null};
return {width:innerWidth,height:innerHeight,horizontalOverflow:container.scrollWidth>container.clientWidth+1,
cards:cards.map(card=>{const host=card.localName==='settings-subpage'?card:card.getRootNode().host,r=card.getBoundingClientRect();
  const controls=all.filter(e=>owner(e)===card&&visible(e)&&e.getBoundingClientRect().width>0&&
    e.matches('button,input,select,cr-button,cr-icon-button,cr-toggle,cr-checkbox,cr-link-row,settings-toggle-button,.list-item,.cr-row'));
  return {title:host.pageTitle,bounds:bounds(card),fits:r.left>=viewport.left-1&&r.right<=viewport.right+1,
    controls:controls.length,
    clipped:controls.filter(e=>{const c=e.getBoundingClientRect();return c.right>r.right+1||c.left<r.left-1||c.bottom>r.bottom+1||c.top<r.top-1})
      .map(e=>({tag:e.localName,id:e.id,label:e.getAttribute('aria-label')||e.textContent.trim().slice(0,80),bounds:bounds(e)}))}})};})()'''.replace('FIND', FIND)


def check(result, name, passed, evidence=None):
    result['checks'].append({'name': name, 'passed': bool(passed), 'evidence': evidence})


def navigate(browser, page, path):
    browser.ev('location.href=' + json.dumps('chrome://settings/' + path), page)
    wait_for(lambda: browser.ev('document.readyState==="complete"&&'
        '!!document.querySelector("settings-ui")?.shadowRoot', page), browser.process)
    time.sleep(.5)


def screenshot(browser, page, path):
    capture = protocol(browser, 'Page.captureScreenshot',
                       {'format': 'png', 'captureBeyondViewport': False}, page)
    path.write_bytes(base64.b64decode(capture['data']))


def resize(browser, page, width, height):
    window = protocol(browser, 'Browser.getWindowForTarget', {'targetId': page['id']}, page)
    protocol(browser, 'Browser.setWindowBounds',
             {'windowId': window['windowId'], 'bounds': {'width': width, 'height': height}}, page)
    time.sleep(.3)


def layouts(browser, page, output, result):
    for width, height in ((1200, 760), (1200, 510), (650, 510), (510, 510)):
        resize(browser, page, width, height)
        for path in ROUTES:
            navigate(browser, page, path)
            def measured_cards():
                metrics = browser.ev(GEOMETRY, page)
                return metrics if metrics['cards'] else False
            metrics = wait_for(measured_cards, browser.process, timeout=15)
            label = f'{path} at {width}x{height}'
            check(result, label + ' cards fit the Settings scroll container',
                  bool(metrics['cards']) and not metrics['horizontalOverflow']
                  and all(card['fits'] for card in metrics['cards']), metrics)
            check(result, label + ' controls remain inside their cards',
                  all(not card['clipped'] for card in metrics['cards']))
            if path == 'languages':
                screenshot(browser, page, output / f'languages-{width}-{height}.png')


def spell(browser, page, body):
    return browser.ev('(async()=>{' + FIND + '''
      const spell=deep(document,'settings-spell-check-page');
      const collapse=spell.shadowRoot.querySelector('#spellCheckCollapse');
      const toggle=spell.shadowRoot.querySelector('#enableSpellcheckingToggle');
      const delay=ms=>new Promise(r=>setTimeout(r,ms));
      const when=async condition=>{const start=performance.now();while(!condition()){
        if(performance.now()-start>3000)throw Error('Spell check state timed out');
        await delay(10)}};
    ''' + body + '})()', page)


def collapse_state(browser, page):
    return spell(browser, page, '''return {opened:collapse.opened,classes:collapse.className,
      maxHeight:collapse.style.maxHeight,height:collapse.getBoundingClientRect().height,
      scrollHeight:collapse.scrollHeight};''')


def collapses(browser, page, output, result):
    resize(browser, page, 1200, 760)
    navigate(browser, page, 'languages')
    wait_for(lambda: spell(browser, page, 'return collapse.opened;'), browser.process)
    spell(browser, page, '''toggle.click();await when(()=>!collapse.opened);
      await delay(400);toggle.click();await when(()=>collapse.opened);
      await delay(50);const {Router,routes}=await import('chrome://settings/settings.js');
      Router.getInstance().navigateTo(routes.PRIVACY);return true;''')
    time.sleep(.45)
    browser.ev('''(async()=>{const {Router,routes}=await import('chrome://settings/settings.js');
      Router.getInstance().navigateTo(routes.LANGUAGES);return true;})()''', page)
    time.sleep(.45)
    state = collapse_state(browser, page)
    check(result, 'Leaving during spell-check expansion restores an unbounded opened panel',
          state['opened'] and 'collapse-opened' in state['classes'] and state['maxHeight'] == '', state)
    resize(browser, page, 850, 510)
    spell(browser, page, "chrome.languageSettingsPrivate.enableLanguage('bg');return true;")
    spell(browser, page, "return new Promise(r=>chrome.settingsPrivate.setPref('spellcheck.dictionaries',['en-US','bg'],'',r));")
    wait_for(lambda: spell(browser, page,
        'return spell.shadowRoot.querySelectorAll(".list-item").length>=3;'), browser.process)
    time.sleep(.5)
    state = collapse_state(browser, page)
    check(result, 'Resized spell-check panel grows for real added language content',
          state['opened'] and state['maxHeight'] == '' and state['height'] >= state['scrollHeight'] - 1, state)
    spell(browser, page, "spell.scrollIntoView({block:'start'});return true;")
    screenshot(browser, page, output / 'spell-check-after-interruption.png')
    component = browser.ev('''(async()=>{
      const delay=ms=>new Promise(r=>setTimeout(r,ms));
      const wrapper=document.createElement('div');wrapper.style.cssText='position:fixed;top:60px;left:0;width:240px';
      const collapse=document.createElement('cr-collapse'),content=document.createElement('div');
      content.style.height='240px';collapse.append(content);wrapper.append(collapse);document.body.append(wrapper);
      const state=()=>({opened:collapse.opened,classes:collapse.className,maxHeight:collapse.style.maxHeight,
        height:collapse.getBoundingClientRect().height,running:collapse.getAnimations().some(a=>a.playState==='running')});
      try{
        await collapse.updateComplete;collapse.opened=true;await collapse.updateComplete;await delay(90);
        collapse.opened=false;await collapse.updateComplete;await delay(25);const reversed=state();
        await delay(400);const closed=state();
        collapse.opened=true;await collapse.updateComplete;await delay(70);
        collapse.opened=false;await collapse.updateComplete;await delay(25);
        collapse.opened=true;await collapse.updateComplete;await delay(25);const rereversed=state();
        await delay(400);const opened=state();
        collapse.opened=false;await collapse.updateComplete;await delay(60);wrapper.hidden=true;
        await delay(400);wrapper.hidden=false;await delay(50);const cancelledClose=state();
        collapse.noAnimation=true;collapse.opened=true;await collapse.updateComplete;
        const noAnimation=state();return {reversed,closed,rereversed,opened,cancelledClose,noAnimation};
      }finally{wrapper.remove()}
    })()''', page)
    check(result, 'Cancelling for a rapid reversal preserves the replacement animation',
          component['reversed']['running'] and component['rereversed']['running'], component)
    check(result, 'Rapid reversals settle to the latest closed and opened states',
          component['closed']['height'] == 0 and 'collapse-closed' in component['closed']['classes']
          and component['opened']['height'] == 240 and component['opened']['maxHeight'] == '')
    check(result, 'Cancelled closing transition settles closed after its parent returns',
          component['cancelledClose']['height'] == 0 and 'collapse-closed' in component['cancelledClose']['classes'])
    check(result, 'The no-animation option still opens immediately',
          component['noAnimation']['height'] == 240 and component['noAnimation']['maxHeight'] == '')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('all', 'layout', 'collapse'), default='all')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.rtc_policy_diagnostic = False
    args.browser = args.browser.resolve(strict=True)
    args.output = args.output.resolve()
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    result = {'passed': False, 'checks': [], 'normal_shutdowns': [],
              'binary_sha256': args.expected_binary, 'resources_sha256': args.expected_resources,
              'home_isolated': True, 'network_isolated_no_egress': True}
    try:
        with LayoutBrowser(args.browser, args.output / 'profile', args.output / 'browser') as browser:
            page = browser.settings
            request(browser.port, '/json/activate/' + page['id'])
            try:
                if args.case in ('all', 'layout'):
                    layouts(browser, page, args.output, result)
                if args.case in ('all', 'collapse'):
                    collapses(browser, page, args.output, result)
            finally:
                result['normal_shutdowns'].append(normal_close(browser))
        result['passed'] = all(item['passed'] for item in result['checks'])
        assert result['passed'], 'Settings containment or collapse recovery failed'
    except BaseException as error:
        result['error'] = str(error)
        raise
    finally:
        write(args.output / 'result.json', result)
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
