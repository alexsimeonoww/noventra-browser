// Run under a private Xvfb display: node youtube_player_startup.mjs BROWSER [PREVIOUS_BROWSER].
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
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
    <script>window.ytcfg = {data_: {
      INNERTUBE_CONTEXT: {client: {userAgent: navigator.userAgent}},
      EXPERIMENT_FLAGS: {all_web_enable_network_machine: true}
    }};
    window.bootErrors = []; window.addEventListener('error', e => bootErrors.push(e.message));</script>
    ${header}
    <div id="movie_player"></div>
    <script>
      window.restarts = [];
      Object.assign(document.getElementById('movie_player'), {
        getPlayerStateObject: () => ({isBuffering: true}),
        getStatsForNerds: () => ({buffer_health_seconds: '0.00 s', resolution: '0x0'}),
        getProgressState: () => ({current: 0, loaded: 0, duration: 2745}),
        getPlayerResponse: () => ({
          videoDetails: {videoId: 'noventra-fixture'}, playabilityStatus: {status: 'OK'}
        }),
        loadVideoById: (id, start) => window.restarts.push([id, start])
      });
    </script>
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
    // Seed the previous browser with an older downloaded list and an extra
    // independent rule. Both the raw download and its other rules must survive.
    const raw = await evaluate(`(async () => {
      const assets = (await import('./js/assets.js')).default;
      const blocker = (await import('./js/background.js')).default;
      const key = 'ublock-quick-fixes';
      if (${stage === 'previous'}) {
        const { content } = await assets.get(key, { favorLocal: true });
        const extra = 'www.youtube.com##+js(set, noventraCachedListWitness, true)';
        await assets.put(key, content + String.fromCharCode(10) + extra);
        await assets.remove('compiled/' + key);
        await blocker.loadFilterLists();
      }
      if (${stage === 'fresh'}) return '';
      return assets.fromCache(key);
    })()`, extension);
    const rawDigest = createHash('sha256').update(raw).digest('hex');
    const hasLegacyHeader = raw.includes('document.getElementById("masthead").attributes["logo-type"].value');
    if (stage !== 'fresh') assert(raw.includes('noventraCachedListWitness'));
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
      await until(() => evaluate(`location.href === ${JSON.stringify(destination)} &&
        document.readyState === 'complete' && !!document.querySelector('#complete')`, tab),
        'the offline fixture');
      observations[currentCase] = await evaluate(`(async () => {
        // YouTube checks this key during startup. Between checks, ordinary
        // page mutations run while the first media response is still pending.
        const messages = new Map();
        for (let i = 0; i < 3; i++) {
          messages.has('onSnackbarMessage');
          document.querySelector('#complete').append(document.createElement('span'));
          await new Promise(resolve => setTimeout(resolve, 25));
        }
        return {
          started: window.playerStarted === true,
          restarts: window.restarts,
          networkFlag: window.ytcfg.data_.EXPERIMENT_FLAGS.all_web_enable_network_machine,
          cachedListWitness: window.noventraCachedListWitness === true,
          errors: window.bootErrors
        };
      })()`, tab);
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
        return filters.includes('onSnackbarMessage') && filters.includes('noventraCachedListWitness') &&
          snapshot?.magic === blocker.systemSettings.selfieMagic;
      })()`, extension);
      assert.equal(cached, true, 'Expected old compiled list and filtering-engine snapshot');
    }
    return { observations, rawDigest, hasLegacyHeader };
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
  let previousDigest;
  for (const [name, executable] of scenarios) {
    const { observations, rawDigest, hasLegacyHeader } = await run(executable, directory, name);
    console.log(JSON.stringify({ name, observations }));
    if (name === 'previous') previousDigest = rawDigest;
    if (name === 'upgraded') assert.equal(rawDigest, previousDigest, 'Raw cached Quick fixes changed');
    for (const [variant, sample] of Object.entries(observations)) {
      const label = name + '/' + variant;
      const headerError = name === 'previous' && hasLegacyHeader &&
        (variant === 'missing' || variant === 'attributeMissing');
      assert.equal(sample.started, !headerError, label + ' player startup');
      if (headerError) assert.match(sample.errors.join(' '), /TypeError.*(attributes|value)/);
      else assert.deepEqual(sample.errors, [], label + ' page errors');
      if (name === 'previous' && variant === 'standard') {
        assert(sample.restarts.length >= 2, 'Previous blocker must reproduce repeated startup reloads');
      } else if (name !== 'previous' || variant === 'premium') {
        assert.deepEqual(sample.restarts, [], label + ' restarted a buffering player');
      }
      assert.equal(sample.networkFlag, false, label + ' lost an independent bundled rule');
      assert.equal(sample.cachedListWitness, name !== 'fresh', label + ' cached-list rule');
    }
  }
} finally { await rm(directory, { recursive: true, force: true }); }
