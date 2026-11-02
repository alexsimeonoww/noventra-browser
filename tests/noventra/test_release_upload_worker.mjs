import test from 'node:test';
import assert from 'node:assert/strict';
import {generateKeyPairSync, sign} from 'node:crypto';
import uploader, {canonical} from '../../noventra/release-site/upload-worker.mjs';
import {Bucket, sha} from './release_site_test_helpers.mjs';

const pair = generateKeyPairSync('ed25519');
const publicDer = pair.publicKey.export({format: 'der', type: 'spki'});
const origin = 'https://owned-upload.example.workers.dev';
const empty = sha('');
function fixture() { return {RELEASES: new Bucket(), UPLOAD_PUBLIC_KEY_SPKI: publicDer.toString('base64')}; }
function request(action, key, data = new Uint8Array(), fields = {}, options = {}) {
  const method = {inspect: 'GET', read: 'GET', put: 'PUT', begin: 'POST', part: 'PUT', complete: 'POST', abort: 'POST'}[action];
  const value = {format: 'noventra-r2-upload-v1', origin, method, path: '/v1/' + action, action, key, expires: Math.floor(Date.now() / 1000) + 240, size: data.length, sha256: sha(data), ...fields};
  const raw = options.raw || canonical(value);
  const signature = sign(null, Buffer.from((options.domain ?? 'noventra-r2-upload-v1\n') + raw), options.key || pair.privateKey);
  return new Request(origin + value.path, {method, headers: {'X-Noventra-Permit': Buffer.from(raw).toString('base64url'), 'X-Noventra-Signature': signature.toString('base64url')}, ...(['PUT', 'POST'].includes(method) ? {body: options.body || data} : {})});
}

test('signed immutable put/read/inspect works; replay cannot replace immutable bytes', async () => {
  const env = fixture();
  const key = 'browser/releases/stable/linux-x86_64/' + '0'.repeat(19) + '1-' + 'a'.repeat(64) + '/manifest.sig';
  const data = new Uint8Array(64);
  assert.equal((await uploader.fetch(request('put', key, data), env)).status, 200);
  assert.equal((await uploader.fetch(request('put', key, data), env)).status, 412);
  assert.equal((await (await uploader.fetch(request('inspect', key), env)).json()).size, 64);
  assert.equal((await (await uploader.fetch(request('read', key), env)).arrayBuffer()).byteLength, 64);
});

test('channel initial absent and later ETag compare-and-swap reject races and replay', async () => {
  const env = fixture();
  const key = 'browser/channels/stable/linux-x86_64.json';
  const first = new TextEncoder().encode('first');
  let result = await uploader.fetch(request('put', key, first, {expected_etag: null}), env);
  assert.equal(result.status, 200);
  const etag = (await result.json()).etag;
  assert.equal((await uploader.fetch(request('put', key, first, {expected_etag: null}), env)).status, 412);
  const next = new TextEncoder().encode('second');
  assert.equal((await uploader.fetch(request('put', key, next, {expected_etag: etag}), env)).status, 200);
  assert.equal((await uploader.fetch(request('put', key, first, {expected_etag: etag}), env)).status, 412);
});

test('real signed multipart begin, hashed parts, ordered complete and authenticated full read', async () => {
  const env = fixture();
  const first = new Uint8Array(8 * 1024 * 1024).fill(97);
  const last = new Uint8Array([98]);
  const whole = new Uint8Array(first.length + 1); whole.set(first); whole.set(last, first.length);
  const digest = sha(whole);
  const key = `browser/artifacts/linux-x86_64/${digest}/noventra-test15.tar.xz`;
  const begun = await uploader.fetch(request('begin', key, new Uint8Array(), {total_size: whole.length, total_sha256: digest}), env);
  assert.equal(begun.status, 200);
  const upload_id = (await begun.json()).upload_id;
  const parts = [];
  for (const [i, data] of [first, last].entries()) {
    const result = await uploader.fetch(request('part', key, data, {upload_id, part_number: i + 1}), env);
    assert.equal(result.status, 200);
    parts.push(await result.json());
  }
  const completed = await uploader.fetch(request('complete', key, new Uint8Array(), {upload_id, parts, total_size: whole.length, total_sha256: digest}), env);
  assert.equal(completed.status, 200);
  assert.equal((await completed.json()).size, whole.length);
  assert.equal(sha(Buffer.from(await (await uploader.fetch(request('read', key), env)).arrayBuffer())), digest);
});

test('wrong signature/domain/key, expired/future and duplicate/unknown fields are rejected', async () => {
  const env = fixture();
  const key = 'browser/channels/stable/linux-x86_64.json';
  const other = generateKeyPairSync('ed25519');
  for (const [fields, options] of [
    [{}, {key: other.privateKey}], [{}, {domain: ''}],
    [{expires: Math.floor(Date.now() / 1000) - 1}, {}],
    [{expires: Math.floor(Date.now() / 1000) + 600}, {}], [{unknown: true}, {}],
    [{origin: 'https://attacker.example'}, {}],
  ]) assert.equal((await uploader.fetch(request('inspect', key, new Uint8Array(), fields, options), env)).status, 403);
  const original = request('inspect', key);
  const value = JSON.parse(Buffer.from(original.headers.get('X-Noventra-Permit'), 'base64url'));
  const raw = canonical(value).replace('"action":"inspect"', '"action":"inspect","action":"inspect"');
  assert.equal((await uploader.fetch(request('inspect', key, new Uint8Array(), {}, {raw}), env)).status, 403);
});

test('unsigned endpoints, invalid prefixes, traversal and unbound payloads cannot write', async () => {
  const env = fixture();
  assert.equal((await uploader.fetch(new Request(origin + '/v1/put', {method: 'PUT', body: 'x'}), env)).status, 403);
  for (const key of ['other-bucket/secret', 'browser/channels/stable/../secret', 'browser/releases/stable/linux-x86_64/invalid/manifest.json', 'browser/artifacts/linux-x86_64/' + 'f'.repeat(64) + '/noventra.tar.xz']) {
    assert.equal((await uploader.fetch(request('put', key, new Uint8Array([1])), env)).status, 403);
  }
  const key = 'browser/releases/stable/linux-x86_64/' + '0'.repeat(19) + '1-' + 'a'.repeat(64) + '/manifest.sig';
  assert.equal((await uploader.fetch(request('put', key, new Uint8Array([1]), {}, {body: new Uint8Array([2])}), env)).status, 400);
  assert.equal((await uploader.fetch(request('put', key, new Uint8Array([1]), {}, {body: new Uint8Array([1, 2])}), env)).status, 400);
  assert.equal(env.RELEASES.objects.size, 0);
});

test('multipart IDs, part order and size are signed; abort is scoped to an owned upload', async () => {
  const env = fixture();
  const key = 'browser/sources/' + 'a'.repeat(64) + '/source.zip';
  const begun = await (await uploader.fetch(request('begin', key, new Uint8Array(), {total_size: 9, total_sha256: 'a'.repeat(64)}), env)).json();
  assert.equal((await uploader.fetch(request('complete', key, new Uint8Array(), {upload_id: begun.upload_id, total_size: 9, total_sha256: 'a'.repeat(64), parts: [{partNumber: 2, etag: 'abc'}]}), env)).status, 403);
  assert.equal((await uploader.fetch(request('abort', key, new Uint8Array(), {upload_id: begun.upload_id}), env)).status, 200);
  assert.equal(env.RELEASES.uploads.size, 0);
  assert.equal(empty, 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
});
