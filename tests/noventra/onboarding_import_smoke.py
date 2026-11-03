"""Import owned HTML/CSV fixtures using real native file pickers on X11/Sway."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from interface_polish_smoke import DEEP, native_key
from layout_test_support import LayoutBrowser


def focus_picker(inp):
    if inp.platform!='x11':return
    # Xvfb has no window manager to focus the newly mapped GTK modal.
    p=inp.pointer
    x=p.x
    x.XDefaultRootWindow.argtypes=[C.c_void_p];x.XDefaultRootWindow.restype=C.c_ulong
    x.XQueryTree.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_ulong),C.POINTER(C.POINTER(C.c_ulong)),C.POINTER(C.c_uint)]
    x.XGetTransientForHint.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong)]
    x.XSetInputFocus.argtypes=[C.c_void_p,C.c_ulong,C.c_int,C.c_ulong]
    x.XRaiseWindow.argtypes=[C.c_void_p,C.c_ulong]
    x.XGetGeometry.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    x.XMoveResizeWindow.argtypes=[C.c_void_p,C.c_ulong,C.c_int,C.c_int,C.c_uint,C.c_uint]
    x.XFree.argtypes=[C.c_void_p]
    root,parent=C.c_ulong(),C.c_ulong();children=C.POINTER(C.c_ulong)();count=C.c_uint()
    assert x.XQueryTree(p.d,x.XDefaultRootWindow(p.d),C.byref(root),C.byref(parent),C.byref(children),C.byref(count))
    try:
        dialogs=[]
        for i in range(count.value):
            owner=C.c_ulong()
            if x.XGetTransientForHint(p.d,children[i],C.byref(owner)) and owner.value:
                dialogs.append(children[i])
        assert dialogs,'Native file picker did not appear'
        def geometry(window):
            owner=C.c_ulong();left,top=C.c_int(),C.c_int()
            width,height,border,depth=C.c_uint(),C.c_uint(),C.c_uint(),C.c_uint()
            assert x.XGetGeometry(p.d,window,C.byref(owner),C.byref(left),C.byref(top),C.byref(width),C.byref(height),C.byref(border),C.byref(depth))
            return left.value,top.value,width.value,height.value
        # Bare Xvfb has no window manager to constrain GTK's requested size.
        # Keep the owned transient's actual confirmation buttons on screen.
        _,_,screen_width,screen_height=geometry(root.value)
        left,top,width,height=geometry(dialogs[-1])
        fitted_width,fitted_height=min(width,screen_width),min(height,screen_height)
        fitted=(max(0,min(left,screen_width-fitted_width)),max(0,min(top,screen_height-fitted_height)),fitted_width,fitted_height)
        if fitted!=(left,top,width,height):
            x.XMoveResizeWindow(p.d,dialogs[-1],*fitted)
        x.XRaiseWindow(p.d,dialogs[-1]);x.XSetInputFocus(p.d,dialogs[-1],2,0);x.XFlush(p.d)
    finally:x.XFree(children)
    time.sleep(.2)
    return dialogs[-1]


def type_path(inp, path):
    picker=focus_picker(inp)
    if inp.platform == 'wayland':
        keyboard=subprocess.Popen([os.environ['NOVENTRA_TEST_KEYBOARD']],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
        try:
            assert keyboard.stdout.readline().strip()=='ready'
            for code,down in ((29,1),(38,1),(38,0),(29,0)):
                keyboard.stdin.write(f'key {code} {down}\n');keyboard.stdin.flush()
                assert keyboard.stdout.readline().strip()=='ok'
            time.sleep(.5)
        finally:
            keyboard.stdin.close();keyboard.wait(timeout=5)
        subprocess.run(['wtype', '-d', '100', str(path)], check=True)
    else:
        native_key(inp,'l',('Control_L',))
        inp.pointer.click(300,20)
        # GTK's Home picker can prefill "."; replace it with the absolute path.
        native_key(inp,'a',('Control_L',))
        keys = {'/':'slash','.':'period','-':'minus','_':'underscore'}
        for char in str(path):
            native_key(inp,keys.get(char,char.lower()),('Shift_L',) if char.isupper() or char=='_' else ())
    if inp.platform=='x11':
        p=inp.pointer;x=p.x
        x.XGetGeometry.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
        root=C.c_ulong();left,top=C.c_int(),C.c_int();width,height,border,depth=C.c_uint(),C.c_uint(),C.c_uint(),C.c_uint()
        assert x.XGetGeometry(p.d,picker,C.byref(root),C.byref(left),C.byref(top),C.byref(width),C.byref(height),C.byref(border),C.byref(depth))
        # GTK deliberately defaults to Cancel; Return does not accept the file.
        p.click(left.value+width.value-52,top.value+height.value-23)
    else:
        inp.pointer.click(1150,23)
    time.sleep(.7)


def gesture(page, expression):
    # A genuine user-gesture evaluation allows the same window.open action as
    # clicking the button; scope is the owned browser's local CDP endpoint.
    script = '''const [endpoint,expression]=process.argv.slice(1);const socket=new WebSocket(endpoint);
    const value=await new Promise((resolve,reject)=>{const t=setTimeout(()=>reject(Error('timeout')),15000);
    socket.onopen=()=>socket.send(JSON.stringify({id:1,method:'Runtime.evaluate',params:{expression,userGesture:true,returnByValue:true}}));
    socket.onmessage=e=>{const m=JSON.parse(e.data);if(m.id===1){clearTimeout(t);if(m.result.exceptionDetails)reject(Error(JSON.stringify(m.result.exceptionDetails)));else resolve(m.result.result.value)}}});
    socket.close();process.stdout.write(JSON.stringify(value));'''
    return json.loads(subprocess.check_output(['node','--input-type=module','-e',script,page['webSocketDebuggerUrl'],expression],text=True,timeout=20))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    # Prevent portal requests from reaching the user's desktop bus. The native
    # picker falls back to GTK on this owned display.
    os.environ['DBUS_SESSION_BUS_ADDRESS']='unix:path=/tmp/noventra-no-session-bus'
    os.environ['GDK_BACKEND']='wayland' if os.environ.get('NOVENTRA_PRIVATE_WAYLAND')=='1' else 'x11'
    result={'passed':False,'checks':[]}
    try:
        with tempfile.TemporaryDirectory(prefix='nv-import-',dir='/tmp') as temp:
            temp=Path(temp)
            html=temp/'bookmarks.html';html.write_text('''<!DOCTYPE NETSCAPE-Bookmark-file-1>
<TITLE>Bookmarks</TITLE><H1>Bookmarks</H1><DL><p>
<DT><A HREF="https://noventra-fixture.invalid/bookmark">Noventra owned import fixture</A>
</DL><p>''')
            csv=temp/'passwords.csv';csv.write_text('name,url,username,password\nFixture,https://noventra-fixture.invalid/,fixture-user,fixture-only-password\n')
            conflict=temp/'conflict.csv';conflict.write_text('name,url,username,password\nFixture,https://noventra-fixture.invalid/,fixture-user,fixture-replacement-password\n')
            invalid=temp/'invalid.csv';invalid.write_text('this,is,not,a,password,export\ninvalid,row\n')
            with LayoutBrowser(args.browser,temp/'profile',args.output,extra_args=('--password-store=basic','--ui-toolkit=gtk')) as b:
                inp=Input(b.platform)
                try:
                    pref=b.ev('new Promise(r=>chrome.settingsPrivate.getPref("credentials_enable_service",r))')
                    assert pref['value'] is False and pref.get('enforcement')!='ENFORCED',pref
                    result['checks'].append('Local password saving stays optional and is not forcibly disabled')
                    page=request(b.port,'/json/new?chrome://setup/','PUT');time.sleep(2)
                    b.ev('document.querySelector("#continue").click();true',page);time.sleep(.3)
                    wait_for(lambda:b.ev('!document.querySelector("#bookmark-file").disabled',page),b.process)
                    gesture(page,'document.querySelector(".file-fallbacks>summary").click();true')
                    gesture(page,'document.querySelector("#bookmark-file").click();true');time.sleep(.7)
                    type_path(inp,html)
                    inp.capture(args.output/"bookmark-picker-after.png")
                    wait_for(lambda:b.ev('document.querySelector(".import-message").innerText.includes("finished")',page),b.process)
                    bookmarks=request(b.port,'/json/new?chrome://bookmarks/','PUT');time.sleep(1)
                    found=b.ev('new Promise(r=>chrome.bookmarks.search({title:"Noventra owned import fixture"},r))',bookmarks)
                    assert any(x['url']=='https://noventra-fixture.invalid/bookmark' for x in found)
                    result['checks'].append('HTML bookmarks reach the actual bookmark model')
                    request(b.port,'/json/activate/'+page['id'])
                    gesture(page,'document.querySelector("#bookmark-file").click();true');time.sleep(.6)
                    focus_picker(inp)
                    native_key(inp,'Escape');time.sleep(.3)
                    assert b.ev('!document.querySelector("#continue").disabled',page)
                    result['checks'].append('Canceling a bookmark file picker leaves Continue usable')
                    gesture(page,'document.querySelector("#password-import").click();true')
                    manager=wait_for(lambda:next((p for p in request(b.port,'/json/list') if 'password-manager' in p['url']),None),b.process)
                    time.sleep(2)
                    # The manager may present Select file instead of auto-opening
                    # a picker while its preferences and route initialize.
                    gesture(manager,'(()=>{'+DEEP+'const p=deep(document,"passwords-importer");const button=p.shadowRoot.querySelector("#selectFileButtonLinkRow");if(p.dialogState_===0&&button&&!button.hidden)button.click();return true})()')
                    time.sleep(.7)
                    type_path(inp,csv)
                    def imported():
                        return b.ev('(()=>{'+DEEP+'const p=deep(document,"passwords-importer");return p?.results_?.numberImported===1})()',manager)
                    wait_for(imported,b.process)
                    inp.capture(args.output/'password-import-success.png')
                    result['checks'].append('Password CSV importer reports one imported fixture entry')
                    b.ev('(()=>{'+DEEP+'deep(document,"passwords-importer").shadowRoot.querySelector("#closeButton").click();return true})()',manager);time.sleep(.4)
                    def saved_fixture_unchanged():
                        return b.ev('(async()=>{const rows=await chrome.passwordsPrivate.getSavedPasswordList();const entry=rows.find(e=>e.username==="fixture-user");return !!entry && await chrome.passwordsPrivate.requestPlaintextPassword(entry.id,chrome.passwordsPrivate.PlaintextReason.VIEW)==="fixture-only-password"})()',manager)
                    assert saved_fixture_unchanged()
                    b.ev('(()=>{'+DEEP+'deep(document,"passwords-importer").shadowRoot.querySelector("#selectFileButtonLinkRow").click();return true})()',manager)
                    time.sleep(.6);type_path(inp,conflict)
                    wait_for(lambda:b.ev('(()=>{'+DEEP+'return !!deep(document,"passwords-importer").shadowRoot.querySelector("#skipButton")})()',manager),b.process)
                    b.ev('(()=>{'+DEEP+'deep(document,"passwords-importer").shadowRoot.querySelector("#cancelButton").click();return true})()',manager);time.sleep(.4)
                    assert saved_fixture_unchanged()
                    result['checks'].append('Duplicate password conflicts require a choice and cancellation preserves the saved entry')
                    b.ev('(()=>{'+DEEP+'deep(document,"passwords-importer").shadowRoot.querySelector("#selectFileButtonLinkRow").click();return true})()',manager)
                    time.sleep(.6);type_path(inp,invalid)
                    wait_for(lambda:b.ev('(()=>{'+DEEP+'return !!deep(document,"passwords-importer").shadowRoot.querySelector("#selectFileButtonError")})()',manager),b.process)
                    inp.capture(args.output/'password-import-error.png')
                    result['checks'].append('Invalid password CSV shows a recoverable native error')
                    b.ev('(()=>{'+DEEP+'deep(document,"passwords-importer").shadowRoot.querySelector("#closeButton").click();return true})()',manager)
                    request(b.port,'/json/activate/'+page['id']);time.sleep(.3)
                    # Browser import has its own empty status div. Check the
                    # CSV status paragraph and its actual destination tab.
                    assert 'opened in another tab' in b.ev('document.querySelector("p.import-message[role=status]").innerText',page)
                    assert any(p['id'] == manager['id'] and 'password-manager' in p['url'] for p in request(b.port, '/json/list'))
                    assert saved_fixture_unchanged()
                    b.ev('document.querySelector("#continue").click();true',page);time.sleep(.3)
                    assert b.ev('!!document.querySelector("#install-bitwarden")',page)
                    b.ev('document.querySelector("#continue").click();true',page);time.sleep(.3)
                    assert b.ev('document.querySelector("#step-title").innerText',page)=='Choose your search engine.'
                    b.ev('document.querySelector("#continue").click();true',page);time.sleep(.3)
                    assert b.ev('document.querySelector("#step-title").innerText',page)=='You’re ready to browse.'
                    summary=b.ev('document.querySelector(".panel-content").innerText',page)
                    assert b.ev('document.querySelector("#data-import-summary .summary-tag").innerText',page)=='Finished'
                    assert 'Check the results' in b.ev('document.querySelector("#password-import-summary").innerText',page)
                    result['checks'].append('Setup summary records bookmark completion and delegates password results truthfully')
                    inp.capture(args.output/'setup-ready.png')
                finally:inp.close()
        result['passed']=True
    finally:(args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
