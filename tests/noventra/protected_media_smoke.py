"""Verify opt-in native Widevine installation and public encrypted playback.

Use an owned private Xvfb display and a NEW output directory. Contacts Google's
component and public Widevine test services only after enabling the real UI
control in a disposable profile. No subscription accounts, cookies or CDMs are
copied from other browsers. Tests normal restart, real EME and decoded frames;
software-display checks do not qualify Netflix, HDCP, resolution or real audio.
"""
import argparse
from contextlib import contextmanager, nullcontext
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from dock_rail_regressions_smoke import Input
from extension_smoke import ROOT, request, wait_for
from interface_polish_smoke import native_key
from layout_test_support import LayoutBrowser

WIDEVINE = 'oimompecagnajdejgnnjijobebaeigek'
PROBE = '''(async()=>{const configs=[{initDataTypes:['cenc'],
videoCapabilities:[{contentType:'video/mp4; codecs="avc1.42E01E"'}],
audioCapabilities:[{contentType:'audio/mp4; codecs="mp4a.40.2"'}]}];
const results={url:location.href,secureContext:isSecureContext};
for(const name of ['org.w3.clearkey','com.widevine.alpha']) {
try{const access=await navigator.requestMediaKeySystemAccess(name,configs);
await access.createMediaKeys();results[name]={supported:true};}
catch(e){results[name]={supported:false,error:e.name,message:e.message};}}
return results;})()'''
PAGE = b'''<!doctype html><meta charset="utf-8"><title>Owned Widevine playback</title>
<button id="play">Play public encrypted test</button><p id="status">Ready</p>
<video width="640" height="360" controls muted></video>
<script src="https://shaka-project.github.io/shaka-player-release/dist/shaka-player.compiled.js"></script>
<script>window.errors=[];document.querySelector('#play').onclick=async()=>{try{
shaka.polyfill.installAll();window.player=new shaka.Player();const video=document.querySelector('video');
await player.attach(video);player.addEventListener('error',e=>errors.push({code:e.detail.code}));
player.configure({drm:{servers:{'com.widevine.alpha':'https://proxy.uat.widevine.com/proxy'}}});
await player.load('https://storage.googleapis.com/shaka-demo-assets/angel-one-widevine/dash.mpd');
await video.play();document.querySelector('#status').textContent='Playing';
}catch(e){errors.push({code:e.code,message:e.message});document.querySelector('#status').textContent='Failed';}};</script>'''


@contextmanager
def fixture():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}'
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


def rpc(browser, page, name, *args):
    return browser.ev('(async()=>{const {sendWithPromise}=await import('
                      '"chrome://resources/js/cr.js");return await sendWithPromise('
                      + ','.join(json.dumps(x) for x in [name, *args]) + ');})()', page)


def components(browser):
    page = request(browser.port, '/json/new?chrome://components/', 'PUT')
    wait_for(lambda: manager_state(browser, page)['visible'], browser.process)
    return page


def components_from_settings(browser, inp):
    page = request(browser.port,
                   '/json/new?chrome://settings/content/protectedContent', 'PUT')
    find_link = '''(()=>{const find=root=>{
const link=root.querySelector('a[href="chrome://components/"]');if(link)return link;
for(const element of root.querySelectorAll('*')){if(element.shadowRoot){
const found=find(element.shadowRoot);if(found)return found;}}return null;};
const link=find(document);if(!link)return false;link.focus();return true;})()'''
    wait_for(lambda: browser.ev(find_link, page), browser.process)
    request(browser.port, '/json/activate/'+page['id'])
    browser.ev(find_link, page)
    native_key(inp, 'Return')
    wait_for(lambda: browser.ev('location.href.startsWith("chrome://components/")', page)
             and manager_state(browser, page)['visible'], browser.process)
    return page


def manager_state(browser, page):
    return browser.ev('''(()=>{const shown=element=>{
if(!element||!element.getClientRects().length)return false;
for(let node=element;node;node=node.parentElement){const s=getComputedStyle(node);
if(s.visibility==='hidden'||s.display==='none'||Number(s.opacity)===0)return false;}
return true;};const checkbox=document.querySelector('#widevine-downloads');
return {visible:shown(document.querySelector('#body-container'))&&shown(checkbox),
downloadsDisabled:checkbox?.disabled??true,
notInstalledVisible:shown(document.querySelector('#widevine-not-installed')),
restartVisible:shown(document.querySelector('#widevine-restart'))};})()''', page)


