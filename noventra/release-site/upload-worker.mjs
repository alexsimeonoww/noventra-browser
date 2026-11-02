// Copyright 2026 The Noventra Authors
// Temporary publisher bridge. Deploy only for an upload, then remove it.
// It contains a PUBLIC key; only the offline signer can authorize requests.

const DOMAIN = 'noventra-r2-upload-v1\n';
const FORMAT = 'noventra-r2-upload-v1';
const EMPTY = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855';
const PART = 8 * 1024 * 1024;
const MAX_OBJECT = 300 * 1024 * 1024;
const HASH = /^[a-f0-9]{64}$/;
const FILE = '[A-Za-z0-9][A-Za-z0-9._+-]{0,150}';
const COMMON = ['format', 'origin', 'method', 'path', 'action', 'key', 'expires', 'size', 'sha256'];
const METHODS = {inspect: 'GET', read: 'GET', put: 'PUT', begin: 'POST', part: 'PUT', complete: 'POST', abort: 'POST'};

/** @typedef {{format: string, origin: string, method: string, path: string, action: keyof typeof METHODS, key: string, expires: number, size: number, sha256: string, expected_etag?: string | null, total_size?: number, total_sha256?: string, upload_id?: string, part_number?: number, parts?: R2UploadedPart[]}} Permit */
/** @returns {never} */
function fail() { throw new Error('Invalid upload permit'); }

/** @param {unknown} value @returns {value is Record<string, unknown>} */
function object(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }

/** @param {unknown} value @returns {string} */
export function canonical(value) {
  if (value === null || typeof value !== 'object') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  if (!object(value)) fail();
  return `{${Object.keys(value).sort().map(k => `${JSON.stringify(k)}:${canonical(value[k])}`).join(',')}}`;
}

/** @param {string | null} value @param {number} limit */
function decode(value, limit) {
  if (!value || value.length > limit || !/^[A-Za-z0-9_-]+$/.test(value)) fail();
  return Uint8Array.from(atob(value.replace(/-/g, '+').replace(/_/g, '/')), c => c.charCodeAt(0));
}

/** @param {string} key @returns {{kind: 'pointer' | 'immutable', hash?: undefined} | {kind: 'blob', hash: string}} */
export function keyKind(key) {
  if (key === 'browser/channels/stable/linux-x86_64.json') return {kind: 'pointer'};
  const blob = new RegExp(`^browser/(artifacts/linux-x86_64|sources)/([a-f0-9]{64})/(${FILE})$`).exec(key);
  if (blob && /\.(tar\.xz|zip)$/.test(blob[3])
      && (blob[1] === 'sources' || blob[3].endsWith('.tar.xz'))) return {kind: 'blob', hash: blob[2]};
  if (new RegExp(`^browser/(artifact-index/linux-x86_64|source-index)/(${FILE})\\.json$`).test(key)) return {kind: 'immutable'};
  if (/^browser\/releases\/stable\/linux-x86_64\/[0-9]{20}-[a-f0-9]{64}\/(manifest\.json|manifest\.sig|distribution\.json|source-info\.json)$/.test(key)) return {kind: 'immutable'};
  fail();
}

/** @param {unknown} value @param {number} min @param {number} max @returns {number} */
function integer(value, min, max) {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < min || value > max) fail();
  return value;
}

