import test from 'node:test';
import assert from 'node:assert/strict';
import site, {byteRange, escapeHtml, validateCatalog} from '../../noventra/release-site/worker.mjs';
import {Bucket, sha} from './release_site_test_helpers.mjs';

const FEED = '/browser/updates/stable/linux-x86_64/';
const req = (path, options) => new Request('https://downloads.noventrastudio.com' + path, options);
test('public catalog counters retain exact integer identity across the safe-number boundary', () => {
  const {catalog} = fixture();
  const safe = {...catalog, sequence: Number.MAX_SAFE_INTEGER};
  safe.release_id = `${String(safe.sequence).padStart(20, '0')}-${safe.manifest_sha256}`;
  assert.deepEqual(validateCatalog(safe), safe);
  const tooLarge = {...safe, sequence: Number.MAX_SAFE_INTEGER + 1};
  tooLarge.release_id = `${String(tooLarge.sequence).padStart(20, '0')}-${tooLarge.manifest_sha256}`;
  assert.throws(() => validateCatalog(tooLarge), /Invalid release catalog/);
});
function fixture() {
  const bucket = new Bucket();
  const manifest = new TextEncoder().encode('{"signed":"original bytes"}\n');
  const artifact = {filename: 'noventra-156-test15.tar.xz', size: 10, sha256: sha('0123456789')};
  const source = {filename: 'noventra-156-test15-source.zip', size: 6, sha256: sha('source')};
  const catalog = {format: 1, product: 'local.noventra.browser', channel: 'stable', target: 'linux-x86_64', sequence: 1, version_label: '156-test15', manifest_sha256: sha(manifest), artifact, source};
  catalog.release_id = '00000000000000000001-' + catalog.manifest_sha256;
  const prefix = `browser/releases/stable/linux-x86_64/${catalog.release_id}/`;
  bucket.save('browser/channels/stable/linux-x86_64.json', JSON.stringify(catalog));
  bucket.save(prefix + 'manifest.json', manifest);
  bucket.save(prefix + 'manifest.sig', new Uint8Array(64));
  bucket.save(prefix + 'distribution.json', '{}');
  bucket.save('browser/artifact-index/linux-x86_64/' + artifact.filename + '.json', JSON.stringify(artifact));
  bucket.save('browser/artifacts/linux-x86_64/' + artifact.sha256 + '/' + artifact.filename, '0123456789');
  bucket.save('browser/source-index/' + source.filename + '.json', JSON.stringify(source));
  bucket.save('browser/sources/' + source.sha256 + '/' + source.filename, 'source');
  return {bucket, env: {RELEASES: bucket}, catalog, prefix};
}

test('landing provides actual Linux preview, checksum, source kit and honest unavailable platforms', async () => {
  const {env, catalog} = fixture();
  const response = await site.fetch(req('/'), env);
  assert.equal(response.status, 200);
  assert.match(response.headers.get('Content-Security-Policy'), /default-src 'none'/);
  const html = await response.text();
  assert.match(html, /Download for Linux/);
  assert.ok(html.includes(catalog.artifact.sha256));
  assert.match(html, /source &amp; build kit/);
  assert.match(html, /not a bundled Chromium source tree/);
  assert.match(html, /macOS \/ Apple silicon<\/td><td>Not available yet/);
  assert.match(html, /Windows<\/td><td>Not available yet/);
  assert.doesNotMatch(html, /href="[^" ]+\.(exe|dmg)"/);
  assert.ok(html.includes('./Noventra.AppDir/install.sh'));
});

test('no catalog gives preparation page; HEAD returns no body', async () => {
  const env = {RELEASES: new Bucket()};
  const response = await site.fetch(req('/browser'), env);
  assert.match(await response.text(), /first Linux preview is being prepared/);
  assert.equal((await site.fetch(req('/browser', {method: 'HEAD'}), env)).body, null);
  assert.equal((await site.fetch(req(FEED + 'manifest.json'), env)).status, 404);
});

