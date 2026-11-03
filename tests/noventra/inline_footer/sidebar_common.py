"""Owned immutable-runtime acceptance helpers, 160s work + 40s evidence/cleanup."""
from copy import deepcopy
import hashlib,json,os,re,shutil,signal,sys,time
from pathlib import Path
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'tests/noventra').is_dir())
sys.path.insert(0,str(ROOT/'tests/noventra'))
import compact_chrome_smoke as native
from extension_smoke import request,wait_for
from interface_polish_smoke import native_key
from layout_editor_smoke import FIND
from native_ui_tree import NativeUITree
from sidebar_inline_address_smoke import FIELD,EDITOR,NEW_TAB,ancestor,screen_rect,resize_sidebar
from sidebar_unified_smoke import close_normally
class Budget(native.Budget):
 def __init__(self):
  super().__init__();self.work_deadline=self.started+160;self.deadline=self.started+200;self.arm(self.work_deadline)
def sha(path):
 with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def write(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def admit(args,source):
 owner=os.environ.get('NOVENTRA_BENCH_XVFB_PID','');assert owner.isdigit() and Path('/proc',owner,'comm').read_text().strip()=='Xvfb'
 assert Path(os.environ['XAUTHORITY']).resolve().is_relative_to(ROOT/'work/ram-investigation')
 args.browser=args.browser.resolve(strict=True);assert sha(args.browser)==args.expected_binary;assert sha(args.browser.parent/'resources.pak')==args.expected_resources
 args.output.mkdir(parents=True,exist_ok=False);shutil.copyfile(source,args.output/'executed-test.py');shutil.copyfile(__file__,args.output/'executed-common.py')
 return {'passed':False,'checks':[],'normal_shutdowns':[],'runtime':str(args.browser),'binary_sha256':sha(args.browser),'resources_sha256':sha(args.browser.parent/'resources.pak'),'test_sha256':sha(source),'common_sha256':sha(__file__)}
def check(result,name,value,actual=None):
 row={'name':name,'passed':bool(value),'actual':actual};result['checks'].append(row);assert value,row

def signature(layout):return sorted((i['id'],i['edge'],i['order'],i['visible']) for i in layout['items'])
def order(layout,edge):return [i['id'] for i in sorted(layout['items'],key=lambda i:i['order']) if i['visible'] and i['edge']==edge]
def byid(layout):return {i['id']:i for i in layout['items']}
def rect(tree,pattern):return screen_rect(tree,native.one(tree.read(),pattern))
def ui(browser,js):return browser.ev('(()=>{'+FIND+js+'})()')
def activate_settings(browser):
 request(browser.port,'/json/activate/'+browser.settings['id']);wait_for(lambda:browser.ev('document.visibilityState==="visible"'),browser.process)
def prefs(profile,layout=None,width=None,side='left',markers=None,collapsed=False):
 value={'helium':{'browser':{'layout':2,'vertical_right_aligned':side=='right','zen_mode':True,'zen_mode_sidebar_pinned':True,'zen_mode_top_chrome_pinned':True}},'vertical_tabs':{'collapsed_state':collapsed}}
 if width is not None:value['vertical_tabs']['uncollapsed_width']=width
 if layout is not None:value['noventra']={'browser':{'dock_layout':deepcopy(layout),**(markers or {})}}
 (profile/'Default').mkdir(parents=True);write(profile/'Default/Preferences',value)
def cli(description):
 import argparse
 p=argparse.ArgumentParser(description=description);p.add_argument('--browser',type=Path,required=True);p.add_argument('--expected-binary',required=True);p.add_argument('--expected-resources',required=True);p.add_argument('--output',type=Path,required=True);return p
from contextlib import contextmanager
@contextmanager
def session(args,profile,output,result,budget,flags=None):
 output.mkdir(parents=True,exist_ok=True);pending=None
 with native.session(args,profile,output,flags or native.FLAGS,result,budget) as (b,t,inp):
  try:yield b,t,inp
  except BaseException as error:
   pending=error;result['live_error']=repr(error)
   try:native.capture(t,inp,output,'preclose-failure')
   except BaseException as capture_error:result.setdefault('capture_errors',[]).append(repr(capture_error))
   native.diagnostics(b,t,inp,output,result,budget)
  finally:result['normal_shutdowns'].append({'phase':output.name,'exit':close_normally(b)})
 if pending is not None:raise pending

def click_dom(b,t,inp,result,selector,scope='editor'):
 activate_settings(b)
 code='''const root=SCOPE==='editor'?editor.shadowRoot:deep(document,'settings-appearance-page').shadowRoot;
 const e=root.querySelector(SELECTOR);if(!e||e.disabled||!e.checkVisibility())return {error:'missing/hidden/disabled'};
 e.scrollIntoView({block:'center',behavior:'instant'});const r=e.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2,w:innerWidth,h:innerHeight};'''.replace('SCOPE',json.dumps(scope)).replace('SELECTOR',json.dumps(selector))
 point=ui(b,code);assert not point.get('error'),(selector,point)
 t.read();matches=[screen_rect(t,n) for n in native.visible(t,r'class_name=ContentsContainerView ') if abs(n['bounds'][2]-point['w'])<=2 and abs(n['bounds'][3]-point['h'])<=2]
 assert len(matches)==1,(selector,point,matches);view=matches[0];assert 0<point['x']<view['w'] and 0<point['y']<view['h'],point
 result.setdefault('native_clicks',[]).append({'selector':selector,'scope':scope,'dom':point,'native':view});inp.pointer.click(view['x']+point['x'],view['y']+point['y']);time.sleep(.15)

def show_layout(b,t,inp,r):
 click_dom(b,t,inp,r,'[data-section="customizeLayout"]','page');wait_for(lambda:ui(b,'return !!editor?.draft && editor.checkVisibility();'),b.process)
def choose(b,t,inp,r,name,apply=True):
 click_dom(b,t,inp,r,'[data-preset="'+name+'"]');draft=ui(b,'return editor.draft.current;')
 if apply and ui(b,'return editor.dirty();'):
  click_dom(b,t,inp,r,'#applyLayout');wait_for(lambda:not ui(b,'return editor.dirty() || editor.applying;'),b.process,timeout=8)
 if apply:check(r,name+' native Apply persists exact draft',signature(b.state()['layout'])==signature(draft))
 return draft
