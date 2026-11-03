"""Read native view geometry from an owned browser's accessibility diagnostics."""
import re
import time
from extension_smoke import request


class NativeUITree:
    def __init__(self, browser):
        self.browser = browser
        self.rtl = '--force-ui-direction=rtl' in browser.extra_args
        self.target = request(browser.port, '/json/new?chrome://accessibility/', 'PUT')
        time.sleep(.7)

    def read(self):
        self.text = self.browser.ev('''(async()=>{
          const xhr=new XMLHttpRequest();xhr.open('GET','targets-data.json',false);xhr.send();
          const data=JSON.parse(xhr.responseText);
          const {addWebUiListener,removeWebUiListener}=await import('chrome://resources/js/cr.js');
          return new Promise((resolve,reject)=>{
            const timer=setTimeout(()=>reject(Error('Native tree timed out')),5000);
            const listener=addWebUiListener('showOrRefreshTree', result=>{
              clearTimeout(timer);removeWebUiListener(listener);resolve(result.tree);
            });
            chrome.send('requestNativeUITree',[{sessionId:data.browsers[0].sessionId,
              requestType:'showOrRefreshTree',filters:{allow:'*',allowEmpty:'',deny:''}}]);
          });
        })()''', self.target)
        self.nodes = {}
        for line in self.text.splitlines():
            # Diagnostic page text can contain an older serialized tree. Only
            # native node headers have class_name before any name attribute.
            identifier = re.match(r'^\+*id=(\d+) (?:\w+ )+class_name=', line)
            bounds = re.search(r'\((-?\d+), (-?\d+)\)-\((\d+), (\d+)\)', line)
            parent = re.search(r'offset_container_id=(\d+)', line)
            if 'class_name=' in line and identifier and bounds:
                self.nodes[int(identifier[1])] = {
                    'line': line, 'bounds': tuple(map(int, bounds.groups())),
                    'parent': int(parent[1]) if parent else None,
                }
        return self

    def find(self, pattern):
        return next(node for node in self.nodes.values()
                    if ' INVISIBLE ' not in node['line'] and re.search(pattern, node['line']))

    def rect(self, node):
        width, height = node['bounds'][2:]
        x = y = 0
        current = node
        while current:
            # Private Wayland input excludes Ozone's fallback frame insets.
            if self.browser.platform == 'wayland' and 'class_name=BrowserView ' in current['line']:
                break
            parent = self.nodes.get(current['parent'])
            left, top, current_width, _ = current['bounds']
            # The diagnostic tree stores layout bounds, before RTL positioning.
            # Views mirrors a child's position through its parent; Noventra's
            # dock explicitly keeps its own child coordinates unmirrored.
            if self.rtl and parent and 'class_name=NoventraDockView ' not in parent['line']:
                left = parent['bounds'][2] - left - current_width
            x += left
            y += top
            current = parent
        return {'x': x, 'y': y, 'w': width, 'h': height}

    def center(self, node):
        rect = self.rect(node)
        return rect['x'] + rect['w'] / 2, rect['y'] + rect['h'] / 2
