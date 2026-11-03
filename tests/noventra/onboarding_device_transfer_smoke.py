"""Exercise the real setup-to-device-transfer handoff in an isolated profile."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess

from dock_rail_regressions_smoke import Input
from extension_smoke import request, wait_for
from interface_polish_smoke import native_key
from layout_editor_smoke import FIND
from onboarding_import_smoke import gesture
from onboarding_managers_smoke import owned_browser, page_ready, reload_page
from portable_transfer_smoke import isolate, ui, focus_owned


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check(result, name, value):
    assert value, name
    result['checks'].append(name)


def seed_source():
    source = Path.home() / '.config/chromium'
    (source / 'Default').mkdir(parents=True)
    (source / 'Local State').write_text(json.dumps({'profile': {'info_cache': {
        'Default': {'name': 'Owned onboarding transfer source'}}}}))
    (source / 'Default/Preferences').write_text('{}\n')
    (source / 'Default/Bookmarks').write_text(json.dumps({'version': 1, 'roots': {
        'bookmark_bar': {'type': 'folder', 'name': 'Bookmarks bar', 'children': [
            {'type': 'url', 'name': 'Owned source bookmark',
             'url': 'https://owned-onboarding-transfer.invalid/bookmark',
             'date_added': '13400000000000000'}]},
        'other': {'type': 'folder', 'name': 'Other bookmarks', 'children': []}}}))
    return {path: sha(path) for path in source.rglob('*') if path.is_file()}


def layout_checks(browser, page, output, result):
    expression = '''(async()=>{
      const next=document.querySelector('#continue');next.scrollIntoView({block:'center',behavior:'instant'});
      await new Promise(requestAnimationFrame);const footer=next.getBoundingClientRect();
      const footerReachable=next.checkVisibility()&&footer.top>=0&&footer.bottom<=innerHeight;
      const footerBounds={top:footer.top,bottom:footer.bottom,height:footer.height,viewport:innerHeight,scrollTop:scrollY,scrollHeight:document.documentElement.scrollHeight};
      scrollTo({top:0,left:0,behavior:'instant'});
      await new Promise(requestAnimationFrame);await new Promise(requestAnimationFrame);
      const card=document.querySelector('#setup-device-transfer'),r=card.getBoundingClientRect();
      const children=[...card.children].map(e=>{const b=e.getBoundingClientRect();return {left:b.left,right:b.right,width:b.width}});
      const s=getComputedStyle(card),main=getComputedStyle(document.querySelector('main'));
      return {width:innerWidth,scroll:document.documentElement.scrollWidth,footerReachable,footerBounds,card:{left:r.left,right:r.right,width:r.width},children,
        foreground:s.color,background:s.backgroundColor,surface:main.backgroundColor,display:s.display};})()'''
    script = '''let data='';for await(const chunk of process.stdin)data+=chunk;
      const config=JSON.parse(data),socket=new WebSocket(process.argv[1]);let next=0;
      const pending=new Map();socket.onmessage=e=>{const v=JSON.parse(e.data),job=pending.get(v.id);
        if(job){clearTimeout(job.timer);pending.delete(v.id);v.error?job.reject(Error(JSON.stringify(v.error))):job.resolve(v.result)}};
      await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject});
      const rpc=(method,params={})=>new Promise((resolve,reject)=>{const id=++next;
        const timer=setTimeout(()=>{pending.delete(id);reject(Error('CDP timeout'))},15000);
        pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params}))});
      const rows=[];try{const scheme=config.scheme;for(const width of [1280,600,360,320]){
        await rpc('Emulation.setDeviceMetricsOverride',{width,height:960,deviceScaleFactor:1,mobile:false});
        const evaluated=await rpc('Runtime.evaluate',{expression:config.expression,awaitPromise:true,returnByValue:true});
        if(evaluated.exceptionDetails)throw Error('Native layout evaluation failed');
        const row={scheme,...evaluated.result.value};
        if(width===1280||width===360)row.png=(await rpc('Page.captureScreenshot',{format:'png',captureBeyondViewport:false})).data;
        rows.push(row);
      }}finally{await rpc('Emulation.clearDeviceMetricsOverride');socket.close()}
      process.stdout.write(JSON.stringify(rows));'''
    appearance = request(browser.port, '/json/new?chrome://settings/appearance', 'PUT')
    lookup = 'const index=deep(document,"settings-appearance-page-index");const p=index?.shadowRoot.querySelector("#appearance");'
    wait_for(lambda: browser.ev('(()=>{' + FIND + lookup + 'return !!p?.colorSchemeModeBrowserProxy_?.handler})()', appearance), browser.process)
    rows = []
    try:
        for scheme, mode in [('light', 1), ('dark', 2)]:
            browser.ev('(()=>{' + FIND + lookup + 'p.colorSchemeModeBrowserProxy_.handler.setColorSchemeMode(' + str(mode) + ');return true})()', appearance)
            wait_for(lambda: browser.ev('(()=>{' + FIND + lookup + 'return p.selectedColorSchemeMode_===' + str(mode) + '})()', appearance), browser.process)
            wait_for(lambda: browser.ev('(()=>{const c=document.createElement("canvas");c.width=c.height=1;const x=c.getContext("2d");x.fillStyle=getComputedStyle(document.querySelector("main")).backgroundColor;x.fillRect(0,0,1,1);return [...x.getImageData(0,0,1,1).data].slice(0,3).reduce((a,b)=>a+b,0)' + ('<382' if mode == 2 else '>382') + '})()', page), browser.process)
            request(browser.port, '/json/activate/' + page['id'])
            rows += json.loads(subprocess.check_output(['node', '--input-type=module', '-e', script,
                page['webSocketDebuggerUrl']], input=json.dumps({'expression': expression, 'scheme': scheme}), text=True, timeout=90))
    finally:
        request(browser.port, '/json/close/' + appearance['id'])
    for row in rows:
        if 'png' in row:
            image = base64.b64decode(row.pop('png'), validate=True)
            assert image.startswith(b'\x89PNG\r\n\x1a\n')
            (output / f"onboarding-transfer-{row['scheme']}-{row['width']}.png").write_bytes(image)
    result['layouts'] = rows
    for row in rows:
        check(result, f"{row['scheme']} setup fits a {row['width']}px viewport without clipped transfer controls",
              row['scroll'] <= row['width'] and row['footerReachable'] and row['card']['width'] > 0
              and 0 <= row['card']['left'] < row['card']['right'] <= row['width']
              and all(child['width'] > 0 and row['card']['left'] <= child['left']
                      < child['right'] <= row['card']['right'] for child in row['children']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--expected-binary', required=True)
    parser.add_argument('--expected-resources', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('handoff',), default='handoff')
    parser.add_argument('--inside-owned-home', action='store_true')
    args = parser.parse_args()
    args.browser, args.output = args.browser.resolve(strict=True), args.output.resolve()
    assert sha(args.browser) == args.expected_binary
    assert sha(args.browser.parent / 'resources.pak') == args.expected_resources
    args.rtc_policy_diagnostic = False
    isolate(args, Path(__file__))
    args.output.mkdir(exist_ok=False)
    before = seed_source()
    result = {'passed': False, 'checks': [], 'binary_sha256': args.expected_binary,
              'resources_sha256': args.expected_resources, 'fixture_sha256': sha(__file__),
              'home_isolated': True, 'network_isolated_no_egress': True,
              'transfer_backend_mocked': False, 'source_data': 'Owned synthetic Chromium bookmarks only'}
    profile = args.output / 'profile'
    try:
        with owned_browser(args.browser, profile, args.output / 'browser', result,
                           'handoff', normal_startup=True) as browser:
            page = wait_for(lambda: next((target for target in request(browser.port, '/json/list')
                if target['type'] == 'page' and target['url'] == 'chrome://setup/'), None), browser.process)
            page_ready(browser, page)
            gesture(page, 'document.querySelector("#continue").click();true')
            wait_for(lambda: browser.ev('document.querySelector("#step-title").textContent==="Bring your browsing data."', page), browser.process)
            check(result, 'The Data step offers a prominent PC transfer choice before local browsers',
                browser.ev('(()=>{const a=document.querySelector("#setup-device-transfer");return !!a&&a.checkVisibility()&&a.href==="chrome://settings/dataDevices"&&a.target==="_blank"&&a.rel.includes("noopener")&&!!(a.compareDocumentPosition(document.querySelector(".source-heading"))&Node.DOCUMENT_POSITION_FOLLOWING)})()', page))
            check(result, 'Setup explains both transfer methods and the actual data limits', browser.ev('(()=>{const a=document.querySelector("#setup-device-transfer"),d=document.querySelector("#setup-device-transfer-details");return a.textContent.includes("code")&&a.textContent.includes("encrypted transfer file")&&d.textContent.includes("Website sign-ins, history and passkeys are not included")})()', page))
            wait_for(lambda: browser.ev('!!document.querySelector(".profile-card input:not(:disabled)")', page), browser.process)
            gesture(page, 'document.querySelector(".profile-card input:not(:disabled)").click();true')
            wait_for(lambda: browser.ev('!!document.querySelector("#queued-import-note")', page), browser.process)
            selected = browser.ev('document.querySelector(".profile-card input:checked").value', page)
            queued = browser.ev('document.querySelector("#queued-import-note").textContent', page)
            browser.settings = page
            pointer = Input(browser.platform)
            try:
                request(browser.port, '/json/activate/' + page['id'])
                focus_owned(browser, pointer)
                native_key(pointer, 'Escape')
                browser.ev('document.querySelector("#step-title").focus();true', page)
                native_key(pointer, 'Tab')
                check(result, 'The PC transfer choice is reachable with the keyboard',
                      browser.ev('document.activeElement.id==="setup-device-transfer"', page))
                layout_checks(browser, page, args.output, result)
            finally:
                pointer.close()
            gesture(page, 'document.querySelector("#setup-device-transfer").click();true')
            target = wait_for(lambda: next((target for target in request(browser.port, '/json/list')
                if target['type'] == 'page' and target['url'] == 'chrome://settings/dataDevices'), None), browser.process)
            browser.settings = target
            wait_for(lambda: browser.ev('(()=>{' + FIND + ';return !!deep(document,"noventra-transfer")?.shadowRoot})()', target), browser.process)
            wait_for(lambda: ui(browser, 'return component.available_&&page.dataDevicesPage&&page.checkVisibility()&&page.shadowRoot.querySelector("#dataDevicesContent")?.checkVisibility();'), browser.process)
            check(result, 'The onboarding choice opens the actual native transfer backend',
                  ui(browser, 'return !root.querySelector("#chooseTransfer").disabled&&!root.querySelector("#startDirectReceive").disabled&&!!root.querySelector("#transferFile")&&!!root.querySelector("#exportTransfer");'))
            gesture(target, '(()=>{' + FIND + ';const p=deep(document,"noventra-transfer");p.shadowRoot.querySelector("#startDirectReceive").click();return true})()')
            wait_for(lambda: ui(browser, 'return component.peerRole_==="receive"&&!!root.querySelector("#incomingCode");'), browser.process)
            check(result, 'Receiving a connection code is available without starting a connection automatically',
                  ui(browser, 'return !component.peer_&&!root.querySelector("#incomingCode").disabled;'))
            request(browser.port, '/json/close/' + target['id'])
            request(browser.port, '/json/activate/' + page['id'])
            browser.settings = page
            check(result, 'Returning from transfer keeps the setup step and queued local-browser choice',
                  browser.ev('document.querySelector("#step-title").textContent==="Bring your browsing data."&&document.querySelector(".profile-card input:checked").value===' + json.dumps(selected) + '&&document.querySelector("#queued-import-note").textContent===' + json.dumps(queued), page))
            reload_page(browser, page)
            wait_for(lambda: browser.ev('!!document.querySelector("#queued-import-note")', page), browser.process)
            check(result, 'Reloading after the handoff restores the same staged choice',
                  browser.ev('document.querySelector(".profile-card input:checked").value===' + json.dumps(selected), page))
        preferences = json.loads((profile / 'Default/Preferences').read_text())
        check(result, 'Opening transfer does not finish setup on behalf of the user',
              preferences.get('helium', {}).get('completed_onboarding') is not True)
        check(result, 'The source browser data remains unchanged', all(sha(path) == digest for path, digest in before.items()))
        result['passed'] = True
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'checks': len(result['checks'])}))


if __name__ == '__main__':
    main()
