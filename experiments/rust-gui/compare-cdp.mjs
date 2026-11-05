// One command against a DevTools endpoint belonging to the private benchmark.
const [endpoint, method, encoded = '{}'] = process.argv.slice(2);
const url = new URL(endpoint);
if (url.protocol !== 'ws:' || url.hostname !== '127.0.0.1') {
  throw new Error('Only an owned loopback DevTools endpoint is supported');
}
const socket = new WebSocket(endpoint);
let timer;
try {
  const value = await new Promise((resolve, reject) => {
    timer = setTimeout(() => reject(new Error(`Timed out: ${method}`)), 25000);
    socket.onopen = () => socket.send(JSON.stringify({id: 1, method, params: JSON.parse(encoded)}));
    socket.onerror = () => reject(new Error('DevTools connection failed'));
    socket.onclose = () => reject(new Error('DevTools closed before replying'));
    socket.onmessage = event => {
      const message = JSON.parse(event.data);
      if (message.id !== 1) return;
      if (message.error || message.result?.exceptionDetails) {
        reject(new Error(JSON.stringify(message.error || message.result.exceptionDetails)));
      } else {
        resolve(message.result);
      }
    };
  });
  process.stdout.write(JSON.stringify(value) + '\n');
} finally {
  clearTimeout(timer);
  socket.close();
}