/** @param {Request} request @param {string} publicKey */
export async function authorize(request, publicKey) {
  const url = new URL(request.url);
  if (url.search || url.protocol !== 'https:') fail();
  const bytes = decode(request.headers.get('X-Noventra-Permit'), 24000);
  const raw = new TextDecoder('utf-8', {fatal: true, ignoreBOM: false}).decode(bytes);
  /** @type {unknown} */
  const value = JSON.parse(raw);
  // Canonical equality rejects duplicate keys and alternative spellings before
  // any permit field is used. Only the exact action schema is accepted.
  if (canonical(value) !== raw || !object(value) || typeof value.key !== 'string'
      || typeof value.action !== 'string' || !Object.hasOwn(METHODS, value.action)) fail();
  const action = /** @type {keyof typeof METHODS} */ (value.action);
  const kind = keyKind(value.key);
  const fields = [...COMMON];
  if (value.action === 'put' && kind.kind === 'pointer') fields.push('expected_etag');
  if (['begin', 'complete'].includes(value.action)) fields.push('total_size', 'total_sha256');
  if (['part', 'complete', 'abort'].includes(value.action)) fields.push('upload_id');
  if (value.action === 'part') fields.push('part_number');
  if (value.action === 'complete') fields.push('parts');
  if (Object.keys(value).sort().join('\0') !== fields.sort().join('\0')) fail();
  if (value.format !== FORMAT || value.origin !== url.origin || value.method !== request.method
      || METHODS[action] !== request.method || value.path !== url.pathname
      || value.path !== `/v1/${value.action}` || typeof value.sha256 !== 'string' || !HASH.test(value.sha256)) fail();
  const now = Math.floor(Date.now() / 1000);
  integer(value.expires, now + 1, now + 300);
  const size = integer(value.size, 0, PART);
  if (!['put', 'part'].includes(value.action) && (value.size !== 0 || value.sha256 !== EMPTY)) fail();
  if (['begin', 'part', 'complete', 'abort'].includes(value.action) && kind.kind !== 'blob') fail();
  if (['begin', 'complete'].includes(value.action)) {
    integer(value.total_size, 1, MAX_OBJECT);
    if (value.total_sha256 !== kind.hash) fail();
  }
  if (['part', 'complete', 'abort'].includes(value.action)
      && (typeof value.upload_id !== 'string' || !/^[A-Za-z0-9+/=_-]{1,2048}$/.test(value.upload_id))) fail();
  if (value.action === 'part') {
    integer(value.part_number, 1, Math.ceil(MAX_OBJECT / PART));
    integer(value.size, 1, PART);
  }
  if (value.action === 'complete') {
    if (!Array.isArray(value.parts) || value.parts.length !== Math.ceil(integer(value.total_size, 1, MAX_OBJECT) / PART)) fail();
    value.parts.forEach((/** @type {unknown} */ p, i) => {
      if (!object(p) || Object.keys(p).sort().join() !== 'etag,partNumber' || p.partNumber !== i + 1
          || typeof p.etag !== 'string' || !/^[A-Za-z0-9+/=_-]{1,256}$/.test(p.etag)) fail();
    });
  }
  if (value.action === 'put') {
    integer(value.size, 1, PART);
    if (kind.kind === 'blob' && value.sha256 !== kind.hash) fail();
    if (kind.kind === 'pointer' && value.expected_etag !== null
        && (typeof value.expected_etag !== 'string' || !/^[A-Za-z0-9+/=_-]{1,256}$/.test(value.expected_etag))) fail();
    if (kind.kind === 'pointer' && size > 16384) fail();
  }
  const signature = decode(request.headers.get('X-Noventra-Signature'), 100);
  if (signature.length !== 64) fail();
  const der = Uint8Array.from(atob(publicKey), c => c.charCodeAt(0));
  if (der.length !== 44) fail();
  const key = await crypto.subtle.importKey('spki', der, {name: 'Ed25519'}, false, ['verify']);
  const message = new TextEncoder().encode(DOMAIN + raw);
  if (!await crypto.subtle.verify('Ed25519', key, signature, message)) fail();
  return /** @type {Permit} */ (value);
}