test('exact signed manifest bytes survive transport; corrupted metadata fails closed', async () => {
  const {env, bucket, prefix} = fixture();
  const result = await site.fetch(req(FEED + 'manifest.json'), env);
  assert.equal(await result.text(), '{"signed":"original bytes"}\n');
  assert.equal(result.headers.get('Cache-Control'), 'no-store');
  assert.equal((await site.fetch(req(FEED + 'manifest.sig'), env)).status, 200);
  bucket.save(prefix + 'manifest.json', 'changed');
  assert.equal((await site.fetch(req(FEED + 'manifest.json'), env)).status, 503);
  bucket.save(prefix + 'manifest.sig', 'short');
  assert.equal((await site.fetch(req(FEED + 'manifest.sig'), env)).status, 503);
});

test('download ranges, HEAD, ETag and immutable cache headers use actual objects', async () => {
  const {env, bucket, catalog} = fixture();
  const path = '/browser/artifacts/linux-x86_64/' + catalog.artifact.filename;
  let result = await site.fetch(req(path, {headers: {Range: 'bytes=2-5'}}), env);
  assert.equal(result.status, 206);
  assert.equal(await result.text(), '2345');
  assert.equal(result.headers.get('Content-Range'), 'bytes 2-5/10');
  assert.equal(result.headers.get('Content-Length'), '4');
  assert.match(result.headers.get('Cache-Control'), /immutable/);
  const etag = result.headers.get('ETag');
  result = await site.fetch(req(path, {headers: {'If-None-Match': `W/${etag}`}}), env);
  assert.equal(result.status, 304);
  assert.equal(result.body, null);
  const before = bucket.gets.length;
  result = await site.fetch(req(path, {method: 'HEAD'}), env);
  assert.equal(result.body, null);
  assert.equal(result.headers.get('Content-Length'), '10');
  assert.equal(bucket.gets.length - before, 1, 'HEAD reads only small immutable filename index');
  result = await site.fetch(req(path, {headers: {Range: 'bytes=90-99'}}), env);
  assert.equal(result.status, 416);
  assert.equal(result.headers.get('Content-Range'), 'bytes */10');
  result = await site.fetch(req(path, {headers: {Range: 'bytes=2-5', 'If-Range': '"old"'}}), env);
  assert.equal(result.status, 200);
  assert.equal(await result.text(), '0123456789');
});

test('old updater archive URL and source remain readable after pointer publication', async () => {
  const {env, bucket, catalog} = fixture();
  bucket.save('browser/channels/stable/linux-x86_64.json', JSON.stringify({...catalog, sequence: 2, release_id: '00000000000000000002-' + catalog.manifest_sha256}));
  assert.equal(await (await site.fetch(req(FEED + catalog.artifact.filename), env)).text(), '0123456789');
  assert.equal(await (await site.fetch(req('/browser/sources/' + catalog.source.filename), env)).text(), 'source');
});

test('public host rejects writes, directory listing, traversal, query and unknown platforms', async () => {
  const {env} = fixture();
  assert.equal((await site.fetch(req('/', {method: 'PUT', body: 'bad'}), env)).status, 405);
  for (const path of [FEED, '/browser/artifacts/linux-x86_64/', '/browser/updates/stable/windows-x86_64/manifest.json', '/v1/put', FEED + 'manifest.json?next=bad', FEED + '%2fmanifest.json', FEED + '.secret']) {
    assert.equal((await site.fetch(req(path), env)).status, 404, path);
  }
});

test('bounded malformed catalog and unsafe labels do not enter HTML', async () => {
  const {env, bucket, catalog} = fixture();
  for (const version_label of ['<script>alert(1)</script>', '../secret']) {
    bucket.save('browser/channels/stable/linux-x86_64.json', JSON.stringify({...catalog, version_label}));
    const result = await site.fetch(req('/'), env);
    assert.equal(result.status, 503);
    assert.doesNotMatch(await result.text(), /script/);
  }
  bucket.save('browser/channels/stable/linux-x86_64.json', ' '.repeat(16385));
  assert.equal((await site.fetch(req('/'), env)).status, 503);
  assert.equal(escapeHtml('<&"\''), '&lt;&amp;&quot;&#39;');
});

test('range parser bounds suffixes and rejects multipart, malformed and unsafe ranges', () => {
  assert.deepEqual(byteRange('bytes=-4', 10), {offset: 6, length: 4});
  assert.deepEqual(byteRange('bytes=8-999', 10), {offset: 8, length: 2});
  for (const value of ['bytes=-0', 'bytes=-', 'bytes=4-2', 'bytes=0-1,4-5', 'items=0-2', 'bytes=999999999999999999999-']) assert.throws(() => byteRange(value, 10));
});
