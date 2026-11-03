import {existsSync, readFileSync, writeFileSync} from 'node:fs';
import {join} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';

const [endpoint, output] = process.argv.slice(2);
const url = new URL(endpoint);
if (url.protocol !== 'ws:' || url.hostname !== '127.0.0.1') throw Error('Owned loopback CDP required');
const socket = new WebSocket(endpoint);
let sequence = 0;
const pending = new Map();
let handled = false;
let opened = false;
let stopped = false;
let commandSent = false;
const save = (name, value) => writeFileSync(join(output, name), JSON.stringify(value, null, 2) + '\n');
const call = (method, params = {}) => new Promise((resolve, reject) => {
  const id = ++sequence;
  pending.set(id, {resolve, reject});
  socket.send(JSON.stringify({id, method, params}));
});
const timer = setTimeout(() => {save('timeout.json', {handled, commandSent}); process.exit(1);}, 45000);
socket.onclose = () => {
  if (stopped) {clearTimeout(timer); clearInterval(stopTimer); process.exit(0);}
  if (commandSent) {save('closed.json', {commandSent, handled}); clearTimeout(timer); process.exit(0);}
  process.exit(1);
};
socket.onmessage = async event => {
  const message = JSON.parse(event.data);
  if (message.id) {
    const task = pending.get(message.id);
    if (task) {pending.delete(message.id); message.error ? task.reject(Error(JSON.stringify(message.error))) : task.resolve(message.result);}
  }
  if (message.method !== 'Page.javascriptDialogOpening' || handled || commandSent) return;
  opened = true;
  save('opened.json', message.params);
  const decisionPath = join(output, 'decision.json');
  while (!existsSync(decisionPath)) await delay(25);
  const decision = JSON.parse(readFileSync(decisionPath));
  if (typeof decision.accept !== 'boolean') throw Error('Boolean dialog decision required');
  commandSent = true;
  save('sent.json', {method: 'Page.handleJavaScriptDialog', accept: decision.accept});
  await call('Page.handleJavaScriptDialog', {accept: decision.accept});
  handled = true;
  save('handled.json', {acknowledged: true, accept: decision.accept});
  clearTimeout(timer);
  clearInterval(stopTimer);
  socket.close();
};
const stopTimer = setInterval(() => {
  if (!opened && existsSync(join(output, 'stop.json'))) {stopped = true; socket.close();}
}, 25);
await new Promise((resolve, reject) => {socket.onopen = resolve; socket.onerror = reject;});
await call('Page.enable');
save('ready.json', {pageEnabled: true});
