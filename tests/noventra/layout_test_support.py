"""Owned browser fixture for native layout integration tests on private displays."""
import json
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import time
from extension_smoke import ROOT, request, stop_browser, wait_for

class LayoutBrowser:
    def __init__(self, browser, profile, output, extra_args=()):
        self.platform = 'wayland' if os.environ.get('NOVENTRA_PRIVATE_WAYLAND') == '1' else 'x11'
        if self.platform == 'x11':
            assert str(ROOT/'work/ram-investigation/xvfb-') in os.environ.get('XAUTHORITY', '')
        else:
            assert os.environ.get('XDG_RUNTIME_DIR', '').startswith('/tmp/nv-')
        self.browser, self.profile, self.output = Path(browser).resolve(), Path(profile).resolve(), Path(output)
        self.extra_args = tuple(extra_args)
        self.process = None
    def __enter__(self):
        self.output.mkdir(parents=True, exist_ok=True)
        active = self.profile/'DevToolsActivePort'
        active.unlink(missing_ok=True)
        self.log = (self.output/'browser.log').open('a')
        # Crashpad uses the default config root, independently of --user-data-dir.
        env = os.environ.copy()
        env['NOVENTRA_CONFIG_HOME'] = str(self.profile / 'test-config')
        self.process = subprocess.Popen([str(self.browser), '--user-data-dir='+str(self.profile), '--ozone-platform='+self.platform, '--disable-gpu', '--window-size=1200,760', '--window-position=0,0', '--no-first-run', '--no-default-browser-check', '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0', *self.extra_args, *([] if '--restore-last-session' in self.extra_args else ['chrome://settings/appearance'])], env=env, stdout=self.log, stderr=self.log, start_new_session=True)
        try:
            def port_ready():
                if not active.exists():
                    return False
                lines = active.read_text().splitlines()
                return len(lines) >= 2 and lines[0].isdigit() and lines[1].startswith('/devtools/browser/')
            wait_for(port_ready, self.process)
            self.port = int(active.read_text().splitlines()[0])
            time.sleep(1.5)
            self.settings = request(self.port, '/json/new?chrome://settings/appearance', 'PUT')
            wait_for(lambda: self.ev('location.href.startsWith("chrome://settings/appearance") && '
                                    'document.readyState === "complete" && '
                                    '!!document.querySelector("settings-ui")?.shadowRoot'),
                     self.process, timeout=15)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise
    def ev(self, js, target=None):
        return json.loads(subprocess.check_output(['node', str(ROOT/'tests/noventra/cdp_eval.mjs'), (target or self.settings)['webSocketDebuggerUrl'], js], text=True, timeout=25))
    def call(self, name, *args):
        js = '(async()=>{const {sendWithPromise}=await import("chrome://resources/js/cr.js");return await Promise.race([sendWithPromise('+','.join(json.dumps(x) for x in [name, *args])+'),new Promise(r=>setTimeout(()=>r({error:"Layout handler did not respond"}),3000))]);})()'
        result = self.ev(js)
        assert 'error' not in result, result
        return result
    def state(self): return self.call('noventraGetLayout')
    def apply(self, layout, revision=None):
        return self.call('noventraApplyLayout', {'expectedRevision': layout['revision'] if revision is None else revision, 'layout': layout})
    def __exit__(self, *_):
        if self.process: stop_browser(self.process)
        self.log.close()