def media_state(browser, page):
    return browser.ev('''(()=>{const v=document.querySelector('video');return {
status:document.querySelector('#status').textContent,errors:window.errors,
time:v.currentTime,paused:v.paused,width:v.videoWidth,height:v.videoHeight,
frames:v.getVideoPlaybackQuality().totalVideoFrames,keys:!!v.mediaKeys,
error:v.error?{code:v.error.code,message:v.error.message}:null};})()''', page)


def normal_quit(browser):
    # This is Chromium's real graceful shutdown command. The inherited test's
    # Ctrl+Q assumption was invalid: this build has no such accelerator.
    version = request(browser.port, '/json/version')
    subprocess.run(['node', str(ROOT/'experiments/rust-gui/compare-cdp.mjs'),
                    version['webSocketDebuggerUrl'], 'Browser.close', '{}'],
                   check=True, capture_output=True, text=True, timeout=15)
    assert browser.process.wait(timeout=20) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--keep-profile', action='store_true',
                        help='Retain the owned test profile for a requested manual playback check')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'checks': [], 'browser': str(args.browser.resolve()),
              'scope': 'Public encrypted Widevine test; no Netflix account or paid title; '
                       'private software display, muted audio'}
    if args.keep_profile:
        retained_profile = args.output/'profile'
        retained_profile.mkdir(mode=0o700)
        profile_context = nullcontext(str(retained_profile))
        result['retained_profile'] = str(retained_profile.resolve())
    else:
        profile_context = tempfile.TemporaryDirectory(prefix='drm-profile-', dir=args.output)

    def save():
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')

    def check(name, actual):
        result['checks'].append({'name': name, 'passed': bool(actual)})
        save()
        assert actual, name

    try:
        with profile_context as tmp, fixture() as base:
            profile = Path(tmp)
            with LayoutBrowser(args.browser, profile, args.output/'install',
                               extra_args=('--mute-audio', '--enable-logging=stderr')) as browser:
                inp = Input(browser.platform)
                try:
                    page = components_from_settings(browser, inp)
                    check('Protected content settings opens the native DRM manager', True)
                    data = rpc(browser, page, 'requestComponentsData')
                    result['initial_components'] = data
                    check('independent Widevine installer exposed', data.get('widevineAvailable'))
                    check('downloads start disabled', not data['widevineDownloadsEnabled'])
                    check('missing Widevine has visible installation guidance',
                          manager_state(browser, page)['notInstalledVisible'])
                    check('Widevine registered once',
                          sum(x['id'] == WIDEVINE for x in data['components']) == 1)
                    media = request(browser.port, '/json/new?'+base, 'PUT')
                    wait_for(lambda: browser.ev('location.origin===' + json.dumps(base)
                        + '&&document.readyState==="complete"&&!!document.querySelector("#play")', media), browser.process)
                    before = browser.ev(PROBE, media)
                    result['eme_before_install'] = before
                    check('ClearKey EME positive control', before['org.w3.clearkey']['supported'])
                    check('fresh profile has no Widevine', not before['com.widevine.alpha']['supported'])
                    request(browser.port, '/json/activate/'+page['id'])
                    browser.ev('document.querySelector("#widevine-downloads").focus();true', page)
                    native_key(inp, 'space')
                    wait_for(lambda: rpc(browser, page, 'requestComponentsData')['widevineDownloadsEnabled'],
                             browser.process)
                    deadline = time.monotonic()+120
                    while time.monotonic() < deadline:
                        data = rpc(browser, page, 'requestComponentsData')
                        item = next(x for x in data['components'] if x['id'] == WIDEVINE)
                        if item['version'] != '0.0.0.0' and data['widevineRestartRequired']:
                            break
                        time.sleep(.5)
                    result['installed_components'] = data
                    check('native verified CDM installed', item['version'] != '0.0.0.0')
                    check('Linux restart requirement reported', data['widevineRestartRequired'])
                    check('missing Widevine guidance clears after installation',
                          not manager_state(browser, page)['notInstalledVisible'])
                    wait_for(lambda: manager_state(browser, page)['restartVisible']
                             and manager_state(browser, page)['visible']
                             and not manager_state(browser, page)['downloadsDisabled'],
                             browser.process)
                    check('installer and restart notice remain visible after updates', True)
                    inp.capture(args.output/'installed-components.png')
                    # Fresh profile preferences are committed asynchronously.
                    # Check persisted consent only after a normal quit flushes
                    # them; do not mistake an unwritten file for a product fault.
                    normal_quit(browser)
                    prefs = json.loads((profile/'Default/Preferences').read_text())
                    result['global_services_after_install'] = prefs.get('helium', {}).get('services', {})
                    check('global services consent remains off',
                          not result['global_services_after_install'].get('user_consented', False))
                finally:
                    inp.close()
            with LayoutBrowser(args.browser, profile, args.output/'restart',
                               extra_args=('--mute-audio', '--enable-logging=stderr')) as browser:
                inp = Input(browser.platform)
                try:
                    page = components(browser)
                    data = rpc(browser, page, 'requestComponentsData')
                    result['restart_components'] = data
                    check('Widevine consent survives restart', data['widevineDownloadsEnabled'])
                    check('restart notice clears', not data['widevineRestartRequired'])
                    media = request(browser.port, '/json/new?'+base, 'PUT')
                    wait_for(lambda: browser.ev('location.origin===' + json.dumps(base)
                        + '&&document.readyState==="complete"&&!!document.querySelector("#play")&&!!window.shaka?.Player', media),
                             browser.process, timeout=40)
                    after = browser.ev(PROBE, media)
                    result['eme_after_restart'] = after
                    check('Widevine MediaKeys load after restart', after['com.widevine.alpha']['supported'])
                    request(browser.port, '/json/activate/'+media['id'])
                    browser.ev('document.querySelector("#play").focus();true', media)
                    native_key(inp, 'Return')
                    deadline = time.monotonic()+60
                    while time.monotonic() < deadline:
                        state = media_state(browser, media)
                        if state['errors'] or (state['keys'] and state['frames'] > 60 and state['time'] > 1):
                            break
                        time.sleep(.3)
                    result['encrypted_playback'] = state
                    check('public encrypted video decodes with MediaKeys',
                          state['keys'] and state['frames'] > 60 and state['time'] > 1
                          and not state['errors'] and not state['error'])
                    browser.ev('''(()=>{const video=document.querySelector('video');
window.protectedSeek={before:video.currentTime,target:15,completed:false};
video.addEventListener('seeked',()=>Object.assign(window.protectedSeek,{
completed:true,time:video.currentTime,
frames:video.getVideoPlaybackQuality().totalVideoFrames}),{once:true});
video.currentTime=15;return true;})()''', media)
                    wait_for(lambda: browser.ev('window.protectedSeek.completed', media),
                             browser.process, timeout=20)
                    seek = browser.ev('window.protectedSeek', media)
                    result['encrypted_seek'] = seek
                    check('encrypted video emits seeked at the requested position',
                          seek['before'] < 10 and abs(seek['time'] - seek['target']) < .8)
                    wait_for(lambda: (lambda state: state['time'] > seek['time'] + .3
                                      and state['frames'] > seek['frames']
                                      and not state['paused'] and not state['error'])(
                                          media_state(browser, media)), browser.process)
                    check('encrypted video resumes decoding after seeking', True)
                    inp.capture(args.output/'public-encrypted-playback.png')
                    request(browser.port, '/json/activate/'+page['id'])
                    browser.ev('document.querySelector("#widevine-downloads").focus();true', page)
                    native_key(inp, 'space')
                    wait_for(lambda: not rpc(browser, page, 'requestComponentsData')['widevineDownloadsEnabled'],
                             browser.process)
                    wait_for(lambda: manager_state(browser, page)['visible']
                             and not manager_state(browser, page)['downloadsDisabled'],
                             browser.process)
                    check('download consent can be revoked independently', True)
                    check('download control remains usable after consent rerender', True)
                    normal_quit(browser)
                finally:
                    inp.close()
            with LayoutBrowser(args.browser, profile, args.output/'disabled-restart',
                               extra_args=('--mute-audio',)) as browser:
                inp = Input(browser.platform)
                try:
                    data = rpc(browser, components(browser), 'requestComponentsData')
                    check('revoked download consent survives restart', not data['widevineDownloadsEnabled'])
                    media = request(browser.port, '/json/new?'+base, 'PUT')
                    wait_for(lambda: browser.ev('document.readyState==="complete"', media), browser.process)
                    check('installed DRM remains available with updates disabled',
                          browser.ev(PROBE, media)['com.widevine.alpha']['supported'])
                    normal_quit(browser)
                finally:
                    inp.close()
            result['passed'] = True
    finally:
        save()
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks']),
                      'output': str(args.output)}, indent=2))


if __name__ == '__main__':
    main()
