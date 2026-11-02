// Copyright 2026 The Noventra Authors
// Public reads only. Offline-signed publication uses a separate upload bridge.

const CHANNEL_KEY = 'browser/channels/stable/linux-x86_64.json';
const FEED = '/browser/updates/stable/linux-x86_64/';
const MAX_CATALOG = 16 * 1024;
const MAX_MANIFEST = 64 * 1024;
const FILE = /^[A-Za-z0-9][A-Za-z0-9._+-]{0,150}$/;
const HASH = /^[a-f0-9]{64}$/;
const RELEASE = /^[0-9]{20}-[a-f0-9]{64}$/;

/** @typedef {{filename: string, size: number, sha256: string}} FileRecord */
/** @typedef {{format: number, product: string, channel: string, target: string, sequence: number, version_label: string, release_id: string, manifest_sha256: string, artifact: FileRecord, source: FileRecord}} Catalog */
/** @param {unknown} value @returns {value is Record<string, unknown>} */
function object(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }
/** @param {unknown} value @returns {value is string} */
function name(value) { return typeof value === 'string' && FILE.test(value); }
/** @param {unknown} value @returns {value is string} */
function hash(value) { return typeof value === 'string' && HASH.test(value); }
/** @param {string} type @param {string} cache */
function headers(type, cache = 'no-store') {
  return new Headers({
    'Content-Type': type,
    'Cache-Control': cache,
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'no-referrer',
    'Strict-Transport-Security': 'max-age=31536000',
    'Content-Security-Policy': "default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
  });
}

/** @param {string} text */
function response(text, status = 200, head = false) {
  return new Response(head ? null : text, {status, headers: headers('text/plain; charset=utf-8')});
}

/** @param {unknown} value @returns {value is FileRecord} */
function record(value) {
  return object(value) && name(value.filename) && hash(value.sha256)
      && typeof value.size === 'number' && Number.isSafeInteger(value.size) && value.size > 0;
}

/** @param {unknown} value @returns {Catalog} */
export function validateCatalog(value) {
  if (!object(value) || value.format !== 1 || value.product !== 'local.noventra.browser'
      || value.channel !== 'stable' || value.target !== 'linux-x86_64'
      || typeof value.sequence !== 'number' || !Number.isSafeInteger(value.sequence) || value.sequence < 1
      || !name(value.version_label) || typeof value.release_id !== 'string' || !RELEASE.test(value.release_id)
      || value.release_id !== `${String(value.sequence).padStart(20, '0')}-${value.manifest_sha256}`
      || !hash(value.manifest_sha256)
      || !record(value.artifact) || !value.artifact.filename.endsWith('.tar.xz')
      || !record(value.source) || !/\.(tar\.xz|zip)$/.test(value.source.filename)) {
    throw new Error('Invalid release catalog');
  }
  return /** @type {Catalog} */ (value);
}

/** @param {R2Bucket} bucket @param {string} key @param {number} limit */
async function small(bucket, key, limit) {
  const object = await bucket.get(key);
  if (!object) return null;
  if (object.size > limit) {
    await object.body.cancel();
    throw new Error('Metadata exceeds limit');
  }
  const bytes = await object.arrayBuffer();
  if (bytes.byteLength > limit) throw new Error('Metadata exceeds limit');
  return bytes;
}

/** @param {R2Bucket} bucket */
async function catalog(bucket) {
  const raw = await small(bucket, CHANNEL_KEY, MAX_CATALOG);
  return raw ? validateCatalog(JSON.parse(new TextDecoder().decode(raw))) : null;
}

/** @param {Request} request @param {R2Bucket} bucket @param {string} name @param {boolean} source */
async function download(request, bucket, name, source = false) {
  const prefix = source ? 'browser/source-index' : 'browser/artifact-index/linux-x86_64';
  const raw = await small(bucket, `${prefix}/${name}.json`, 2048);
  if (!raw) return response('Not found', 404, request.method === 'HEAD');
  const index = JSON.parse(new TextDecoder().decode(raw));
  if (!record(index) || index.filename !== name) throw new Error('Invalid filename index');
  const root = source ? 'browser/sources' : 'browser/artifacts/linux-x86_64';
  return objectResponse(request, bucket, `${root}/${index.sha256}/${name}`, {name, expected: index.size});
}

