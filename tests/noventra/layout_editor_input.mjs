// Native input through an owned test browser's renderer input pipeline.
import {writeFileSync} from 'node:fs';
import {setTimeout as delay} from 'node:timers/promises';
const [endpoint, encoded] = process.argv.slice(2);
const url = new URL(endpoint);
if (url.protocol !== 'ws:' || url.hostname !== '127.0.0.1') throw Error('Only a local test browser is supported');
const socket = new WebSocket(endpoint);
await new Promise((resolve, reject) => {socket.onopen=resolve;socket.onerror=reject;});
let sequence=0;
const pending=new Map();
socket.onmessage=e=>{const m=JSON.parse(e.data);const p=pending.get(m.id);if(p){pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result);}};
const timeout=setTimeout(()=>{socket.close();process.exit(1);},20000);
try {
 for(const step of JSON.parse(encoded)) {
  if(step.delay){await delay(step.delay);continue;}
  const id=++sequence;
  const result=await new Promise((resolve,reject)=>{pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method:step.method,params:step.params}));});
  if(step.assert && !result.result?.value) throw Error(JSON.stringify(result));
  if(step.returnValue) process.stdout.write(JSON.stringify(result)+'\n');
  if(step.capture) writeFileSync(step.capture,Buffer.from(result.data,'base64'));
 }
} finally {clearTimeout(timeout);socket.close();}
