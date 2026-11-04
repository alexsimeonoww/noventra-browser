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
    'Content-Security-Policy': type.startsWith('text/html')
      ? "default-src 'none'; style-src 'unsafe-inline' https://www.noventrastudio.com; font-src https://www.noventrastudio.com; img-src https://www.noventrastudio.com; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
      : "default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
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

/** @typedef {'linux' | 'macos' | 'windows' | 'android' | 'ios' | 'chromeos' | 'unknown'} Platform */
/** @typedef {'x86_64' | 'x86' | 'arm64' | 'arm' | 'unknown'} Architecture */
/** @typedef {{platform: Platform, architecture: Architecture}} Device */

/** @param {Request} request @returns {Device} */
export function detectDevice(request) {
  const ua = (request.headers.get('User-Agent') ?? '').slice(0, 2048).toLowerCase();
  const platformHint = (request.headers.get('Sec-CH-UA-Platform') ?? '').toLowerCase();
  /** @type {Record<string, Platform>} */
  const platforms = {'"linux"': 'linux', '"macos"': 'macos', '"windows"': 'windows',
    '"android"': 'android', '"ios"': 'ios', '"chrome os"': 'chromeos', '"chromium os"': 'chromeos'};
  const platform = (Object.hasOwn(platforms, platformHint) ? platforms[platformHint] : undefined) ?? (
    /android/.test(ua) ? 'android'
      : /iphone|ipad|ipod|macintosh.*mobile/.test(ua) ? 'ios'
      : /\bcros\b/.test(ua) ? 'chromeos'
      : /windows nt/.test(ua) ? 'windows'
      : /macintosh|mac os x/.test(ua) ? 'macos'
      : /linux/.test(ua) ? 'linux' : 'unknown');
  const archHint = request.headers.get('Sec-CH-UA-Arch');
  const bits = request.headers.get('Sec-CH-UA-Bitness');
  /** @type {Architecture} */
  let architecture = /aarch64|arm64/.test(ua) ? 'arm64'
    : /armv\d/.test(ua) ? 'arm'
    : /x86_64|amd64|win64|wow64|x64/.test(ua) ? 'x86_64'
    : /i[3-6]86/.test(ua) ? 'x86' : 'unknown';
  if (archHint === '"arm"') architecture = bits === '"64"' ? 'arm64' : 'arm';
  if (archHint === '"x86"') {
    architecture = bits === '"64"' ? 'x86_64' : bits === '"32"' ? 'x86'
      : architecture === 'x86_64' ? 'x86_64' : 'unknown';
  }
  // Macintosh UAs report Intel on Apple silicon too; do not infer a Mac CPU.
  if (platform === 'macos' && archHint !== '"arm"' && archHint !== '"x86"') architecture = 'unknown';
  return {platform, architecture};
}

