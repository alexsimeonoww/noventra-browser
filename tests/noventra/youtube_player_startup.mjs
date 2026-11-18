// Run under a private Xvfb display: node youtube_player_startup.mjs BROWSER [PREVIOUS_BROWSER].
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, realpath, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const [browser, previousBrowser] = process.argv.slice(2);
assert(browser && process.argv.length <= 4, 'Expected BROWSER [PREVIOUS_BROWSER]');
const displayPid = process.env.NOVENTRA_BENCH_XVFB_PID;
assert(/^\d+$/.test(displayPid), 'Use the private Xvfb wrapper');
const display = (await readFile('/proc/' + displayPid + '/cmdline')).toString().split('\0');
assert(display[0].endsWith('/Xvfb') && display.includes(process.env.XAUTHORITY));

const url = 'https://www.youtube.com/watch?v=noventra-fixture';
const cases = {
  missing: '',
  attributeMissing: '<div id="masthead"></div>',
  standard: '<div id="masthead" logo-type="YOUTUBE_LOGO"></div>',
  premium: '<div id="masthead" logo-type="YOUTUBE_PREMIUM_LOGO"></div>',
};

function fixture(header) {
  return `<!doctype html><title>Offline player fixture</title>
    <script>window.ytcfg = {data_: {INNERTUBE_CONTEXT: {client: {userAgent: navigator.userAgent}}}};
    window.bootErrors = []; window.addEventListener('error', e => bootErrors.push(e.message));</script>
    ${header}
    <script src="/gate.js"></script>
    <script id="player">(function serverContract() { window.playerStarted = true; })()</script>
    <div id="complete">Loaded</div>`;
}

async function until(read, description) {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const result = await read();
    if (result) return result;
    await delay(100);
  }
  throw Error('Timed out waiting for ' + description);
}

// The DevTools pipe belongs to this child process. No listener is exposed.
function connection(child) {
  let sequence = 0;
  let remainder = '';
  const calls = new Map();
  let paused;
  child.stdio[4].setEncoding('utf8');
  child.stdio[4].on('data', chunk => {
    const messages = (remainder + chunk).split('\0');
    remainder = messages.pop();
    for (const raw of messages) {
      const message = JSON.parse(raw);
      if (message.id) {
        const pending = calls.get(message.id);
        if (!pending) continue;
        calls.delete(message.id);
        clearTimeout(pending.timer);
        if (message.error) pending.reject(Error(message.error.message));
        else pending.resolve(message.result);
      } else if (message.method === 'Fetch.requestPaused') {
        paused(message.params, message.sessionId);
      }
    }
  });
  function send(method, params = {}, sessionId) {
    return new Promise((resolve, reject) => {
      const id = ++sequence;
      const timer = setTimeout(() => {
        calls.delete(id);
        reject(Error('Timed out calling ' + method));
      }, 15_000);
      calls.set(id, { resolve, reject, timer });
      child.stdio[3].write(JSON.stringify({ id, method, params, sessionId }) + '\0');
    });
  }
  async function evaluate(expression, session) {
    const answer = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }, session);
    if (answer.exceptionDetails) throw Error(answer.exceptionDetails.exception?.description || answer.exceptionDetails.text);
    return answer.result.value;
  }
  return { send, evaluate, handleRequest: handler => { paused = handler; } };
}

