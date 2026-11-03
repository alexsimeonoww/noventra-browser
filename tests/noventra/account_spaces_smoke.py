"""Native same-window account spaces: cookie/storage isolation and tab lifecycle.

Uses an authenticated private Xvfb display, temporary profile, and local server.
No personal accounts or external test sites are used.
"""
import argparse
import csv
import ctypes as C
import http.server
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import urllib.parse
from extension_smoke import ROOT, request, stop_browser, wait_for
from layout_test_support import apply_native_test_layout
from PIL import Image

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_GET(self):
        path=urllib.parse.urlsplit(self.path)
        if path.path=='/favicon.ico':
            buf=io.BytesIO();Image.new('RGB',(32,32),(10,210,60)).save(buf,format='PNG');data=buf.getvalue()
            self.send_response(200);self.send_header('Content-Type','image/png');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
        if path.path=='/set':
            value=urllib.parse.parse_qs(path.query)['value'][0]
            self.send_response(200); self.send_header('Set-Cookie',f'account={value}; HttpOnly; SameSite=Lax; Path=/; Max-Age=3600'); self.end_headers(); return
        if path.path=='/cookie':
            data=self.headers.get('Cookie','').encode(); kind='text/plain'
        elif path.path=='/redirect':
            self.send_response(302);self.send_header('Location','/page?redirect');self.end_headers();return
        elif path.path=='/worker.js':
            data=b"self.addEventListener('install',()=>self.skipWaiting());self.addEventListener('activate',e=>e.waitUntil(self.clients.claim()));";kind='application/javascript'
        else:
            title=urllib.parse.parse_qs(path.query).get('title',['Account fixture'])[0]
            data=f'''<title>{title}</title><h1>{title}</h1><input id="draft" value="original"><p><a id="link" href="/redirect">Open account page</a></p>'''.encode(); kind='text/html'
        self.send_response(200);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)

