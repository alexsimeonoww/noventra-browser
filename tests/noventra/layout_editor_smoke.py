"""Exercise the real Settings draft editor and its atomic native Apply boundary."""
import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from layout_test_support import LayoutBrowser
from extension_smoke import ROOT, request

FIND='''function deep(root,selector){const match=root.querySelector(selector);if(match)return match;for(const el of root.querySelectorAll('*')){if(el.shadowRoot){const nested=deep(el.shadowRoot,selector);if(nested)return nested;}}return null;}const editor=deep(document,'noventra-layout-editor');'''

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--browser',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True);result={'passed':False,'checks':[]}
    try:
      with tempfile.TemporaryDirectory(prefix='profile-',dir=a.output) as tmp:
        with LayoutBrowser(a.browser,Path(tmp),a.output) as b:
          def ui(js):return b.ev('(()=>{'+FIND+js+'})()')
          assert ui('return !!editor;'),'Visual layout editor is missing from Customize Noventra'
          def capture(name):
              if b.platform=='x11':subprocess.run(['import','-window','root',str(a.output/name)],check=True)
              else:
                  cmd=['swaymsg','-t','get_outputs','-r'] if os.environ.get('NOVENTRA_TEST_COMPOSITOR')=='sway' else ['hyprctl','-j','monitors']
                  monitor=json.loads(subprocess.check_output(cmd,text=True))[0]
                  subprocess.run(['grim','-o',monitor['name'],str(a.output/name)],check=True,timeout=8)
          assert ui('return !editor.shadowRoot.querySelector("#moveTo") && !editor.shadowRoot.querySelector(".inspector");'), 'Lower placement menu was not removed'
          assert any(c['id']=='action:settings' for c in b.state()['capabilities']), 'Settings is missing from the native catalog'
          assert ui('const input=editor.shadowRoot.querySelector(".control-search");input.value="settings";input.dispatchEvent(new Event("input",{bubbles:true}));return true;')
          time.sleep(.1)
          assert ui('return [...editor.shadowRoot.querySelectorAll(".tray [data-control]")].map(e=>e.dataset.control);')==['action:settings']
          ui('const input=editor.shadowRoot.querySelector(".control-search");input.value="";input.dispatchEvent(new Event("input",{bubbles:true}));return true;')
          time.sleep(.1)
          saved=b.state()['layout']
          assert ui('return [...editor.shadowRoot.querySelectorAll(".zone[data-edge]")].map(e=>e.dataset.edge);')==['top','left','right']
          assert ui('return [...editor.shadowRoot.querySelectorAll("[data-preset]")].map(e=>e.dataset.preset);')==['workspace','sidebar-top','minimal-left','minimal-right','classic-top']
          assert ui('return !editor.shadowRoot.querySelector("[data-position-edge=bottom],[data-quick-edge=bottom],[data-place-edge=bottom],[data-preset=bottom-controls]");')
          def click(selector):
              assert ui('const el=editor.shadowRoot.querySelector('+json.dumps(selector)+');if(!el)return false;el.click();return true;')
              time.sleep(.15)
          def move(edge):
              click('[data-place-edge="'+edge+'"]')
              assert ui('return editor.shadowRoot.querySelector('+json.dumps('[data-place-edge="'+edge+'"]')+').getAttribute("aria-pressed")==="true";')
              time.sleep(.15)
          def placement(name):return ui('return editor.shadowRoot.querySelector('+json.dumps('[data-control="'+name+'"]')+').closest("[data-edge]")?.dataset.edge || "tray";')
          click('[data-control="action:settings"]');move('left')
          assert placement('action:settings')=='left' and b.state()['layout']==saved
          click('#hideControl');assert placement('action:settings')=='tray'
          click('#cancelLayout')
          result['checks'].append('Settings is optional and edge buttons reflect its draft placement')
          click('[data-control="back"]');before=placement('back');move('right')
          assert placement('back')=='right' and b.state()['layout']==saved
          click('#undoLayout');assert placement('back')==before
          click('#redoLayout');assert placement('back')=='right'
          result['checks'].append('Keyboard movement and Undo/Redo affect only the draft')
          click('[data-control="home"]')
          for edge in ('top','left','right'):
              move(edge);assert placement('home')==edge
          click('#hideControl');assert placement('home')=='tray'
          assert b.state()['layout']==saved
          result['checks'].append('Tray controls can be added to every edge and hidden again')
          click('#resetLayout');assert b.state()['layout']==saved
          click('#cancelLayout');assert placement('back')==before and b.state()['layout']==saved
          result['checks'].append('Reset stays in the draft; Cancel restores saved placement')
          click('[data-control="back"]');move('right');click('#applyLayout');time.sleep(.6)
          applied=b.state()['layout'];assert applied['revision']==saved['revision']+1
          assert next(i for i in applied['items'] if i['id']=='back')['edge']=='right'
          result['checks'].append('Apply commits the real native layout once')
          click('[data-control="reload"]');move('right')
          remote=deepcopy(applied)
          for i,item in enumerate(remote['items']):
              item['order']=i
              if item['id']=='back':item['edge']='left'
          assert b.apply(remote)['status']=='ok';time.sleep(.3)
          click('#applyLayout');time.sleep(.3)
          assert next(i for i in b.state()['layout']['items'] if i['id']=='back')['edge']=='left'
          assert 'changed' in ui('return editor.shadowRoot.textContent;').lower()
          assert placement('reload')=='right'
          result['checks'].append('A stale draft keeps its edits and cannot overwrite another window')
          click('#cancelLayout')
          # Use real renderer input events, not synthetic DOM drag events.
          def input_steps(steps):
              subprocess.run(['node',str(ROOT/'tests/noventra/layout_editor_input.mjs'),b.settings['webSocketDebuggerUrl'],json.dumps(steps)],check=True,timeout=25)
          def rect(selector):
              return ui('const el=editor.shadowRoot.querySelector('+json.dumps(selector)+');el.scrollIntoView({block:"nearest"});const r=el.getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height};')
          def drag(source,destination,cancel=False):
              start=rect(source);end=rect(destination)
              # Re-read after destination scrolling; both zones must fit in the viewport.
              start=rect(source);end=ui('const r=editor.shadowRoot.querySelector('+json.dumps(destination)+').getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height};')
              result.setdefault('drags',[]).append({'source':source,'destination':destination,'start':start,'end':end,'viewport':b.ev('({w:innerWidth,h:innerHeight})')})
              capture('before-drag.png')
              x,y=start['x']+start['w']/2,start['y']+start['h']/2
              ex,ey=end['x']+end['w']/2,end['y']+end['h']/2
              steps=[{'method':'Input.dispatchMouseEvent','params':{'type':'mouseMoved','x':x,'y':y}}, {'delay':200}, {'method':'Input.dispatchMouseEvent','params':{'type':'mousePressed','x':x,'y':y,'button':'left','clickCount':1}}, {'delay':120}]
              for i in range(1,16):steps.extend([{'method':'Input.dispatchMouseEvent','params':{'type':'mouseMoved','x':x+(ex-x)*i/15,'y':y+(ey-y)*i/15,'button':'left','buttons':1}},{'delay':25}])
              steps.extend([{'delay':250},{'method':'Input.dispatchMouseEvent','params':{'type':'mouseMoved','x':ex,'y':ey,'button':'left','buttons':1}},{'delay':150}])
              ghost_check='(()=>{'+FIND+'return !!editor.shadowRoot.querySelector(".drag-ghost") && !!editor.shadowRoot.querySelector("[data-over]");})()'
              steps.append({'method':'Runtime.evaluate','params':{'expression':ghost_check,'returnByValue':True},'assert':True})
              if cancel:
                  steps.extend([{'method':'Input.dispatchKeyEvent','params':{'type':kind,'key':'Escape','code':'Escape','windowsVirtualKeyCode':27}} for kind in ('keyDown','keyUp')])
              steps.append({'method':'Input.dispatchMouseEvent','params':{'type':'mouseReleased','x':ex,'y':ey,'button':'left','clickCount':1}})
              if b.platform=='x11':input_steps(steps)
              else:
                  viewport=b.ev('({w:innerWidth,h:innerHeight})')
                  # Wayland deliberately reports fallback outer dimensions. The
                  # owned compositor defines this fixture's actual window bounds.
                  output=json.loads(subprocess.check_output(['swaymsg','-t','get_outputs','-r'],text=True))[0]['rect']
                  geo={'x':output['width']-viewport['w'],'y':output['height']-viewport['h'],'w':output['width'],'h':output['height']}
                  result['drags'][-1]['nativeGeometry']=geo
                  pointer=subprocess.Popen([os.environ['NOVENTRA_TEST_POINTER']],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
                  try:
                      def send(command):pointer.stdin.write(command+'\n');pointer.stdin.flush();assert pointer.stdout.readline().strip()=='ok'
                      for step in steps:
                          if 'delay' in step:time.sleep(step['delay']/1000);continue
                          if step['method']=='Runtime.evaluate':
                              assert b.ev(step['params']['expression']) is True,'Drag has no visible feedback'
                              continue
                          if step['method']=='Input.dispatchKeyEvent':
                              if step['params']['type']=='keyDown':subprocess.run(['wtype','-k','Escape'],check=True)
                              continue
                          e=step['params'];send(f"motion {int(e['x']+geo['x'])} {int(e['y']+geo['y'])} {geo['w']} {geo['h']}")
                          if e['type']=='mousePressed':send('button 1')
                          if e['type']=='mouseReleased':send('button 0');time.sleep(.3)
                  finally:pointer.stdin.close();pointer.wait(timeout=5)
              time.sleep(.2);capture('after-drag.png')
          # Drag the hidden Home control into a supported sidebar.
          click('[data-control="home"]')
          if placement('home')!='tray':click('#hideControl')
          drag('[data-control="home"]','.zone[data-edge="right"]',cancel=True)
          assert placement('home')=='tray','Escape did not cancel the drag'
          result['checks'].append('Escape cancels a captured drag without changing the draft')
          drag('[data-control="home"]','.zone[data-edge="right"]')
          assert placement('home')=='right','Real tray drag did not add Home'
          click('[data-control="home"]');move('left')
          drag('[data-control="home"]','[data-control="additional-actions"]')
          order=ui('return [...editor.shadowRoot.querySelectorAll("[data-edge=left] [data-control]")].map(e=>e.dataset.control);')
          assert order.index('home')<order.index('additional-actions'),order
          result['checks'].append('Real pointer drag adds from the tray and reorders before another control')
          # Keyboard-only movement through the renderer input pipeline.
          ui("editor.shadowRoot.querySelector('[data-control=\"home\"]').focus();return true;")
          input_steps([{'method':'Input.dispatchKeyEvent','params':{'type':'keyDown','key':'ArrowRight','code':'ArrowRight','windowsVirtualKeyCode':39,'modifiers':9}}, {'method':'Input.dispatchKeyEvent','params':{'type':'keyUp','key':'ArrowRight','code':'ArrowRight','windowsVirtualKeyCode':39,'modifiers':9}}])
          time.sleep(.2);assert placement('home')=='right'
          result['checks'].append('Alt + Shift + Right moves the focused control without a pointer')
          before_down=ui('return {layout:editor.draft.current,undo:editor.draft.undo.length,redo:editor.draft.redo.length};')
          input_steps([{'method':'Input.dispatchKeyEvent','params':{'type':kind,'key':'ArrowDown','code':'ArrowDown','windowsVirtualKeyCode':40,'modifiers':9}} for kind in ('keyDown','keyUp')])
          time.sleep(.2)
          assert ui('return {layout:editor.draft.current,undo:editor.draft.undo.length,redo:editor.draft.redo.length};')==before_down
          result['checks'].append('Alt + Shift + Down leaves placement and Undo/Redo unchanged')
          capture('editor-draft.png')
          # Guard Settings router navigation, including browser history traversal.
          def navigate(path):
              return ui('const link=deep(document, '+json.dumps('a[href="'+path+'"]')+');if(!link)throw Error("Missing Settings navigation link");link.click();return location.pathname;')
          assert navigate('/privacy')=='/appearance'
          assert ui('return editor.shadowRoot.querySelector("dialog").open;')
          click('#keepEditing');assert placement('home')=='right'
          navigate('/privacy');click('#discardChanges');time.sleep(.3)
          assert b.ev('location.pathname')=='/privacy'
          navigate('/appearance');time.sleep(.3)
          click('[data-control="home"]');move('right')
          b.ev('history.back();true');time.sleep(.4)
          assert b.ev('location.pathname')=='/appearance'
          assert ui('return editor.shadowRoot.querySelector("dialog").open;')
          click('#keepEditing');assert placement('home')=='right'
          b.ev('history.back();true');time.sleep(.4);click('#discardChanges');time.sleep(.4)
          assert b.ev('location.pathname')=='/privacy'
          b.ev('history.forward();true');time.sleep(.3)
          assert b.ev('location.pathname')=='/appearance'
          result['checks'].append('Unsaved SPA navigation and Back/Forward preserve history; Keep and Discard both work')
          input_steps([{'method':'Emulation.setEmulatedMedia','params':{'features':[{'name':'prefers-reduced-motion','value':'reduce'}]}},{'method':'Runtime.evaluate','params':{'expression':'matchMedia("(prefers-reduced-motion: reduce)").matches','returnByValue':True},'assert':True}])
          for scheme,name in [(1,'light'),(2,'dark')]:
              assert ui('const select=deep(document,"#noventraColorScheme");select.value='+str(scheme)+';select.dispatchEvent(new Event("change",{bubbles:true}));return true;')
              ui('editor.scrollIntoView({block:"start"});return true;');time.sleep(.2);capture('editor-'+name+'.png')
          input_steps([{'method':'Emulation.setDeviceMetricsOverride','params':{'width':480,'height':700,'deviceScaleFactor':1,'mobile':False}}, {'delay':300},
              {'method':'Runtime.evaluate','params':{'expression':'(()=>{'+FIND+'return editor.scrollWidth<=editor.clientWidth+1;})()','returnByValue':True},'assert':True},
              {'method':'Page.captureScreenshot','params':{'format':'png'},'capture':str((a.output/'editor-narrow.png').resolve())}])
          result['checks'].append('Light/dark appearance, reduced motion and a 480px Settings viewport stay usable')
          result['passed']=True
    except BaseException as error:result['error']=repr(error);raise
    finally:(a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
