"""Native edge docking preserves the page and moves working navigation buttons."""
import argparse
from copy import deepcopy
import ctypes as C
import http.server
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from layout_test_support import LayoutBrowser
from native_ui_tree import NativeUITree
from extension_smoke import request

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_):pass
    def do_GET(self):
        data=('<title>Dock '+self.path+'</title><style>body{margin:0;background:#e8edf2;font:24px sans-serif}</style><h1>'+self.path+'</h1><input value="Keep this draft">').encode()
        self.send_response(200);self.send_header('Content-Type','text/html');self.end_headers();self.wfile.write(data)

class Pointer:
    def __init__(self):
        self.x=C.CDLL('libX11.so.6');self.xt=C.CDLL('libXtst.so.6')
        self.x.XOpenDisplay.argtypes=[C.c_char_p];self.x.XOpenDisplay.restype=C.c_void_p
        self.x.XFlush.argtypes=[C.c_void_p];self.x.XCloseDisplay.argtypes=[C.c_void_p]
        self.d=self.x.XOpenDisplay(None);assert self.d
        self.xt.XTestFakeMotionEvent.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
    def click(self,x,y):
        self.xt.XTestFakeMotionEvent(self.d,-1,int(x),int(y),0)
        self.xt.XTestFakeButtonEvent(self.d,1,1,0);self.xt.XTestFakeButtonEvent(self.d,1,0,0)
        self.x.XFlush(self.d);time.sleep(.4)
    def close(self):self.x.XCloseDisplay(self.d)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--browser',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    result={'passed':False,'checks':[]}
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
    pointer=None
    try:
        with tempfile.TemporaryDirectory(prefix='profile-',dir=a.output) as tmp:
            with LayoutBrowser(a.browser,Path(tmp),a.output, ['--force-renderer-accessibility']) as b:
                pointer=Pointer();tree=NativeUITree(b);base=f'http://127.0.0.1:{server.server_port}'
                page=request(b.port,'/json/new?'+base+'/first','PUT');time.sleep(.5)
                pointer.click(600,400)  # Real interaction makes the first history entry eligible for toolbar Back.
                b.ev('location.href='+json.dumps(base+'/second'),page);time.sleep(.5)
                b.ev('window.layoutWitness=42;document.querySelector("input").value="Draft survives";true',page)
                # Start with only a top tab row and a side address field so adding
                # top navigation and a right rail must reserve both axes.
                baseline=deepcopy(b.state()['layout'])
                for i,item in enumerate(baseline['items']):
                    item.update(order=i,edge='left' if item['id'] in ('address','spaces') else 'top',visible=item['id'] in ('tabs','address'))
                assert b.apply(baseline)['status']=='ok';time.sleep(.4)
                before=b.ev('({w:innerWidth,h:innerHeight,x:screenX,y:screenY,ow:outerWidth,oh:outerHeight})',page)
                state=b.state()['layout'];draft=deepcopy(state)
                for i,item in enumerate(draft['items']):
                    item['order']=i
                    if item['id']=='back':item['edge']='top';item['visible']=True
                    elif item['id']=='reload':item['edge']='right';item['visible']=True
                    elif item['id']=='menu':item['edge']='top';item['visible']=True
                    elif item['id'] not in ['address','tabs']:item['visible']=False
                assert b.apply(draft)['status']=='ok';time.sleep(.5)
                after=b.ev('({w:innerWidth,h:innerHeight,doc:window.layoutWitness,draft:document.querySelector("input").value})',page)
                subprocess.run(['import','-window','root',str(a.output/'docked.png')],check=True)
                assert after['w']<before['w'] and after['h']<before['h'],'Native edge docks did not reserve page space'
                assert after['doc']==42 and after['draft']=='Draft survives','Apply recreated or navigated the document'
                result['checks'].append('Native top/right docks reserve space and preserve loaded document/input')
                # Follow the actual compact header and unchanged side control bounds.
                tree.read()
                pointer.click(*tree.center(tree.find(r'class_name=ReloadButton ')))
                assert b.ev('window.layoutWitness===undefined',page),'Right-docked Reload did not reload'
                assert b.ev('location.pathname',page)=='/second'
                result['checks'].append('Right-docked native Reload works')
                tree.read();pointer.click(*tree.center(tree.find(r'class_name=BackForwardButton name=Back ')))
                path=b.ev('({path:location.pathname,history:history.length})',page)
                subprocess.run(['import','-window','root',str(a.output/'after-back.png')],check=True)
                assert path['path']=='/first',('Top-docked Back did not navigate',path)
                result['checks'].append('Top-docked native Back works')
                result['passed']=True
    except BaseException as e:result['error']=repr(e);raise
    finally:
        if pointer:pointer.close()
        server.shutdown();server.server_close();(a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
