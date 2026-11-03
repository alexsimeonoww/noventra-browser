// Read a value from a DevTools target owned by an isolated smoke-test browser.
import {readFileSync} from 'node:fs';
const [endpoint, argumentExpression] = process.argv.slice(2);
// Filter-list probes can exceed the OS per-argument size limit.
const expression = argumentExpression ?? readFileSync(0, 'utf8');
const url = new URL(endpoint);
if (url.protocol !== 'ws:' || url.hostname !== '127.0.0.1') {
  throw new Error('Only the local test browser is supported');
}
const socket = new WebSocket(endpoint);
let timeout;
try {
  const value = await new Promise((resolve, reject) => {
    timeout = setTimeout(() => reject(new Error('DevTools evaluation timed out')), 20000);
    socket.onopen = () => socket.send(JSON.stringify({
      id: 1, method: 'Runtime.evaluate',
      params: {expression, returnByValue: true, awaitPromise: true},
    }));
    socket.onerror = () => reject(new Error('DevTools connection failed'));
    socket.onclose = () => reject(new Error('DevTools closed before replying'));
    socket.onmessage = event => {
      const message = JSON.parse(event.data);
      if (message.id !== 1) return;
      if (message.error || message.result?.exceptionDetails) {
        reject(new Error(JSON.stringify(message.error || message.result.exceptionDetails)));
      } else {
        resolve(message.result.result.value);
      }
    };
  });
  process.stdout.write(JSON.stringify(value) + '\n');
} finally {
  clearTimeout(timeout);
  socket.close();
}