READ='''(async()=>({cookie:await(await fetch('/cookie')).text(),local:localStorage.account,session:sessionStorage.account,cache:await caches.keys(),idb:(await indexedDB.databases()).map(d=>d.name),workers:(await navigator.serviceWorker.getRegistrations()).length,draft:document.querySelector('#draft')?.value}))()'''
def write_state(value):
    return f'''(async()=>{{await fetch('/set?value={value}');localStorage.account='{value}';sessionStorage.account='{value}';await caches.open('{value}');await new Promise((resolve,reject)=>{{let r=indexedDB.open('{value}',1);r.onsuccess=()=>{{r.result.close();resolve()}};r.onerror=()=>reject(r.error)}});await navigator.serviceWorker.register('/worker.js');await navigator.serviceWorker.ready;document.querySelector('#draft').value='{value} draft';window.loadedToken='{value} live';return true}})()'''

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--browser',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--platform',choices=['x11','wayland'],default='x11');ap.add_argument('--basic',action='store_true',help='Native space-menu/lifecycle subset without compositor screenshots')
    ap.add_argument('--dock-layout',action='store_true')
    args=ap.parse_args();args.output.parent.mkdir(parents=True,exist_ok=True)
    result={'passed':False,'checks':[],'platform':args.platform,'basic':args.basic}
    if args.platform=='x11':
        assert str(ROOT/'work/ram-investigation/xvfb-') in os.environ.get('XAUTHORITY','')
        x=C.CDLL('libX11.so.6');xt=C.CDLL('libXtst.so.6')
        x.XOpenDisplay.restype=C.c_void_p;x.XOpenDisplay.argtypes=[C.c_char_p];x.XFlush.argtypes=[C.c_void_p];x.XCloseDisplay.argtypes=[C.c_void_p]
        x.XKeysymToKeycode.argtypes=[C.c_void_p,C.c_ulong];x.XKeysymToKeycode.restype=C.c_uint
        d=x.XOpenDisplay(None);assert d
        xt.XTestFakeMotionEvent.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_ulong];xt.XTestFakeButtonEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong];xt.XTestFakeKeyEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
        def key(sym,down):xt.XTestFakeKeyEvent(d,x.XKeysymToKeycode(d,sym),down,0)
        def chord(sym,mods=()):
            for m in mods:key(m,1)
            key(sym,1);key(sym,0)
            for m in reversed(mods):key(m,0)
            x.XFlush(d);time.sleep(.6)
        def click(px,py,button=1,mods=()):
            xt.XTestFakeMotionEvent(d,-1,int(px),int(py),0);x.XFlush(d);time.sleep(.3)
            for m in mods:key(m,1)
            for down in (1,0):xt.XTestFakeButtonEvent(d,button,down,0)
            for m in reversed(mods):key(m,0)
            x.XFlush(d);time.sleep(.6)
        def capture(name):
            path=args.output.parent/(name+'.png');subprocess.run(['import','-window','root',str(path)],check=True);return path
    else:
        assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND')=='1'
        runtime=Path(os.environ['XDG_RUNTIME_DIR']);assert runtime.parent==Path('/tmp') and runtime.name.startswith('nv-')
        pointer=subprocess.Popen([os.environ['NOVENTRA_TEST_POINTER']],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
        def send(command):
            pointer.stdin.write(command+'\n');pointer.stdin.flush();assert pointer.stdout.readline().strip()=='ok'
        def chord(sym,mods=()):
            keys={65293:'Return',65360:'Home',65364:'Down'}
            modifiers=' '.join({65507:'CTRL',65505:'SHIFT',65513:'ALT'}[m] for m in mods)
            expression='hl.dispatch(hl.dsp.send_shortcut({mods='+json.dumps(modifiers)+',key='+json.dumps(keys.get(sym,chr(sym)))+'}))'
            subprocess.run(['hyprctl','eval',expression],check=True,stdout=subprocess.DEVNULL);time.sleep(.6)
        def click(px,py,button=1,mods=()):
            for m in mods:subprocess.run(['wtype','-M',{65507:'ctrl',65505:'shift'}[m]],check=True)
            monitor=json.loads(subprocess.check_output(['hyprctl','-j','monitors'],text=True))[0]
            send(f"motion {int(px)} {int(py)} {monitor['width']} {monitor['height']}");time.sleep(.3)
            code={1:272,2:274,3:273}[button]
            send(f'button 1 {code}');send(f'button 0 {code}')
            for m in reversed(mods):subprocess.run(['wtype','-m',{65507:'ctrl',65505:'shift'}[m]],check=True)
            time.sleep(.6)
        def capture(name):
            path=args.output.parent/(name+'.png')
            monitor=json.loads(subprocess.check_output(['hyprctl','-j','monitors'],text=True))[0]
            subprocess.run(['grim','-o',monitor['name'],str(path)],check=True,timeout=8);return path
    native_height=760
    if args.basic:
        def capture(name):return None
    def menu(label,name,exact=False):
        if args.basic:
            offset={'New account space':92,'Personal':173,'Work':141}[label]
            click(80,native_height-offset)
            return
        path=capture(name);data=subprocess.check_output(['tesseract',str(path),'stdout','--psm','11','tsv'],stderr=subprocess.DEVNULL,text=True)
        lines={}
        for w in csv.DictReader(io.StringIO(data),delimiter='\t'):
            if w['text'].strip():lines.setdefault(tuple(w[k] for k in ('block_num','par_num','line_num')),[]).append(w)
        found=[]
        for words in lines.values():
            text=' '.join(w['text'] for w in words);found.append(text)
            if text.lower()==label.lower() if exact else label.lower() in text.lower():
                click(min(int(w['left']) for w in words)+24,min(int(w['top']) for w in words)+8);return
        raise AssertionError((label,found))
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
    url=f'http://127.0.0.1:{server.server_port}'
    try:
      with tempfile.TemporaryDirectory(prefix='profile-',dir=args.output.parent) as tmp:
        profile=Path(tmp).resolve()
        (profile/'Default').mkdir()
        (profile/'Default/Preferences').write_text(json.dumps({'profile':{'default_content_setting_values':{'notifications':1}}}))
        fixture=profile/'test-extension';fixture.mkdir()
        (fixture/'manifest.json').write_text(json.dumps({'manifest_version':3,'name':'Account space test','version':'1.0','permissions':['tabs','tabGroups'],'background':{'service_worker':'worker.js'}}))
        (fixture/'worker.js').write_text('chrome.runtime.onMessage.addListener(()=>{});')
        (fixture/'test.html').write_text('<title>Space API fixture</title>')
        for restart in range(2):
          (profile/'DevToolsActivePort').unlink(missing_ok=True)
          with (args.output.parent/f'browser-{restart}.log').open('w') as log:
            extra=['--restore-last-session'] if restart else [url+'/page?title=Personal-page']
            p=subprocess.Popen([str(args.browser.resolve()),'--user-data-dir='+str(profile),'--ozone-platform='+args.platform,'--disable-gpu','--window-size=1200,760','--window-position=0,0','--no-first-run','--no-default-browser-check','--remote-debugging-address=127.0.0.1','--remote-debugging-port=0','--load-extension='+str(fixture),*extra],stdout=log,stderr=log,start_new_session=True)
            try:
              wait_for((profile/'DevToolsActivePort').exists,p);port=int((profile/'DevToolsActivePort').read_text().splitlines()[0]);time.sleep(2)
              if args.platform=='wayland':
                  # The host tiles the nested compositor to arbitrary widths.
                  # Keep the test browser geometry stable on the private socket.
                  clients=json.loads(subprocess.check_output(['hyprctl','-j','clients'],text=True))
                  client=next(c for c in clients if c['class']=='noventra-browser')
                  address='address:'+client['address']
                  monitor=json.loads(subprocess.check_output(['hyprctl','-j','monitors'],text=True))[0]
                  height=min(760,monitor['height'])
                  width=min(1200,monitor['width'])
                  for expression in [f'hl.dispatch(hl.dsp.window.float({{action="set",window="{address}"}}))',f'hl.dispatch(hl.dsp.window.resize({{x={width},y={height},relative=false,window="{address}"}}))',f'hl.dispatch(hl.dsp.window.move({{x=0,y=0,relative=false,window="{address}"}}))']:
                      subprocess.run(['hyprctl','eval',expression],check=True)
                  time.sleep(1)
              def targets():return [t for t in request(port,'/json/list') if t['type']=='page']
              def evaluate(t,js):return json.loads(subprocess.check_output(['node',str(ROOT/'tests/noventra/cdp_eval.mjs'),t['webSocketDebuggerUrl'],js],text=True,timeout=25))
              def activate(t):request(port,'/json/activate/'+t['id']);time.sleep(.6)
              def close(t):
                  request(port,'/json/close/'+t['id'])
                  wait_for(lambda:all(x['id']!=t['id'] for x in targets()),p)
                  time.sleep(.3)
              def navigate(t,path):
                  evaluate(t,'location.href='+json.dumps(url+path));time.sleep(1)
                  wait_for(lambda:evaluate(t,'location.href')==url+path,p)
              def latest_new(before):
                  wait_for(lambda:any(t['id'] not in before for t in targets()),p)
                  return next(t for t in targets() if t['id'] not in before)
              if restart:
                wait_for(lambda:len(targets())>=2,p)
                for title,value in [('Personal-page','personal'),('Work-page','work')]:
                  t=next(t for t in targets() if title in t['url']);activate(t)
                  wait_for(lambda:evaluate(t,'document.readyState')=='complete',p)
                  state=evaluate(t,READ);result['restored-'+value]=state
                  assert state['cookie']=='account='+value and state['local']==value and state.get('session')==value,state
                  assert state['cache']==[value] and state['idb']==[value],state
                result['checks'].append('Browser restart preserves each space cookie jar, localStorage, IndexedDB and CacheStorage')
                settings=request(port,'/json/new?'+urllib.parse.quote('chrome://settings/clearBrowserData',safe=''),'PUT');time.sleep(2)
                result['clear-data']=evaluate(settings,'''(async()=>{const {sendWithPromise}=await import('chrome://resources/js/cr.js');await sendWithPromise('initializeClearBrowsingData');return await sendWithPromise('clearBrowsingData',['browser.clear_data.cookies','browser.clear_data.cache'],4)})()''')
                close(settings)
                for t in targets():
                  if '/page?' not in t['url']:continue
                  state=evaluate(t,READ)
                  assert not state['cookie'] and 'local' not in state and not state['cache'] and not state['idb'] and state['workers']==0,state
                result['checks'].append('Clear browsing data removes cookies and site storage from Personal and Work')
              else:
                workers=wait_for(lambda:[t for t in request(port,'/json/list') if t['url'].endswith('/worker.js') and t['url'].startswith('chrome-extension://')],p)
                fixture_origin=workers[0]['url'].rsplit('/',1)[0]
                if args.dock_layout:apply_native_test_layout(port,visible=('tabs','spaces'))
                personal=next(t for t in targets() if 'Personal-page' in t['url'])
                assert evaluate(personal,write_state('personal'))
                before={t['id'] for t in targets()}
                if args.platform=='wayland':
                    result['private-monitors']=json.loads(subprocess.check_output(['hyprctl','-j','monitors'],text=True))
                    result['private-clients']=json.loads(subprocess.check_output(['hyprctl','-j','clients'],text=True))
                    result['outer-height']=evaluate(personal,'outerHeight')
                    native_height=result['outer-height']
                activate(personal);time.sleep(1)
                click(80,evaluate(personal,'outerHeight')-52);menu('New account space','spaces-menu')
                capture('create-space');time.sleep(1)
                if args.platform=='wayland' and args.basic:click(evaluate(personal,'outerWidth')/2+163,324)
                else:chord(65293)
                work=latest_new(before);navigate(work,'/page?title=Work-page')
                empty=evaluate(work,READ);result['new-space']=empty
                assert not empty['cookie'] and 'local' not in empty and not empty['cache'] and not empty['idb'] and empty['workers']==0,empty
                assert evaluate(work,write_state('work'))
                endpoint=request(port,'/json/version')['webSocketDebuggerUrl']
                result['personal-notification-permission']=evaluate(personal,'Notification.permission')
                assert evaluate(personal,"navigator.permissions.query({name:'notifications'}).then(p=>p.state)")=='granted'
                assert evaluate(work,'Notification.permission')=='denied'
                assert evaluate(work,"(async()=>{try{await(await navigator.serviceWorker.ready).showNotification('Must not display');return false}catch(e){return e.name==='TypeError'}})()")
                result['checks'].append('Additional-space notifications fail closed even when the shared profile permission is granted')
                result['checks'].append('Native space creation opens an independent cookie, localStorage, IndexedDB, CacheStorage and service-worker partition')
                activate(work);click(80,evaluate(work,'outerHeight')-52);menu('Personal','choose-personal',True)
                wait_for(lambda:evaluate(personal,'document.visibilityState')=='visible',p)
                assert evaluate(personal,'window.loadedToken')=='personal live'
                assert evaluate(personal,READ)['cookie']=='account=personal'
                capture('personal-visible')
                activate(personal);click(80,evaluate(personal,'outerHeight')-52);menu('Work','choose-work',True)
                wait_for(lambda:evaluate(work,'document.visibilityState')=='visible',p)
                assert evaluate(work,'window.loadedToken')=='work live'
                assert evaluate(work,READ)['draft']=='work draft'
                result['checks'].append('Same-window switching preserves loaded documents and unfinished form edits')
                # Native new tab must inherit Work even through the privileged new-tab page.
                before={t['id'] for t in targets()}
                if args.platform=='wayland' and args.basic:click(75,130)
                else:chord(ord('t'),(65507,))
                new=latest_new(before)
                wait_for(lambda:evaluate(new,'document.readyState')=='complete',p)
                result['new-tab-page']=evaluate(new,'({url:location.href,text:document.body.innerText})')
                assert 'chrome-error' not in result['new-tab-page']['url'],result['new-tab-page']
                navigate(new,'/page?title=Work-new-tab');assert evaluate(new,READ)['cookie']=='account=work'
                result['checks'].append(('Native New tab action' if args.basic else 'Ctrl+T')+' and subsequent navigation keep the current account space')
                close(new);activate(work)
                if not args.basic:
                    # A normal middle-click follows the source space through a redirect.
                    point=evaluate(work,'(()=>{const r=document.querySelector("#link").getBoundingClientRect();return [r.x+15,r.y+8]})()')
                    before={t['id'] for t in targets()};click(point[0]+232,point[1]+(evaluate(work,'outerHeight-innerHeight') if args.dock_layout else 0),2)
                    child=latest_new(before);time.sleep(1)
                    assert evaluate(child,READ)['cookie']=='account=work';close(child)
                    result['checks'].append('Middle-click links and redirects inherit the source space')
                    # Duplicate, close and reopen are native tab menu / shortcut paths.
                    activate(work);before={t['id'] for t in targets()};click(70,14 if args.dock_layout else 100,3);menu('Duplicate','duplicate-menu')
                    duplicate=latest_new(before);time.sleep(1)
                    assert evaluate(duplicate,READ)['cookie']=='account=work'
                    close(duplicate);activate(work)
                    before={t['id'] for t in targets()};chord(ord('t'),(65507,65505));reopened=latest_new(before);time.sleep(1)
                    reopened_state=evaluate(reopened,READ);assert reopened_state['cookie']=='account=work' and reopened_state.get('session')=='work',reopened_state
                    close(reopened)
                    result['checks'].append('Duplicate and reopen-closed-tab retain account identity')
                    # Numbered shortcuts and bulk close must stay in Work.
                    activate(work);before={t['id'] for t in targets()}
                    chord(ord('t'),(65507,));other=latest_new(before)
                    navigate(other,'/page?title=Work-other')
                    chord(ord('1'),(65507,))
                    assert evaluate(personal,'document.visibilityState')=='hidden'
                    wait_for(lambda:evaluate(work,'document.visibilityState')=='visible',p)
                    click(70,14 if args.dock_layout else 100,3);menu('Close other tabs','close-other-menu')
                    wait_for(lambda:all(t['id']!=other['id'] for t in targets()),p)
                    assert evaluate(personal,'window.loadedToken')=='personal live'
                    assert evaluate(personal,READ)['draft']=='personal draft'
                    result['checks'].append('Numbered shortcuts and Close other tabs stay in the visible account space')
                    assert {t['id'] for t in targets()}=={personal['id'],work['id']},targets()
                    # Exercise extension entry points and an interleaved native range selection.
                    extension=request(port,'/json/new?'+urllib.parse.quote(fixture_origin+'/test.html',safe=''),'PUT');time.sleep(1)
                    api='''(async()=>{const all=await chrome.tabs.query({});const personal=all.find(t=>t.url.includes('Personal-page'));const work=all.find(t=>t.url.includes('Work-page'));let rejected=false;try{await chrome.tabs.createSplit({tabIds:[personal.id,work.id]})}catch(e){rejected=true};const group=await chrome.tabs.group({tabIds:[personal.id]});await chrome.tabs.group({groupId:group,tabIds:[work.id]});const after=await chrome.tabs.get(work.id);await chrome.tabs.ungroup(personal.id);return {rejected,workGroup:after.groupId,personal:personal.id,work:work.id}})()'''
                    guarded=evaluate(extension,api);assert guarded['rejected'] and guarded['workGroup']==-1,guarded
                    activate(work);before={t['id'] for t in targets()};chord(ord('t'),(65507,));other=latest_new(before);navigate(other,'/page?title=Work-range')
                    evaluate(extension,f"chrome.tabs.get({guarded['work']}).then(w=>chrome.tabs.move({guarded['personal']},{{index:w.index+1}}))")
                    activate(work);click(70,44 if args.dock_layout else 130,1,(65505,))
                    highlighted=evaluate(extension,"chrome.tabs.query({highlighted:true}).then(t=>t.map(x=>x.id))")
                    assert guarded['personal'] not in highlighted and len(highlighted)==2,highlighted
                    activate(work)
                    move_check='''(async()=>{const all=await chrome.tabs.query({});const personal=all.find(t=>t.url.includes('Personal-page'));const other=all.find(t=>t.url.includes('Work-range'));const clone=await chrome.tabs.duplicate(personal.id);await chrome.tabs.group({tabIds:[personal.id,clone.id]});await chrome.windows.create({tabId:other.id});const target=await chrome.tabs.get(personal.id);let rejected=false;try{await chrome.tabs.move(other.id,{windowId:target.windowId,index:target.index+1})}catch(e){rejected=true;await chrome.tabs.move(other.id,{windowId:target.windowId,index:-1})};const after=await chrome.tabs.get(other.id);await chrome.tabs.remove(clone.id);await chrome.tabs.ungroup(personal.id);return {rejected,group:after.groupId,window:after.windowId,expectedWindow:target.windowId}})()'''
                    moved=evaluate(extension,move_check)
                    assert moved['group']==-1 and moved['window']==moved['expectedWindow'],moved
                    assert evaluate(other,READ)['cookie']=='account=work'
                    result['checks'].append('Cross-window extension moves preserve account identity; invalid grouped destinations are rejected')
                    activate(work);close(other);close(extension)
                    assert evaluate(personal,'window.loadedToken')=='personal live'
                    result['checks'].append('Mixed-account split/group APIs are guarded and range selection skips hidden account tabs')
                    capture('work-visible')
                    settings=request(port,'/json/new?'+urllib.parse.quote('chrome://settings/appearance',safe=''),'PUT');time.sleep(1)
                    assert evaluate(settings,'new Promise(r=>chrome.settingsPrivate.setPref("noventra.browser.tab_letters",true,"",ok=>r(!!ok)))')
                    close(settings);activate(work)
                    click(70,14 if args.dock_layout else 100,3);menu('Pin','pin-menu');time.sleep(1)
                    icon_shot=Image.open(capture('pinned-icon')).convert('RGB')
                    green=sum(1 for r,g,b in icon_shot.crop((0,0,230,44) if args.dock_layout else (0,84,230,130)).getdata() if g>140 and r<80 and b<100)
                    assert green>30,('Pinned favicon missing',green)
                    result['checks'].append('Pinned site retains its full-color favicon')
                # Enable session restore through the actual shared Settings page.
                settings=request(port,'/json/new?'+urllib.parse.quote('chrome://settings/onStartup',safe=''),'PUT');time.sleep(1)
                assert evaluate(settings,'new Promise(r=>chrome.settingsPrivate.setPref("session.restore_on_startup",1,"",ok=>r(!!ok)))')
                close(settings);activate(work);time.sleep(2)
              # Normal browser shutdown writes the real session, not a synthetic fixture.
              endpoint=request(port,'/json/version')['webSocketDebuggerUrl']
              subprocess.run(['node','-e',"const s=new WebSocket(process.argv[1]);s.onopen=()=>s.send(JSON.stringify({id:1,method:'Browser.close'}));s.onclose=()=>process.exit(0);setTimeout(()=>process.exit(1),10000);",endpoint],check=True,timeout=15)
              p.wait(timeout=15)
            finally:stop_browser(p)
        result['passed']=True
    except Exception as error:
        result['error']=repr(error);capture('failure');raise
    finally:
        server.shutdown();server.server_close()
        if args.platform=='x11':x.XCloseDisplay(d)
        else:pointer.terminate();pointer.wait(timeout=5)
        args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
