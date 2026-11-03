"""Pending native integration gate: run only on a packaged unified candidate.

Use work/ram-investigation/with-xvfb.py and an unused --output directory.
This script is prepared offline; its presence does not establish UI acceptance.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import subprocess
import tempfile
import time
from PIL import Image

ROOT = next(p for p in Path(__file__).resolve().parents if (p/'tests/noventra').is_dir())
sys.path.insert(0,str(ROOT/'tests/noventra'))
import compact_chrome_smoke as n
from extension_smoke import request, wait_for
from interface_polish_smoke import native_key


def resize_window(browser, page, width):
    # CDP changes only this owned test window, on the private display.
    script = r"""
      const socket=new WebSocket(process.argv[1]);
      await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
      let id=0;const pending=new Map();socket.onmessage=event=>{
        const data=JSON.parse(event.data);const entry=pending.get(data.id);
        if(entry){pending.delete(data.id);data.error?entry.reject(Error(JSON.stringify(data.error))):entry.resolve(data.result);}
      };
      const call=(method,params)=>new Promise((resolve,reject)=>{const request=++id;pending.set(request,{resolve,reject});socket.send(JSON.stringify({id:request,method,params}));});
      try{const current=await call('Browser.getWindowForTarget',{targetId:process.argv[2]});
        await call('Browser.setWindowBounds',{windowId:current.windowId,bounds:{width:Number(process.argv[3]),height:760}});
      }finally{socket.close();}
    """
    subprocess.run(['node','--input-type=module','-e',script,page['webSocketDebuggerUrl'],page['id'],str(width)],check=True,timeout=15)
    time.sleep(.4)


def close_normally(browser):
    # Match the proven Browser.close transport in onboarding_managers_smoke.
    # The socket may disconnect without acknowledging shutdown; the browser's
    # actual exit code, not a transport error after sending, decides success.
    endpoint=request(browser.port,'/json/version')['webSocketDebuggerUrl']
    script = r"""const s=new WebSocket(process.argv[1]);let sent=false,ack=false;
      const timer=setTimeout(()=>process.exit(1),10000);
      s.onopen=()=>{sent=true;s.send(JSON.stringify({id:1,method:'Browser.close'}))};
      s.onmessage=e=>{const m=JSON.parse(e.data);if(m.id===1&&!m.error)ack=true};
      s.onclose=()=>{clearTimeout(timer);process.stdout.write(JSON.stringify({sent,ack}));process.exit(sent?0:1)};"""
    observed=json.loads(subprocess.check_output(['node','-e',script,endpoint],text=True,timeout=15))
    observed['exit_code']=browser.process.wait(timeout=15)
    (browser.output/'normal-close.json').write_text(json.dumps(observed,indent=2)+'\n')
    assert observed['sent'] and observed['exit_code']==0,observed
    return observed['exit_code']


def run(args):
    assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') != '1', 'Use the private X11 wrapper for this fixture'
    args.output.mkdir(parents=True,exist_ok=False)
    budget=n.Budget()
    result={'passed':False,'checks':[],'native_candidate':str(args.browser.resolve())}
    try:
        for side in ('left','right'):
            out=args.output/side;out.mkdir()
            with tempfile.TemporaryDirectory(prefix='profile-',dir=out) as tmp:
                profile=Path(tmp);(profile/'Default').mkdir()
                (profile/'Default/Preferences').write_text(json.dumps({
                    'vertical_tabs':{'collapsed_state':False,'uncollapsed_width':232},
                    'helium':{'browser':{'zen_mode':True,'zen_mode_sidebar_pinned':True,'zen_mode_top_chrome_pinned':True}}}))
                with n.session(args,profile,out,n.FLAGS,result,budget) as (browser,tree,inp):
                    layout=n.preset(browser,'minimal-'+side)
                    assert next(i for i in layout['items'] if i['id']=='bookmarks')['visible']
                    page,before=n.make_page(browser,'Unified retained draft')
                    n.activate(browser,tree,inp,page)
                    n.capture(tree,inp,out,'merged-row')
                    # Native TabView is an accessibility leaf; its label is not
                    # a separately exposed TabTitle node. Check the real pixels.
                    tab=n.one(tree,r'class_name=TabView name=Unified retained draft ')
                    row=tree.rect(tab)
                    with Image.open(out/'merged-row.png') as image:
                        label=image.crop((row['x']+30,row['y'],row['x']+row['w']-34,row['y']+row['h']))
                        label.resize((label.width*4,label.height*4)).save(out/'address-row-ocr.png')
                    ocr=subprocess.run(['tesseract',str(out/'address-row-ocr.png'),'stdout','--psm','7'],capture_output=True,text=True,check=True,timeout=15)
                    (out/'address-row-ocr.txt').write_text(ocr.stdout)
                    assert 'about:blank' in ocr.stdout,ocr.stdout
                    assert not n.visible(tree,r'class_name=OmniboxViewViews '), 'A second full address row remains visible'
                    assert len(n.visible(tree,r'class_name=BrowserAppMenuButton '))==1
                    assert not n.visible(tree,r'class_name=AccountSpaceDotButton name=Account spaces: ')
                    inp.pointer.click(row['x']+70,row['y']+row['h']/2)
                    wait_for(lambda:n.visible(tree.read(),r'class_name=OmniboxViewViews '),browser.process)
                    n.capture(tree,inp,out,'native-address-editor')
                    native_key(inp,'Escape');n.activate(browser,tree,inp,page)
                    assert not n.visible(tree.read(),r'class_name=OmniboxViewViews ')
                    result['checks'].append(side+': active tab address opens and dismisses the real omnibox without a second address row')

                    def footer():
                        tree.read()
                        menu=n.one(tree,r'class_name=BrowserAppMenuButton ')
                        spaces=n.one(tree,r'class_name=AccountSpacesButton ')
                        dock=tree.nodes[menu['parent']]
                        m,s,d=tree.rect(menu),tree.rect(spaces),tree.rect(dock)
                        assert m['y']+m['h']==d['y']+d['h'],(m,d)
                        assert not n.overlaps(m,s),(m,s)
                        assert not n.visible(tree,r'class_name=AccountSpaceDotButton name=Account spaces: ')
                        return m,d
                    for width,expected in ((400,280),(34,34),(232,232)):
                        tree.read();handle=tree.rect(n.one(tree,r'class_name=ResizeArea name=Vertical Tab Strip Resize Handle '))
                        menu,dock=footer();delta=width-dock['w']
                        start=(handle['x']+handle['w']/2,handle['y']+handle['h']/2)
                        inp.move(*start);inp.button(True)
                        try:inp.move(start[0]+(delta if side=='left' else -delta),start[1]);time.sleep(.4)
                        finally:inp.button(False)
                        time.sleep(.25);menu,dock=footer();assert abs(dock['w']-expected)<=1,(width,expected,dock)
                        n.capture(tree,inp,out,'width-'+str(width))
                        inp.pointer.click(menu['x']+menu['w']/2,menu['y']+menu['h']/2)
                        time.sleep(.25);n.capture(tree,inp,out,'menu-'+str(width))
                        # Native popup content is also captured for visual review.
                        # Keyboard dismissal must return to the same retained page.
                        native_key(inp,'Escape');n.activate(browser,tree,inp,page)
                    if side == 'right':
                        split_layout = browser.state()['layout']
                        next(item for item in split_layout['items'] if item['id']=='bookmarks')['edge']='left'
                        n.apply(browser,split_layout)
                        resize_window(browser,page,420)
                        tree.read()
                        handle=n.one(tree,r'class_name=ResizeArea name=Vertical Tab Strip Resize Handle ')
                        region=tree.nodes[handle['parent']]
                        assert tree.rect(region)['w']<160,tree.rect(region)
                        search=n.one(tree,r'class_name=ToolbarButton name=Search or enter address ')
                        assert tree.rect(search)['w']>0
                        inp.pointer.click(*tree.center(search))
                        wait_for(lambda:n.visible(tree.read(),r'class_name=OmniboxViewViews '),browser.process)
                        n.capture(tree,inp,out,'two-sided-narrow-search')
                        native_key(inp,'Escape')
                        resize_window(browser,page,1200)
                        n.apply(browser,layout)
                        n.activate(browser,tree,inp,page)
                        result['checks'].append('Right: actual narrow two-sided allocation keeps a visible native Search fallback')
                    n.retained(browser,[(page,before)])
                    result['checks'].append(side+': permanent bottom menu, compact/expanded transitions, 280-pixel drag cap, and retained document')
                    result.setdefault('normal_shutdowns',[]).append({'side':side,'exit':close_normally(browser)})
                saved=json.loads((profile/'Default/Preferences').read_text())['vertical_tabs']
                assert saved['uncollapsed_width']==232 and not saved['collapsed_state'],saved
                result['checks'].append(side+': final resized width persists on normal quit')
        result['passed']=True
    except BaseException as error:
        result['error']=repr(error);raise
    finally:
        budget.close();(args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--browser',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    run(p.parse_args())