/** @param {Catalog | null} current @param {Device} device */
function page(current, device) {
  const studio = 'https://www.noventrastudio.com';
  const github = 'https://github.com/alexsimeonoww/noventra-browser';
  const arrow = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" aria-hidden="true"><path d="M5 12h14m-6-6 6 6-6 6" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>';
  const available = current !== null;
  const matchesLinux = device.platform === 'linux'
      && ['x86_64', 'unknown'].includes(device.architecture);
  /** @type {Record<Platform, string>} */
  const platformNames = {linux: 'Linux', macos: 'macOS', windows: 'Windows',
    android: 'Android', ios: 'iOS or iPadOS', chromeos: 'ChromeOS', unknown: ''};
  const deviceNotice = !available || matchesLinux ? ''
    : device.platform === 'unknown' ? 'Choose your platform below. The current preview is for Linux x86_64.'
      : device.platform === 'linux' ? 'This preview needs a 64-bit Intel or AMD Linux computer. A Linux ARM or 32-bit build is not available.'
        : `No ${platformNames[device.platform]} download is available yet. The current preview is for Linux x86_64.`;
  const version = available ? escapeHtml(current.version_label) : 'Coming soon';
  const linuxDownload = available
      ? `<a class="button button-solid" id="download-linux" href="/browser/artifacts/linux-x86_64/${current.artifact.filename}">Download for Linux <svg width="18" height="18" viewBox="0 0 24 24" fill="none" aria-hidden="true"><path d="M12 5v14m-6-6 6 6 6-6" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg></a>`
      : '';
  const download = !available
      ? '<p class="notice">The first Linux preview is being prepared. Downloads will appear here after verification.</p>'
      : matchesLinux ? linuxDownload
        : `<a class="button button-solid" href="#available-downloads">View available downloads ${arrow}</a>`;
  const metadata = available
      ? `<p class="download-meta">Linux x86_64 · ${Math.ceil(current.artifact.size / 1024 / 1024)} MB · ${version}</p>`
      : '';
  const verification = available ? `<details id="verify-download">
      <summary>Verify your download</summary>
      <div class="disclosure-content">
        <p>Compare the SHA-256 checksum of your downloaded archive with this value.</p>
        <code class="checksum">${current.artifact.sha256}</code>
        <p><a class="editorial-link" href="/browser/sources/${current.source.filename}">Download the source &amp; build kit ${arrow}</a></p>
        <p class="fine-print">Exact Noventra changes, pinned upstream sources and build instructions. This is not a bundled Chromium source tree or a reproducible-build guarantee.</p>
        <p class="fine-print">Source kit SHA-256</p>
        <code class="checksum">${current.source.sha256}</code>
      </div>
    </details>` : '';
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#ffffff">
  <meta name="description" content="Download the Noventra Linux preview. Find installation guidance, signed updates and the open source project on GitHub.">
  <title>Download Noventra Browser</title>
  <link rel="stylesheet" href="${studio}/assets/fonts/geist.css">
  <link rel="icon" type="image/png" href="${studio}/assets/noventra-favicon-light.png" media="(prefers-color-scheme: light)">
  <link rel="icon" type="image/png" href="${studio}/assets/noventra-favicon-dark.png" media="(prefers-color-scheme: dark)">
  <style>
:root{font-family:Geist,"Geist Fallback",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:#111;background:#fff;font-synthesis:none;color-scheme:light;--ground:#fff;--ink:#111;--muted:#626267;--wash:#f5f5f4;--hair:#dedede;--outline:#aaa}
*{box-sizing:border-box}body{margin:0;min-width:320px;font-size:17px;line-height:1.5;-webkit-font-smoothing:antialiased}a{color:inherit;text-decoration:none}a,summary{-webkit-tap-highlight-color:transparent;touch-action:manipulation}p,h1,h2{margin:0}:focus-visible{outline:2px solid var(--ink);outline-offset:5px}.page-width{width:87.8%;max-width:1440px;margin-inline:auto}
.skip-link{position:fixed;z-index:20;top:12px;left:12px;background:var(--ink);color:var(--ground);padding:12px 18px;transform:translateY(-150%)}.skip-link:focus{transform:translateY(0)}
.site-header{background:var(--ground)}.header-content{height:64px;display:flex;align-items:center;justify-content:space-between;gap:24px}.brand{display:inline-flex;align-items:center;gap:10px;font-size:20px;font-weight:500;white-space:nowrap}.brand-symbol{object-fit:contain;filter:brightness(0);display:block;flex-shrink:0}.site-header nav{display:flex;align-items:center;gap:26px;height:100%;font-size:14px;color:var(--muted)}.site-header nav a{display:flex;align-items:center;height:100%;border-bottom:2px solid transparent;padding-top:2px}.site-header nav a[aria-current]{color:var(--ink);border-color:var(--ink)}.site-header nav a:hover{color:var(--ink)}.site-header nav .contact-link{height:26px;margin-left:10px;padding-left:26px;border-left:1px solid var(--hair);border-bottom:0}
.eyebrow{text-transform:uppercase;font-size:12px;font-weight:500;letter-spacing:2px;color:var(--muted)}.horizon-hero{position:relative;min-height:clamp(600px,48vw,770px);isolation:isolate;overflow:hidden;background:#fff;color:#080a0d}.horizon-landscape{position:absolute;inset:0;width:100%;height:100%;object-fit:cover;object-position:center;z-index:-1}.hero-copy{padding-top:clamp(125px,10vw,165px);padding-bottom:100px}.hero-copy .eyebrow{color:#4e555e;font-size:13px}.hero-copy h1{font-size:clamp(44px,4.25vw,68px);letter-spacing:-2.5px;font-weight:500;line-height:1.1;margin:20px 0 16px;max-width:960px}.hero-description{font-size:22px;line-height:1.4;color:#353b43;max-width:650px}.hero-actions{display:flex;align-items:center;flex-wrap:wrap;gap:24px 40px;margin-top:26px}.button{display:inline-flex;align-items:center;justify-content:center;gap:14px;min-height:48px;padding:0 26px;border:1px solid transparent;border-radius:999px;font-size:15px;font-weight:500;transition:background .18s;white-space:nowrap}.button-solid{background:#07090d;color:#fff}.button-solid:hover{background:#333}.text-link{display:inline-flex;align-items:center;gap:12px;color:#51555b;font-size:16px}.text-link:hover{color:var(--ink)}.download-meta{color:#4e555e;font-size:13px;margin-top:16px;overflow-wrap:anywhere}.notice{max-width:600px;font-size:18px;line-height:1.6}
.editorial-link{display:inline-flex;align-items:center;gap:14px;font-size:16px;font-weight:500;border-bottom:1px solid #b8b8b8;padding-bottom:3px}.editorial-link:hover{border-color:var(--ink)}.download-content{display:grid;grid-template-columns:1.1fr 1fr;gap:clamp(50px,7.5vw,115px);padding-block:42px 56px}.download-content h2,.platforms h2{font-weight:500;font-size:28px;letter-spacing:-.6px;line-height:1.3;margin:14px 0 24px}.download-content p:not(.eyebrow){color:var(--muted);max-width:650px}.download-content p+p{margin-top:18px}.install-steps{padding-left:23px;margin:0 0 20px;color:var(--muted)}.install-steps li{padding:0 0 18px 6px}.install-steps code{display:inline-block;margin-top:8px}.fine-print{font-size:13px;line-height:1.6;color:var(--muted)}code{font:14px/1.6 ui-monospace,SFMono-Regular,Consolas,monospace;overflow-wrap:anywhere;background:var(--wash);padding:4px 7px;border-radius:4px}.platforms{padding-block:36px 42px;border-top:1px solid var(--hair)}table{border-collapse:collapse;width:100%;font-size:16px}th,td{text-align:left;border-bottom:1px solid var(--hair);padding:17px 0;font-weight:400}thead th{color:var(--muted);font-size:13px}th[scope=row]{width:50%}
.download-disclosures{padding-bottom:60px}.download-disclosures details{border-top:1px solid var(--hair)}.download-disclosures summary{padding-block:22px;cursor:pointer;font-size:16px;font-weight:500}.disclosure-content{max-width:840px;padding-bottom:24px;color:var(--muted);line-height:1.7}.disclosure-content p+p,.disclosure-content code+p{margin-top:18px}.disclosure-content a:not(.editorial-link){text-decoration:underline;text-underline-offset:4px}.disclosure-content .fine-print{margin-top:18px}.checksum{display:block;margin-top:12px;white-space:normal;overflow-wrap:anywhere;max-width:100%;padding:12px 14px}.site-footer{display:flex;align-items:center;justify-content:space-between;gap:20px;border-top:1px solid var(--hair);padding:20px 0 25px;font-size:13px;color:var(--muted)}.site-footer p span{margin-left:12px}.site-footer nav{display:flex;flex-wrap:wrap;gap:18px}.site-footer a:hover{color:var(--ink)}
@media(max-width:1000px){.page-width{width:calc(100% - 64px)}.site-header nav{gap:18px}.site-header nav .contact-link{padding-left:18px;margin-left:0}.hero-copy h1{font-size:50px}.hero-description{font-size:20px}}
@media(max-width:720px){.page-width{width:calc(100% - 40px)}.header-content{height:auto;min-height:96px;flex-wrap:wrap;gap:10px;padding-block:14px}.brand{font-size:18px}.brand-symbol{width:29px;height:29px}.site-header nav{width:100%;height:auto;flex-wrap:wrap;gap:4px 20px;font-size:13px}.site-header nav a{height:34px;min-height:34px;padding-top:0}.site-header nav .contact-link{height:26px;margin-left:auto;padding-left:15px}.horizon-hero{min-height:660px}.hero-copy{padding-top:66px;padding-bottom:120px}.hero-copy h1{font-size:46px;line-height:1.1;letter-spacing:-1.8px;max-width:440px;margin-top:20px}.hero-description{font-size:19px;line-height:1.5;max-width:440px}.hero-actions{align-items:flex-start;flex-direction:column;gap:20px;margin-top:25px}.hero-copy .eyebrow{font-size:11px;letter-spacing:1.6px}.hero-copy .text-link{font-size:15px}.horizon-landscape{object-position:40% center}.download-meta{max-width:300px;font-size:12px}.download-content{grid-template-columns:1fr;gap:40px;padding-block:32px 40px}.download-content h2,.platforms h2{font-size:25px}.download-content p:not(.eyebrow),.install-steps{font-size:16px}.platforms{padding-top:30px}th,td{font-size:14px;vertical-align:top}th[scope=row]{width:55%}.download-disclosures{padding-bottom:40px}.disclosure-content{font-size:15px}.editorial-link{font-size:15px}.site-footer{align-items:flex-start;font-size:12px;flex-wrap:wrap;padding-block:20px}.site-footer nav{gap:16px}.checksum{font-size:12px}}
@media(max-width:380px){.site-header nav{column-gap:16px}.site-header nav .contact-link{display:none}.hero-copy h1{font-size:41px}.hero-description{font-size:18px}}
@media(prefers-reduced-motion:reduce){*,::before,::after{transition:none!important;animation:none!important}}
  </style>
</head>
<body>
  <a class="skip-link" href="#main">Skip to content</a>
  <header class="site-header">
    <div class="header-content page-width">
      <a class="brand" href="${studio}/" aria-label="Noventra home"><img class="brand-symbol" src="${studio}/assets/noventra-symbol.png" width="34" height="34" alt=""><span>Noventra</span></a>
      <nav aria-label="Main navigation">
        <a href="${studio}/">Home</a>
        <a href="${studio}/browser">Browser</a>
        <a href="https://downloads.noventrastudio.com/" aria-current="page">Download</a>
        <a href="${studio}/about">About</a>
        <a href="${studio}/support">Support</a>
        <a href="${github}">GitHub</a>
        <a class="contact-link" href="mailto:hello@noventrastudio.com">Contact</a>
      </nav>
    </div>
  </header>
  <main id="main">
    <section class="horizon-hero" aria-labelledby="download-title">
      <img class="horizon-landscape" src="${studio}/assets/open-horizon.webp" alt="" fetchpriority="high">
      <div class="hero-copy page-width">
        <p class="eyebrow">Noventra Browser · Linux preview</p>
        <h1 id="download-title">Download Noventra.</h1>
        <p class="hero-description">Your tabs, your spaces, your way to browse.<br>${available ? deviceNotice || 'Try the Linux preview and follow the project on GitHub.' : 'The first Linux preview is on its way. Follow development on GitHub.'}</p>
        <div class="hero-actions">
          ${download}
          <a class="text-link" href="${github}">View on GitHub ${arrow}</a>
        </div>
        ${matchesLinux ? metadata : ''}
      </div>
    </section>
    <div class="download-content page-width">
      <section aria-labelledby="install-title">
        <p class="eyebrow">Get started</p>
        <h2 id="install-title">Make yourself at home.</h2>
        <ol class="install-steps">
          <li>Extract the downloaded archive.</li>
          <li>Open the extracted folder in a terminal and run<br><code>./Noventra.AppDir/install.sh</code></li>
          <li>Open Noventra from your application menu.</li>
        </ol>
        <p class="fine-print">No sudo is needed. Close all Noventra windows before installing over an existing copy. Your browser profile stays in your user account.</p>
      </section>
      <section aria-labelledby="updates-title">
        <p class="eyebrow">Keep moving forward</p>
        <h2 id="updates-title">Updates, without starting over.</h2>
        <p>After installation, Noventra checks for signed updates in the background. When an update is ready, choose <strong>Update and restart</strong> in About Noventra.</p>
        <p>You can also close all Noventra windows and reopen the browser to install a downloaded update. Updates never force the browser to quit.</p>
        <p><a class="editorial-link" href="${github}/releases">See the releases ${arrow}</a></p>
      </section>
    </div>
    <section class="platforms page-width" id="available-downloads" aria-labelledby="platforms-title">
      <p class="eyebrow">Find your platform</p>
      <h2 id="platforms-title">${available ? 'Available on Linux.' : 'Linux preview in preparation.'}</h2>
      <table aria-label="Platform availability">
        <thead><tr><th scope="col">Platform</th><th scope="col">Availability</th></tr></thead>
        <tbody>
          <tr><th scope="row">Linux x86_64</th><td>${available ? matchesLinux ? 'Preview available' : `<a id="download-linux" href="/browser/artifacts/linux-x86_64/${current.artifact.filename}" style="text-decoration:underline;text-underline-offset:4px">Download preview</a>` : 'In preparation'}</td></tr>
          <tr><th scope="row">macOS / Apple silicon</th><td>Not available yet</td></tr>
          <tr><th scope="row">Windows</th><td>Not available yet</td></tr>
        </tbody>
      </table>
      ${matchesLinux ? '' : metadata}
      <p class="fine-print" style="margin-top:18px">x86_64 means a 64-bit Intel or AMD computer. macOS and Windows downloads will be listed here when available.</p>
    </section>
    <section class="download-disclosures page-width" aria-label="Download details">
      ${verification}
      <details>
        <summary>System requirements</summary>
        <div class="disclosure-content">
          <p>This is a Linux x86_64 preview. Installation requires Python 3.8 or newer.</p>
          <p>Automatic updates require Python 3.11 or newer at <code>/usr/bin/python3</code>, OpenSSL with Ed25519 support, and enough free disk space. Replacing an installed app also requires Linux and filesystem support for atomic directory exchange.</p>
          <p>If a requirement is unmet, the installed version is kept. Python 3.8–3.10 can install and launch the browser, but cannot run its online updater.</p>
        </div>
      </details>
      <details>
        <summary>Licenses and credits</summary>
        <div class="disclosure-content">
          <p>Noventra is open source and includes work from <a href="https://www.chromium.org/chromium-projects/">Chromium</a>, <a href="https://github.com/imputnet/helium-linux">Helium</a> and other open source projects. Original copyright notices and licenses are retained with the application and source &amp; build kit.</p>
          <p><a href="${github}/blob/main/noventra/package/CREDITS.md">Read the credits</a> or <a href="${github}/blob/main/LICENSE">view the license</a>.</p>
          <p class="fine-print">Signed update delivery verifies the release; the Linux application is distributed as an unsigned AppDir.</p>
        </div>
      </details>
    </section>
  </main>
  <footer class="site-footer page-width">
    <p>Noventra Studio <span>© ${new Date().getUTCFullYear()}</span></p>
    <nav aria-label="Footer navigation"><a href="${github}">GitHub</a><a href="${studio}/support">Support</a><a href="mailto:hello@noventrastudio.com">Contact</a></nav>
  </footer>
</body>
</html>`;
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
    const h = headers('text/html; charset=utf-8');
    h.set('Vary', 'User-Agent, Sec-CH-UA-Platform, Sec-CH-UA-Arch, Sec-CH-UA-Bitness');
    return new Response(head ? null : page(current, detectDevice(request)), {headers: h});
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
