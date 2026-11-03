// Inspect only the isolated browser's explicit local DevTools endpoint.
const [port] = process.argv.slice(2);
if (!/^\d+$/.test(port) || +port < 1 || +port > 65535) throw Error('Valid local port required');
const base = `http://127.0.0.1:${port}`;
const version = await (await fetch(`${base}/json/version`)).json();
const pages = (await (await fetch(`${base}/json/list`)).json()).filter(t => t.type === 'page');
const endpoint = new URL(version.webSocketDebuggerUrl);
if (endpoint.protocol !== 'ws:' || endpoint.hostname !== '127.0.0.1') throw Error('Nonlocal endpoint');
const socket = new WebSocket(endpoint.href);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let id = 0;
const pending = new Map();
socket.onmessage = event => {
  const message = JSON.parse(event.data), entry = pending.get(message.id);
  if (!entry) return;
  pending.delete(message.id); clearTimeout(entry.timer);
  if (message.error || message.result?.exceptionDetails) {
    entry.reject(Error(JSON.stringify(message.error || message.result.exceptionDetails)));
  } else entry.resolve(message.result);
};
function call(method, params = {}, sessionId) {
  return new Promise((resolve, reject) => {
    const key = ++id;
    const timer = setTimeout(() => reject(Error(`Timed out: ${method}`)), 10000);
    pending.set(key, {resolve, reject, timer});
    socket.send(JSON.stringify({id: key, method, params, sessionId}));
  });
}
try {
  const result = [];
  for (const page of pages) {
    const {sessionId} = await call('Target.attachToTarget', {targetId: page.id, flatten: true});
    try {
      const state = await call('Runtime.evaluate', {returnByValue: true,
        expression: '({visibility:document.visibilityState,focused:document.hasFocus(),width:innerWidth,height:innerHeight,userAgent:navigator.userAgent})'}, sessionId);
      const window = await call('Browser.getWindowForTarget', {targetId: page.id});
      result.push({id: page.id, url: page.url, ...state.result.value, window});
    } finally {
      await call('Target.detachFromTarget', {sessionId});
    }
  }
  process.stdout.write(JSON.stringify(result) + '\n');
} finally {
  for (const entry of pending.values()) clearTimeout(entry.timer);
  socket.close();
}
