"""Native sidebar shrink stress on owned private X11 and Wayland outputs."""
import http.server
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import pinned_tabs_smoke as p
from sidebar_inline_address_smoke import ancestor, screen_rect
from layout_test_support import private_wayland_output
from chrome_surface_smoke import Keyboard


def geometry(tree):
    tree.read()
    region=p.native.one(tree,r'class_name=VerticalTabStripRegionView ')
    return screen_rect(tree,ancestor(tree,region,r'class_name=NoventraDockView '))


def open_page(browser, tree, inp, keyboard, url):
    before = {page['id'] for page in p.pages(browser)}
    keyboard.key('t', control=True)
    page = p.wait_for(lambda: next((page for page in p.pages(browser)
                      if page['id'] not in before), None), browser.process, timeout=6)
    p.navigate(browser, page, url)
    p.tab.activate(browser, tree, inp, page)
    return page


def admit(args):
    if os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1':
        return p.admit(args, Path(__file__))
    # The compositor wrapper owns this fresh socket/output; never use the
    # desktop's Wayland display or any of its input devices.
    runtime = Path(os.environ['XDG_RUNTIME_DIR']).resolve(strict=True)
    assert str(runtime).startswith('/tmp/nv-')
    assert (runtime / os.environ['WAYLAND_DISPLAY']).is_socket()
    private_wayland_output()
    args.browser = args.browser.resolve(strict=True)
    assert p.sha(args.browser) == args.expected_binary
    assert p.sha(args.browser.parent / 'resources.pak') == args.expected_resources
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'executed-test.py').write_bytes(Path(__file__).read_bytes())
    return {'passed': False, 'checks': [], 'normal_shutdowns': [],
            'runtime': str(args.browser), 'binary_sha256': args.expected_binary,
            'resources_sha256': args.expected_resources, 'platform': 'wayland',
            'test_sha256': p.sha(Path(__file__))}


def main():
    parser=p.cli(__doc__)
    parser.add_argument('--side',choices=['left','right'],default='left')
    parser.add_argument('--glass',action='store_true')
    parser.add_argument('--floating',action='store_true')
    args=parser.parse_args();result=admit(args);result['side']=args.side;result['glass']=args.glass;result['floating']=args.floating
    budget=p.Budget();server=http.server.ThreadingHTTPServer(('127.0.0.1',0),p.Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();base='http://127.0.0.1:'+str(server.server_port)
    try:
        with tempfile.TemporaryDirectory(prefix='owned-sidebar-shrink-',dir=args.output) as tmp:
            profile=Path(tmp);p.prefs(profile,width=160,side=args.side)
            pref=profile/'Default/Preferences';v=json.loads(pref.read_text());v.setdefault('noventra',{}).setdefault('browser',{})['glass_enabled']=args.glass;pref.write_text(json.dumps(v))
            extension=p.extension_fixture(profile);flags=(*p.native.FLAGS,'--load-extension='+str(extension.resolve()))
            with p.session(args,profile,args.output/'native',result,budget,flags) as (browser,tree,inp), Keyboard(inp) as keyboard:
                api=p.tab.controller(browser)
                one=open_page(browser,tree,inp,keyboard,base+'/pin-one');p.pin(browser,api,one)
                two=open_page(browser,tree,inp,keyboard,base+'/pin-two');p.pin(browser,api,two)
                ordinary=open_page(browser,tree,inp,keyboard,base+'/retained');before=p.state(browser,ordinary)
                p.activate_settings(browser);p.native.capture(tree,inp,args.output/'native','before-shrink')
                if args.floating:
                    p.check(result, 'Native preference enables the floating sidebar', browser.ev('new Promise(resolve=>chrome.settingsPrivate.setPref("helium.browser.zen_mode_sidebar_pinned",false,"",resolve))'))
                    inp.move(1199 if args.side=='right' else 0,300)
                    time.sleep(.8)
                    p.native.capture(tree,inp,args.output/'native','floating-before-shrink')
                for active in ['settings','ordinary','pin']:
                    if active=='ordinary':p.tab.activate(browser,tree,inp,ordinary)
                    elif active=='pin':p.tab.activate(browser,tree,inp,one)
                    else:p.activate_settings(browser)
                    for location in ['header','middle','footer']:
                        for target in ([48,200,48,144,320,160,48] if args.floating else [320,200,160,144,130,48,144,320,48,160]):
                            if args.floating:
                                inp.move(1199 if args.side=='right' else 0,300)
                                time.sleep(.4)
                            d=geometry(tree);x=d['x']+d['w']-2 if args.side=='left' else d['x']+1;y={'header':d['y']+20,'middle':d['y']+d['h']/2,'footer':d['y']+d['h']-12}[location]
                            change=(target-d['w'])*(1 if args.side=='left' else -1)
                            result['last_resize']={'active':active,'location':location,'from':d,'target':target}
                            inp.move(x,y);inp.button(True)
                            try:
                                for step in range(1,13):
                                    inp.move(x+change*step/12,y);time.sleep(.006)
                            finally:inp.button(False)
                            time.sleep(.4 if args.floating else .05);after=geometry(tree);expected=48 if target<=48 else max(144,target)
                            p.check(result,active+' '+location+' shrink/expand to'+str(target)+' survives and reaches correct width',abs(after['w']-expected)<=1,after)
                p.tab.activate(browser,tree,inp,ordinary)
                p.check(result,'Resizing retains the same ordinary document and draft',p.state(browser,ordinary)==before)
                p.check(result,'Resizing keeps both real pinned sites',sum(t['pinned'] for t in api('chrome.tabs.query({})'))==2)
                p.native.capture(tree,inp,args.output/'native','after-resize-stress')
            p.check(result,'Owned browser exits normally',all(s['exit']==0 for s in result['normal_shutdowns']))
            if args.floating:
                saved=json.loads(pref.read_text())['vertical_tabs']
                p.check(result, 'Floating collapse preserves the saved compact state and expanded width', saved['collapsed_state'] and saved['uncollapsed_width']==160, saved)
        result['passed']=True
    except BaseException as error:
        result['error']=repr(error)
        if 'browser' in locals():result['browser_exit_at_failure']=browser.process.poll()
        raise
    finally:
        server.shutdown();server.server_close();thread.join(timeout=2);budget.close();p.write(args.output/'result.json',result)


if __name__=='__main__':main()