/** @param {unknown} value */
export function escapeHtml(value) {
  /** @type {Record<string, string>} */
  const escapes = {'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'};
  return String(value).replace(/[&<>"']/g, c => escapes[c] ?? c);
}

/** @param {Catalog | null} current */
function page(current) {
  const available = current !== null;
  const version = available ? escapeHtml(current.version_label) : 'Coming soon';
  const download = available
      ? `<a class="download" href="/browser/artifacts/linux-x86_64/${current.artifact.filename}">Download for Linux <span>↓</span></a><p class="meta">x86_64 · ${Math.ceil(current.artifact.size / 1024 / 1024)} MB · ${version}</p>`
      : '<p class="notice">The first Linux preview is being prepared. Downloads will appear here after verification.</p>';
  const details = available ? `<details><summary>Verify your download</summary><p>SHA-256</p><code>${current.artifact.sha256}</code><p><a href="/browser/sources/${current.source.filename}">Download the source &amp; build kit</a></p><p class="subtle">Exact Noventra changes, pinned upstream sources and build instructions. This is not a bundled Chromium source tree or a reproducible-build guarantee.</p><p>Source kit SHA-256</p><code>${current.source.sha256}</code></details>` : '';
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#17191f"><title>Download Noventra Browser</title><style>
*{box-sizing:border-box}body{margin:0;background:#17191f;color:#f4f5f8;font:16px/1.6 system-ui,sans-serif}main{max-width:860px;margin:auto;padding:38px 26px 72px}nav{display:flex;justify-content:space-between;gap:24px;align-items:center;margin-bottom:76px}a{color:#bfcaff;text-underline-offset:4px}.brand{font-size:23px;font-weight:600;color:inherit;text-decoration:none;letter-spacing:-.6px}.eyebrow{color:#bfcaff;font-size:13px;letter-spacing:.08em;text-transform:uppercase}h1{font-size:clamp(36px,7vw,64px);letter-spacing:-2px;line-height:1.08;max-width:700px;margin:16px 0 24px}h2{font-size:22px;letter-spacing:-.5px;margin-top:42px}.intro{color:#b7bbc7;max-width:590px;font-size:19px}.download{display:inline-flex;gap:30px;align-items:center;background:#c5ceff;color:#17191f;font-weight:650;text-decoration:none;border-radius:12px;padding:14px 22px;margin:24px 0 4px}.download:focus-visible,a:focus-visible,summary:focus-visible{outline:3px solid #fff;outline-offset:5px}.download:hover{background:#d8deff}.meta,.subtle{font-size:14px;color:#b7bbc7}.notice{border:1px solid #4c5266;border-radius:12px;padding:20px}.steps{padding-left:22px;max-width:680px}.steps li{padding:5px 0}code{font:14px/1.6 ui-monospace,monospace;overflow-wrap:anywhere;background:#252833;padding:3px 6px;border-radius:4px}details{margin-top:30px;border:1px solid #393d49;border-radius:12px;padding:16px 20px}summary{cursor:pointer}details p{margin-bottom:8px}table{border-collapse:collapse;width:100%;margin-top:24px}td,th{text-align:left;border-bottom:1px solid #393d49;padding:12px 0;font-weight:400}th{color:#b7bbc7}footer{margin-top:48px;padding-top:20px;border-top:1px solid #393d49;color:#b7bbc7;font-size:14px}@media(prefers-color-scheme:light){body{background:#f6f7fc;color:#20232d}.intro,.meta,.subtle,th,footer{color:#585d6d}a,.eyebrow{color:#3d4f9c}code{background:#e6e9f3}td,th,details,footer{border-color:#cfd4e2}.download{background:#36457f;color:white}.download:hover{background:#46599f}.download:focus-visible,a:focus-visible,summary:focus-visible{outline-color:#36457f}}
</style></head><body><main><nav><a class="brand" href="https://noventrastudio.com/">noventra</a><a href="https://noventrastudio.com/">Noventra Studio ↗</a></nav><p class="eyebrow">Noventra Browser · Linux preview</p><h1>A little more room<br>for your internet.</h1><p class="intro">Spaces, vertical tabs and a browser you can arrange around the way you work.</p>${download}<h2>Install in your user account</h2><ol class="steps"><li>Extract the downloaded archive.</li><li>Open the extracted folder in a terminal and run <code>./Noventra.AppDir/install.sh</code>. No sudo is needed.</li><li>Open Noventra from your application menu.</li></ol><p class="subtle">This is a Linux x86_64 preview. Installation requires Python 3.8 or newer. Close all Noventra windows before installing over an existing copy. Your browser profile stays in your user account.</p><h2>Updates, without reinstalling every time</h2><p>After per-user installation, Noventra checks for signed updates in the background. A downloaded update installs the next time you open Noventra after closing all its windows. You can also check from About Noventra. Updates never force the browser to quit.</p><p class="subtle">Automatic updates require Python 3.11 or newer at <code>/usr/bin/python3</code>, OpenSSL with Ed25519 support, and enough free disk space. Replacing an installed app also requires Linux and filesystem support for atomic directory exchange. If a requirement is unmet, the installed version is kept. Python 3.8–3.10 can install and launch the browser, but cannot run its online updater.</p><table aria-label="Platform availability"><thead><tr><th>Platform</th><th>Availability</th></tr></thead><tbody><tr><td>Linux x86_64</td><td>${available ? 'Preview available' : 'In preparation'}</td></tr><tr><td>macOS / Apple silicon</td><td>Not available yet</td></tr><tr><td>Windows</td><td>Not available yet</td></tr></tbody></table>${details}<footer><a href="https://github.com/alexsimeonoww/noventra-browser">Noventra source on GitHub</a>. Based on <a href="https://www.chromium.org/chromium-projects/">Chromium</a> and <a href="https://github.com/imputnet/helium-linux">Helium</a>. Upstream source references and license notices are also included in the source &amp; build kit. Signed update delivery verifies the release; the Linux application is distributed as an unsigned AppDir.</footer></main></body></html>`;
}

/** @param {string | null} header @param {number} size */
export function byteRange(header, size) {
  if (!header) return null;
  const match = /^bytes=(\d*)-(\d*)$/.exec(header);
  if (!match || (!match[1] && !match[2])) throw new Error('Invalid byte range');
  let first;
  let last;
  if (!match[1]) {
    const suffix = Number(match[2]);
    if (!Number.isSafeInteger(suffix) || suffix < 1) throw new Error('Invalid byte range');
    first = Math.max(0, size - suffix);
    last = size - 1;
  } else {
    first = Number(match[1]);
    last = match[2] ? Math.min(Number(match[2]), size - 1) : size - 1;
    if (!Number.isSafeInteger(first) || !Number.isSafeInteger(last)
        || first >= size || last < first) throw new Error('Invalid byte range');
  }
  return {offset: first, length: last - first + 1};
}

/** @param {Request} request @param {R2Bucket} bucket @param {string} key
 * @param {{name?: string, cache?: string, expected?: number, type?: string}} options */
async function objectResponse(request, bucket, key, options = {}) {
  const metadata = await bucket.head(key);
  const head = request.method === 'HEAD';
  if (!metadata) return response('Not found', 404, head);
  if (options.expected !== undefined && metadata.size !== options.expected) {
    return response('Release unavailable', 503, head);
  }
  const h = headers(options.type || 'application/octet-stream', options.cache || 'public, max-age=31536000, immutable');
  h.set('ETag', metadata.httpEtag);
  h.set('Accept-Ranges', 'bytes');
  if (options.name) h.set('Content-Disposition', `attachment; filename="${options.name}"`);
  const tags = request.headers.get('If-None-Match');
  if (tags && tags.split(',').some(t => t.trim() === '*' || t.trim().replace(/^W\//, '') === metadata.httpEtag)) {
    return new Response(null, {status: 304, headers: h});
  }
  let range;
  try {
    const ifRange = request.headers.get('If-Range');
    range = byteRange(!ifRange || ifRange === metadata.httpEtag ? request.headers.get('Range') : null, metadata.size);
  } catch {
    h.set('Content-Range', `bytes */${metadata.size}`);
    return new Response(null, {status: 416, headers: h});
  }
  const size = range ? range.length : metadata.size;
  h.set('Content-Length', String(size));
  if (range) h.set('Content-Range', `bytes ${range.offset}-${range.offset + range.length - 1}/${metadata.size}`);
  if (head) return new Response(null, {status: range ? 206 : 200, headers: h});
  const object = await bucket.get(key, {onlyIf: {etagMatches: metadata.etag}, ...(range ? {range} : {})});
  if (!object || !('body' in object)) return response('Release changed; retry', 503);
  return new Response(object.body, {status: range ? 206 : 200, headers: h});
}

/** @param {Request} request @param {Env} env */
export async function handle(request, env) {
  const head = request.method === 'HEAD';
  if (!['GET', 'HEAD'].includes(request.method)) {
    const result = response('Method not allowed', 405, head);
    result.headers.set('Allow', 'GET, HEAD');
    return result;
  }
  const url = new URL(request.url);
  if (url.search || /[%\\]/.test(url.pathname)) return response('Not found', 404, head);
  if (['/', '/browser', '/browser/'].includes(url.pathname)) {
    const current = await catalog(env.RELEASES);
    return new Response(head ? null : page(current), {headers: headers('text/html; charset=utf-8')});
  }
  if (url.pathname.startsWith(FEED)) {
    const name = url.pathname.slice(FEED.length);
    if (!FILE.test(name)) return response('Not found', 404, head);
    if (name.endsWith('.tar.xz')) {
      return download(request, env.RELEASES, name);
    }
    if (!['manifest.json', 'manifest.sig', 'distribution.json', 'source-info.json'].includes(name)) return response('Not found', 404, head);
    const current = await catalog(env.RELEASES);
    if (!current) return response('No release available', 404, head);
    const key = `browser/releases/stable/linux-x86_64/${current.release_id}/${name}`;
    if (name === 'manifest.json') {
      const bytes = await small(env.RELEASES, key, MAX_MANIFEST);
      if (!bytes) return response('Release unavailable', 503, head);
      const digest = [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))].map(b => b.toString(16).padStart(2, '0')).join('');
      if (digest !== current.manifest_sha256) return response('Release unavailable', 503, head);
      return new Response(head ? null : bytes, {headers: headers('application/json')});
    }
    return objectResponse(request, env.RELEASES, key, {cache: 'no-store', ...(name === 'manifest.sig' ? {expected: 64} : {}), type: name.endsWith('.json') ? 'application/json' : 'application/octet-stream'});
  }
  const artifact = /^\/browser\/artifacts\/linux-x86_64\/([^/]+)$/.exec(url.pathname);
  const source = /^\/browser\/sources\/([^/]+)$/.exec(url.pathname);
  if (artifact && FILE.test(artifact[1]) && artifact[1].endsWith('.tar.xz')) {
    return download(request, env.RELEASES, artifact[1]);
  }
  if (source && FILE.test(source[1]) && /\.(tar\.xz|zip)$/.test(source[1])) {
    return download(request, env.RELEASES, source[1], true);
  }
  return response('Not found', 404, head);
}

/** @type {ExportedHandler<Env>} */
export default {
  async fetch(request, env) {
    try {
      return await handle(request, env);
    } catch {
      // Keep storage diagnostics and metadata out of public responses/logs.
      return response('Release service temporarily unavailable', 503, request.method === 'HEAD');
    }
  },
};