/** @param {Request} request @param {Permit} permit */
async function body(request, permit) {
  const output = new Uint8Array(permit.size);
  let offset = 0;
  if (request.body) {
    const reader = request.body.getReader();
    try {
      while (true) {
        const remaining = permit.expires * 1000 - Date.now();
        if (remaining <= 0) fail();
        /** @type {number | undefined} */
        let timer;
        /** @type {ReadableStreamReadResult<Uint8Array>} */
        let next;
        try {
          next = await Promise.race([reader.read(), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('Upload permit expired')), remaining);
          })]);
        } finally {
          if (timer !== undefined) clearTimeout(timer);
        }
        if (next.done) break;
        if (offset + next.value.byteLength > output.length) fail();
        output.set(next.value, offset);
        offset += next.value.byteLength;
      }
    } finally {
      await reader.cancel();
      reader.releaseLock();
    }
  }
  if (offset !== output.length) fail();
  const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', output))].map(b => b.toString(16).padStart(2, '0')).join('');
  if (hash !== permit.sha256) fail();
  return output;
}

/** @param {unknown} value */
function json(value, status = 200) {
  return Response.json(value, {status, headers: {'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff'}});
}

/** @param {R2Object} object */
function info(object) {
  return {key: object.key, size: object.size, etag: object.etag};
}

/** @param {Request} request @param {UploadEnv} env */
export async function handleUpload(request, env) {
  let permit;
  try { permit = await authorize(request, env.UPLOAD_PUBLIC_KEY_SPKI); }
  catch { return json({error: 'Unauthorized'}, 403); }
  let bytes;
  try { bytes = await body(request, permit); }
  catch { return json({error: 'Body does not match signed permit'}, 400); }
  const bucket = env.RELEASES;
  if (permit.action === 'inspect') {
    const object = await bucket.head(permit.key);
    return object ? json(info(object)) : json({error: 'Not found'}, 404);
  }
  if (permit.action === 'read') {
    const object = await bucket.get(permit.key);
    if (!object) return json({error: 'Not found'}, 404);
    return new Response(object.body, {headers: {'Cache-Control': 'no-store', 'Content-Type': 'application/octet-stream', 'Content-Length': String(object.size), ETag: object.httpEtag}});
  }
  if (permit.action === 'put') {
    const conditional = new Headers();
    if (permit.key === 'browser/channels/stable/linux-x86_64.json' && permit.expected_etag !== null) {
      conditional.set('If-Match', `"${permit.expected_etag}"`);
    } else {
      conditional.set('If-None-Match', '*');
    }
    const object = await bucket.put(permit.key, bytes, {onlyIf: conditional, sha256: permit.sha256,
      customMetadata: {sha256: permit.sha256}, httpMetadata: {contentType: 'application/octet-stream'}});
    return object ? json(info(object)) : json({error: 'Publication precondition failed'}, 412);
  }
  if (permit.action === 'begin') {
    if (await bucket.head(permit.key)) return json({error: 'Object already exists'}, 409);
    if (typeof permit.total_sha256 !== 'string') fail();
    const upload = await bucket.createMultipartUpload(permit.key, {customMetadata: {sha256: permit.total_sha256}, httpMetadata: {contentType: 'application/octet-stream'}});
    return json({key: upload.key, upload_id: upload.uploadId});
  }
  if (typeof permit.upload_id !== 'string') fail();
  const upload = bucket.resumeMultipartUpload(permit.key, permit.upload_id);
  if (permit.action === 'part') {
    const part = await upload.uploadPart(integer(permit.part_number, 1, Math.ceil(MAX_OBJECT / PART)), bytes);
    return json({partNumber: part.partNumber, etag: part.etag});
  }
  if (permit.action === 'abort') {
    await upload.abort();
    return json({aborted: true});
  }
  if (permit.action === 'complete') {
    if (await bucket.head(permit.key)) return json({error: 'Object already exists'}, 409);
    if (!permit.parts) fail();
    const object = await upload.complete(permit.parts);
    if (object.size !== permit.total_size) return json({error: 'Completed size mismatch'}, 409);
    return json(info(object));
  }
  return json({error: 'Unsupported action'}, 400);
}

/** @type {ExportedHandler<UploadEnv>} */
export default {
  async fetch(request, env) {
    try { return await handleUpload(request, env); }
    catch { return json({error: 'Upload operation failed'}, 503); }
  },
};