async function run(executable, directory, stage) {
  const binary = await realpath(resolve(executable));
  const child = spawn(binary, [
    '--user-data-dir=' + join(directory, 'profile'), '--ozone-platform=x11',
    '--remote-debugging-pipe', '--no-first-run', '--no-default-browser-check',
    '--host-resolver-rules=MAP * ~NOTFOUND', 'about:blank',
  ], {
    env: { ...process.env, XDG_CONFIG_HOME: join(directory, 'config'),
      XDG_CACHE_HOME: join(directory, 'cache'), NOVENTRA_CONFIG_HOME: join(directory, 'config'),
      DBUS_SESSION_BUS_ADDRESS: 'unix:path=/tmp/noventra-no-session-bus' },
    stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'],
  });
  const exited = once(child, 'exit');
  const { send, evaluate, handleRequest } = connection(child);
  try {
    const target = await until(async () => {
      const { targetInfos } = await send('Target.getTargets');
      return targetInfos.find(t => t.type === 'background_page' && t.url.startsWith('chrome-extension://blockjmkbacgjkknlgpkjjiijinjdanf/'));
    }, 'the built-in blocker');
    const { sessionId: extension } = await send('Target.attachToTarget', { targetId: target.targetId, flatten: true });
    await until(() => evaluate(`(async () => {
      if (document.readyState !== 'complete' || typeof globalThis.vAPI?.defer?.create !== 'function') return false;
      const blocker = (await import('./js/background.js')).default;
      return blocker.readyToFilter && blocker.selectedFilterLists.includes('ublock-quick-fixes') &&
        blocker.getNetFilteringSwitch(${JSON.stringify(url)});
    })()`, extension), 'Quick fixes to be active');
    if (stage !== 'fresh') {
      // Represent an existing profile with a locally cached copy of an older
      // filter download. Its bytes should survive the update unchanged.
      const retained = await evaluate(`(async () => {
        const assets = (await import('./js/assets.js')).default;
        const key = 'ublock-quick-fixes';
        const marker = '! Noventra cached-list fixture';
        if (${stage === 'previous'}) {
          const { content } = await assets.get(key, { favorLocal: true });
          await assets.put(key, content + String.fromCharCode(10) + marker);
        }
        return (await assets.fromCache(key)).endsWith(marker);
      })()`, extension);
      assert.equal(retained, true, 'Expected the original raw list in the profile cache');
    }
    const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
    const { sessionId: tab } = await send('Target.attachToTarget', { targetId, flatten: true });
    let currentCase;
    handleRequest(async ({ requestId, request }, session) => {
      assert.equal(session, tab);
      if (request.url === 'https://www.youtube.com/gate.js') {
        // A parser-blocking script gives the real extension time to install
        // its document_start observers, as during an ordinary network load.
        await delay(850);
        return send('Fetch.fulfillRequest', {
          requestId, responseCode: 200, body: '',
          responseHeaders: [{ name: 'Content-Type', value: 'application/javascript' }],
        }, session);
      }
      assert.equal(request.url, url + '&case=' + currentCase);
      await send('Fetch.fulfillRequest', { requestId, responseCode: 200,
        responseHeaders: [{ name: 'Content-Type', value: 'text/html; charset=utf-8' },
          { name: 'Cache-Control', value: 'no-store' }],
        body: Buffer.from(fixture(cases[currentCase])).toString('base64') }, session);
    });
    await send('Fetch.enable', { patterns: [
      { urlPattern: url + '*', resourceType: 'Document' },
      { urlPattern: 'https://www.youtube.com/gate.js', resourceType: 'Script' },
    ] }, tab);
    const observations = {};
    for (currentCase of Object.keys(cases)) {
      const destination = url + '&case=' + currentCase;
      await send('Page.navigate', { url: destination }, tab);
      observations[currentCase] = await until(() => evaluate(`location.href === ${JSON.stringify(destination)} &&
        document.readyState === 'complete' && document.querySelector('#complete') && ({
          started: window.playerStarted === true,
          rewritten: document.querySelector('#player').textContent.includes('YOUTUBE_PREMIUM_LOGO'),
          errors: window.bootErrors
        })`, tab), 'the offline fixture');
    }
    if (stage === 'previous') {
      // Materialize both caches that an existing profile can use on reopen.
      // Ordinarily uBlock delays the full compiled-engine snapshot until idle.
      const cached = await evaluate(`(async () => {
        const blocker = (await import('./js/background.js')).default;
        const assets = (await import('./js/assets.js')).default;
        await blocker.selfieManager.create();
        const filters = await assets.fromCache('compiled/ublock-quick-fixes');
        const snapshot = await assets.fromCache('selfie/staticMain');
        return filters.includes('YOUTUBE_PREMIUM_LOGO') &&
          snapshot?.magic === blocker.systemSettings.selfieMagic;
      })()`, extension);
      assert.equal(cached, true, 'Expected old compiled list and filtering-engine snapshot');
    }
    return observations;
  } finally {
    try { await send('Browser.close'); } finally {
      const timeout = setTimeout(() => child.kill('SIGKILL'), 10_000);
      try {
        const [status, signal] = await exited;
        assert.equal(signal, null);
        assert.equal(status, 0);
      } finally { clearTimeout(timeout); }
    }
  }
}

const directory = await mkdtemp(join(tmpdir(), 'noventra-youtube-startup-'));
try {
  const scenarios = previousBrowser ? [['previous', previousBrowser], ['upgraded', browser]] : [['fresh', browser]];
  for (const [name, executable] of scenarios) {
    const observations = await run(executable, directory, name);
    console.log(JSON.stringify({ name, observations }));
    for (const [variant, sample] of Object.entries(observations)) {
      assert.equal(sample.rewritten, true, name + '/' + variant + ' did not exercise the bundled filter');
      const expectedFailure = name === 'previous' && (variant === 'missing' || variant === 'attributeMissing');
      assert.equal(sample.started, !expectedFailure, name + '/' + variant + ' player startup');
      if (expectedFailure) assert.match(sample.errors.join(' '), /TypeError.*(attributes|value)/);
      else assert.deepEqual(sample.errors, [], name + '/' + variant + ' page errors');
    }
  }
} finally { await rm(directory, { recursive: true, force: true }); }
