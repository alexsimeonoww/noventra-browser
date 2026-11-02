import {createHash, randomUUID} from 'node:crypto';

export const sha = bytes => createHash('sha256').update(bytes).digest('hex');
export class Bucket {
  constructor() { this.objects = new Map(); this.uploads = new Map(); this.gets = []; }
  save(key, data, options = {}) {
    const bytes = new Uint8Array(typeof data === 'string' ? new TextEncoder().encode(data) : data);
    const etag = sha(bytes);
    const value = {key, size: bytes.length, etag, httpEtag: `"${etag}"`, customMetadata: options.customMetadata || {}, bytes};
    this.objects.set(key, value);
    return value;
  }
  async head(key) {
    const v = this.objects.get(key);
    return v ? {key: v.key, size: v.size, etag: v.etag, httpEtag: v.httpEtag, customMetadata: v.customMetadata} : null;
  }
  async get(key, options = {}) {
    this.gets.push(key);
    const meta = await this.head(key);
    if (!meta) return null;
    if (options.onlyIf?.etagMatches && options.onlyIf.etagMatches !== meta.etag) return meta;
    let bytes = this.objects.get(key).bytes;
    if (options.range) bytes = bytes.slice(options.range.offset, options.range.offset + options.range.length);
    return {...meta, body: new Response(bytes).body, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)};
  }
  async put(key, bytes, options = {}) {
    const current = await this.head(key);
    const conditional = options.onlyIf;
    if (conditional instanceof Headers) {
      if (conditional.get('If-None-Match') === '*' && current) return null;
      const match = conditional.get('If-Match');
      if (match && (!current || match !== current.httpEtag)) return null;
    }
    if (options.sha256 && options.sha256 !== sha(bytes)) throw new Error('R2 checksum mismatch');
    return this.save(key, bytes, options);
  }
  async createMultipartUpload(key, options) {
    const uploadId = randomUUID();
    this.uploads.set(uploadId, {key, options, parts: new Map()});
    return this.resumeMultipartUpload(key, uploadId);
  }
  resumeMultipartUpload(key, uploadId) {
    const store = this;
    const item = () => {
      const v = store.uploads.get(uploadId);
      if (!v || v.key !== key) throw new Error('Unknown upload');
      return v;
    };
    return {key, uploadId,
      async uploadPart(partNumber, bytes) {
        const etag = sha(bytes);
        item().parts.set(partNumber, {etag, bytes});
        return {partNumber, etag};
      },
      async complete(parts) {
        const v = item();
        const chunks = parts.map(p => {
          const part = v.parts.get(p.partNumber);
          if (!part || part.etag !== p.etag) throw new Error('Wrong part');
          return part.bytes;
        });
        const bytes = new Uint8Array(chunks.reduce((n, p) => n + p.length, 0));
        let position = 0;
        for (const chunk of chunks) { bytes.set(chunk, position); position += chunk.length; }
        const result = store.save(key, bytes, v.options);
        store.uploads.delete(uploadId);
        return result;
      },
      async abort() { item(); store.uploads.delete(uploadId); },
    };
  }
}