def apply_native_test_layout(port, *, edge='left', visible=('tabs',)):
    """Apply through the real Settings API without leaving a fixture tab open."""
    settings = request(port, '/json/new?chrome://settings/appearance', 'PUT')
    try:
        time.sleep(1)
        js = '''(async()=>{
          const {sendWithPromise}=await import('chrome://resources/js/cr.js');
          const state=await sendWithPromise('noventraGetLayout');
          const visible=VISIBLE;
          const layout=state.layout;
          layout.items.sort((a,b)=> (a.id==='tabs'?-1:1)-(b.id==='tabs'?-1:1));
          layout.items.forEach((item,index)=>{item.edge=EDGE;item.order=index;item.visible=visible.includes(item.id)});
          return await sendWithPromise('noventraApplyLayout',{expectedRevision:layout.revision,layout});
        })()'''.replace('VISIBLE',json.dumps(list(visible))).replace('EDGE',json.dumps(edge))
        result=json.loads(subprocess.check_output(['node',str(ROOT/'tests/noventra/cdp_eval.mjs'),settings['webSocketDebuggerUrl'],js],text=True,timeout=25))
        assert result['status']=='ok',result
        time.sleep(.5)
    finally:
        request(port,'/json/close/'+settings['id'])

def private_wayland_output():
    """Read only the explicitly owned test compositor's single output."""
    assert os.environ.get('NOVENTRA_PRIVATE_WAYLAND') == '1'
    backend = os.environ.get('NOVENTRA_TEST_COMPOSITOR')
    if backend == 'sway':
        outputs = json.loads(subprocess.check_output(
            ['swaymsg', '-t', 'get_outputs', '-r'], text=True, timeout=5))
        assert len(outputs) == 1
        return outputs[0]
    assert backend == 'hyprland', 'Unsupported private test compositor'
    runtime = Path(os.environ['XDG_RUNTIME_DIR']).resolve()
    assert str(runtime).startswith('/tmp/nv-')
    signature = os.environ['HYPRLAND_INSTANCE_SIGNATURE']
    assert signature not in ('', '.', '..') and Path(signature).name == signature
    expected_pid = int(os.environ['NOVENTRA_TEST_COMPOSITOR_PID'])
    assert expected_pid > 1
    endpoint = runtime / 'hypr' / signature / '.socket.sock'
    info = endpoint.stat()
    assert stat.S_ISSOCK(info.st_mode) and info.st_uid == os.getuid()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        deadline = time.monotonic() + 3
        connection.settimeout(3)
        connection.connect(str(endpoint))
        pid, uid, _ = struct.unpack('3i', connection.getsockopt(
            socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i')))
        assert pid == expected_pid and uid == os.getuid(), 'Unowned compositor IPC'
        connection.sendall(b'j/monitors')
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Private compositor reply exceeded its deadline')
            connection.settimeout(remaining)
            block = connection.recv(65536)
            if time.monotonic() > deadline:
                raise TimeoutError('Private compositor reply exceeded its deadline')
            if not block:
                break
            data.extend(block)
            assert len(data) <= 1024 * 1024
    outputs = json.loads(data)
    assert len(outputs) == 1
    output = outputs[0]
    assert (output['width'], output['height'], output['x'], output['y'],
            output['scale']) == (1200, 760, 0, 0, 1)
    assert not output.get('disabled', False)
    return {'name': output['name'], 'rect': {'x': 0, 'y': 0, 'width': 1200,
            'height': 760}, 'peer_pid': pid, 'backend': backend}


class NativeWaylandPointer:
    """Pointer and capture on an explicitly owned 1200x760 test output."""
    def __init__(self):
        self.output=private_wayland_output()
        self.process=subprocess.Popen([os.environ['NOVENTRA_TEST_POINTER']],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def send(self,command):
        self.process.stdin.write(command+'\n');self.process.stdin.flush();assert self.process.stdout.readline().strip()=='ok'
    def click(self,x,y):
        rect=self.output['rect'];self.send(f"motion {int(x)} {int(y)} {rect['width']} {rect['height']}");time.sleep(.2)
        self.send('button 1');self.send('button 0');time.sleep(.4)
    def escape(self):subprocess.run(['wtype','-k','Escape'],check=True);time.sleep(.2)
    def capture(self,path):subprocess.run(['grim','-o',self.output['name'],str(path)],check=True,timeout=8)
    def close(self):self.process.stdin.close();self.process.wait(timeout=5)
